#pragma once
#include <JuceHeader.h>
#include "ProjectModel.h"

// Visual-only track list; nothing here is mouse-interactive.
class TrackListComponent : public juce::Component
{
public:
    void setTracks(const std::vector<Track>& tracksIn, int currentIndex);

    void paint(juce::Graphics& g) override;

private:
    juce::StringArray trackNames;
    int currentTrack = 0;
};
