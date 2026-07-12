# PianoRollMaestro

A from-scratch, standalone macOS DAW built around one rule: **zero mouse during editing**. The right hand stays on a physical MIDI keyboard the whole time; every navigation and editing command lives on the left hand's side of a PC keyboard.

Built with [JUCE](https://juce.com/) (8.0.3), as a Projucer `guiapp` project.

## Demo

[🎥 Watch a demo on X](https://x.com/watchan/status/2075808532528795857) — humming a note in, committing it with `Shift+F`, tying with `T`, switching between 1/8 and 1/8-triplet input.

## Why

Step input in mainstream DAWs (Logic, Cubase, Ableton) usually forces you to reach for the mouse constantly. This project scraps that entirely: a piano-roll editor where the cursor, track switching, note editing, and playback are all keyboard-driven, so your hands never have to leave home position.

## Features

- **Step-input MIDI editing** — mouse-free cursor navigation, note entry, ties, pitch/octave nudging.
- **Per-track VST3/AU instrument hosting** — load real plugins per track, or fall back to a built-in synth.
- **Unified note input** — the physical MIDI keyboard and hum-to-MIDI (mic pitch detection via a self-implemented YIN algorithm) are both pure live monitors feeding the same pending-note slot; `f` commits whichever was heard from most recently, into the step grid. The MIDI keyboard can hold a full chord (hum stays monophonic, one pitch at a time).
- **Triplet-capable grid** — 12 steps per quarter note, so eighth-note triplets are representable alongside 16th/8th/quarter notes.
- **Note-aware navigation** — jump between notes, not just grid steps; note-off-aware duration editing; whole-note delete.
- **Piano-roll view controls** — pitch scroll, independent vertical/horizontal zoom, playback locator, live hum-pitch preview.
- **Loop/cycle playback** — a project-wide start/end region, set from the edit cursor with a keystroke, that playback wraps around when enabled.
- **Persistent shortcut help bar** — the full key map is always on screen, plus a live "what did I just press" indicator.
- **Audio/MIDI device selection** — with device state persisted across launches.
- **Chord-progression estimate** — a strip above the piano roll guesses the chord from the notes across all tracks, analyzed every half beat so a mid-bar chord change is actually caught (template matching against major/minor/6/7/maj7/m7/sus2/sus4/dim/aug, with slash/"on-chord" notation like `Dm6/F` when the bass note isn't the chord's own root). Only a genuine 3+ note chord sustained for at least a full beat gets labelled -- single notes, dyads, and fleeting passing harmonies are left blank. Consecutive same-chord segments are merged into one span with the label shown once at the step it starts, pixel-aligned to the piano roll's current pan/zoom.
- **Session View** — an Ableton-Live-style grid of independently-launchable clip slots per track, alongside the piano roll's linear editing view (toggle with `s`). Each track's slots play back independently of the others (launching a clip on one track doesn't affect what's already playing on another), switching instantly rather than waiting for a bar/loop boundary.

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
| `f` | Ableton-Live-style: places the last-heard note (from the MIDI keyboard or hum, whichever's pending) into the step grid, otherwise moves to the next note (or a duration-step, on a rest) |
| `d` | Moves to the previous note (or a duration-step, on a rest) -- unless the cursor is on a note that shares a pitch with the currently pending note(s) (last heard from hum/MIDI), in which case it removes just the matching pitch(es) from that chord (other notes in the chord stay); if that empties the chord, the whole note is cleared |
| `a` | Clear the note under the cursor (whole note, including its tied continuation steps) -- or the current step if it's a plain rest. Also discards whatever's currently pending (hum or MIDI) so a stray/misdetected pitch can be cancelled without committing or waiting for a new one to overwrite it |
| `g` | Delete — removes the whole note if the cursor is on one, cursor lands at its start |
| `t` | Tie — extend the note at/before the cursor by the current duration preset |
| `z` / `x` | Octave shift (live input preview) |
| `3` / `e` | Nudge the pending HUM-detected pitch by a semitone, up/down (correction for pitch-detector drift; doesn't affect MIDI input) |
| `Option+3` / `Option+E` (or `Option+R`) | Nudge the note at the cursor by a semitone, up/down |
| `Shift+Option+3` (or `Shift+Option+W`) / `Shift+Option+E` (or `Shift+Option+R`) | Nudge the note at the cursor by an octave, up/down |
| `Option+Z` / `Option+X` | Tempo down/up (1 BPM) |
| `v` | Toggle hum-to-MIDI listening on/off -- 'f'/'d' only actually write/delete notes while this is ON, otherwise they're pure navigation |
| `c` | Toggle loop/cycle playback on/off |
| `Shift+C` / `Cmd+C` | Drop the loop start / end marker at the edit cursor's current position |
| `Shift+Z` / `Shift+X` | Cycle the commit duration preset (16th / 8th-triplet / 8th / quarter) |
| `Shift+D` / `Shift+F` | Jump the locator back/forward by one measure (4/4 assumed) |
| `Shift+3` (or `Shift+W`) / `Shift+E` | Switch to previous/next track |
| `Space` | Advance the locator by the current duration preset -- pure navigation, doesn't touch step content |
| `Tab` | Play / stop |
| `Cmd+S` / `Cmd+Shift+S` | Save / Save As |
| `Cmd+O` | Open |
| `Cmd+N` | New project |
| `Cmd+T` | Add track |
| `Cmd+Y` | Open instrument panel |
| `Cmd+P` | Show/hide the current track's plugin editor window -- each track remembers its own window and visibility, so switching tracks auto-hides/shows accordingly; loading a new instrument shows its editor right away |
| `Cmd+,` | Open Audio/MIDI settings |
| `Cmd+G` / `Cmd+B` | Switch to previous/next track |
| `Cmd+F` / `Cmd+D` | Zoom the piano-roll horizontally in/out |
| `Cmd+3` / `Cmd+E` | Zoom the piano-roll vertically out/in |
| `Cmd+Option+3` / `Cmd+Option+E` | Scroll the piano-roll's visible pitch range up/down |
| `Cmd+M` | Cycle the piano-roll scale tint: Major → Natural Minor → off (default: C major) |
| `Cmd+A` | Toggle whether the current track is included in the chord-progression estimate (multiple tracks can be independently included/excluded, e.g. to exclude drums/percussion) -- shown as a "C" badge next to each track's name in the track list |
| `Cmd+Z` / `Cmd+Shift+Z` | Undo / redo note edits |
| `s` | Toggle Session View (see below, and start there) -- going into Piano Roll always opens the slot at the cursor (creating a fresh one if empty); everything above still works the same in both views except `3`/`e`/`d`/`f`/`g`/`t`/`z`/`x`, which Session View repurposes |

Loop playback: the region between the start/end markers (shown as an orange bar along the top of the piano roll) is project-wide, not per-track. Once looping is enabled and playback reaches the end marker, it jumps back to the start marker and keeps playing -- the wrap is quantized to the audio block size (a few ms), not sample-accurate.

### Session View

The app starts here, not in Piano Roll -- Session View is the only way into Piano Roll, and entering it (`s` or `t`) always opens a specific, linked slot (creating a fresh one if it was empty). There's no "floating" editing buffer that exists outside this grid.

Rows = tracks, columns = clip slots -- each track's clips laid out left-to-right (rotated from Ableton's own track-as-column layout), docked directly beside the always-visible track list on the left. The intent: a vertical column (one slot index, across every track) reads as a single song section (verse, chorus, ...) you can eventually arrange left-to-right into a full song, rather than a stack of scenes for live-performance launching. At least 8 slot columns are always shown as empty placeholders, growing further if you capture into or navigate past slot 8.

| Key | Action |
|---|---|
| `3` / `e` | Move to the previous/next track (row) -- same as `Cmd+G`/`Cmd+B`, which also still work in both views |
| `d` / `f` | Move the slot cursor to the previous/next slot (column) |
| `z` | Stop the current track (silences it, independent of every other track) |
| `x` | Launch the slot at the cursor on the current track (no-op if it's empty) |
| `g` | Capture the current track's piano-roll clip into the slot at the cursor (grows the track's slot list if needed), and live-links the editor to that slot (see below) |
| `t` | Load the slot at the cursor into the piano-roll editor, live-link it the same way, and switch back to Piano Roll view -- if that slot is empty, creates a fresh clip there first, so there's always something to start writing into |

A slot cell is filled grey once it has a captured clip, bright green while it's the track's currently-launched/playing slot, and outlined in blue at the cursor position. Launching is instant (no bar/loop-boundary quantization in this version) and only affects the track it's launched on -- other tracks keep playing whatever they already were. Whole-column ("song section") launching isn't implemented yet.

**Editing a specific clip:** once a slot has been captured (`g`) or loaded (`t`), the piano roll's editing buffer is live-linked to it -- every further edit made in Piano Roll view is automatically written back to that same slot, no repeated `g` needed. The link is per-track and stays in place until you load/capture a *different* slot. To start a brand-new clip, move the slot cursor to an empty column and press `t` -- it creates a fresh clip there and opens it, already linked.

## Project structure

- `Source/ProjectModel.*` — data model (`Project` → `Track` → `MidiClip` → `Step`), XML save/load.
- `Source/PlaybackEngine.*` — sample-accurate scheduler, per-track plugin/synth rendering.
- `Source/MidiInputRouter.*` — physical MIDI keyboard input, a pure live monitor feeding the same pending-note slot hum input does.
- `Source/HumInputListener.*`, `Source/PitchDetector.*` — hum-to-MIDI capture and YIN pitch detection.
- `Source/ChordEstimator.*` — template-matching chord-progression guesser, one symbol per bar.
- `Source/PluginHost.*` — VST3/AU scanning and hosting.
- `Source/MainEditorComponent.*` — top-level UI, keyboard command dispatch.
- `Source/*Component.h` — individual UI panels (step grid, chord bar, track list, transport bar, shortcut help, mic meter).
