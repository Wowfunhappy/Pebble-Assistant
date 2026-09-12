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
- `src/c/ui_list.c` — every list (chats, settings, models, thinking, chat options).
- `src/c/toast.c` — the confirmation overlay. Shared because most toasts are
  raised by an action that leaves you on a different screen than it started on.
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
- **Keep the watch menu to what was asked for.** It is Model, Thinking, Web
  search, Location, Confirm speech and Listen on open -- nothing else. Anything
  that is set once and forgotten (text size, credentials, places) belongs on the
  phone, where there is a keyboard and a screen to read it on.
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
- "Ask again" carries the turn index on `WREQ_ASK` (`WINT`, -1 to append) rather
  than arming state on the phone: the conversation is only truncated when the
  replacement question actually arrives, so a cancelled dictation destroys
  nothing. An absent `WINT` must mean append -- defaulting to 0 would wipe the
  thread.
- Cancelling dictation hides the reply view, revealing the chats list.
- No list exits by scrolling off its end -- every list simply stops, with a
  rubber-band bounce. The way back into a conversation is to select it, and BACK
  leaves a submenu. An over-scroll that silently changed screens was far too
  easy to trigger while hunting for a row.
- Text size lives on the phone only. A change arrives in PEVT_SETTINGS and must
  re-measure both windows: every cached text metric was taken with the old font.
- The header clock means a minute tick has to redraw whichever window is front.

Dictation is a system modal that owns all four buttons; BACK is the only signal
it gives back. Do not design gestures that need UP/DOWN during recording.

## Model catalog

**Never hardcode a model list or a reasoning ladder.** Both come from

```
GET https://chatgpt.com/backend-api/codex/models?client_version=X.Y.Z
```

(`15_models.js`), authenticated like a chat request; `client_version` is
required and the request 400s without it. Per model the response carries
`slug`, `display_name`, `description`, `visibility` (list/hide/none),
`priority`, `minimal_client_version`, `default_reasoning_level` and
`supported_reasoning_levels`. Those levels are per-model — currently low,
medium, high, xhigh, max, ultra in varying subsets — so the Thinking menu is
built from the selected model, not from a constant. The catalog is trimmed
before caching because the raw payload is tens of KB per model and the config
page travels through a URL hash.

## Codex backend

`POST https://chatgpt.com/backend-api/codex/responses`, streaming Responses API.

- Headers: `Authorization: Bearer`, `chatgpt-account-id`, `OpenAI-Beta:
  responses=experimental`, `originator: codex_cli_rs`, `session_id`.
- Body: `store: false`, `stream: true`,
  `include: ["reasoning.encrypted_content"]`, `prompt_cache_key`.
- Streaming is not optional; the reply is parsed out of the SSE body.
- **`response.completed` arrives with `output: []`.** The output items only ever
  come as `response.output_item.done` events and must be accumulated as they
  stream past. Reading `output` off the completion event returns nothing, which
  looks exactly like the model saying nothing. Any mock that populates
  `completed.output` is testing a protocol that does not exist.
- Echoed items keep `encrypted_content` but **lose `id` and `status`** — the API
  rejects server-assigned ids on a `store: false` request (`sanitizeItem`).
- Token refresh: `POST https://auth.openai.com/oauth/token` with client id
  `app_EMoamEEZ73f0CkXaXp7hrann`. Refreshed tokens are cached under a
  fingerprint of the pasted blob, so pasting a new `auth.json` invalidates them.

## Building and testing

The SDK installs cleanly in a sandbox, so there is no excuse for shipping an
unbuilt change.

```sh
uv venv /tmp/pblenv --python 3.11
uv pip install --python /tmp/pblenv/bin/python pebble-tool   # 5.x, Core Devices, py>=3.10
/tmp/pblenv/bin/pebble sdk install 4.33.1                    # pulls the ARM toolchain too
export PATH="/tmp/pblenv/bin:$PATH"
pebble build
```

A clean build must produce **zero warnings from `src/c/`**, and the PBW must
contain a single root-level `pebble-js-app.js`:

```sh
python3 -c "import zipfile; print(zipfile.ZipFile('build/Pebble-Assistant.pbw').namelist())"
```

### The emulator

Worth the setup: it runs the real firmware *and* the real PebbleKit JS, and it
has already caught bugs that reading the code did not.

```sh
apt-get install -y --no-install-recommends libsdl2-2.0-0   # qemu-pebble needs it
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
pebble install --emulator emery
pebble screenshot --emulator emery --no-open shot.png
pebble emu-button --emulator emery click up|down|select|back
pebble logs --emulator emery
```

Two environment gotchas:

- **No IPv6 in the sandbox.** pypkjs binds its websocket with an unspecified
  address, which gevent resolves to AF_INET6, and it dies with
  `OSError: [Errno 97]` while QEMU keeps running — so the tool just reports
  `Connection refused`. Patch the installed package to bind IPv4:
  `pypkjs/runner/websocket.py`, `pywsgi.WSGIServer(("", self.port)` ->
  `("127.0.0.1", self.port)`.
- **Stale state.** `/tmp/pb-emulator.json` holds the QEMU/pypkjs pids. Recycled
  pids make the tool insist "QEMU is already running". Delete the file and
  `pkill -f qemu-pebble; pkill -f pypkjs`.

What to expect on the emulator, so you do not chase ghosts:

- There is no voice service, so dictation always ends with status 3
  (`SystemAborted`) after ~8 s. The app retries once and then lands on the chats
  list. Allow ~20 s after install before driving the UI.
- `pebble send-app-message` does not deliver in this build. To exercise the
  reply view, temporarily stub `askModel` in `90_main.js` to deliver a canned
  turn and revert it afterwards, rather than injecting `PEVT_*` by hand.

### Without the SDK

If the SDK genuinely cannot be installed, the fallbacks are a stub `pebble.h`
plus `gcc -fsyntax-only` and an `nm` pass for unresolved symbols, and running
the concatenated JS bundle in a `vm` context with stubs for `Pebble`,
`localStorage`, `XMLHttpRequest` and `navigator`. Both are worth keeping in a
scratch directory; the JS one, driven against a mocked Codex SSE stream and a
mocked CalDAV server, found two real bugs.

Before release, check in this order: dictation opens on launch; BACK lands on
the chats list with New Chat selected; a reply scrolls and the edges move
between turns; submenus repaint after every keypress; Settings round-trips and
Diagnostics reports each service by name; a timer both vibrates and appears in
the timeline.

## Config page

Hosted from `docs/` on `main` via GitHub Pages; `CONFIG_URL` in
`05_settings.js` points at it. Editing it locally changes nothing for the user
until it is deployed. Settings travel in the URL hash and come back as one
replacement object — keep the element ids and that response shape stable.

The page cannot reach CalDAV or Gmail (no CORS), which is why connection checks
run phone-side after a save and are handed back on the next open. Do not move
them into the page.
