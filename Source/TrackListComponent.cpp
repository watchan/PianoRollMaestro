#include "TrackListComponent.h"

void TrackListComponent::setTracks(const std::vector<Track>& tracksIn, int currentIndex)
{
    trackNames.clear();
    for (auto& t : tracksIn)
        trackNames.add(t.name);

    currentTrack = currentIndex;
    repaint();
}

void TrackListComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.brighter(0.05f));

    auto rowHeight = 28;
    for (int i = 0; i < trackNames.size(); ++i)
    {
        auto rowBounds = juce::Rectangle<int>(0, i * rowHeight, getWidth(), rowHeight);

        if (i == currentTrack)
        {
            g.setColour(juce::Colours::dodgerblue.withAlpha(0.4f));
            g.fillRect(rowBounds);
        }

        g.setColour(juce::Colours::white);
        g.drawText(trackNames[i], rowBounds.reduced(8, 0), juce::Justification::centredLeft);
    }
}
