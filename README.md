# RasterNoteNative

Hand-built Windows sticky notes starter in `C++20 + Win32 + Direct2D + DirectWrite`.

The project is intentionally built without `RichEdit`. The text core is owned by the app:

- Plain text only
- Bold is the only inline style
- Floating launcher with recent notes
- Minimal main window with a custom rendered editor surface
- IME composition preview through IMM
- Autosave to local files

## Design Direction

The UI follows a restrained `Raster-NOTON` inspired direction:

- neutral paper-like background
- black grid lines and thin borders
- uppercase utility labels
- dense but quiet information hierarchy
- almost no ornamental chrome

## Why No RichEdit

`RichEdit` is powerful, but it is also heavier than this app needs. This starter keeps the critical path in local code:

- custom gap-buffer text storage
- custom bold-range model
- DirectWrite layout generated on demand
- Win32 input, selection, clipboard and IME handling
- note persistence in a tiny custom file format

That keeps the editing model understandable, hackable, and performance-focused.

## Core Architecture

`src/text_document.*`

- custom gap buffer for low-overhead insertion near the caret
- selection state, caret movement, delete/insert logic
- non-overlapping bold ranges with merge/split logic

`src/note_store.*`

- stores notes in `%LOCALAPPDATA%\\RasterNoteNative\\notes`
- one `.rnote` file per note
- UTF-8 text body plus serialized bold spans

`src/note_app.*`

- launcher window and main window
- Direct2D render targets
- DirectWrite text layout cache
- hit testing, selection painting, caret painting
- clipboard, IME preview, autosave timer

## Performance Notes

This starter is biased toward smoothness first:

- no heavyweight edit control
- text layout is rebuilt lazily, only when content or width changes
- only one lightweight brush per render target
- note files are tiny and saved independently
- selection and caret are rendered directly from cached layout hit-testing

For the next optimization pass, the most valuable upgrades are:

1. add an undo/redo command stack
2. move note saving to a background worker
3. switch from `ID2D1HwndRenderTarget` to a DXGI flip-model swap chain
4. add virtualization if the recent-note list grows large
5. add richer IME caret feedback inside the composition range

## Build

Recommended toolchain:

- Visual Studio 2022 Build Tools, or full Visual Studio 2022
- Windows 10/11 SDK
- CMake 3.24+

Configure:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
```

Build:

```powershell
cmake --build build --config Release
```

## Current Shortcuts

- `Ctrl+N`: new note
- `Ctrl+S`: save now
- `Ctrl+B`: toggle bold
- `Ctrl+A`: select all
- `Ctrl+C / Ctrl+X / Ctrl+V`: clipboard
- `Esc`: hide the main window or close the expanded launcher

## Storage Format

Each note is stored as a simple text file:

```text
RASTERNOTE/1
modified=1711111111
bold=0:5,18:4
---
actual text body here
```

This is easy to inspect, migrate, sync, and replace later.
