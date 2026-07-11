#pragma once
#include <JuceHeader.h>

// Visual-only mic input level meter (peak, 0..1). Lets the user confirm the
// mic/channel selection is actually receiving signal, independent of
// whether hum listening is toggled on.
class MicLevelMeterComponent : public juce::Component
{
public:
    void setLevel(float newLevel)
    {
        auto clamped = juce::jlimit(0.0f, 1.0f, newLevel);
        if (juce::approximatelyEqual(clamped, level))
            return;

        level = clamped;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);
        g.setColour(juce::Colours::grey);
        g.drawRect(getLocalBounds());

        auto bounds = getLocalBounds().reduced(1).toFloat();
        auto filledWidth = bounds.getWidth() * level;

        g.setColour(level > 0.9f ? juce::Colours::red : juce::Colours::limegreen);
        g.fillRect(bounds.removeFromLeft(filledWidth));
    }

private:
    float level = 0.0f;
};
