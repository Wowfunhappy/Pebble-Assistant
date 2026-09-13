# Assistant

A voice assistant for the Pebble Time 2 that talks to OpenAI models through the
Codex backend, and can actually do things: your calendar, your reminders, your
Apple Notes, and timers that vibrate on your wrist whether or not the phone is
in range.

The app opens straight into the microphone. Speak, read the answer, press
SELECT to keep talking. That is the whole interaction.

---

## How it works

```
[Pebble Time 2]  <--Bluetooth-->  [iPhone (PebbleKit JS)]  <--HTTPS-->  ChatGPT / Codex backend
                                             |                          CalDAV (calendar, reminders)
                                             |                          Gmail API (Apple Notes)
```

There is no server in the middle. Your phone talks directly to OpenAI and to
your own servers; conversations are stored in the Pebble companion app and
nowhere else.

The watch is deliberately a thin renderer. Every menu, label and piece of text
is built on the phone, which is why the model list, the settings copy and the
conversation history can all change without a new watch build.

| File | Role |
|---|---|
| `src/c/` | Watch UI: reply view, lists, dictation, wakeups, animations |
| `src/pkjs/parts/` | Phone side: Codex client, tools, CalDAV, Gmail, storage |
| `docs/` | Settings page and the Gmail connection helper (GitHub Pages) |

---

## Using it

### On the watch

**Launch → the microphone opens immediately.** Speak your question.

| Screen | Button | Action |
|---|---|---|
| Dictation | BACK | Cancel and show the chats list |
| Reply | UP / DOWN | Scroll, 32px a press. Hold to keep scrolling. A press at the top or bottom edge moves to the adjacent turn |
| Reply | UP ×2 / DOWN ×2 | Previous / next turn |
| Reply | SELECT | Keep talking in this conversation |
| Reply | SELECT (hold) | Options for this conversation |
| Reply | BACK | Chats list |
| Chats list | UP / DOWN | Move through the list |
| Chats list | SELECT | Open the row &mdash; pick a conversation to go back into it |
| Chats list | BACK | Leave the app |
| Submenu | UP / DOWN | Move through the list |
| Submenu | BACK | Back one level |
| Reminder alert | SELECT / UP | Dismiss / snooze 9 minutes |

Long-pressing SELECT inside a conversation opens **Options**:

- **Ask again** re-records the question for the turn you are looking at. Turns
  before it stay as context; that turn and anything after it are discarded,
  because the later ones were answers to the question being replaced. Nothing
  is thrown away until a replacement actually arrives, so backing out of the
  microphone leaves the conversation exactly as it was.
- **Delete chat** removes the whole conversation. There is no confirmation step.

The chats list is `[Settings]`, `[New Chat]`, then your history, with **New
Chat** preselected — so backing out of the microphone leaves a retry one press
away.

Lists do not wrap and do not exit at their ends; they stop. The way back into a
conversation is to select it. A selected row too long for the screen slides
sideways so you can read all of it, as does a conversation's title.

The title bar carries the clock everywhere except inside a conversation, where
the turn counter has the space instead.

Pebble's dictation is a system screen that owns all four buttons while it is
recording, so BACK is the only gesture available inside it. That is why
cancelling is wired to the chats list rather than to quitting.

### On the phone

Settings live in the Pebble app under Assistant. Everything is optional except
the first section.

**Connection** — run `codex login` on a computer with the Codex CLI, then paste
the contents of `~/.codex/auth.json`. The app refreshes the access token itself
from then on, using the same OAuth client the CLI uses.

**Models** — the list is whatever your ChatGPT account actually offers, fetched
from OpenAI's own model catalog (see below). Switch models off to keep them off
the watch, and pick which one new chats start with. Nothing is hardcoded, so a
newly released model shows up on its own.

