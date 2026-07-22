#pragma once
#include <map>

// Shared between MainEditorComponent (actual note dispatch, see
// pollVirtualKeyboardInput()) and KeyboardOverlayComponent (the live
// cheat-sheet window, which needs to know exactly the same key->pitch
// mapping to compute its labels) -- kept in one place so the two can never
// silently drift apart.

// One of these keys, held with NO modifier, is a substitute MIDI keyboard,
// played with the right hand (this is note PERFORMANCE, not an editing
// command, so the left-hand-only rule the rest of this app follows doesn't
// apply here -- a real MIDI keyboard was never left-hand-only either).
// Always active -- there's no held-modifier gate anymore (previously
// Ctrl); Enter toggles between this map and the drum grid below instead
// (see MainEditorComponent::toggleDrumGridMode()). Value = semitones above
// 'N' (which maps to MIDI 60 -- "C3" in this app's own note-name
// convention, see StepGridComponent.cpp's noteName()/getMidiNoteName(...,
// 3), NOT the also-common "C4 = 60" convention).
//
// Each row is chromatic left-to-right; each row's own starting key is a
// perfect fourth (5 semitones) above the row below it -- an isomorphic
// "fourths" layout (as used on several grid MIDI controllers), where a
// chord or scale shape stays roughly the same shape no matter where on the
// grid you play it. B/G/T/5 (each row's original starting key) are
// deliberately NOT included here -- they're needed as plain-key editing
// shortcuts (Set Clip End, Jump Back 1 Bar, Tie, Toggle Triplet Quantize)
// now that this map has no modifier of its own to disambiguate against
// them, so every row starts one key later than it used to.
inline const std::map<char, int>& virtualKeyboardKeyMap()
{
    static const std::map<char, int> map = {
        { 'N', 0 }, { 'M', 1 }, { ',', 2 }, { '.', 3 }, { '/', 4 },
        { 'H', 5 }, { 'J', 6 }, { 'K', 7 }, { 'L', 8 }, { ';', 9 }, { '\'', 10 },
        { 'Y', 10 }, { 'U', 11 }, { 'I', 12 }, { 'O', 13 }, { 'P', 14 }, { '[', 15 }, { ']', 16 },
        { '6', 15 }, { '7', 16 }, { '8', 17 }, { '9', 18 }, { '0', 19 }, { '-', 20 }, { '=', 21 },
    };
    return map;
}

// One of these keys, held with NO modifier, is a 4x4 drum-pad grid --
// mutually exclusive with the melodic keyboard above, switched with Enter
// (see MainEditorComponent::toggleDrumGridMode()), not a second modifier
// tier anymore. Right-hand-reachable on purpose, same as the melodic
// keyboard -- the left hand stays on editing commands, so both note-
// PERFORMANCE inputs (drums and the chromatic keyboard) live on the right.
// Several physical keys are shared with the melodic map
// above (M, J, K, L, U, I, O, P, comma, period, slash, semicolon, 6-9, 0)
// -- harmless, since only one of the two maps is ever polled at a time.
//
// Straight chromatic left-to-right, then bottom-to-top, no octave overlap
// between rows (unlike the melodic map's fourths layout -- a drum rack's
// pads are conventionally just "next pad = next MIDI note," not a
// tonal/movable-shape instrument). Value = semitones above MIDI 48 -- "C2"
// in this app's own note-name convention (60 = "C3", see
// virtualKeyboardKeyMap()'s comment above), so the 16 pads span 48-63
// ("C2" to "D#3"). Each row starts one key further left than a plain
// "M J U 7" grid would, so the bottom row's first pad is 'N' -- the same
// starting key as the melodic keyboard above, for a consistent "where do I
// put my hand" reference between the two modes.
inline const std::map<char, int>& virtualDrumKeyMap()
{
    static const std::map<char, int> map = {
        { 'N', 0 }, { 'M', 1 }, { ',', 2 }, { '.', 3 },
        { 'H', 4 }, { 'J', 5 }, { 'K', 6 }, { 'L', 7 },
        { 'Y', 8 }, { 'U', 9 }, { 'I', 10 }, { 'O', 11 },
        { '6', 12 }, { '7', 13 }, { '8', 14 }, { '9', 15 },
    };
    return map;
}
