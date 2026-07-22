#pragma once
#include <JuceHeader.h>

// Always-visible reference of the main editor's keyboard shortcuts -- so the
// key map never has to be memorized or looked up elsewhere. Visual-only, no
// mouse interaction. Mode-aware (setViewMode()): only the piano-roll OR
// Session View block is shown at once, alongside the shortcuts that mean
// the same thing in both -- showing every shortcut from both views at once
// overflowed a single line so badly most of it was clipped off-screen and
// simply invisible. Two
// further, mutually-exclusive-with-each-other-and-with-the-above overlay
// states narrow this down even further to just what's actually usable
// right now: setAutomationEditMode() while editing automation, and
// setNotePending() while a just-played note is waiting to be acted on
// (commit, tie, etc.) -- see each one's own declaration.
class ShortcutHelpBarComponent : public juce::Component
{
public:
    // Shows what was just pressed and what it did, e.g. "Cmd+E - Next Track"
    // -- confirms the key landed and shows what it triggered, since the
    // editor otherwise gives no other feedback for some commands.
    void setLastAction(const juce::String& description)
    {
        lastAction = description;
        repaint();
    }

    // Switches which mode-specific shortcut block is shown -- call whenever
    // MainEditorComponent::currentViewMode changes (from refreshChildViews()).
    void setViewMode(bool isSessionViewIn)
    {
        if (isSessionView == isSessionViewIn)
            return;

        isSessionView = isSessionViewIn;
        repaint();
    }

    // Switches to the automation-only shortcut block once automation edit
    // mode is on -- call whenever MainEditorComponent::automationEditModeActive
    // changes (from refreshChildViews()). Every note-editing shortcut in
    // pianoRollText/universalText is irrelevant while editing automation
    // (d/f mean something completely different, pitch-nudge doesn't apply,
    // etc.), so showing them alongside the automation block was just noise
    // crowding out the shortcuts actually usable right now -- same
    // reasoning as the existing Piano Roll/Session View split above.
    void setAutomationEditMode(bool activeIn)
    {
        if (automationEditModeActive == activeIn)
            return;

        automationEditModeActive = activeIn;
        repaint();
    }

    // Switches to the note-operations shortcut block once a note has just
    // been played and is pending -- call whenever
    // MainEditorComponent::pendingChord's emptiness changes (from
    // refreshChildViews()). Stays showing this block for as long as
    // pendingChord is non-empty, which in practice covers the whole
    // "just played/committed a note, about to act on it" window --
    // pendingChord isn't cleared by committing itself (repeated `Ctrl+V`
    // re-commits the same chord), only by playing something new or the
    // ~2s idle auto-clear (see pendingChordIdleSinceMs's declaration), so
    // this block stays up exactly as long as that note is still "fresh".
    void setNotePending(bool pendingIn)
    {
        if (notePending == pendingIn)
            return;

        notePending = pendingIn;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);

        auto bounds = getLocalBounds().reduced(6, 2);
        auto lastActionArea = bounds.removeFromRight(220);

        // Automation edit mode takes priority -- pendingChord/note-commit
        // concepts don't exist there at all.
        auto shownText = automationEditModeActive ? automationText
            : notePending ? noteOperationsText
            : (isSessionView ? sessionText : pianoRollText) + "  |  " + universalText;

        g.setColour(juce::Colours::grey);
        g.setFont(juce::FontOptions(11.0f));
        // Wraps across up to 3 lines and, only if it still doesn't fit,
        // horizontally compresses each line down to 70% width before
        // finally falling back to an ellipsis -- graceful degradation
        // instead of the old single-line drawText's silent off-screen clip.
        g.drawFittedText(shownText, bounds, juce::Justification::topLeft, 3, 0.7f);

        if (lastAction.isNotEmpty())
        {
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            g.drawText(lastAction, lastActionArea, juce::Justification::topRight);
        }
    }