**Calendar / Reminders** — two independent CalDAV accounts, because calendars
and to-dos often do not live on the same server. iCloud is
`https://caldav.icloud.com` with your Apple ID and an app-specific password
from [account.apple.com](https://account.apple.com). If discovery is awkward on
your server, paste the collection URL directly and it is used as-is.

**Places** — name the places you want to be reminded at. "Remind me to water the
plants when I get home" then writes a reminder carrying Apple's location
trigger, and your phone geofences it: it fires on arrival with the watch off.

**Notes** — Apple Notes kept on a Gmail account lives in a Gmail label, normally
`Notes`, with each note stored as a message carrying
`X-Uniform-Type-Identifier: com.apple.mail-note`. Connect Gmail through the
[helper page](docs/google-auth.html) and the assistant writes real notes there
that show up in Apple Notes on every device.

**Diagnostics** — the settings page is a web page and cannot reach your CalDAV
or Gmail servers itself. So the checks run on the phone right after each save,
and the results appear the next time you open settings. Save once, reopen, and
you get the failing step by name instead of silence.

---

## What the model can do

### Where the model list comes from

The app asks the backend, rather than shipping a list that would go stale:

```
GET https://chatgpt.com/backend-api/codex/models?client_version=<version>
```

authenticated exactly like a chat request. Each model comes back with a display
name and description, a `visibility` of list/hide/none, a `priority` for
ordering, a `minimal_client_version`, and its own
`supported_reasoning_levels` — which genuinely differ between models, so the
**Thinking** menu on the watch follows whichever model is selected rather than
offering a fixed ladder. The catalog is refreshed on launch and after every
save, and cached so a flaky connection never empties the menus.

| Tool | Needs |
|---|---|
| Web search | on by default, toggleable from the watch |
| Current time, calculator | nothing |
| Location | location permission |
| Read and create calendar events | calendar CalDAV |
| Read, add and complete reminders, including location-triggered ones | reminders CalDAV |
| Timers and alarms on the watch, mirrored into the Pebble timeline | nothing |
| Save and search Apple Notes | Gmail connection |

Tools whose service is not configured are not advertised to the model at all.
That is deliberate: a model told it has a calendar will cheerfully claim to have
checked one.

Timers are delivered twice on purpose — as a watch Wakeup, which fires with no
phone nearby, and as a Timeline pin you can find by scrolling forward from the
watchface.

---

## Building

Needs the Core Devices `pebble-tool` (5.x, Python 3.10+); it fetches the ARM
toolchain for you.

```sh
uv venv /tmp/pblenv --python 3.11
uv pip install --python /tmp/pblenv/bin/python pebble-tool
/tmp/pblenv/bin/pebble sdk install 4.33.1
export PATH="/tmp/pblenv/bin:$PATH"

pebble build
pebble install --phone 192.168.x.x
```

It also runs in the emulator, which executes the real firmware and the real
PebbleKit JS:

```sh
pebble install --emulator emery
pebble screenshot --emulator emery --no-open shot.png
pebble emu-button --emulator emery click down
```

The emulator has no voice service, so dictation aborts after a few seconds and
the app falls back to the chats list — that is expected, not a bug.

The JS side is authored as ordered parts under `src/pkjs/parts/` and
concatenated by `wscript` into a single flat `pebble-js-app.js`. Keep it that
way: the Core Devices iOS app can hang on "Loading watchapp" when the JS entry
point is a multi-file or `index.js` bundle.

### Hosting the settings page

The app opens `https://<user>.github.io/Pebble-Assistant/`. Turn on GitHub Pages
for this repository with the source set to the **`docs/` folder on `main`**, and
change `CONFIG_URL` in `src/pkjs/parts/05_settings.js` if your URL differs.
Editing `docs/index.html` locally changes nothing until it is deployed.

---

## Known limitations

- **Dictation cannot be interrupted with UP or DOWN.** The recording screen
  belongs to the firmware and only reports BACK.
- **CalDAV needs `PROPFIND`, `REPORT` and `PUT` through PebbleKit JS.** Those are
  ordinary HTTP verbs, but the companion app is the one making the call. If it
  ever refuses them the Diagnostics panel says so by name rather than degrading
  silently — there is no honest fallback for a two-way protocol.
- **Times carrying a `TZID` are read as local time.** Shipping a timezone
  database to a watch companion is not worth it.
- **Location-triggered reminders depend on the Reminders app** on your phone
  honouring Apple's alarm fields on a non-iCloud CalDAV account. The reminder
  itself always syncs; whether the geofence fires is your phone's call.
- **Reasoning context is per session.** Within a running conversation the full
  chain of thought is carried across tool calls; reopening an old chat rebuilds a
  plain transcript instead.
- **Replies are trimmed** to fit the watch (1500 characters by default,
  adjustable). Markdown, links and tables are stripped before display.
- **No text-to-speech yet.** The Pebble Time 2 has a speaker, but the ChatGPT
  auth token does not cover audio endpoints, so reading answers aloud would need
  a second credential.

---

## Credit

The Pebble-specific groundwork — the single-file iOS PKJS bundle, the 240-byte
AppMessage chunking, the local Timeline bridge — was learned from
[Wrist AI](https://github.com/deusaw/Pebble-Wrist-AI) by deusaw.
