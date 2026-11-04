# Bug reports (Ctrl+B)

A player who sees something wrong presses **Ctrl+B** (`config.ini [KeyMap] BugReport`; `None`
turns it off). The framework takes the moment, opens a short form over the frozen game and
writes everything a developer needs to reproduce it into one folder.

## What happens

1. **The moment** (the game keeps running for a frame or two):
   - the next presented picture, as the player sees it (effects, window size): `photo.png`;
   - the PSX picture without any post-processing (the native-wide surface when one is
     presented, else the display area): `photo_ps1.png`;
   - a save state in a slot of its own (`SAVESTATE_BUG_SLOT`, never listed in the menu; no
     "Saved slot" notice), moved into the folder: `state.pst`;
   - the presented-image ring (debug-tools builds) frozen, so its last three seconds become
     the clip.
2. **The form**, a title page over the frozen game (it opens once the picture and the state
   landed, or after three seconds): type, severity (Critical / Annoying / Minor), frequency
   (Always / Sometimes / Once) and a description typed on the keyboard (SDL text input: any
   language; Ctrl+V pastes).
   - **Dear ImGui** (builds with recomp-ui, OpenGL window; `bug_report_ui.cpp`): a panel
     drawn over the presented frame in the launcher's font and PlayStation colours, the
     three choices as drop-down lists, the moment's picture, Unicode text shown as typed
     (accents), mouse. Keys: Tab / arrows move, Enter / Space open a list or press a button,
     Enter in the description is a new line, **Ctrl+Enter saves**, **Esc discards** (twice
     when something was written; with a list open it closes the list). Controller: D-pad,
     Cross = open / pick / press, Circle = close a list or discard, Start = save. Typing
     anywhere puts the caret in the description.
   - **Canvas** (other builds: Vulkan, no recomp-ui): the form drawn on the page canvas
     (the title's look or the framework's 8x8 panel; accents folded). Keys: arrows / Tab
     move, left / right change a value or move the caret, Enter goes to the next row
     (Shift+Enter: a new line), Ctrl+Enter saves, Esc discards. Controller: D-pad, Cross =
     next / press a button, Circle = discard, Start = save.
3. **Save** writes the folder; **discard** deletes it and the state.

## The folder

`<reports>/<YYYY-MM-DD_HHMMSS>_<type>/`, where `<reports>` is `PSX_BUG_REPORT_DIR`, else
`bug_reports/` beside the exe (a junction there can send it to another disk).

