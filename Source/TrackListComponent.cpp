#include "TrackListComponent.h"

void TrackListComponent::setTracks(const std::vector<Track>& tracksIn, int currentIndex, const juce::StringArray& instrumentNamesIn)
{
    trackNames.clear();
    chordIncluded.clear();
    for (auto& t : tracksIn)
    {
        trackNames.add(t.name);
        chordIncluded.push_back(t.includeInChordEstimate);
    }

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

        // "C" badge: whether this track feeds ChordEstimator (Cmd+A toggles
        // it) -- bright/filled when included (the default), dim/outlined
        // when excluded, same filled-vs-outlined convention TransportBar's
        // HUM/LOOP badges use.
        auto badgeBounds = nameBounds.removeFromRight(20).reduced(2);
        auto included = i < (int) chordIncluded.size() ? chordIncluded[(size_t) i] : true;
        if (included)
        {
            g.setColour(juce::Colours::mediumseagreen);
            g.fillRoundedRectangle(badgeBounds.toFloat(), 3.0f);
            g.setColour(juce::Colours::black);
        }
        else
        {
            g.setColour(juce::Colours::grey.withAlpha(0.5f));
            g.drawRoundedRectangle(badgeBounds.toFloat().reduced(0.5f), 3.0f, 1.0f);
        }
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText("C", badgeBounds, juce::Justification::centred);

        g.setColour(juce::Colours::white);
        g.drawText(trackNames[i], nameBounds.reduced(8, 0), juce::Justification::centredLeft);

        auto instrumentName = i < instrumentNames.size() ? instrumentNames[i] : juce::String();
        g.setColour(juce::Colours::lightgrey);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(instrumentName.isEmpty() ? "(built-in synth)" : instrumentName,
                   rowBounds.reduced(8, 0), juce::Justification::centredLeft);
    }
}
