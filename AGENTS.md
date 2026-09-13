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
- `src/c/ui_reply.c` — conversation view: a ScrollLayer under a drawn header.
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
- **CalDAV servers are not all Basic.** Baikal, and SabreDAV generally, default
  to Digest and reject Basic outright with a 401 -- indistinguishable from a
  wrong password unless you read the challenge. Every request starts as Basic
  and is redone with Digest when the server asks; the challenge is cached per
  host, so a session pays one extra round trip in total. MD5 lives in
  `00_util.js` (validated against known vectors) because PebbleKit JS ships no
  crypto. Digest signs the request *path*, not the whole URL. Only MD5 and
  MD5-sess with `qop=auth` are implemented; anything else is reported rather
  than failing silently.
- **iCloud redirects, and the redirect is the whole game.** `caldav.icloud.com`
  is only a bootstrap host: it answers the principal lookup and then 301s
  everything else to a per-account shard (`pNN-caldav.icloud.com`). `davSend`
  follows 301/302/307/308 itself, re-issuing the same method and body -- a
  redirect followed automatically can turn a PROPFIND into a GET, which returns
  200 with a body that parses to nothing, so the failure looks like an empty
  calendar rather than a redirect. Relative hrefs must then be resolved against
  where the request *landed* (`effectiveUrl`, which prefers `xhr.responseURL`),
  not against what was asked for.
- **All CalDAV traffic must go through `davSend`.** Two write paths once called
  `httpRequest` directly and so skipped authentication entirely.
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
- **Scrolling is the platform's, not ours.** Lists are a `MenuLayer`, the
  conversation is a `ScrollLayer`, and both hand UP and DOWN to the widget with
  `*_set_click_config_onto_window`. An earlier version scrolled by hand -- a
  measured step, a measured repeat, an eased animation, a rubber-band bounce at
  the ends, over-scroll and double-tap to change turns -- and it still read as
  subtly wrong next to every other app on the watch. Matching the platform is
  not a target you converge on by tuning constants; either the platform is doing
  it or it is not. Do not reintroduce a hand-rolled scroll, and do not add a
  gesture the widget does not already have.
- Lists use `menu_layer_set_center_focused`, the style the system launcher and
  Settings use: the selected row holds the middle of the screen. Short lists
  therefore open with empty space above the first row -- that is the mode
  working, not a layout bug.
- No list exits by scrolling off its end: a MenuLayer simply stops there, which
  is both the stock behaviour and the one asked for. The way back into a
  conversation is to select it, and BACK leaves a submenu.
- The options behind a long press of SELECT are a native `ActionMenu` built on
  the watch, not a list fetched from the phone: it opens on the press rather
  than after a round trip, and the watch is the side that knows which turn is on
  screen, so it offers a direction only when there is a turn that way. The cost
  is that a new option needs a new `.pbw`. `LIST_CHAT_ACTIONS` and
  `sendChatActionsList` are retired; the list id stays reserved.
- The conversation's question and answer are `TextLayer`s inside the ScrollLayer.
  TextLayer does not size itself, so `recompute_layout` still measures both and
  sets frames -- it feeds frames now instead of draw calls. The layers hold
  pointers into `s_turn` rather than copies, so the text is re-pointed whenever
  the turn changes, which is exactly when that function runs.
- Running off the top or bottom of a turn steps to the neighbouring one. This is
  **not** a return to hand-rolled scrolling: `scroll_layer_scroll_up_click_handler`
  and its DOWN twin are exported by the SDK precisely so a caller can wrap them,
  and the ScrollLayer's own `click_config_provider` is documented as the place to
  change what UP and DOWN do. All that is added is the edge case; the step and the
  animation are still the widget's. A repeat must never cross
  (`click_recognizer_is_repeating`): holding is a request to scroll, and being
  flung into the next turn mid-hold loses your place. At the first or last turn
  nothing happens and nothing is announced, the way a list that has run out
  behaves.
- `SCROLL_REPEAT_MS` is the one number that has to match the platform, because
  re-subscribing the buttons means owning the repeat cadence. Do not guess it and
  do not reason about it: measure. A stock ScrollLayer holds at ~162px/s on
  Emery; log `scroll_layer_get_content_offset` from a
  `content_offset_changed_handler` under a temporary build flag, hold the button,
  and compare steady-state velocity. 170ms lands at ~158px/s. Re-measure after
  any change here.
- There are no scroll arrows. A `ContentIndicator` was tried and removed: on a
  200px screen the two strips cost more than the affordance was worth.
- A ScrollLayer hands its window's remaining buttons to the app through
  `ScrollLayerCallbacks.click_config_provider`; a MenuLayer has no such hook, so
  anything it does not bind keeps its default. Neither may re-bind UP or DOWN.
- A widget draws its own content, so a flag set inside a draw callback -- like
  the marquee's "still sliding" -- has no frame boundary to clear it. Read it
  and clear it in the tick, or the redraw timer never stops.
- **Marquee draw order.** Pebble has no per-draw clipping, so
  `theme_draw_marquee` paints the text wide and masks the overflow back in the
  background colour. Anything sharing the strip -- a badge, the clock, a chevron,
  the current-row dot -- must be measured first but drawn *after* the label, or
  the mask (or the overflowing text) eats it.
- Do not pre-truncate text that will scroll. Titles and row labels go to the
  watch at full length; trimming them phone-side leaves the marquee nothing to
  reveal, which is exactly the bug it is there to solve.
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
the chats list with New Chat selected; a reply scrolls and stops at both ends;
the options list moves between turns; submenus repaint after every keypress; Settings round-trips and
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