| File | |
|---|---|
| `report.json` | format 1: `id`, `created`, `status` ("new"), `auto`, `type`, `severity`, `frequency`, `description`, `files`, `clip` (frames, VBlanks), `runtime` (exe, its date, frame, presentation pacing of the last 5 s), `game` (the title's members), `history` |
| `photo.png`, `photo_ps1.png` | compressed PNGs (zlib) |
| `photo_marked.png` | `photo.png` with the player's rectangles, when there are any |
| `state.pst` | the save state |
| `clip.gif` | every presented frame of the last 180 VBlanks (at most 360), 120 lines tall, fixed 6x7x6 palette with ordered dither, delays from the VBlanks between frames (at least 2/100 s) |
| `clip_sheet.png` | twelve of those frames in a 4 x 3 grid (for reading without a GIF player) |
| `config/` | settings.toml, config.ini, input.ini, keybinds.ini, game.toml and the title's files |
| `<id>.zip`, `issue_url.txt` | what a release build sends (below) |

`status` and `history` belong to whoever triages the reports: the framework writes "new".

A **freeze** (frames stopped: wedge kind 1) or a **fatal halt** (kind 4) leaves a report too:
`<date>_freeze/report.json` with `"auto": true` and the freeze dump's path in
`files.freeze_dump`. The other wedge kinds are not reported (they also fire while the
picture keeps going).

## Marking the picture

**Mark on picture** in the ImGui form hides the form and the veil, so the frozen game shows
bright. The player draws red rectangles on it, any number up to 16, or none:

- **Mouse:** drag; the right button removes the last one.
- **Keyboard:** arrows move a cross (Shift is faster), Enter or Space sets each of two corners,
  Backspace undoes, Esc goes back to the form.
- **Controller:** D-pad, Cross sets each corner, Square undoes, Circle or Start go back.

`report.json` has them in `marks`, as `x`, `y`, `w`, `h` in 0..1 of the picture. `photo.png` stays
clean, so whoever reads the report still sees what is under the red.

## Sending a report (release builds)

A release build of a title that calls `psx_mod_set_bug_report_send` adds **Save and send**. GitHub
has no public API for attachments, so the player finishes it in the browser: the game writes
`<id>.zip`, opens the title's new-issue page already filled in, and opens the folder with the file
selected. The player drags the file into the page and submits. No account or token goes through
the game.

Builds with the debug tools keep their reports at home (`PSX_BUG_REPORT_SEND=1` tests the path,
`PSX_BUG_REPORT_NO_OPEN=1` stops before the browser). Those builds also keep the presented-image
ring: a release has no clip and no thumbnail.

| In the ZIP | |
|---|---|
| `report.json` | the report, with the exe's name only: no path of the player's machine |
| `photo.jpg` | the picture with its marks, at most 1920 wide, JPEG quality 88 |
| `photo_ps1.png` | as saved |
| `clip_sheet.png` | when there is a clip |
| `private.bin` | the save state and `config/`, sealed: only the title's developer opens it |

The save state holds the game's RAM, VRAM and sound memory, and settings can hold a player's
paths, so neither goes out readable. `private.bin` is a sealed box (X25519 to the title's public
key, BLAKE2b, XChaCha20-Poly1305, `runtime/src/bug_report_pack.c`, Monocypher). A title with no key
sends neither. A ZIP is about 3.6 MB; GitHub takes 25 MB.

`tools/bug_key/bug_key.c` makes the key pair and opens a box. The private key opens every report
of the title: keep it out of every repository and keep a copy. `tools/bug_issue_import.py` brings
reports from a title's issues into its reports folder. An issue is written by a stranger: that tool
treats it as data, reads an attachment member by member against a fixed list, and never runs
anything from it. Save states from strangers belong in a disposable test folder.

## For a title (mod_plugins.h)

- `psx_mod_bug_report_add_type(name)`: its own bug types, listed before the framework's
  (Graphics, Sound, Crash / freeze, Slow / stutter, Controls, Menu / text, Other).
- `psx_mod_bug_report_add_file(path)`: settings files copied into `config/`. Never a file
  holding a password, token or key.
- `psx_mod_set_bug_report_info(fn)`: `fn` writes `"name": value` members of the report's
  `game` object (where the player was, the title's modes).
- `psx_mod_set_bug_report_send(new_issue_url, public_key)`: where a release build sends a report,
  above. Each title has its own page and key.
- The form follows the system's language: Portuguese words are in the framework, any other
  language shows the English (`PSX_HOST_LANGUAGE=pt|en` tests one). `report.json` and the issue
  keep the English names.
- `psx_mod_bug_report_translate(english, shown)`: a title's own words, ahead of those. Only for
  what the framework does not know, such as its types, and only when `psx_mod_host_language()`
  matches.
- `psx_mod_set_bug_report_renderer(fn)`: the canvas form in the title's look, drawn from a
  `PSXModBugReportView` (rows, values, text and caret, the moment's thumbnail, what was
  taken); return 0 to have the framework's panel. Not called where the ImGui form is
  drawn. Input stays the framework's.
- `psx_mod_bug_report_open()` (a title's own shortcut), `psx_mod_bug_report_active()`.

All of it is cleared at each session's activation.

## Debug server

`{"cmd":"bug_report","op":"status"|"open"|"save"|"discard"}`,
`{"op":"text","chars":"..."}`, `{"op":"key","sdlk":K,"mods":M}`,
`{"op":"set","row":R,"value":V}` (value -1 selects the row). The answer carries the stage
(0 idle, 1 taking the moment, 2 form), the folder and what was taken. With the ImGui form,
`text` and `key` go through ImGui's own input at the next presented frame (a status read
right after may not show them yet).

## Not covered

- Release builds (no debug tools) have no presented-image ring: no clip, and the PSX
  picture is the 4:3 display area.
- Vulkan and headless have no present readback: no `photo.png`.
- Netplay: refused.
