#include "TransportBarComponent.h"

void TransportBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    auto bounds = getLocalBounds().reduced(8, 0);

    // HUM ON/OFF badge -- a filled block, not just text, so input-mode state
    // is visible at a glance (this gates whether 'f'/'d' actually write or
    // delete notes vs. just navigate).
    auto humBadge = bounds.removeFromLeft(78).reduced(0, 7);
    if (humInputActive)
    {
        g.setColour(juce::Colours::limegreen);
        g.fillRoundedRectangle(humBadge.toFloat(), 4.0f);
        g.setColour(juce::Colours::black);
    }
    else
    {
        g.setColour(juce::Colours::grey.withAlpha(0.5f));
        g.drawRoundedRectangle(humBadge.toFloat(), 4.0f, 1.0f);
        g.setColour(juce::Colours::grey);
    }
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(juce::String("HUM ") + (humInputActive ? "ON" : "OFF"), humBadge, juce::Justification::centred);
    bounds.removeFromLeft(12);

    // LOOP ON/OFF badge -- same treatment as HUM, orange to match the loop
    // region drawn in the step grid.
    auto loopBadge = bounds.removeFromLeft(84).reduced(0, 7);
    if (loopEnabled)
    {
        g.setColour(juce::Colours::orange);
        g.fillRoundedRectangle(loopBadge.toFloat(), 4.0f);
        g.setColour(juce::Colours::black);
    }
    else
    {
        g.setColour(juce::Colours::grey.withAlpha(0.5f));
        g.drawRoundedRectangle(loopBadge.toFloat(), 4.0f, 1.0f);
        g.setColour(juce::Colours::grey);
    }
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(juce::String("LOOP ") + (loopEnabled ? "ON" : "OFF"), loopBadge, juce::Justification::centred);
    bounds.removeFromLeft(12);

    juce::String text;
    text << (playing ? "> PLAYING" : "|| STOPPED")
         << "    BPM " << juce::String(bpmValue, 0)
         << "    OCT: " << (octaveShift >= 0 ? "+" : "") << octaveShift;

    if (!pendingNotePitches.empty())
    {
        juce::String durationName = pendingNoteDurationSteps == 3  ? "1/16"
                                   : pendingNoteDurationSteps == 4  ? "1/8T"
                                   : pendingNoteDurationSteps == 6  ? "1/8"
                                   : pendingNoteDurationSteps == 12 ? "1/4"
                                                                     : juce::String(pendingNoteDurationSteps) + " steps";
        juce::StringArray names;
        for (auto pitch : pendingNotePitches)
            names.add(juce::MidiMessage::getMidiNoteName(pitch, true, true, 3));

        text << "    NOTE: " << names.joinIntoString("+")
             << " (" << durationName << ", f to commit)";
    }

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(16.0f));
    g.drawText(text, bounds, juce::Justification::centredLeft);
}
