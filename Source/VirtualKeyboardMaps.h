#pragma once
#include <map>

// Shared between MainEditorComponent (actual note dispatch, see
// pollVirtualKeyboardInput()) and KeyboardOverlayComponent (the live
// cheat-sheet window, which needs to know exactly the same key->pitch
// mapping to compute its labels) -- kept in one place so the two can never
// silently drift apart.

// Ctrl + one of these keys is a substitute MIDI keyboard, played with
// the right hand (this is note PERFORMANCE, not an editing command, so
// the left-hand-only rule the rest of this app follows doesn't apply
// here -- a real MIDI keyboard was never left-hand-only either). Value
// = semitones above 'B' (which maps to MIDI 60 -- "C3" in this app's
// own note-name convention, see StepGridComponent.cpp's noteName()/
// getMidiNoteName(..., 3), NOT the also-common "C4 = 60" convention).
// Each row is chromatic left-to-right; each row's own starting key (B,
// G, T, 5) is a perfect fourth (5 semitones) above the row below it, so
// adjacent rows deliberately overlap by one note -- an isomorphic
// "fourths" layout (as used on several grid MIDI controllers), where a
// chord or scale shape stays the same shape no matter where on the grid
// you play it. Requested layout: "クロマチックで...BとGの間は四度にする".
inline const std::map<char, int>& virtualKeyboardKeyMap()
{
    static const std::map<char, int> map = {
        { 'B', 0 }, { 'N', 1 }, { 'M', 2 }, { ',', 3 }, { '.', 4 }, { '/', 5 },
        { 'G', 5 }, { 'H', 6 }, { 'J', 7 }, { 'K', 8 }, { 'L', 9 }, { ';', 10 }, { '\'', 11 },
        { 'T', 10 }, { 'Y', 11 }, { 'U', 12 }, { 'I', 13 }, { 'O', 14 }, { 'P', 15 }, { '[', 16 }, { ']', 17 },
        { '5', 15 }, { '6', 16 }, { '7', 17 }, { '8', 18 }, { '9', 19 }, { '0', 20 }, { '-', 21 }, { '=', 22 },
    };
    return map;
}

// Ctrl+Shift + one of these keys is a 4x4 drum-pad grid, separate from
// (and takes priority over -- see MainEditorComponent::pollVirtualKeyboardInput())
// the plain Ctrl virtual keyboard above. Right-hand-reachable on purpose,
// same as the melodic keyboard -- the left hand stays on editing commands,
// so both note-PERFORMANCE inputs (drums and the chromatic keyboard) live
// on the right ("左手に寄せるのはドラムパッドやクロマチック鍵盤以外
// ...ドラムやけんばんは右手にしたい"). Tried Cmd and Ctrl+Cmd as the
// second modifier instead of Shift, but Cmd+M is unconditionally
// reserved by macOS for "Minimize Window" -- confirmed via debug
// logging that the M keydown event never even reaches the app while
// Cmd is held, regardless of Ctrl also being down, so nothing built on
// Cmd could ever ship for a grid that starts at M. Back to Ctrl+Shift.
//
// NOTE keys are the SHIFTED symbols, not the unshifted ones: macOS's
// charactersIgnoringModifiers (what JUCE's isKeyCurrentlyDown() keys off
// of) does NOT ignore Shift (only Ctrl/Option/Cmd), so with Shift held
// the physical ',' key reports as '<', '7' reports as '&', etc. -- using
// the unshifted characters here meant Ctrl+Shift+, could never match.
// Straight chromatic left-to-right, then bottom-to-top, no octave
// overlap between rows (unlike the melodic map's fourths layout -- a
// drum rack's pads are conventionally just "next pad = next MIDI note,"
// not a tonal/movable-shape instrument). Value = semitones above MIDI
// 48 -- "C2" in this app's own note-name convention (60 = "C3", see
// virtualKeyboardKeyMap()'s comment above), so the 16 pads span 48-63
// ("C2" to "D#3").
inline const std::map<char, int>& virtualDrumKeyMap()
{
    static const std::map<char, int> map = {
        { 'M', 0 }, { '<', 1 }, { '>', 2 }, { '?', 3 },
        { 'J', 4 }, { 'K', 5 }, { 'L', 6 }, { ':', 7 },
        { 'U', 8 }, { 'I', 9 }, { 'O', 10 }, { 'P', 11 },
        { '&', 12 }, { '*', 13 }, { '(', 14 }, { ')', 15 },
    };
    return map;
}
