#include "StepGridComponent.h"

void StepGridComponent::setClip(const MidiClip* clipIn, int cursorStepIn)
{
    clip = clipIn;
    cursorStep = cursorStepIn;
    repaint();
}

void StepGridComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.brighter(0.02f));

    auto numRows = highestPitch - lowestPitch + 1;
    auto colWidth = (float) getWidth() / (float) visibleSteps;
    auto rowHeight = (float) getHeight() / (float) numRows;

    // Keep the cursor roughly centred in the visible window.
    auto firstVisibleStep = juce::jmax(0, cursorStep - visibleSteps / 2);

    // Grid lines every 4 steps (one beat, at the default 16th-note resolution).
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    for (int col = 0; col <= visibleSteps; ++col)
    {
        auto stepIndex = firstVisibleStep + col;
        auto x = (float) col * colWidth;
        g.drawVerticalLine((int) x, 0.0f, (float) getHeight());
        juce::ignoreUnused(stepIndex);
    }

    // Cursor column highlight.
    {
        auto col = cursorStep - firstVisibleStep;
        if (col >= 0 && col < visibleSteps)
        {
            g.setColour(juce::Colours::dodgerblue.withAlpha(0.25f));
            g.fillRect(juce::Rectangle<float>((float) col * colWidth, 0.0f, colWidth, (float) getHeight()));
        }
    }

    if (clip == nullptr)
        return;

    for (int col = 0; col < visibleSteps; ++col)
    {
        auto stepIndex = firstVisibleStep + col;
        if (stepIndex < 0 || stepIndex >= (int) clip->steps.size())
            continue;

        auto& step = clip->steps[(size_t) stepIndex];
        if (step.notes.empty())
            continue;

        auto x = (float) col * colWidth;
        // Tied continuations draw with no left inset so they visually merge
        // into the block that started the note.
        auto inset = step.tiedFromPrevious ? 0.0f : 1.0f;

        for (auto& note : step.notes)
        {
            if (note.pitch < lowestPitch || note.pitch > highestPitch)
                continue;

            auto row = highestPitch - note.pitch;
            auto y = (float) row * rowHeight;

            g.setColour(juce::Colours::orange.withAlpha(0.85f));
            g.fillRect(juce::Rectangle<float>(x + inset, y + 1.0f, colWidth - 1.0f, rowHeight - 2.0f));
        }
    }
}
