#include "StepGridComponent.h"

void StepGridComponent::setClip(const MidiClip* clipIn, int cursorStepIn)
{
    clip = clipIn;
    cursorStep = cursorStepIn;
    repaint();
}

void StepGridComponent::setPreviewNotes(const std::vector<int>& pitches)
{
    if (previewNotes == pitches)
        return;

    previewNotes = pitches;
    repaint();
}

void StepGridComponent::setPlaybackStep(int stepIndexOrMinusOne)
{
    if (playbackStep == stepIndexOrMinusOne)
        return;

    playbackStep = stepIndexOrMinusOne;
    repaint();
}

void StepGridComponent::scrollPitchView(int deltaSemitones)
{
    lowestVisiblePitch = juce::jlimit(0, 127 - visiblePitchRows + 1, lowestVisiblePitch + deltaSemitones);
    repaint();
}

void StepGridComponent::centerPitchView(int pitch)
{
    // Only scroll if the pitch is actually off-screen -- re-centering on
    // every single hum pitch change (even tiny ones still comfortably
    // within view) made the view jump around unnecessarily.
    auto highestVisiblePitch = lowestVisiblePitch + visiblePitchRows - 1;
    if (pitch >= lowestVisiblePitch && pitch <= highestVisiblePitch)
        return;

    auto newLowest = juce::jlimit(0, juce::jmax(0, 127 - visiblePitchRows + 1), pitch - visiblePitchRows / 2);
    if (newLowest == lowestVisiblePitch)
        return;

    lowestVisiblePitch = newLowest;
    repaint();
}

void StepGridComponent::setScale(const std::array<bool, 12>& inScaleByPitchClass)
{
    if (inScale == inScaleByPitchClass)
        return;

    inScale = inScaleByPitchClass;
    repaint();
}

void StepGridComponent::setLoopRegion(int startStepIn, int endStepIn, bool enabledIn)
{
    if (loopStartStep == startStepIn && loopEndStep == endStepIn && loopRegionEnabled == enabledIn)
        return;

    loopStartStep = startStepIn;
    loopEndStep = endStepIn;
    loopRegionEnabled = enabledIn;
    repaint();
}

void StepGridComponent::zoomVertical(float factor)
{
    auto centrePitch = lowestVisiblePitch + visiblePitchRows / 2;
    visiblePitchRows = juce::jlimit(4, 128, (int) std::round((float) visiblePitchRows * factor));
    lowestVisiblePitch = juce::jlimit(0, juce::jmax(0, 127 - visiblePitchRows + 1), centrePitch - visiblePitchRows / 2);
    repaint();
}

void StepGridComponent::zoomHorizontal(float factor)
{
    visibleStepsCount = juce::jlimit(8, 2000, (int) std::round((float) visibleStepsCount * factor));
    repaint();
}

static juce::String noteName(int pitch)
{
    return juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
}

int StepGridComponent::getFirstVisibleStep() const
{
    // Mirrors paint()'s followStep/firstVisibleStep math exactly.
    auto followStep = playbackStep >= 0 ? playbackStep : cursorStep;
    return juce::jmax(0, followStep - visibleStepsCount / 2);
}

void StepGridComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.brighter(0.02f));

    auto numRows = visiblePitchRows;
    auto highestVisiblePitch = lowestVisiblePitch + visiblePitchRows - 1;
    auto gridWidth = (float) getWidth() - labelGutterWidth;
    auto colWidth = gridWidth / (float) visibleStepsCount;

    // Top strip reserved for measure numbers -- everything below (rows,
    // cursor/playhead lines, note blocks) is offset down by gridTop instead
    // of starting at y=0.
    auto gridTop = measureLabelHeight;
    auto gridHeight = (float) getHeight() - gridTop;
    auto rowHeight = gridHeight / (float) numRows;

    // 4/4 assumed throughout this app (no separate time-signature field --
    // see Shift+D/F's bar-jump in MainEditorComponent), so a measure is
    // always 4 quarter notes. Falls back to MidiClip's own default (12) if
    // there's no clip yet, matching stepsPerQuarterNote's default.
    auto stepsPerQuarterNote = clip != nullptr ? clip->stepsPerQuarterNote : 12;
    auto stepsPerMeasure = stepsPerQuarterNote * 4;

    // While playing, follow the playhead instead of the (possibly stationary)
    // edit cursor, so playback stays on screen even if you haven't moved the
    // edit cursor since pressing Tab.
    auto firstVisibleStep = getFirstVisibleStep();

    // Scale tint: a faint background wash across each in-scale row, purely
    // visual (doesn't restrict entry) -- lets you see at a glance which rows
    // are "in key" without reading every note name.
    for (int row = 0; row < numRows; ++row)
    {
        auto pitch = highestVisiblePitch - row;
        auto pitchClass = ((pitch % 12) + 12) % 12;
        if (!inScale[(size_t) pitchClass])
            continue;

        auto y = gridTop + (float) row * rowHeight;
        g.setColour(juce::Colours::cornflowerblue.withAlpha(0.14f));
        g.fillRect(juce::Rectangle<float>(labelGutterWidth, y, gridWidth, rowHeight));
    }

    // Pitch-name axis down the left edge.
    g.setColour(juce::Colours::black);
    g.fillRect(juce::Rectangle<float>(0.0f, gridTop, labelGutterWidth, gridHeight));
    g.setFont(juce::FontOptions(juce::jmin(11.0f, rowHeight * 0.7f)));
    for (int row = 0; row < numRows; ++row)
    {
        auto pitch = highestVisiblePitch - row;
        auto y = gridTop + (float) row * rowHeight;
        // Sharps get a dimmer label so the C/D/E/... row names stand out.
        g.setColour(juce::MidiMessage::isMidiNoteBlack(pitch) ? juce::Colours::grey : juce::Colours::lightgrey);
        g.drawText(noteName(pitch), juce::Rectangle<float>(2.0f, y, labelGutterWidth - 4.0f, rowHeight),
                    juce::Justification::centredLeft);
    }

    // Faint line at every step, brighter line every beat (one quarter note),
    // and a thicker line + number at every measure (4 beats, 4/4 assumed)
    // so both the beat and bar structure stay visible even with fine
    // (eighth-note-triplet-capable) steps.
    for (int col = 0; col <= visibleStepsCount; ++col)
    {
        auto stepIndex = firstVisibleStep + col;
        auto x = labelGutterWidth + (float) col * colWidth;
        auto isMeasureLine = stepIndex % stepsPerMeasure == 0;
        auto isBeatLine = stepIndex % stepsPerQuarterNote == 0;

        if (isMeasureLine)
        {
            g.setColour(juce::Colours::white.withAlpha(0.4f));
            g.drawLine(x, gridTop, x, (float) getHeight(), 2.0f);

            g.setColour(juce::Colours::white.withAlpha(0.7f));
            g.setFont(juce::FontOptions(11.0f));
            g.drawText(juce::String(stepIndex / stepsPerMeasure + 1),
                       juce::Rectangle<float>(x + 2.0f, 0.0f, 40.0f, gridTop),
                       juce::Justification::centredLeft);
        }
        else
        {
            g.setColour(juce::Colours::white.withAlpha(isBeatLine ? 0.2f : 0.06f));
            g.drawVerticalLine((int) x, gridTop, (float) getHeight());
        }
    }

    // Loop region: a thin bar along the very top of the grid, spanning
    // [loopStartStep, loopEndStep) -- bright orange while looping is
    // actually enabled, a dim outline when a region is set but toggled off.
    if (loopEndStep > loopStartStep)
    {
        auto startCol = loopStartStep - firstVisibleStep;
        auto endCol = loopEndStep - firstVisibleStep;
        if (endCol > 0 && startCol < visibleStepsCount)
        {
            auto visibleStartCol = juce::jmax(0, startCol);
            auto visibleEndCol = juce::jmin(visibleStepsCount, endCol);
            auto x = labelGutterWidth + (float) visibleStartCol * colWidth;
            auto width = (float) (visibleEndCol - visibleStartCol) * colWidth;

            g.setColour(loopRegionEnabled ? juce::Colours::orange : juce::Colours::orange.withAlpha(0.35f));
            g.fillRect(juce::Rectangle<float>(x, gridTop, width, 4.0f));
        }
    }

    // Cursor column highlight.
    {
        auto col = cursorStep - firstVisibleStep;
        if (col >= 0 && col < visibleStepsCount)
        {
            g.setColour(juce::Colours::dodgerblue.withAlpha(0.25f));
            g.fillRect(juce::Rectangle<float>(labelGutterWidth + (float) col * colWidth, gridTop, colWidth, gridHeight));
        }
    }

    // Preview row highlight: the WHOLE row lights up (not just the small
    // locator box at the cursor column) so it's visible at a glance which
    // pitch(es) are about to be committed without having to spot a small box
    // -- requested after the box-only version was reported hard to see. One
    // row per pending note, so a MIDI chord highlights all its rows at once.
    for (auto previewNote : previewNotes)
    {
        if (previewNote < lowestVisiblePitch || previewNote > highestVisiblePitch)
            continue;

        auto row = highestVisiblePitch - previewNote;
        auto y = gridTop + (float) row * rowHeight;
        g.setColour(juce::Colours::yellow.withAlpha(0.18f));
        g.fillRect(juce::Rectangle<float>(labelGutterWidth, y, gridWidth, rowHeight));
    }

    if (clip == nullptr)
        return;

    // Draw each note as ONE block spanning its full duration -- including any
    // tied continuation steps that follow -- rather than one same-colour
    // block per step, so a tied note's actual length is visible at a glance
    // instead of looking identical to several separate short notes in a row.
    // Only a non-tied step with notes starts a new block; tiedFromPrevious
    // steps just extend the block that started earlier and are skipped here
    // (mirrors PlaybackEngine::scheduleUpTo's own duration calculation).
    for (int stepIndex = 0; stepIndex < (int) clip->steps.size(); ++stepIndex)
    {
        auto& step = clip->steps[(size_t) stepIndex];
        if (step.notes.empty() || step.tiedFromPrevious)
            continue;

        auto totalLengthInSteps = step.lengthInSteps;
        auto lookahead = stepIndex + 1;
        while (lookahead < (int) clip->steps.size() && clip->steps[(size_t) lookahead].tiedFromPrevious)
        {
            totalLengthInSteps += clip->steps[(size_t) lookahead].lengthInSteps;
            ++lookahead;
        }

        auto blockStartCol = stepIndex - firstVisibleStep;
        auto blockEndCol = blockStartCol + totalLengthInSteps; // exclusive
        if (blockEndCol <= 0 || blockStartCol >= visibleStepsCount)
            continue; // entirely off-screen

        auto visibleStartCol = juce::jmax(0, blockStartCol);
        auto visibleEndCol = juce::jmin(visibleStepsCount, blockEndCol);
        auto x = labelGutterWidth + (float) visibleStartCol * colWidth;
        auto width = (float) (visibleEndCol - visibleStartCol) * colWidth;

        for (auto& note : step.notes)
        {
            if (note.pitch < lowestVisiblePitch || note.pitch > highestVisiblePitch)
                continue;

            auto row = highestVisiblePitch - note.pitch;
            auto y = gridTop + (float) row * rowHeight;
            auto noteRect = juce::Rectangle<float>(x + 1.0f, y + 1.0f, width - 2.0f, rowHeight - 2.0f);

            g.setColour(juce::Colours::orange.withAlpha(0.85f));
            g.fillRect(noteRect);

            if (width >= 18.0f)
            {
                g.setColour(juce::Colours::black);
                g.setFont(juce::FontOptions(juce::jmin(11.0f, rowHeight * 0.7f)));
                g.drawText(noteName(note.pitch), noteRect.reduced(2.0f, 0.0f), juce::Justification::centredLeft);
            }
        }
    }

    // Preview locator: precise cursor-column box on top of the row highlight
    // above, so both "roughly which pitch(es)" (row) and "exactly where it
    // lands" (column) are visible together. One box per pending note.
    {
        auto col = cursorStep - firstVisibleStep;
        if (col >= 0 && col < visibleStepsCount)
        {
            for (auto previewNote : previewNotes)
            {
                if (previewNote < lowestVisiblePitch || previewNote > highestVisiblePitch)
                    continue;

                auto x = labelGutterWidth + (float) col * colWidth;
                auto row = highestVisiblePitch - previewNote;
                auto y = gridTop + (float) row * rowHeight;

                g.setColour(juce::Colours::yellow.withAlpha(0.9f));
                g.drawRect(juce::Rectangle<float>(x + 1.0f, y + 1.0f, colWidth - 2.0f, rowHeight - 2.0f), 2.0f);
            }
        }
    }

    // Playhead: a bright vertical line at the currently-sounding step,
    // distinct from the (blue, filled) edit-cursor column so both are
    // visible at once (they're usually in different places during playback).
    if (playbackStep >= 0)
    {
        auto col = playbackStep - firstVisibleStep;
        if (col >= 0 && col < visibleStepsCount)
        {
            auto x = labelGutterWidth + (float) col * colWidth;
            g.setColour(juce::Colours::white);
            g.drawVerticalLine((int) x, gridTop, (float) getHeight());
        }
    }
}
