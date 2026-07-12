#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"

// Visual-only track list; nothing here is mouse-interactive.
class TrackListComponent : public juce::Component
{
public:
    void setTracks(const std::vector<Track>& tracksIn, int currentIndex, const juce::StringArray& instrumentNamesIn);

    void paint(juce::Graphics& g) override;

private:
    juce::StringArray trackNames;
    juce::StringArray instrumentNames;
    std::vector<bool> chordIncluded; // Track::includeInChordEstimate, cached per row for paint()
    int currentTrack = 0;
};