private:
    bool isSessionView = false;
    bool automationEditModeActive = false;
    bool notePending = false;

    juce::String pianoRollText =
        "PianoRoll: d:prev(note) f:next(note) c:back(step) v:advance(step) Ctrl+V:commit(Manual/Auto REC) a:clear g/b:pitch up/down z/x:octave Cmd+X:delete+retreat Cmd+C/V:copy/paste Option+D/F:nudge note left/right(1 step) "
        "Shift+G/B:octave Shift+Q/R:add to selection Shift+A:jump to start Shift+D/F:extend note selection "
        "s:jump back 1 bar Shift+C/V:jump back/fwd 1 bar q/Cmd+D:duplicate range Cmd+Ctrl+B:loop Cmd+Ctrl+E:clip end w:metronome Shift+W:count-in toggle "
        "Cmd+1/2/3:quantize 1/4-1/8-1/16 Cmd+4:triplet toggle Cmd+S:unquantize Cmd+U:cycle quantize amount(25/50/75/100%) Cmd+Shift+U:toggle auto-quantize on record Space:play Shift+Space:play from locator";
    juce::String sessionText =
        "Session: d/f:prev/next slot z:stop track x:launch slot t:load slot a:delete clip b:duplicate clip Cmd+X:capture to slot";
    juce::String universalText =
        "3/e:prev/next track  Tab:toggle session view  r:cycle REC mode(Browse/Manual/Auto/Realtime)  1/2/4:note repeat 1/4-1/8-1/16(press again to turn off)  5:note repeat triplet toggle  Enter:toggle drum grid  [N M , . / H J K L ; ' Y U I O P [ ] 6 7 8 9 0 - =]:virtual keyboard(always on)  [N M , . H J K L Y U I O 6 7 8 9]:drum pads(after Enter)  Ctrl+Z/X:transpose  Ctrl+S:sustain  Ctrl+Shift+Z/X:velocity  Ctrl+T:tie  Ctrl+G:jump fwd 1 bar  "
        "Cmd+G/B:select top/down note  Option+Z/X:tempo  Shift+Option+G/B:pitch up/down(Browse)/prev-next track(Manual,Auto)  Cmd+Shift+G/B:zoom v(note rows)  Cmd+Shift+D/F:zoom h(shared)  Shift+Z/X:duration  Cmd+Shift+C:loop start  Cmd+Ctrl+C:loop end  Cmd+Ctrl+5:range start  Cmd+Ctrl+R:range end  Cmd+5:raise lowest note(octave)  Cmd+R:lower highest note(octave)  Cmd+Option+G/B:scroll pitch  |  "
        "Cmd+0/Shift+S/O/N:save/save-as/open/new  Cmd+Ctrl+T:add track  Cmd+Y:instrument  Cmd+P:plugin editor  Cmd+,:audio  Cmd+K:keyboard overlay  "
        "Cmd+Ctrl+P/N:track switch  Cmd+M:scale  Cmd+A:select all notes  Cmd+Ctrl+H:chord track  Cmd+Z:undo  Cmd+Shift+Z:redo  Cmd+Ctrl+A:enter automation edit mode  Cmd+Ctrl+W:toggle plugin-parameter automation Read/Touch(while Touch: playing writes automatically, moving a plugin's own knob records it live; stopped just previews -- the point on its lane tracks the knob, Cmd+Ctrl+I commits it -- either way auto-creates/selects a lane for that parameter the first time it's touched)";

    // Shown INSTEAD of pianoRollText/universalText while automation edit
    // mode is on (see setAutomationEditMode()'s declaration) -- every
    // note-editing shortcut above is irrelevant here (d/f mean point-jump
    // instead of note-jump, pitch-nudge doesn't apply, etc.), so this is a
    // fully separate, self-contained list rather than a filtered subset.
    juce::String automationText =
        "Automation: Cmd+Ctrl+A:exit automation edit mode  Cmd+Ctrl+L:cycle lane(sustain/pitch bend/filter cutoff/plugin parameters, one per touched param)  d/f:jump to prev/next point  c/v:move by duration(position a new point)  Option+D/F:nudge selected point(s) left/right(1 step)  g/b:value up/down(fine)  Shift+G/B:value up/down(coarse)  Cmd+Ctrl+Z/X:curve amount up/down(fine, -1..+1 continuous ease strength)  Cmd+Ctrl+Shift+Z/X:curve amount up/down(coarse)  Ctrl+V/Cmd+Ctrl+I:insert point  a/Cmd+Ctrl+D:delete point(selects nearest remaining point)  Cmd+Ctrl+S:toggle sustain point  Cmd+Ctrl+V:toggle curve type(Curve/Step -- a point's curve shapes the segment arriving at it; with no point at cursor, toggles the pending curve type for the next placement instead, shown live in the ghost preview)  (plugin-parameter lanes: a lane only ever gets CREATED by touching the plugin's own knob in Touch mode -- Cmd+Ctrl+W -- which picks which parameter, but every key above then edits it exactly like pitch bend/filter cutoff, no-op only on Sustain-only concepts)  "
        "Shift+D/F:extend point selection  Cmd+C/V:copy/paste selected points  Shift+A/Cmd+Shift+A:jump to start/end  Cmd+Shift+G/B:zoom v(automation lanes, independent of note-row zoom)  Tab:toggle session view  Cmd+Z/Cmd+Shift+Z:undo/redo  Space:play";

    // Shown INSTEAD of pianoRollText/universalText while a note is pending
    // (see setNotePending()'s declaration) -- the shortcuts most relevant
    // to a note that was just played/committed and is about to be
    // acted on further, rather than the full always-on reference.
    juce::String noteOperationsText =
        "Note: Ctrl+V:commit(Manual/Auto REC)  Ctrl+T:tie(extend previous note)  a:clear pending/selected note  Cmd+X:delete+retreat  Option+D/F:nudge left/right(1 step)  g/b:pitch up/down(fine)  Shift+G/B:octave up/down  z/x:octave shift(live input preview)  Cmd+G/B:select individual note in chord  Shift+Q/R:add to selection  Cmd+C/V:copy/paste  Cmd+1/2/3:quantize 1/4-1/8-1/16  Cmd+S:unquantize";

    juce::String lastAction;
};
