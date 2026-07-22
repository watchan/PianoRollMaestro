#include "StepGridComponent.h"
#include <algorithm>
#include <cmath>

void StepGridComponent::setClip(const MidiClip* clipIn, int cursorStepIn)
{
    clip = clipIn;
    cursorStep = cursorStepIn;
    followStepIfOffscreen();
    repaint();
}

void StepGridComponent::setPreviewNotes(const std::vector<int>& pitches, int durationSteps)
{
    durationSteps = juce::jmax(1, durationSteps);
    if (previewNotes == pitches && previewDurationSteps == durationSteps)
        return;

    previewNotes = pitches;
    previewDurationSteps = durationSteps;
    repaint();
}

void StepGridComponent::setPreviewAlpha(float alpha)
{
    if (juce::approximatelyEqual(previewAlpha, alpha))
        return;

    previewAlpha = alpha;
    repaint();
}

void StepGridComponent::setPlaybackStep(int stepIndexOrMinusOne)
{
    if (playbackStep == stepIndexOrMinusOne)
        return;

    playbackStep = stepIndexOrMinusOne;
    followStepIfOffscreen();
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

void StepGridComponent::setSelectedPitches(const std::vector<int>& pitches)
{
    if (selectedPitches == pitches)
        return;

    selectedPitches = pitches;
    repaint();
}

void StepGridComponent::setSelectedNoteStarts(const std::vector<int>& stepIndices)
{
    if (selectedNoteStarts == stepIndices)
        return;

    selectedNoteStarts = stepIndices;
    repaint();
}

void StepGridComponent::setRangeSelection(int startStepIn, int endStepIn)
{
    if (rangeSelectionStart == startStepIn && rangeSelectionEnd == endStepIn)
        return;

    rangeSelectionStart = startStepIn;
    rangeSelectionEnd = endStepIn;
    repaint();
}

void StepGridComponent::setLiveRecordingPreview(const std::vector<LiveRecordingPreviewNote>& notes)
{
    if (liveRecordingNotes == notes)
        return;

    liveRecordingNotes = notes;
    repaint();
}

void StepGridComponent::setAutomationEditMode(bool active, AutomationLane lane, int parameterLaneIndex)
{
    if (automationEditModeActive == active && automationEditLane == lane && activeParameterLaneIndex == parameterLaneIndex)
        return;

    automationEditModeActive = active;
    automationEditLane = lane;
    activeParameterLaneIndex = parameterLaneIndex;
    repaint();
}

void StepGridComponent::setAutomationPendingValue(int value)
{
    if (automationPendingValue == value)
        return;

    automationPendingValue = value;
    repaint();
}

void StepGridComponent::setAutomationPendingCurveType(AutomationCurveType type)
{
    if (automationPendingCurveType == type)
        return;

    automationPendingCurveType = type;
    repaint();
}

void StepGridComponent::setAutomationPendingCurveAmount(float amount)
{
    if (automationPendingCurveAmount == amount)
        return;

    automationPendingCurveAmount = amount;
    repaint();
}

void StepGridComponent::setParameterAutomationPendingValue(float value)
{
    if (parameterPendingValue == value)
        return;

    parameterPendingValue = value;
    repaint();
}

void StepGridComponent::setParameterAutomationPreviewValues(const std::map<int, float>& valuesByLaneIndex)
{
    if (parameterPreviewValuesByLaneIndex == valuesByLaneIndex)
        return;

    parameterPreviewValuesByLaneIndex = valuesByLaneIndex;
    repaint();
}

void StepGridComponent::setParameterAutomationPendingCurveType(AutomationCurveType type)
{
    if (parameterPendingCurveType == type)
        return;

    parameterPendingCurveType = type;
    repaint();
}

void StepGridComponent::setParameterAutomationPendingCurveAmount(float amount)
{
    if (parameterPendingCurveAmount == amount)
        return;

    parameterPendingCurveAmount = amount;
    repaint();
}

void StepGridComponent::setSelectedAutomationSteps(const std::vector<int>& stepIndices)
{
    if (selectedAutomationSteps == stepIndices)
        return;

    selectedAutomationSteps = stepIndices;
    repaint();
}

void StepGridComponent::zoomVerticalNoteRows(float factor)
{
    auto centrePitch = lowestVisiblePitch + visiblePitchRows / 2;
    visiblePitchRows = juce::jlimit(4, 128, (int) std::round((float) visiblePitchRows * factor));
    lowestVisiblePitch = juce::jlimit(0, juce::jmax(0, 127 - visiblePitchRows + 1), centrePitch - visiblePitchRows / 2);
    repaint();
}

void StepGridComponent::zoomVerticalAutomationLanes(float factor)
{
    // See sustainLaneHeight/automationLaneHeight's declarations -- scaled
    // by the INVERSE of factor: a caller passes factor<1 meaning "zoom
    // in" (the note-row convention, fewer rows = bigger on screen), but a
    // lane's own height is already a direct pixel size, so "zoom in"
    // needs to make IT bigger, not smaller -- applying factor directly
    // here would shrink the lanes on every "zoom in" press instead of
    // growing them.
    auto laneFactor = 1.0f / factor;
    sustainLaneHeight = juce::jlimit(6.0f, 60.0f, sustainLaneHeight * laneFactor);
    automationLaneHeight = juce::jlimit(10.0f, 100.0f, automationLaneHeight * laneFactor);
    repaint();
}

void StepGridComponent::zoomHorizontal(float factor)
{
    // Limits scaled by the same 80x MidiClip::stepsPerQuarterNote itself
    // was raised by (12 -> 960, see its declaration) so the min/max zoom
    // still cover the same musical time range as before (was 8-2000 steps
    // = a fraction of a beat up to ~41 bars at the old resolution).
    visibleStepsCount = juce::jlimit(8 * 80, 2000 * 80, (int) std::round((float) visibleStepsCount * factor));
    followStepIfOffscreen(); // window size changed -- re-check, don't just leave the follow step stranded outside it
    repaint();
}

static juce::String noteName(int pitch)
{
    return juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
}

// Landscape gradient across the FULL MIDI pitch range (0-127, fixed --
// never the current scroll window) -- soil brown at the bottom, sky blue at
// the top, with a grassy green mid-point so it reads as ground rising into
// sky rather than an arbitrary two-colour blend. Used for the pitch-height
// strip drawn down the left edge, which exists purely so scrolling the
// piano roll up/down gives an immediate, at-a-glance sense of whether
// you're up in a high register or down in a low one -- the row window
// alone doesn't convey that ("画面遷移した時にぱっと見高い方にいるのか、
// 低い方にいるのかわからない").
static juce::Colour pitchHeightColour(int pitch)
{
    auto t = juce::jlimit(0.0f, 1.0f, (float) pitch / 127.0f);

    static const juce::Colour soil (0xFF4E3524);
    static const juce::Colour grass(0xFF6B8E23);
    static const juce::Colour sky  (0xFF6FB1E8);

    return t < 0.5f ? soil.interpolatedWith(grass, t / 0.5f)
                     : grass.interpolatedWith(sky, (t - 0.5f) / 0.5f);
}

void StepGridComponent::followStepIfOffscreen()
{
    // See firstVisibleStep's declaration -- only scroll once the followed
    // step actually leaves the current window.
    auto followStep = playbackStep >= 0 ? playbackStep : cursorStep;
    if (followStep >= firstVisibleStep && followStep < firstVisibleStep + visibleStepsCount)
        return;

    firstVisibleStep = juce::jmax(0, followStep - visibleStepsCount / 2);
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
    // of starting at y=0. Bottom strips reserved for the velocity lane, the
    // sustain-pedal lane, and two more for pitch bend/filter cutoff --
    // gridHeight (and rowHeight, derived from it) covers pitch ROWS only;
    // full-height indicators (measure/beat lines, playhead, clip-end
    // boundary) still reach all the way to getHeight(), past the rows and
    // through every lane, so they stay readable against them. Pitch bend/
    // filter cutoff/plugin-parameter lanes are always shown, same as
    // velocity/sustain -- automationEditModeActive no longer gates their
    // visibility, only which one gets the "active lane" outline/ghost-
    // preview treatment below (see its use further down) -- values still
    // get written into any of them (hardware/Touch capture) regardless of
    // whether you've ever entered automation edit mode, so hiding them
    // until you explicitly did meant recorded automation could exist
    // invisibly ("オートメーションは最初からレーンを表示させる。基本的に
    // 入力はどのレーンも常に受け付ける").
    // +1 more lane per touch-recorded plugin parameter (see
    // MidiClip::parameterLanes' declaration) -- clip can be null here
    // (nothing loaded yet), checked explicitly since the null guard
    // further down in this function doesn't run until later.
    auto parameterLaneCount = clip != nullptr ? (int) clip->parameterLanes.size() : 0;
    auto activeAutomationLanesHeight = automationLaneHeight * (2.0f + (float) parameterLaneCount);
    auto gridTop = measureLabelHeight;
    auto gridHeight = (float) getHeight() - gridTop - velocityLaneHeight - sustainLaneHeight - activeAutomationLanesHeight;
    auto rowHeight = gridHeight / (float) numRows;
    auto velocityLaneTop = gridTop + gridHeight;
    auto sustainLaneTop = velocityLaneTop + velocityLaneHeight;
    auto pitchBendLaneTop = sustainLaneTop + sustainLaneHeight;
    auto filterCutoffLaneTop = pitchBendLaneTop + automationLaneHeight;

    // 4/4 assumed throughout this app (no separate time-signature field --
    // see Shift+D/F's bar-jump in MainEditorComponent), so a measure is
    // always 4 quarter notes. Falls back to MidiClip's own default (960) if
    // there's no clip yet, matching stepsPerQuarterNote's default.
    auto stepsPerQuarterNote = clip != nullptr ? clip->stepsPerQuarterNote : 960;
    auto stepsPerMeasure = stepsPerQuarterNote * 4;

    // While playing, follow the playhead instead of the (possibly stationary)
    // edit cursor, so playback stays on screen even if you haven't moved the
    // edit cursor since pressing Tab.
    auto firstVisibleStep = getFirstVisibleStep();

    // 4-measure phrase bar: a slim strip along the top of the grid, in the
    // same spirit as the pitch-height strip down the left edge
    // (pitchBarWidth) -- shifts hue every 4 measures and gradients
    // smoothly across each one, so it's visible at a glance both which
    // phrase you're looking at (hue) and where within it (the gradient)
    // ("縦に引いた色のバーと同じように、4小節の中でのグラデーションと、
    // 4小節単位で色相が変わってブロックが見えるような色合いにしたい").
    // Deliberately a thin, muted bar -- just a hint of colour up top, not a
    // full-height background wash (an earlier version tinted the whole
    // grid) and not a loud/saturated one either (an earlier version of
    // this bar itself was too vivid: "全体にやると見づらすぎる...上の方に
    // 色のバーが見えるというくらいをイメージしてた"). Sits in the existing
    // measure-label header strip, just above the grid rows.
    {
        auto stepsPerBlock = stepsPerMeasure * 4;
        auto firstBlockIndex = firstVisibleStep / stepsPerBlock;
        auto lastBlockIndex = (firstVisibleStep + visibleStepsCount) / stepsPerBlock;
        auto barY = gridTop - pitchBarWidth;

        for (int blockIndex = firstBlockIndex; blockIndex <= lastBlockIndex; ++blockIndex)
        {
            auto blockStartStep = blockIndex * stepsPerBlock;
            auto startCol = blockStartStep - firstVisibleStep;
            auto endCol = startCol + stepsPerBlock;
            auto visibleStartCol = juce::jmax(0, startCol);
            auto visibleEndCol = juce::jmin(visibleStepsCount, endCol);
            if (visibleEndCol <= visibleStartCol)
                continue;

            // Small, fixed hue step per block -- deliberately NOT the
            // golden-angle trick (max-contrast jumps like red-to-blue
            // between neighbours), which read as jarring rather than a
            // clean, easy cut ("赤、アオみたいな急展開ではなくそこも切れ目
            // はわかりやすいけど、キツすぎず目に優しいところを狙いたい").
            // Adjacent blocks land a modest step apart on the colour
            // wheel -- close enough to stay calm, far enough that the
            // boundary is still obvious. Low saturation/value throughout
            // keeps the whole thing dark and unobtrusive.
            auto hue = std::fmod((float) blockIndex * 0.09f, 1.0f);
            auto colourStart = juce::Colour::fromHSV(hue, 0.35f, 0.10f, 1.0f);
            auto colourEnd   = juce::Colour::fromHSV(hue, 0.35f, 0.22f, 1.0f);

            // Gradient spans the block's FULL width (not just whatever
            // slice is currently visible), so scrolling mid-block never
            // makes the fade appear to restart.
            auto fullBlockX = labelGutterWidth + (float) startCol * colWidth;
            auto fullBlockWidth = (float) stepsPerBlock * colWidth;
            juce::ColourGradient gradient(colourStart, fullBlockX, barY, colourEnd, fullBlockX + fullBlockWidth, barY, false);
            g.setGradientFill(gradient);

            auto x = labelGutterWidth + (float) visibleStartCol * colWidth;
            auto width = (float) (visibleEndCol - visibleStartCol) * colWidth;
            g.fillRect(juce::Rectangle<float>(x, barY, width, pitchBarWidth));
        }
    }

    // Piano-key row stripe: a faint background wash on the C-major
    // "white key" rows, FIXED regardless of the estimated key -- purely a
    // static visual reference (like a real keyboard's black/white keys),
    // no longer tied to KeyEstimator's guess or the Cmd+M Auto/Off toggle.
    // In-key/out-of-key marking is the red note-outline below instead
    // ("ノートの縞模様はCメジャー想定の、ピアノの色で固定する。Keyによ
    // らず。インキー、アウトキーは赤枠でノートが囲まれて気づけるので").
    static constexpr bool pianoWhiteKeyByPitchClass[12] =
        { true, false, true, false, true, true, false, true, false, true, false, true };
    for (int row = 0; row < numRows; ++row)
    {
        auto pitch = highestVisiblePitch - row;
        auto pitchClass = ((pitch % 12) + 12) % 12;
        if (!pianoWhiteKeyByPitchClass[pitchClass])
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

        // Pitch-height strip -- see pitchHeightColour()'s comment.
        g.setColour(pitchHeightColour(pitch));
        g.fillRect(juce::Rectangle<float>(0.0f, y, pitchBarWidth, rowHeight));

        // Sharps get a dimmer label so the C/D/E/... row names stand out.
        g.setColour(juce::MidiMessage::isMidiNoteBlack(pitch) ? juce::Colours::grey : juce::Colours::lightgrey);
        g.drawText(noteName(pitch), juce::Rectangle<float>(pitchBarWidth + 2.0f, y, labelGutterWidth - pitchBarWidth - 4.0f, rowHeight),
                    juce::Justification::centredLeft);
    }

    // Faint line at every step, brighter line every beat (one quarter note),
    // and a thicker line + number at every measure (4 beats, 4/4 assumed)
    // so both the beat and bar structure stay visible even with fine
    // (eighth-note-triplet-capable) steps. The per-step faint lines are
    // skipped once zoomed out far enough that a step is well under a pixel
    // wide (stepsPerQuarterNote raised to 960 means up to hundreds of them
    // can land on the very same screen column) -- without this, drawing
    // that many overlapping ~6%-alpha lines on top of each other on the
    // same pixel accumulates toward fully opaque, washing the whole grid
    // white instead of reading as faint guide lines
    // ("線が細かくなりすぎて白くなってしまってみづらい"). Beat/measure
    // lines are far sparser (960/48-per-quarter apart) and stay
    // comfortably distinguishable even at maximum zoom-out, so they're
    // never skipped.
    auto showStepLines = colWidth >= 3.0f;

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
            // Height stops short of gridTop -- leaves the 4-measure phrase
            // bar's strip (see above) clear underneath instead of drawing
            // the number on top of it.
            g.drawText(juce::String(stepIndex / stepsPerMeasure + 1),
                       juce::Rectangle<float>(x + 2.0f, 0.0f, 40.0f, gridTop - pitchBarWidth),
                       juce::Justification::centredLeft);
        }
        else if (isBeatLine || showStepLines)
        {
            g.setColour(juce::Colours::white.withAlpha(isBeatLine ? 0.2f : 0.06f));
            g.drawVerticalLine((int) x, gridTop, (float) getHeight());
        }
    }

    // Range selection ('r', MainEditorComponent::duplicateSelectedRange()):
    // a translucent cyan wash across the full column height of
    // [rangeSelectionStart, rangeSelectionEnd) -- deliberately a full-height
    // fill rather than the loop region's thin top bar, so the two "start/
    // end marker" features never look like the same thing.
    if (rangeSelectionEnd > rangeSelectionStart)
    {
        auto startCol = rangeSelectionStart - firstVisibleStep;
        auto endCol = rangeSelectionEnd - firstVisibleStep;
        if (endCol > 0 && startCol < visibleStepsCount)
        {
            auto visibleStartCol = juce::jmax(0, startCol);
            auto visibleEndCol = juce::jmin(visibleStepsCount, endCol);
            auto x = labelGutterWidth + (float) visibleStartCol * colWidth;
            auto width = (float) (visibleEndCol - visibleStartCol) * colWidth;

            g.setColour(juce::Colours::cyan.withAlpha(0.12f));
            g.fillRect(juce::Rectangle<float>(x, gridTop, width, (float) getHeight() - gridTop));
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

    // Cursor column highlight. At least ~2px wide even when colWidth itself
    // is sub-pixel (stepsPerQuarterNote's 80x resolution increase means a
    // single step, the cursor's own width, can render thinner than a
    // pixel at the default zoom) -- otherwise this tint is effectively
    // invisible at exactly the moments it matters most, e.g. right after
    // opening the app, with no visual sign of where the (mouse-free-only)
    // edit cursor actually is ("ロケータが見えない" -- traced to this,
    // not the playhead, which already had no such problem since a whole
    // beat/measure line is never sub-pixel).
    {
        auto col = cursorStep - firstVisibleStep;
        if (col >= 0 && col < visibleStepsCount)
        {
            g.setColour(juce::Colours::dodgerblue.withAlpha(0.25f));
            g.fillRect(juce::Rectangle<float>(labelGutterWidth + (float) col * colWidth, gridTop, juce::jmax(2.0f, colWidth), (float) getHeight() - gridTop));
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
        g.setColour(juce::Colours::yellow.withAlpha(0.18f * previewAlpha));
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
        auto blockEndCol = blockStartCol + totalLengthInSteps; // exclusive, whole-chord envelope
        if (blockEndCol <= 0 || blockStartCol >= visibleStepsCount)
            continue; // envelope (== the chord's longest note) is entirely off-screen, so every note in it is too

        for (auto& note : step.notes)
        {
            if (note.pitch < lowestVisiblePitch || note.pitch > highestVisiblePitch)
                continue;

            // This note's OWN width -- may be shorter than the chord's
            // envelope if it was released earlier than the rest (see
            // StepNote::durationSteps), so a chord's notes draw as a
            // staircase of different-length blocks instead of one uniform
            // block for every pitch.
            auto noteLengthInSteps = note.durationSteps > 0 ? note.durationSteps : totalLengthInSteps;
            auto noteEndCol = blockStartCol + noteLengthInSteps;
            auto visibleStartCol = juce::jmax(0, blockStartCol);
            auto visibleEndCol = juce::jmin(visibleStepsCount, noteEndCol);
            if (visibleEndCol <= visibleStartCol)
                continue; // this note's own (shorter) span happens to be entirely off-screen

            auto x = labelGutterWidth + (float) visibleStartCol * colWidth;
            auto width = (float) (visibleEndCol - visibleStartCol) * colWidth;

            auto row = highestVisiblePitch - note.pitch;
            auto y = gridTop + (float) row * rowHeight;
            // width can be sub-2px for a short note at a zoomed-out view
            // (especially since stepsPerQuarterNote's 80x resolution
            // increase -- see its declaration), so the -2.0f inset below
            // must be clamped at 0 rather than going negative, which
            // g.drawRect() (used for the outlines further down) asserts
            // against.
            auto noteRect = juce::Rectangle<float>(x + 1.0f, y + 1.0f, juce::jmax(0.0f, width - 2.0f), juce::jmax(0.0f, rowHeight - 2.0f));

            g.setColour(juce::Colours::orange.withAlpha(0.85f));
            g.fillRect(noteRect);

            if (width >= 18.0f)
            {
                g.setColour(juce::Colours::black);
                g.setFont(juce::FontOptions(juce::jmin(11.0f, rowHeight * 0.7f)));
                g.drawText(noteName(note.pitch), noteRect.reduced(2.0f, 0.0f), juce::Justification::centredLeft);
            }

            // Out-of-key marker: a thin red outline on any note whose pitch
            // class isn't in the current scale (setScale()) -- the existing
            // row tint alone only hints at this via background colour,
            // which isn't obvious per-note, especially zoomed out or when
            // an out-of-key note sits right next to an in-key one at
            // similar brightness ("ノートがin keyかout keyがわかるように
            // 印をつけたい"). inScale is all-true when the scale tint is
            // toggled Off (Cmd+M), so this naturally draws nothing then.
            // Drawn before the (thicker, white) selection outline below so
            // a selected out-of-key note still reads as "selected" first.
            auto pitchClass = ((note.pitch % 12) + 12) % 12;
            if (!inScale[(size_t) pitchClass])
            {
                g.setColour(juce::Colours::red.withAlpha(0.9f));
                g.drawRect(noteRect, 1.5f);
            }

            // Individual-note (pitch-within-chord) selection outline -- see
            // setSelectedPitches()) -- only the chord the cursor is
            // currently inside can have a selection, so other note blocks
            // never get this outline. Compares against the whole tied span
            // (stepIndex..stepIndex+totalLengthInSteps), not just
            // stepIndex itself -- a tied note's StepNote data only lives at
            // its root step, but the cursor can sit anywhere within the
            // tie's continuation steps too (MainEditorComponent's own
            // findOwningNoteStepIndex() resolves those the same way), and
            // comparing stepIndex == cursorStep directly meant landing on
            // the tail of a tie never showed the narrowed selection at all
            // -- it looked permanently stuck on "whole chord selected"
            // ("タイで伸ばしたノートについて、終端の方で触れた状態の時は
            // 全て選択された状態で...変わらない").
            if (cursorStep >= stepIndex && cursorStep < stepIndex + totalLengthInSteps
                && std::find(selectedPitches.begin(), selectedPitches.end(), note.pitch) != selectedPitches.end())
            {
                g.setColour(juce::Colours::white);
                g.drawRect(noteRect, 2.0f);
            }
        }

        // Time-axis multi-note selection (Shift+D/Shift+F, quantize target
        // -- see setSelectedNoteStarts()) -- a cyan outline around the
        // WHOLE note block (every visible pitch in the chord, full tied
        // duration), distinct from the white per-pitch outline above which
        // only ever marks the single cursor step. Was gold, but that's too
        // close to the note fill's own orange to read clearly
        // ("選択したNoteの枠が見づらいので少し色を変えて見やすくしたい").
        if (std::find(selectedNoteStarts.begin(), selectedNoteStarts.end(), stepIndex) != selectedNoteStarts.end())
        {
            auto topRow = -1, bottomRow = -1;
            for (auto& note : step.notes)
            {
                if (note.pitch < lowestVisiblePitch || note.pitch > highestVisiblePitch)
                    continue;
                auto row = highestVisiblePitch - note.pitch;
                if (topRow < 0 || row < topRow) topRow = row;
                if (bottomRow < 0 || row > bottomRow) bottomRow = row;
            }

            if (topRow >= 0)
            {
                // The envelope's own extent (whole chord, longest note) --
                // deliberately not any one note's individual width, since
                // this outline marks "this note-start step is selected,"
                // not any particular pitch's length.
                auto envelopeVisibleStartCol = juce::jmax(0, blockStartCol);
                auto envelopeVisibleEndCol = juce::jmin(visibleStepsCount, blockEndCol);
                auto envelopeX = labelGutterWidth + (float) envelopeVisibleStartCol * colWidth;
                auto envelopeWidth = (float) (envelopeVisibleEndCol - envelopeVisibleStartCol) * colWidth;

                auto y = gridTop + (float) topRow * rowHeight;
                auto height = (float) (bottomRow - topRow + 1) * rowHeight;
                g.setColour(juce::Colours::cyan);
                g.drawRect(juce::Rectangle<float>(envelopeX + 1.0f, y + 1.0f, juce::jmax(0.0f, envelopeWidth - 2.0f), juce::jmax(0.0f, height - 2.0f)), 2.0f);
            }
        }
    }

    // Real-time REC in-progress preview (setLiveRecordingPreview()) -- these
    // notes don't exist in clip->steps yet (only written on commit), so
    // drawn independently of the note-block loop above. Each entry grows/
    // freezes on its OWN startStep/endStep, not a single shared span for
    // the whole chord: a pitch still actually held keeps extending live to
    // the current playhead every tick, while one that's already been
    // released (but the gesture hasn't ended yet, e.g. some OTHER note is
    // still sustaining) is frozen exactly where its own note-off happened
    // instead of visually continuing to stretch alongside whatever's still
    // held. Red to match the "REC: LIVE" transport badge, and pulsing (a
    // slow sine-based alpha wobble keyed off the wall clock) so an entirely
    // static-looking held note still visibly reads as "actively recording"
    // even while its width isn't changing between two ticks.
    if (!liveRecordingNotes.empty())
    {
        auto pulse = (float) (0.55 + 0.25 * std::sin(juce::Time::getMillisecondCounterHiRes() * 0.006));

        for (auto& note : liveRecordingNotes)
        {
            if (note.endStep <= note.startStep || note.pitch < lowestVisiblePitch || note.pitch > highestVisiblePitch)
                continue;

            auto blockStartCol = note.startStep - firstVisibleStep;
            auto blockEndCol = note.endStep - firstVisibleStep;
            if (blockEndCol <= 0 || blockStartCol >= visibleStepsCount)
                continue;

            auto visibleStartCol = juce::jmax(0, blockStartCol);
            auto visibleEndCol = juce::jmin(visibleStepsCount, blockEndCol);
            auto x = labelGutterWidth + (float) visibleStartCol * colWidth;
            auto width = (float) (visibleEndCol - visibleStartCol) * colWidth;

            auto row = highestVisiblePitch - note.pitch;
            auto y = gridTop + (float) row * rowHeight;
            // width can be sub-2px for a short note at a zoomed-out view
            // (especially since stepsPerQuarterNote's 80x resolution
            // increase -- see its declaration), so the -2.0f inset below
            // must be clamped at 0 rather than going negative, which
            // g.drawRect() (used for the outlines further down) asserts
            // against.
            auto noteRect = juce::Rectangle<float>(x + 1.0f, y + 1.0f, juce::jmax(0.0f, width - 2.0f), juce::jmax(0.0f, rowHeight - 2.0f));

            g.setColour(juce::Colours::red.withAlpha(pulse));
            g.fillRect(noteRect);
            g.setColour(juce::Colours::white);
            g.drawRect(noteRect, 1.5f);
        }
    }

    // Clip-end boundary ('b', MidiClip::explicitLengthInSteps): a bright
    // marker line at the clip's actual end, with everything past it dimmed
    // -- so a rest sitting BEFORE the line reads as "still inside the
    // clip" (normal brightness, e.g. an intentional trailing rest before a
    // Session View loop wraps) rather than being indistinguishable from
    // "the clip just doesn't go any further here." Drawn after the note
    // blocks so the dim overlay actually dims any notes past the boundary
    // too. Nothing drawn at all while unset (0) -- unchanged appearance.
    if (clip->explicitLengthInSteps > 0)
    {
        auto boundaryCol = clip->explicitLengthInSteps - firstVisibleStep;
        if (boundaryCol < visibleStepsCount)
        {
            auto visibleBoundaryCol = juce::jmax(0, boundaryCol);
            auto x = labelGutterWidth + (float) visibleBoundaryCol * colWidth;

            if (x < (float) getWidth())
            {
                g.setColour(juce::Colours::black.withAlpha(0.55f));
                g.fillRect(juce::Rectangle<float>(x, gridTop, (float) getWidth() - x, (float) getHeight() - gridTop));
            }

            if (boundaryCol >= 0)
            {
                g.setColour(juce::Colours::magenta.withAlpha(0.85f));
                g.drawLine(x, gridTop, x, (float) getHeight(), 3.0f);
            }
        }
    }

    // Preview locator: precise cursor-column box on top of the row highlight
    // above, so both "roughly which pitch(es)" (row) and "exactly where it
    // lands" (column) are visible together. One box per pending note, sized
    // to previewDurationSteps (the current commit duration preset) rather
    // than a single fixed step, so the box already shows how long the note
    // will actually be written as.
    {
        auto blockStartCol = cursorStep - firstVisibleStep;
        auto blockEndCol = blockStartCol + previewDurationSteps; // exclusive
        if (blockEndCol > 0 && blockStartCol < visibleStepsCount)
        {
            auto visibleStartCol = juce::jmax(0, blockStartCol);
            auto visibleEndCol = juce::jmin(visibleStepsCount, blockEndCol);
            auto x = labelGutterWidth + (float) visibleStartCol * colWidth;
            auto width = (float) (visibleEndCol - visibleStartCol) * colWidth;

            for (auto previewNote : previewNotes)
            {
                if (previewNote < lowestVisiblePitch || previewNote > highestVisiblePitch)
                    continue;

                auto row = highestVisiblePitch - previewNote;
                auto y = gridTop + (float) row * rowHeight;

                g.setColour(juce::Colours::yellow.withAlpha(0.9f * previewAlpha));
                g.drawRect(juce::Rectangle<float>(x + 1.0f, y + 1.0f, juce::jmax(0.0f, width - 2.0f), juce::jmax(0.0f, rowHeight - 2.0f)), 2.0f);
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

    // Velocity lane: a thin strip along the bottom, one bar per note at its
    // START step (velocity is an onset property, not smeared across the
    // note's duration -- a tied continuation step is skipped here the same
    // way the note-block loop above skips it). Bar height is proportional
    // to velocity (0.0-1.0), anchored to the bottom of the lane. A chord's
    // notes split that step's column width evenly, side by side.
    {
        g.setColour(juce::Colours::white.withAlpha(0.15f));
        g.drawLine(labelGutterWidth, velocityLaneTop, (float) getWidth(), velocityLaneTop, 1.0f);

        for (int col = 0; col < visibleStepsCount; ++col)
        {
            auto stepIndex = firstVisibleStep + col;
            if (stepIndex < 0 || stepIndex >= (int) clip->steps.size())
                continue;

            auto& step = clip->steps[(size_t) stepIndex];
            if (step.notes.empty() || step.tiedFromPrevious)
                continue;

            auto x = labelGutterWidth + (float) col * colWidth;
            auto barWidth = colWidth / (float) step.notes.size();

            for (size_t i = 0; i < step.notes.size(); ++i)
            {
                auto velocity = juce::jlimit(0.0f, 1.0f, step.notes[i].velocity);
                auto barHeight = velocityLaneHeight * velocity;
                // At least ~1.5px wide even when heavily zoomed out or a
                // chord splits an already-thin column many ways -- otherwise
                // barWidth rounds down toward 0 and the bar effectively
                // disappears ("ウィンドウの幅を狭くするとピクセルが細かす
                // ぎて消えてみるみたい" / "最低1-2px確保はした方が良い
                // かも"). Bars can overlap slightly in this case, but that's
                // preferable to being invisible.
                auto visibleBarWidth = juce::jmax(1.5f, barWidth - 1.0f);
                auto barRect = juce::Rectangle<float>(x + (float) i * barWidth + 0.5f,
                                                        velocityLaneTop + velocityLaneHeight - barHeight,
                                                        visibleBarWidth, barHeight);

                g.setColour(juce::Colours::orange.withAlpha(0.85f));
                g.fillRect(barRect);
            }
        }
    }

    // Sustain pedal (CC64) automation lane -- ON/OFF over time, drawn from
    // the exact same recorded events PlaybackEngine resends during
    // playback (see MidiClip::sustainPedalEvents' declaration). A thin
    // strip beneath the velocity lane, filled pink while the pedal is
    // down, empty otherwise ("Sustainもピアノロールの下の方にON／OFFが
    // わかるように表示してほしい").
    {
        g.setColour(juce::Colours::white.withAlpha(0.15f));
        g.drawLine(labelGutterWidth, sustainLaneTop, (float) getWidth(), sustainLaneTop, 1.0f);

        if (clip != nullptr && !clip->sustainPedalEvents.empty())
        {
            auto& events = clip->sustainPedalEvents;

            // The pedal's state entering the visible window -- the last
            // recorded event at or before firstVisibleStep, if any.
            auto startIt = std::upper_bound(events.begin(), events.end(), firstVisibleStep,
                [](int step, const SustainPedalEvent& ev) { return step < ev.stepIndex; });
            auto pedalDown = startIt != events.begin() && std::prev(startIt)->pedalDown;

            auto drawSegment = [&](int fromCol, int toCol, bool isDown)
            {
                if (!isDown || toCol <= fromCol)
                    return;
                auto x = labelGutterWidth + (float) fromCol * colWidth;
                auto width = (float) (toCol - fromCol) * colWidth;
                g.setColour(juce::Colours::deeppink.withAlpha(0.75f));
                g.fillRect(juce::Rectangle<float>(x, sustainLaneTop + 2.0f, width, sustainLaneHeight - 4.0f));
            };

            auto lastVisibleStepExclusive = firstVisibleStep + visibleStepsCount;
            auto currentCol = 0;
            for (auto it = startIt; it != events.end() && it->stepIndex < lastVisibleStepExclusive; ++it)
            {
                auto eventCol = juce::jmax(0, it->stepIndex - firstVisibleStep);
                drawSegment(currentCol, eventCol, pedalDown);
                pedalDown = it->pedalDown;
                currentCol = eventCol;
            }
            drawSegment(currentCol, visibleStepsCount, pedalDown);
        }
    }

    // Pitch bend / filter cutoff automation lanes -- always shown (see
    // activeAutomationLanesHeight's declaration for why), not gated on
    // Cmd+Ctrl+A's edit mode. Each draws a polyline connecting its recorded
    // breakpoints (see MidiClip::AutomationPoint's declaration) -- flat
    // before the first point and after the last, since that's what
    // PlaybackEngine's own interpolation does (hold, don't invent values
    // outside the recorded range). The lane matching automationEditLane
    // gets a brighter accent (outline, ghost preview, curve label) so it's
    // clear which one Cmd+Ctrl+Z/X/I/D currently act on -- but only while
    // automationEditModeActive is actually on (isActiveLane's callers below
    // all AND it in), since PC-keyboard automation commands only mean
    // anything in that mode to begin with; the recorded curve/points
    // themselves stay visible either way.
    if (clip != nullptr)
    {
        auto drawAutomationLane = [&](float laneTop, const std::vector<AutomationPoint>& points,
                                       int minValue, int maxValue, juce::Colour colour, bool isActiveLane, const juce::String& label)
        {
            g.setColour(juce::Colours::white.withAlpha(0.15f));
            g.drawLine(labelGutterWidth, laneTop, (float) getWidth(), laneTop, 1.0f);

            if (isActiveLane)
            {
                g.setColour(colour.withAlpha(0.5f));
                g.drawRect(juce::Rectangle<float>(labelGutterWidth, laneTop, (float) getWidth() - labelGutterWidth, automationLaneHeight), 1.5f);
            }

            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(label, juce::Rectangle<float>(2.0f, laneTop, labelGutterWidth - 4.0f, automationLaneHeight),
                       juce::Justification::centredLeft);

            auto valueToY = [&](int value)
            {
                auto t = (float) (value - minValue) / (float) juce::jmax(1, maxValue - minValue);
                return laneTop + automationLaneHeight - 2.0f - t * (automationLaneHeight - 4.0f);
            };
            auto stepToX = [&](int step)
            {
                return labelGutterWidth + (float) juce::jlimit(0, visibleStepsCount, step - firstVisibleStep) * colWidth;
            };

            // Appends a shaped sub-path from (fromStep,fromValue) to
            // (toStep,toValue) using curveType/curveAmount (see
            // AutomationCurveType's declaration) -- matches
            // PlaybackEngine::interpolateAutomationValue()'s own math
            // exactly (including sharing automationCurveAmountExponentScale),
            // so whatever's drawn always matches what actually plays back.
            // Shared by the real (committed) path below and the dashed
            // pending-point preview further down.
            auto addShapedSegment = [&](juce::Path& path, int fromStep, int fromValue, AutomationCurveType curveType, float curveAmount, int toStep, int toValue)
            {
                if (curveType == AutomationCurveType::Step)
                {
                    // Holds the previous value right up to this point, then
                    // jumps -- drawn as a flat line followed by a vertical
                    // rise, not a diagonal.
                    path.lineTo(stepToX(toStep), valueToY(fromValue));
                    path.lineTo(stepToX(toStep), valueToY(toValue));
                    return;
                }
                if (curveAmount == 0.0f)
                {
                    path.lineTo(stepToX(toStep), valueToY(toValue));
                    return;
                }
                // X (time) must advance LINEARLY here -- only the VALUE
                // follows the eased curve, exactly like
                // PlaybackEngine::interpolateAutomationValue() (t there is
                // a linear time fraction, then only THAT gets eased before
                // lerping the value). Easing both X and value together (a
                // prior bug) made every sample an affine function of the
                // same eased parameter, so the plotted points always fell
                // on one straight line no matter the curve shape -- Ease
                // In/Out was visually indistinguishable from Linear
                // ("ease in outってリニアと違いが見えない").
                auto exponent = 1.0 + std::abs((double) curveAmount) * automationCurveAmountExponentScale;
                constexpr int subdivisions = 16;
                for (int s = 1; s <= subdivisions; ++s)
                {
                    auto tLinear = (double) s / (double) subdivisions;
                    auto tEased = curveAmount > 0.0f ? std::pow(tLinear, exponent) : 1.0 - std::pow(1.0 - tLinear, exponent);
                    auto sampleX = stepToX(fromStep) + (float) tLinear * (stepToX(toStep) - stepToX(fromStep));
                    auto sampleValue = fromValue + (int) std::round(tEased * (double) (toValue - fromValue));
                    path.lineTo(sampleX, valueToY(sampleValue));
                }
            };

            auto lastVisibleStepExclusive = firstVisibleStep + visibleStepsCount;

            if (!points.empty())
            {
                // Connects every recorded breakpoint in order -- including
                // the nearest one just outside each edge of the visible
                // window, so the drawn line reaches (rather than stopping
                // short of) the window's left/right border instead of
                // visibly floating.
                juce::Path path;
                auto started = false;
                const AutomationPoint* previousPoint = nullptr;
                for (size_t i = 0; i < points.size(); ++i)
                {
                    auto& point = points[i];
                    auto isLastBeforeWindow = point.stepIndex < firstVisibleStep
                        && (i + 1 == points.size() || points[i + 1].stepIndex >= firstVisibleStep);
                    auto isInsideWindow = point.stepIndex >= firstVisibleStep && point.stepIndex < lastVisibleStepExclusive;
                    auto isFirstAfterWindow = point.stepIndex >= lastVisibleStepExclusive
                        && (i == 0 || points[i - 1].stepIndex < lastVisibleStepExclusive);
                    if (!isLastBeforeWindow && !isInsideWindow && !isFirstAfterWindow)
                        continue;

                    if (!started)
                    {
                        path.startNewSubPath(stepToX(point.stepIndex), valueToY(point.value));
                        started = true;
                    }
                    else if (previousPoint != nullptr)
                    {
                        // point.curveType (not previousPoint's) shapes this
                        // segment -- see AutomationCurveType's declaration:
                        // curve-on-arrival, not curve-on-departure.
                        addShapedSegment(path, previousPoint->stepIndex, previousPoint->value, point.curveType, point.curveAmount, point.stepIndex, point.value);
                    }
                    previousPoint = &point;
                }

                if (started)
                {
                    g.setColour(colour);
                    g.strokePath(path, juce::PathStrokeType(2.0f));
                }

                // A recorded point marker, at a fixed on-screen size
                // regardless of zoom -- at typical zoom levels colWidth
                // itself is far under a pixel (same reason the edit cursor
                // needed a minimum-width clamp), so without this a point
                // placed via Cmd+Ctrl+I was invisible, indistinguishable
                // from empty space on the curve between its neighbors
                // ("オートメーションポイントは点は見えるような大きさに
                // したい"). Only drawn for points actually inside the
                // visible window -- the "just outside" ones used above
                // purely to extend the line to the window's edge aren't
                // real points at that on-screen position.
                constexpr float markerRadius = 3.5f;
                g.setColour(colour);
                for (auto& point : points)
                {
                    if (point.stepIndex < firstVisibleStep || point.stepIndex >= lastVisibleStepExclusive)
                        continue;
                    auto x = stepToX(point.stepIndex);
                    auto y = valueToY(point.value);
                    g.setColour(colour);
                    g.fillEllipse(x - markerRadius, y - markerRadius, markerRadius * 2.0f, markerRadius * 2.0f);

                    // Shift+D/F multi-selection -- a ring around the
                    // marker, mirroring the cyan note-block outline
                    // setSelectedNoteStarts() draws for notes.
                    if (std::find(selectedAutomationSteps.begin(), selectedAutomationSteps.end(), point.stepIndex) != selectedAutomationSteps.end())
                    {
                        constexpr float ringRadius = 6.5f;
                        g.setColour(juce::Colours::cyan);
                        g.drawEllipse(x - ringRadius, y - ringRadius, ringRadius * 2.0f, ringRadius * 2.0f, 1.5f);
                    }
                }

            }

            // Ghost preview of what Ctrl+V/Cmd+Ctrl+I would draw right now
            // -- see automationPendingValue's declaration. Only for the
            // lane currently being edited (Cmd+Ctrl+L).
            const AutomationPoint* beforePoint = nullptr;
            const AutomationPoint* afterPoint = nullptr;
            for (auto& point : points)
            {
                if (point.stepIndex <= cursorStep)
                    beforePoint = &point;
                else if (afterPoint == nullptr)
                    afterPoint = &point;
            }
            auto realPointAtCursor = beforePoint != nullptr && beforePoint->stepIndex == cursorStep;

            if (isActiveLane && automationPendingValue >= 0)
            {
                juce::Path ghostPath;
                auto cursorX = stepToX(cursorStep);
                auto cursorY = valueToY(automationPendingValue);

                if (beforePoint != nullptr && !realPointAtCursor)
                {
                    // The PENDING curve type/amount (Cmd+Ctrl+V /
                    // Cmd+Ctrl+Z/X, not committed yet) shapes this segment
                    // -- it's exactly what would be created by placing a
                    // new point here, so the preview must show what's
                    // about to be committed, not some unrelated existing
                    // point's curveType.
                    ghostPath.startNewSubPath(stepToX(beforePoint->stepIndex), valueToY(beforePoint->value));
                    addShapedSegment(ghostPath, beforePoint->stepIndex, beforePoint->value, automationPendingCurveType, automationPendingCurveAmount, cursorStep, automationPendingValue);
                }
                else
                {
                    ghostPath.startNewSubPath(cursorX, cursorY);
                }

                if (afterPoint != nullptr)
                {
                    // afterPoint's OWN curveType/curveAmount always govern
                    // the segment arriving at it, regardless of what's
                    // newly inserted/edited before it -- see
                    // AutomationCurveType's declaration (curve-on-arrival,
                    // not curve-on-departure).
                    addShapedSegment(ghostPath, cursorStep, automationPendingValue, afterPoint->curveType, afterPoint->curveAmount, afterPoint->stepIndex, afterPoint->value);
                }

                juce::Path dashedGhostPath;
                float dashLengths[] = { 4.0f, 3.0f };
                juce::PathStrokeType(2.0f).createDashedStroke(dashedGhostPath, ghostPath, dashLengths, 2);
                g.setColour(colour.withAlpha(0.35f));
                g.fillPath(dashedGhostPath);

                // Hollow marker at the pending position -- distinct from
                // the solid filled circles real, already-committed points
                // get above.
                constexpr float ghostMarkerRadius = 4.0f;
                g.setColour(colour.withAlpha(0.7f));
                g.drawEllipse(cursorX - ghostMarkerRadius, cursorY - ghostMarkerRadius, ghostMarkerRadius * 2.0f, ghostMarkerRadius * 2.0f, 1.5f);
            }

            // Names the curve type/amount in effect at the cursor, in the
            // lane currently being edited -- a real point's own
            // curveType/curveAmount if one sits exactly there (Cmd+Ctrl+V/
            // Cmd+Ctrl+Z/X edit it directly), otherwise the PENDING
            // curve type/amount Cmd+Ctrl+V/Z/X are cycling for the next
            // placement. Without this there was no way to tell which
            // shape was selected until a next point actually existed to
            // visibly bend ("カーブのサイクルでどんなカーブが選ばれている
            // のかよくわからない").
            if (isActiveLane)
            {
                auto hasCurve = realPointAtCursor || automationPendingValue >= 0;
                auto curveType = realPointAtCursor ? beforePoint->curveType : automationPendingCurveType;
                auto curveAmount = realPointAtCursor ? beforePoint->curveAmount : automationPendingCurveAmount;
                if (hasCurve)
                {
                    // curveAmount's sign already reads as "Ease In" (+) or
                    // "Ease Out" (-) -- see AutomationCurveType's
                    // declaration -- so the label just names the number
                    // rather than relying on a small fixed set of names.
                    auto curveName = curveType == AutomationCurveType::Step ? juce::String("Step")
                                    : curveAmount == 0.0f ? juce::String("Linear")
                                    : juce::String(curveAmount > 0.0f ? "Ease In " : "Ease Out ") + juce::String(std::abs(curveAmount), 2);
                    g.setColour(juce::Colours::white.withAlpha(0.9f));
                    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                    g.drawText(curveName, juce::Rectangle<float>(stepToX(cursorStep) + 6.0f, laneTop, 90.0f, automationLaneHeight * 0.5f),
                               juce::Justification::centredLeft);
                }
            }
        };

        // Read-only view of MidiClip::parameterLanes -- unlike the two
        // above, these are never keyboard-edited (they're only ever
        // written by physically touching a plugin's own knob during
        // Touch mode, see MainEditorComponent::recordParameterAutomationPoint()),
        // so this deliberately skips the cursor/ghost-preview/curve-type/
        // selection machinery drawAutomationLane() above needs -- just the
        // recorded points themselves, so it's actually visible that
        // something got recorded ("値は取れていて、再生もできるけど、
        // レーンには何も出ない"). One lane per touched parameter, stacked
        // below Pitch Bend/Filter Cutoff in the order they were first
        // touched.
        auto drawParameterLane = [&](float laneTop, const ParameterAutomationLane& lane, juce::Colour colour, bool isActiveLane, int laneIndex)
        {
            g.setColour(juce::Colours::white.withAlpha(0.15f));
            g.drawLine(labelGutterWidth, laneTop, (float) getWidth(), laneTop, 1.0f);

            if (isActiveLane)
            {
                g.setColour(colour.withAlpha(0.5f));
                g.drawRect(juce::Rectangle<float>(labelGutterWidth, laneTop, (float) getWidth() - labelGutterWidth, automationLaneHeight), 1.5f);
            }

            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.setFont(juce::FontOptions(10.0f));
            g.drawText(lane.parameterName.isNotEmpty() ? lane.parameterName : lane.parameterID,
                       juce::Rectangle<float>(2.0f, laneTop, labelGutterWidth - 4.0f, automationLaneHeight),
                       juce::Justification::centredLeft);

            auto valueToY = [&](float value)
            {
                return laneTop + automationLaneHeight - 2.0f - juce::jlimit(0.0f, 1.0f, value) * (automationLaneHeight - 4.0f);
            };
            auto stepToX = [&](int step)
            {
                return labelGutterWidth + (float) juce::jlimit(0, visibleStepsCount, step - firstVisibleStep) * colWidth;
            };

            // Float-valued sibling of drawAutomationLane()'s own
            // addShapedSegment() above -- same curve math (including
            // sharing automationCurveAmountExponentScale), just interpolating
            // a float 0.0-1.0 value instead of an int, to match
            // ParameterAutomationPoint. A plugin-parameter lane is a full
            // automation lane like any other (see AutomationLane's
            // declaration) -- it must render curves the exact same way
            // Pitch Bend/Filter Cutoff do, not as a visually different,
            // curve-less approximation ("プラグインからオートメーション
            // レーンが追加された場合Curveが出てこない。別物の扱いに
            // しないで").
            auto addShapedSegment = [&](juce::Path& path, int fromStep, float fromValue, AutomationCurveType curveType, float curveAmount, int toStep, float toValue)
            {
                if (curveType == AutomationCurveType::Step)
                {
                    path.lineTo(stepToX(toStep), valueToY(fromValue));
                    path.lineTo(stepToX(toStep), valueToY(toValue));
                    return;
                }
                if (curveAmount == 0.0f)
                {
                    path.lineTo(stepToX(toStep), valueToY(toValue));
                    return;
                }
                auto exponent = 1.0 + std::abs((double) curveAmount) * automationCurveAmountExponentScale;
                constexpr int subdivisions = 16;
                for (int s = 1; s <= subdivisions; ++s)
                {
                    auto tLinear = (double) s / (double) subdivisions;
                    auto tEased = curveAmount > 0.0f ? std::pow(tLinear, exponent) : 1.0 - std::pow(1.0 - tLinear, exponent);
                    auto sampleX = stepToX(fromStep) + (float) tLinear * (stepToX(toStep) - stepToX(fromStep));
                    auto sampleValue = fromValue + (float) tEased * (toValue - fromValue);
                    path.lineTo(sampleX, valueToY(sampleValue));
                }
            };

            auto lastVisibleStepExclusive = firstVisibleStep + visibleStepsCount;

            if (!lane.points.empty())
            {
                juce::Path path;
                auto started = false;
                const ParameterAutomationPoint* previousPoint = nullptr;
                for (size_t i = 0; i < lane.points.size(); ++i)
                {
                    auto& point = lane.points[i];
                    auto isLastBeforeWindow = point.stepIndex < firstVisibleStep
                        && (i + 1 == lane.points.size() || lane.points[i + 1].stepIndex >= firstVisibleStep);
                    auto isInsideWindow = point.stepIndex >= firstVisibleStep && point.stepIndex < lastVisibleStepExclusive;
                    auto isFirstAfterWindow = point.stepIndex >= lastVisibleStepExclusive
                        && (i == 0 || lane.points[i - 1].stepIndex < lastVisibleStepExclusive);
                    if (!isLastBeforeWindow && !isInsideWindow && !isFirstAfterWindow)
                        continue;

                    if (!started)
                    {
                        path.startNewSubPath(stepToX(point.stepIndex), valueToY(point.value));
                        started = true;
                    }
                    else if (previousPoint != nullptr)
                    {
                        // point.curveType (not previousPoint's) shapes this
                        // segment -- curve-on-arrival, same convention as
                        // drawAutomationLane()'s own addShapedSegment() use.
                        addShapedSegment(path, previousPoint->stepIndex, previousPoint->value, point.curveType, point.curveAmount, point.stepIndex, point.value);
                    }
                    previousPoint = &point;
                }
                if (started)
                {
                    g.setColour(colour);
                    g.strokePath(path, juce::PathStrokeType(1.5f));
                }

                constexpr float markerRadius = 2.5f;
                g.setColour(colour);
                for (auto& point : lane.points)
                {
                    if (point.stepIndex < firstVisibleStep || point.stepIndex >= lastVisibleStepExclusive)
                        continue;
                    auto x = stepToX(point.stepIndex);
                    auto y = valueToY(point.value);
                    g.fillEllipse(x - markerRadius, y - markerRadius, markerRadius * 2.0f, markerRadius * 2.0f);
                }
            }

            // Ghost preview of what Cmd+Ctrl+I would place right now -- a
            // live touch-preview value for THIS specific lane (see
            // parameterPreviewValuesByLaneIndex's declaration) takes
            // priority, shown regardless of whether this is the
            // Cmd+Ctrl+L-selected lane -- a single physical touch can move
            // several parameters/lanes at once, and all of their ghosts
            // should be visible together. Falls back to
            // parameterPendingValue (keyboard-driven Cmd+Ctrl+Z/X
            // adjustment) only for the selected lane, same as before this
            // lane could show more than one preview at a time. Curve-
            // shaped via addShapedSegment(), same as drawAutomationLane()'s
            // own ghost preview -- a touch-only preview (not the selected
            // lane) always uses the default Curve/0.0 shape since Touch
            // capture never records a custom curve of its own, which
            // addShapedSegment() draws identically to a straight line
            // anyway (see its own curveAmount==0.0f branch).
            auto previewIt = parameterPreviewValuesByLaneIndex.find(laneIndex);
            auto hasTouchPreview = previewIt != parameterPreviewValuesByLaneIndex.end();
            auto previewValue = hasTouchPreview ? previewIt->second
                : (isActiveLane && parameterPendingValue >= 0.0f) ? parameterPendingValue : -1.0f;

            // Computed unconditionally (not just when previewValue >= 0.0f)
            // since the curve-name label below needs realPointAtCursor/
            // beforePoint too, and can apply even with no ghost preview
            // showing (a real point already sitting exactly at the
            // cursor).
            const ParameterAutomationPoint* beforePoint = nullptr;
            const ParameterAutomationPoint* afterPoint = nullptr;
            for (auto& point : lane.points)
            {
                if (point.stepIndex <= cursorStep)
                    beforePoint = &point;
                else if (afterPoint == nullptr)
                    afterPoint = &point;
            }
            auto realPointAtCursor = beforePoint != nullptr && beforePoint->stepIndex == cursorStep;

            if (previewValue >= 0.0f)
            {
                auto cursorX = stepToX(cursorStep);
                auto cursorY = valueToY(previewValue);

                if (beforePoint != nullptr || afterPoint != nullptr)
                {
                    juce::Path ghostPath;
                    // Only the Cmd+Ctrl+L-selected lane has a meaningful
                    // keyboard-driven pending curve type/amount to shape the
                    // incoming segment with -- any other, touch-only-
                    // previewed lane falls back to the default (Curve/0.0,
                    // i.e. a straight line, since Touch capture doesn't
                    // record a custom shape).
                    auto pendingCurveType = isActiveLane ? parameterPendingCurveType : AutomationCurveType::Curve;
                    auto pendingCurveAmount = isActiveLane ? parameterPendingCurveAmount : 0.0f;

                    if (beforePoint != nullptr && !realPointAtCursor)
                    {
                        ghostPath.startNewSubPath(stepToX(beforePoint->stepIndex), valueToY(beforePoint->value));
                        addShapedSegment(ghostPath, beforePoint->stepIndex, beforePoint->value, pendingCurveType, pendingCurveAmount, cursorStep, previewValue);
                    }
                    else
                    {
                        ghostPath.startNewSubPath(cursorX, cursorY);
                    }

                    if (afterPoint != nullptr)
                    {
                        // afterPoint's OWN curveType/curveAmount always
                        // govern the segment arriving at it -- curve-on-
                        // arrival, same convention as drawAutomationLane().
                        addShapedSegment(ghostPath, cursorStep, previewValue, afterPoint->curveType, afterPoint->curveAmount, afterPoint->stepIndex, afterPoint->value);
                    }

                    juce::Path dashedGhostPath;
                    float dashLengths[] = { 4.0f, 3.0f };
                    juce::PathStrokeType(2.0f).createDashedStroke(dashedGhostPath, ghostPath, dashLengths, 2);
                    g.setColour(colour.withAlpha(0.35f));
                    g.fillPath(dashedGhostPath);
                }

                constexpr float ghostMarkerRadius = 4.0f;
                g.setColour(colour.withAlpha(0.8f));
                g.drawEllipse(cursorX - ghostMarkerRadius, cursorY - ghostMarkerRadius, ghostMarkerRadius * 2.0f, ghostMarkerRadius * 2.0f, 1.5f);
            }

            // Names the curve type/amount in effect at the cursor -- a real
            // point's own curveType/curveAmount if one sits exactly there
            // (Cmd+Ctrl+V/Z/X edit it directly), otherwise the PENDING
            // curve type/amount those same keys are cycling for the next
            // placement. This label was missing entirely for plugin-
            // parameter lanes (drawAutomationLane()'s own copy, just above,
            // already had it for Pitch Bend/Filter Cutoff) -- without it
            // there was no way to tell the curve amount was actually being
            // applied at all, especially at this lane's small 24px height
            // where a subtle bend is hard to see by eye alone
            // ("リニアとか、文字が出てこない。カーブの強さも反映されて
            // いるように見えない").
            if (isActiveLane)
            {
                auto hasCurve = realPointAtCursor || previewValue >= 0.0f;
                auto curveType = realPointAtCursor ? beforePoint->curveType : parameterPendingCurveType;
                auto curveAmount = realPointAtCursor ? beforePoint->curveAmount : parameterPendingCurveAmount;
                if (hasCurve)
                {
                    auto curveName = curveType == AutomationCurveType::Step ? juce::String("Step")
                                    : curveAmount == 0.0f ? juce::String("Linear")
                                    : juce::String(curveAmount > 0.0f ? "Ease In " : "Ease Out ") + juce::String(std::abs(curveAmount), 2);
                    g.setColour(juce::Colours::white.withAlpha(0.9f));
                    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                    g.drawText(curveName, juce::Rectangle<float>(stepToX(cursorStep) + 6.0f, laneTop, 90.0f, automationLaneHeight * 0.5f),
                               juce::Justification::centredLeft);
                }
            }
        };

        static const juce::Colour parameterLaneColours[] = {
            juce::Colours::gold, juce::Colours::hotpink, juce::Colours::limegreen,
            juce::Colours::lightskyblue, juce::Colours::orangered
        };
        for (size_t i = 0; i < clip->parameterLanes.size(); ++i)
            drawParameterLane(filterCutoffLaneTop + automationLaneHeight * (1.0f + (float) i), clip->parameterLanes[i],
                               parameterLaneColours[i % (sizeof(parameterLaneColours) / sizeof(parameterLaneColours[0]))],
                               automationEditModeActive && automationEditLane == AutomationLane::Parameter && activeParameterLaneIndex == (int) i,
                               (int) i);

        drawAutomationLane(pitchBendLaneTop, clip->pitchBendPoints, 0, 16383, juce::Colours::cyan,
                            automationEditModeActive && automationEditLane == AutomationLane::PitchBend, "PB");
        drawAutomationLane(filterCutoffLaneTop, clip->filterCutoffPoints, 0, 127, juce::Colours::orange,
                            automationEditModeActive && automationEditLane == AutomationLane::FilterCutoff, "CUT");
    }
}
