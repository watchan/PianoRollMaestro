#include "TransportBarComponent.h"

void TransportBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    juce::String text;
    text << (playing ? "> PLAYING" : "|| STOPPED")
         << "    BPM " << juce::String(bpmValue, 0)
         << "    MODE: " << (mode == MidiInputMode::StepRecord ? "STEP-RECORD (C to preview)" : "PLAY-MONITOR (C to record)")
         << "    OCT: " << (octaveShift >= 0 ? "+" : "") << octaveShift;

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(16.0f));
    g.drawText(text, getLocalBounds().reduced(8, 0), juce::Justification::centredLeft);
}
