#include "TrackListComponent.h"

void TrackListComponent::setTracks(const std::vector<Track>& tracksIn, int currentIndex, const juce::StringArray& instrumentNamesIn)
{
    trackNames.clear();
    for (auto& t : tracksIn)
        trackNames.add(t.name);

    instrumentNames = instrumentNamesIn;
    currentTrack = currentIndex;
    repaint();
}

void TrackListComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.brighter(0.05f));

    auto rowHeight = 36;
    for (int i = 0; i < trackNames.size(); ++i)
    {
        auto rowBounds = juce::Rectangle<int>(0, i * rowHeight, getWidth(), rowHeight);

        if (i == currentTrack)
        {
            g.setColour(juce::Colours::dodgerblue.withAlpha(0.4f));
            g.fillRect(rowBounds);
        }

        auto nameBounds = rowBounds.removeFromTop(rowHeight / 2);
        g.setColour(juce::Colours::white);
        g.drawText(trackNames[i], nameBounds.reduced(8, 0), juce::Justification::centredLeft);

        auto instrumentName = i < instrumentNames.size() ? instrumentNames[i] : juce::String();
        g.setColour(juce::Colours::lightgrey);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(instrumentName.isEmpty() ? "(built-in synth)" : instrumentName,
                   rowBounds.reduced(8, 0), juce::Justification::centredLeft);
    }
}
