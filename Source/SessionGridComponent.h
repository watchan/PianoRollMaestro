#pragma once
#include <vector>
#include <JuceHeader.h>
#include "ProjectModel.h"

// Session View: rows = tracks, columns = clip slots, each track's clips
// laid out left-to-right. Deliberately rotated from Ableton's own
// track-as-column layout -- the intent is that a vertical column (one
// slot index, across every track) reads as a single song section (verse,
// chorus, ...) you can arrange left-to-right into a full song, rather than
// a stack of scenes for live-performance launching. Docked directly beside
// TrackListComponent (which stays visible in both views) rather than
// drawing its own track-name column -- rowHeight below MUST match
// TrackListComponent's fixed 36px rows exactly, or the two would drift out
// of alignment. Visual-only, nothing here is mouse-interactive --
// launching/stopping/capturing/loading a slot is entirely keyboard-driven
// from MainEditorComponent (see 's' to enter this view, and f/d/g/t/z/x
// while in it).
class SessionGridComponent : public juce::Component
{
public:
    void setTracks(const std::vector<Track>& tracksIn, int cursorTrackIndexIn, int cursorSlotIndexIn)
    {
        rows.clear();
        for (auto& track : tracksIn)
        {
            TrackRow row;
            row.playingSlotIndex = track.playingSlotIndex;
            for (auto& slotClip : track.sceneClips)
            {
                auto hasContent = false;
                for (auto& step : slotClip.steps)
                    if (!step.notes.empty()) { hasContent = true; break; }
                row.slotHasContent.push_back(hasContent);
            }
            rows.push_back(std::move(row));
        }

        cursorTrackIndex = cursorTrackIndexIn;
        cursorSlotIndex = cursorSlotIndexIn;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black.brighter(0.02f));

        if (rows.empty())
            return;

        // At least 8 columns shown by default (empty placeholders ready to
        // capture into), even though sceneClips itself only grows lazily
        // as slots actually get captured -- this is purely a display
        // minimum. Grows further to cover every track's actual slots AND
        // the cursor, if either extends past that.
        constexpr int minVisibleCols = 8;
        auto numCols = juce::jmax(minVisibleCols, cursorSlotIndex + 1);
        for (auto& row : rows)
            numCols = juce::jmax(numCols, (int) row.slotHasContent.size());

        constexpr float rowHeight = 36.0f; // must match TrackListComponent.cpp's own rowHeight constant
        auto colWidth = (float) getWidth() / (float) numCols;

        for (size_t r = 0; r < rows.size(); ++r)
        {
            auto y = (float) r * rowHeight;

            for (int col = 0; col < numCols; ++col)
            {
                auto cellBounds = juce::Rectangle<float>((float) col * colWidth + 1.0f, y + 1.0f,
                                                          colWidth - 2.0f, rowHeight - 2.0f);

                auto hasContent = col < (int) rows[r].slotHasContent.size() && rows[r].slotHasContent[(size_t) col];
                auto isPlaying = rows[r].playingSlotIndex == col;

                if (isPlaying)
                {
                    g.setColour(juce::Colours::limegreen);
                    g.fillRect(cellBounds);
                }
                else if (hasContent)
                {
                    g.setColour(juce::Colours::grey.withAlpha(0.6f));
                    g.fillRect(cellBounds);
                }
                else
                {
                    g.setColour(juce::Colours::grey.withAlpha(0.3f));
                    g.drawRect(cellBounds, 1.0f);
                }

                if ((int) r == cursorTrackIndex && col == cursorSlotIndex)
                {
                    g.setColour(juce::Colours::dodgerblue);
                    g.drawRect(cellBounds, 2.0f);
                }
            }
        }
    }

private:
    struct TrackRow
    {
        std::vector<bool> slotHasContent;
        int playingSlotIndex = -1;
    };

    std::vector<TrackRow> rows;
    int cursorTrackIndex = 0;
    int cursorSlotIndex = 0;
};
