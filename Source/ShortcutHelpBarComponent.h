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
        "d/f:move  a:clear  g:delete  t:tie  z/x:octave  3/e:pitch  1/2:scroll pitch  c:mode  v:hum  Space:rest  Tab:play  |  "
        "Shift+Z/X:duration  Shift+F:commit  |  "
        "Cmd+S/Shift+S/O/N:save/save-as/open/new  Cmd+T:track  Cmd+Y:instrument  Cmd+,:audio  Cmd+3/E:track switch  Cmd+Z/X:zoom";

    juce::String lastAction;
};
