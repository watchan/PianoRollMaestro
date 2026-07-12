#pragma once
#include <JuceHeader.h>

// Always-visible, one-line reference of the main editor's keyboard shortcuts
// -- so the key map never has to be memorized or looked up elsewhere.
// Visual-only, no mouse interaction.
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

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);

        auto bounds = getLocalBounds().reduced(6, 0);
        auto lastActionArea = bounds.removeFromRight(220);

        g.setColour(juce::Colours::grey);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(helpText, bounds, juce::Justification::centredLeft);

        if (lastAction.isNotEmpty())
        {
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            g.drawText(lastAction, lastActionArea, juce::Justification::centredRight);
        }
    }

private:
    juce::String helpText =
        "s:toggle session view  |  PianoRoll: d:prev(delete if match) f:place/next a:clear g:delete t:tie z/x:octave 3/e:hum pitch v:hum c:loop Space:advance Tab:play  |  "
        "Session: 3/e:prev/next track d/f:prev/next slot z:stop track x:launch slot g:capture to slot t:load slot  |  "
        "Option+3/E(or R):pitch  Option+Z/X:tempo  Shift+Option+3/E(or W/R):octave  Shift+Z/X:duration  Shift+D/F:jump 1 bar  Shift+3/E(or W):track switch  Shift+C:loop start  Cmd+C:loop end  Cmd+Option+3/E:scroll pitch  |  "
        "Cmd+S/Shift+S/O/N:save/save-as/open/new  Cmd+T:track  Cmd+Y:instrument  Cmd+P:plugin editor  Cmd+,:audio  "
        "Cmd+G/B:track switch  Cmd+F/D:zoom h  Cmd+3/E:zoom v(out/in)  Cmd+M:scale  Cmd+A:toggle chord track  Cmd+Z:undo  Cmd+Shift+Z:redo";

    juce::String lastAction;
};
