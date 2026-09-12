# Assistant — agent handoff

## What this is

A voice assistant watchapp for Pebble Time 2 (Emery) talking to the Codex
backend. `README.md` covers behaviour and setup; this file covers the things
that will bite you.

- App UUID: `69142e36-d120-437a-b89f-0401f24b4c62`
- Targets: emery (primary), basalt, diorite, flint. **Chalk is not supported** —
  the layouts assume a rectangular screen.
- Development branch: `claude/serene-tesla-4f9azb`

## Canonical files

- `src/c/assistant.h` — the whole watch/phone protocol lives here.
- `src/c/ui_reply.c` — conversation view, scrolling, turn transitions.
- `src/c/ui_list.c` — every list (chats, settings, models, thinking, quick prompts).
- `src/pkjs/parts/*.js` — phone side, concatenated by `wscript` in the order
  listed in `PKJS_PARTS`. Order matters; only `90_main.js` registers listeners.
- `docs/index.html` — the hosted settings page.

## Invariants

- **Never change the UUID.**
- **Message keys are append-only.** Reordering `messageKeys` in `package.json`
  breaks the C/JS contract silently. Same for the `WREQ_*` / `PEVT_*` /
  `ACT_*` constants, which are duplicated in `src/c/assistant.h` and
  `src/pkjs/parts/60_watchlink.js` — change both or neither.
- **The PKJS bundle must stay a single flat `pebble-js-app.js`.** Do not add an
  `index.js` or turn on `enableMultiJS`. The Core Devices iOS app can hang on
  "Loading watchapp" otherwise. Inspect the script inside the built PBW, not
  just the sources.
- **ES5 only in `src/pkjs/`.** No `let`, arrow functions, `const`, template
  literals or `Promise`. The companion's JS engine level has moved around
  between releases; ES5 has never broken.
- **The watch renders, the phone decides.** Menus arrive as `PEVT_LIST_*`
  messages with an action code per row. Resist adding menu logic to C.
- **Only advertise tools whose service is configured** (`buildToolDefinitions`).
  A model told it has a calendar will claim to have checked one.
- Static buffers on the watch are sized for the 24 KB platforms. `MAX_ANSWER_LEN`
  is 2048; the phone trims to 2000 UTF-8 bytes before sending.
- AppMessage text is chunked at 240 UTF-8 bytes on character boundaries
  (`splitUtf8`). Do not raise this without testing on iOS.
- Wakeups: eight slots, shared with nothing else. `timers.c` recycles the
  furthest-out slot when full and drops slots the firmware has forgotten.

## Navigation model

Two screens. The chats list is the root window; the reply view is pushed on top
of it. Submenus stack above whichever is in front.

- Launch pushes the chats list, then opens dictation ~60 ms later.
- Cancelling dictation hides the reply view, revealing the chats list.
- Every list exits by scrolling past either edge, as well as by BACK.
- The root list pops to the conversation if one exists, otherwise it bounces.

Dictation is a system modal that owns all four buttons; BACK is the only signal
it gives back. Do not design gestures that need UP/DOWN during recording.

## Codex backend

`POST https://chatgpt.com/backend-api/codex/responses`, streaming Responses API.

- Headers: `Authorization: Bearer`, `chatgpt-account-id`, `OpenAI-Beta:
  responses=experimental`, `originator: codex_cli_rs`, `session_id`.
- Body: `store: false`, `stream: true`,
  `include: ["reasoning.encrypted_content"]`, `prompt_cache_key`.
- Streaming is not optional; the reply is parsed out of the SSE body.
- Echoed items keep `encrypted_content` but **lose `id` and `status`** — the API
  rejects server-assigned ids on a `store: false` request (`sanitizeItem`).
- Token refresh: `POST https://auth.openai.com/oauth/token` with client id
  `app_EMoamEEZ73f0CkXaXp7hrann`. Refreshed tokens are cached under a
  fingerprint of the pasted blob, so pasting a new `auth.json` invalidates them.

## Testing without hardware

There is no Pebble SDK in most sandboxes, so two harnesses exist to keep this
honest. They are not in the repo; recreate them if you need them.

- **C**: write a stub `pebble.h` declaring the SDK surface, then
  `gcc -fsyntax-only -Wall -Wextra -I<stub> src/c/*.c`, plus an `nm` pass over
  the objects to catch unresolved cross-module symbols.
- **JS**: concatenate the parts exactly as `wscript` does, run the bundle in a
  `vm` context with stubs for `Pebble`, `localStorage`, `XMLHttpRequest` and
  `navigator`, then drive `appmessage` events and assert on what gets sent to
  the watch. Mock the Codex SSE stream and a CalDAV server; both found real bugs.

With hardware:

```sh
for f in src/pkjs/parts/*.js; do node --check "$f"; done
pebble build && pebble install --phone 192.168.x.x
```

Then check, in this order: dictation opens on launch; BACK lands on the chats
list with New Chat selected; a reply scrolls and the edges move between turns;
Settings round-trips and Diagnostics reports each service by name; a timer both
vibrates and appears in the timeline.

## Config page

Hosted from `docs/` on `main` via GitHub Pages; `CONFIG_URL` in
`05_settings.js` points at it. Editing it locally changes nothing for the user
until it is deployed. Settings travel in the URL hash and come back as one
replacement object — keep the element ids and that response shape stable.

The page cannot reach CalDAV or Gmail (no CORS), which is why connection checks
run phone-side after a save and are handed back on the next open. Do not move
them into the page.
