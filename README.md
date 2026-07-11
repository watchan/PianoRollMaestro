# PianoRollMaestro

A from-scratch, standalone macOS DAW built around one rule: **zero mouse during editing**. The right hand stays on a physical MIDI keyboard the whole time; every navigation and editing command lives on the left hand's side of a PC keyboard.

Built with [JUCE](https://juce.com/) (8.0.3), as a Projucer `guiapp` project.

## Why

Step input in mainstream DAWs (Logic, Cubase, Ableton) usually forces you to reach for the mouse constantly. This project scraps that entirely: a piano-roll editor where the cursor, track switching, note editing, and playback are all keyboard-driven, so your hands never have to leave home position.

## Features

- **Step-input MIDI editing** — mouse-free cursor navigation, note entry, ties, pitch/octave nudging.
- **Per-track VST3/AU instrument hosting** — load real plugins per track, or fall back to a built-in synth.
- **Hum-to-MIDI input** — hum into a mic as an alternative to the physical MIDI keyboard; pitch detection via a self-implemented YIN algorithm.
- **Triplet-capable grid** — 12 steps per quarter note, so eighth-note triplets are representable alongside 16th/8th/quarter notes.
- **Note-aware navigation** — jump between notes, not just grid steps; note-off-aware duration editing; whole-note delete.
- **Piano-roll view controls** — pitch scroll, independent vertical/horizontal zoom, playback locator, live hum-pitch preview.
- **Persistent shortcut help bar** — the full key map is always on screen, plus a live "what did I just press" indicator.
- **Audio/MIDI device selection** — with device state persisted across launches.

## Requirements

- macOS, Xcode 16+
- [JUCE](https://github.com/juce-framework/JUCE) checked out locally (point Projucer at it)

## Building

```sh
/path/to/JUCE/Projucer.app/Contents/MacOS/Projucer --resave PianoRollMaestro.jucer
cd Builds/MacOSX
xcodebuild -project PianoRollMaestro.xcodeproj -configuration Debug
```

The app requests microphone access on first launch (needed for hum-to-MIDI input).

## Keyboard reference

All commands below use the left hand only; the right hand stays on the MIDI keyboard.

| Key | Action |
|---|---|
| `d` / `f` | Move cursor to previous/next note (or one duration-step, on a rest) |
| `a` | Clear current step |
| `g` | Delete — removes the whole note if the cursor is on one, cursor lands at its start |
| `t` | Tie — extend the note at/before the cursor by the current duration preset |
| `z` / `x` | Octave shift (live input preview) |
| `3` / `e` | Nudge the note at the cursor by a semitone, up/down |
| `Shift+3` / `Shift+E` | Nudge the note at the cursor by an octave, up/down |
| `1` / `2` | Scroll the piano-roll's visible pitch range down/up |
| `c` | Toggle StepRecord / PlayMonitor mode |
| `v` | Toggle hum-to-MIDI listening on/off |
| `Shift+Z` / `Shift+X` | Cycle the hum-commit duration preset (16th / 8th-triplet / 8th / quarter) |
| `Shift+F` | Commit the last-heard hum pitch into the step grid |
| `Space` | Insert rest, advance |
| `Tab` | Play / stop |
| `Cmd+S` / `Cmd+Shift+S` | Save / Save As |
| `Cmd+O` | Open |
| `Cmd+N` | New project |
| `Cmd+T` | Add track |
| `Cmd+Y` | Open instrument panel |
| `Cmd+,` | Open Audio/MIDI settings |
| `Cmd+3` / `Cmd+E` | Switch to previous/next track |
| `Cmd+Z` / `Cmd+X` | Zoom the piano-roll out/in (both axes) |

## Project structure

- `Source/ProjectModel.*` — data model (`Project` → `Track` → `MidiClip` → `Step`), XML save/load.
- `Source/PlaybackEngine.*` — sample-accurate scheduler, per-track plugin/synth rendering.
- `Source/MidiInputRouter.*` — physical MIDI keyboard input, StepRecord/PlayMonitor modes.
- `Source/HumInputListener.*`, `Source/PitchDetector.*` — hum-to-MIDI capture and YIN pitch detection.
- `Source/PluginHost.*` — VST3/AU scanning and hosting.
- `Source/MainEditorComponent.*` — top-level UI, keyboard command dispatch.
- `Source/*Component.h` — individual UI panels (step grid, track list, transport bar, shortcut help, mic meter).
