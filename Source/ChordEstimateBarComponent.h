#pragma once
#include <JuceHeader.h>
#include "ChordEstimator.h"
#include "StepGridComponent.h"

// Thin strip above the step grid showing one estimated chord symbol per bar,
// x-aligned to stepGrid's CURRENT pan/zoom (read live from it every repaint,
// via stepGrid -- see ChordEstimator.h for what the estimate itself is).
// An earlier attempt at this same alignment was blamed for mislabeled bars,
// but that turned out to be a chord-matching tie-break bug (see
// ChordEstimator::bestChordLabel's bass-note handling), not a positioning
// bug -- once that was fixed, pixel alignment with the piano roll is both
// correct and what was actually asked for, so it's back. As a consequence,
// a bar can scroll out of view once the edit cursor moves past it, same as
// its notes do in stepGrid right below -- consistent, not a bug. Visual
// only, no mouse handling.
class ChordEstimateBarComponent : public juce::Component
{
public:
    // stepGridRef must outlive this component -- MainEditorComponent owns
    // both as sibling members, so that's guaranteed.
    void attachToStepGrid(const StepGridComponent& stepGridRef) { stepGrid = &stepGridRef; }

    void setChords(std::vector<ChordEstimate> newChords)
    {
        chords = std::move(newChords);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);

        if (stepGrid == nullptr)
            return;

        auto firstVisibleStep = stepGrid->getFirstVisibleStep();
        auto visibleStepsCount = stepGrid->getVisibleStepsCount();
        auto labelGutterWidth = StepGridComponent::getLabelGutterWidth();
        auto gridWidth = (float) getWidth() - labelGutterWidth;
        auto colWidth = gridWidth / (float) visibleStepsCount;

        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));

        for (auto& chordEstimate : chords)
        {
            auto blockStartCol = chordEstimate.startStep - firstVisibleStep;
            auto blockEndCol = blockStartCol + chordEstimate.lengthInSteps;
            if (blockEndCol <= 0 || blockStartCol >= visibleStepsCount)
                continue; // off-screen

            auto visibleStartCol = juce::jmax(0, blockStartCol);
            auto visibleEndCol = juce::jmin(visibleStepsCount, blockEndCol);
            auto x = labelGutterWidth + (float) visibleStartCol * colWidth;
            auto width = (float) (visibleEndCol - visibleStartCol) * colWidth;
            auto cellBounds = juce::Rectangle<float>(x + 1.0f, 2.0f, width - 2.0f, (float) getHeight() - 4.0f);

            // A thin separator so adjacent bars with the same or no label
            // don't visually blur into one block.
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            g.drawVerticalLine((int) x, 0.0f, (float) getHeight());

            if (chordEstimate.label.isEmpty())
                continue; // silent span -- draw nothing but the separator

            // Left-aligned at the span's own start step (its "head"), not
            // centred across it -- a merged span covering several bars of
            // the same sustained chord should still show exactly where
            // that chord begins, not float in the middle of a wide block.
            g.setColour(juce::Colours::khaki);
            g.drawText(chordEstimate.label, cellBounds, juce::Justification::centredLeft);
        }
    }

private:
    const StepGridComponent* stepGrid = nullptr;
    std::vector<ChordEstimate> chords;
};
