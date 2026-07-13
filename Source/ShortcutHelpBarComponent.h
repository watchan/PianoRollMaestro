#pragma once
#include <JuceHeader.h>

// Always-visible reference of the main editor's keyboard shortcuts -- so the
// key map never has to be memorized or looked up elsewhere. Visual-only, no
// mouse interaction. Mode-aware (setViewMode()): only the piano-roll OR
// Session View block is shown at once, alongside the shortcuts that mean
// the same thing in both -- showing every shortcut from both views at once
// overflowed a single line so badly most of it was clipped off-screen and
// simply invisible ("下のヘルプ、ショートカット多すぎて見えない").
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

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);

        auto bounds = getLocalBounds().reduced(6, 2);
        auto lastActionArea = bounds.removeFromRight(220);

        auto shownText = (isSessionView ? sessionText : pianoRollText) + "  |  " + universalText;

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

    juce::String pianoRollText =
        "PianoRoll: d:prev(delete if match) f:place/next a:clear g:delete t:tie z/x:octave "
        "3/e:hum pitch(HUM on)/select note in chord(HUM off) Cmd+Shift+W/E:add to selection(HUM off) "
        "v:hum c:loop b:clip end w:metronome Space:advance Tab:play";
    juce::String sessionText =
        "Session: 3/e:prev/next track d/f:prev/next slot z:stop track x:launch slot g:capture to slot t:load slot a:delete clip b:duplicate clip";
    juce::String universalText =
        "s:toggle session view  Ctrl+[B N M , . / G H J K L ; ' T Y U I O P [ ] 5 6 7 8 9 0 - =]:virtual keyboard  Ctrl+Z/X:transpose  Ctrl+F:sustain  Ctrl+Shift+Z/X:velocity  Ctrl+Shift+[M < > ? J K L : U I O P & * ( )]:drum pads  "
        "Option+T/G:pitch  Option+Z/X:tempo  Shift+Option+3/E(or W/R):octave  Shift+Z/X:duration  Shift+D/F:jump 1 bar  Shift+C:loop start  Cmd+C:loop end  Cmd+Option+3/E:scroll pitch  |  "
        "Cmd+S/Shift+S/O/N:save/save-as/open/new  Cmd+T:track  Cmd+Y:instrument  Cmd+P:plugin editor  Cmd+,:audio  Cmd+K:keyboard overlay  "
        "Cmd+G/B:track switch  Cmd+F/D:zoom h  Cmd+3/E:zoom v  Cmd+M:scale  Cmd+A:chord track  Cmd+Z:undo  Cmd+Shift+Z:redo";

    juce::String lastAction;
};
