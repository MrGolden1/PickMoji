# PickMoji

A fast, native Qt 6 emoji picker for Windows — a lightweight replacement for the
built-in emoji panel, with a look inspired by modern messaging-app pickers.

## Features

- 3,944 fully-qualified Unicode Emoji 17 entries, including skin tones, ZWJ sequences and flags
- User-configurable global shortcut (`Alt+.` by default)
- Direct Unicode insertion into the previously focused application
- Optional compatibility paste mode for applications that reject direct input
- Virtualized emoji canvas: only visible rows are painted
- Collapsed skin-tone families: the main grid shows one canonical emoji; `Alt+click`
  opens its compact tone palette (including mixed-tone multi-person variants)
- English and Persian search, extensible per-language via drop-in keyword packs
  (see `keywords/README.md`)
- Multi-timescale frequently-used ranking (recency + short-term + long-term frequency);
  each emoji is counted once per panel-open, so repeat-clicking cannot inflate its rank,
  and right-clicking a Frequently Used emoji offers to remove it
- Familiar category filters, keyboard navigation and a draggable frameless window
- Resizable panel: the whole grid zooms with the window (tray → **Panel size**, or **Ctrl +/-**)
- Non-activating window: it floats without stealing focus, so clicking an emoji inserts
  straight into the app you were using — no title-bar flicker
- Escape/click-outside dismissal, system tray menu and start with Windows
  (on by default; untick it in the tray menu to opt out)
- Single-instance IPC: launching the application again reopens the existing picker

Left-click to insert an emoji while keeping the picker open for more selections.
Right-click or Shift+click copies it instead — except in **Frequently Used**, where
right-click opens a small menu to copy the emoji or remove it from the list. The picker
closes on Escape, when you click another window, or when you press the global shortcut again.

**Typing to search:** because the picker never steals focus from the app you are typing in,
click the search box first — that is when it takes keyboard focus. Arrow-key navigation and
Enter work once the search box has focus. Clicking emoji never needs focus.

Hold Alt while clicking an emoji with a blue variant marker to choose another skin tone.

Country flags use the CC BY 4.0 Twemoji graphics because Windows does not render regional
indicator sequences as color flags. See `THIRD_PARTY_NOTICES.md`. Note that when a flag is
inserted, the receiving application still uses its own font to display it — most native
Windows apps show the two-letter region code because Segoe UI Emoji has no flag glyphs.

## Build

```powershell
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DQt6_DIR=E:/Qt/6.8.2/msvc2022_64/lib/cmake/Qt6
cmake --build build-release
# Qt 6.8's deploy step requires an absolute install prefix:
cmake --install build-release --prefix "$PWD/dist"
```

Run `dist/bin/PickMoji.exe`. The installed directory includes the required Qt runtime files.

## Application icon

The executable icon is embedded from `assets/PickMoji.ico` via `app.rc`. To regenerate it
from the source design:

```powershell
pip install pillow
python tools/generate_icon.py
```

## Releasing

PickMoji is distributed as a portable app — no installer. Ship the contents of `dist/`
(the exe plus the Qt runtime it was deployed with). To publish a *single-file* release
on GitHub, build against a **static Qt** so `PickMoji.exe` is self-contained; the
open-source repository satisfies Qt's LGPL terms for static linking.

## Updating Unicode data

Download the desired `emoji-test.txt` release and run:

```powershell
python tools/generate_emoji_data.py data/emoji-test.txt data/emoji.json
```
