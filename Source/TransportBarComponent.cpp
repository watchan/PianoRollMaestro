#include "TransportBarComponent.h"

void TransportBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    juce::String text;
    text << (playing ? "> PLAYING" : "|| STOPPED")
         << "    BPM " << juce::String(bpmValue, 0)
         << "    MODE: " << (mode == MidiInputMode::StepRecord ? "STEP-RECORD (C to preview)" : "PLAY-MONITOR (C to record)")
         << "    OCT: " << (octaveShift >= 0 ? "+" : "") << octaveShift;

    if (humActive)
    {
        juce::String durationName = humDurationSteps == 3  ? "1/16"
                                   : humDurationSteps == 4  ? "1/8T"
                                   : humDurationSteps == 6  ? "1/8"
                                   : humDurationSteps == 12 ? "1/4"
                                                             : juce::String(humDurationSteps) + " steps";
        text << "    HUM: ";
        if (humNote >= 0)
            text << juce::MidiMessage::getMidiNoteName(humNote, true, true, 3);
        else
            text << "-";
        text << " (" << durationName << ", Shift+F to commit)";
    }

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(16.0f));
    g.drawText(text, getLocalBounds().reduced(8, 0), juce::Justification::centredLeft);
}
