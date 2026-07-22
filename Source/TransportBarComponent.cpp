#include "TransportBarComponent.h"
#include <cmath>

std::vector<TransportBarComponent::Badge> TransportBarComponent::buildBadges() const
{
    std::vector<Badge> badges;

    // REC mode badge -- a filled block, not just text, so the current REC
    // mode is visible at a glance. recMode 0 = Browse (pure navigation, no
    // commits at all); recMode 1 = Manual (Step REC, confirm -- Ctrl+V
    // commits, nothing commits on its own); recMode 2 = Auto (Step REC
    // auto, commits the instant a gesture completes, any transport state);
    // recMode 3 = Realtime, a dedicated real-time-recording mode that's
    // pure preview (like Browse -- outlined, not filled) while stopped,
    // and only actually captures once playback is started -- see
    // MainEditorComponent::RecMode's declaration. Outlined (not filled)
    // whenever nothing can currently be written, filled whenever it can.
    {
        juce::String recText;
        juce::Colour recColour;
        bool recFilled = true;
        if (recMode == 0)
        {
            recText = "REC: BROWSE";
            recColour = juce::Colours::grey;
            recFilled = false;
        }
        else if (recMode == 1)
        {
            recText = "REC: MANUAL";
            recColour = juce::Colours::dodgerblue;
        }
        else if (recMode == 2)
        {
            recText = "REC: STEP";
            recColour = juce::Colours::orange;
        }
        else if (!playing)
        {
            recText = "REC: ARMED";
            recColour = juce::Colours::red;
            recFilled = false;
        }
        else
        {
            recText = "REC: LIVE";
            recColour = juce::Colours::red;
        }
        badges.push_back({ recText, recColour, recFilled, 96, false, 12.0f });
    }

    // Estimated-key badge (Cmd+M toggles Auto/Off) -- omitted entirely when
    // off, rather than showing a stale/meaningless key.
    if (estimatedKeyShown)
    {
        static const char* const noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        badges.push_back({ juce::String("KEY: ") + noteNames[estimatedKeyRootPitchClass] + (estimatedKeyIsMinor ? "m" : ""),
                            juce::Colours::mediumpurple, true, 84, false, 13.0f });
    }

    // LOOP ON/OFF badge -- orange to match the loop region drawn in the
    // step grid.
    badges.push_back({ juce::String("LOOP ") + (loopEnabled ? "ON" : "OFF"),
                        loopEnabled ? juce::Colours::orange : juce::Colours::grey, loopEnabled, 84, false, 13.0f });

    // METRONOME ON/OFF badge -- same treatment as LOOP.
    badges.push_back({ juce::String("METRONOME ") + (metronomeEnabled ? "ON" : "OFF"),
                        metronomeEnabled ? juce::Colours::cornflowerblue : juce::Colours::grey, metronomeEnabled, 130, false, 13.0f });

    // AUTO-Q ON/OFF badge -- same treatment as LOOP/METRONOME. Whether
    // Real-time REC auto-quantizes every note it commits (Cmd+Shift+U).
    badges.push_back({ juce::String("AUTO-Q ") + (autoQuantizeOnRecordEnabled ? "ON" : "OFF"),
                        autoQuantizeOnRecordEnabled ? juce::Colours::mediumspringgreen : juce::Colours::grey,
                        autoQuantizeOnRecordEnabled, 110, false, 13.0f });

    // REPEAT OFF / 1-4 / 1-8 / 1-16 badge -- same treatment as LOOP/
    // METRONOME/AUTO-Q. '1'/'2'/'4' pick the rate (and turn this on), '5'
    // toggles triplet width -- see MainEditorComponent::updateNoteRepeat()'s
    // declaration.
    {
        juce::String noteRepeatText("REPEAT ");
        if (noteRepeatEnabled)
        {
            noteRepeatText << (noteRepeatGridSteps == 240 ? "1/16" : noteRepeatGridSteps == 480 ? "1/8" : "1/4");
            if (noteRepeatTripletMode)
                noteRepeatText << "T";
        }
        else
        {
            noteRepeatText << "OFF";
        }
        badges.push_back({ noteRepeatText, noteRepeatEnabled ? juce::Colours::orangered : juce::Colours::grey,
                            noteRepeatEnabled, 150, false, 13.0f });
    }

    // SUSTAIN ON/OFF badge -- a real MIDI pedal's CC64, debounced (see
    // MainEditorComponent::pendingSustainCrossingMs's declaration). Same
    // filled-badge treatment as LOOP/METRONOME/AUTO-Q/REPEAT. Deliberately
    // NOT the PC-keyboard's separate Ctrl+S software sustain -- that one's
    // fully under keyboard control (no jittery hardware signal to debounce
    // or need visual confirmation for), so it doesn't need this badge.
    badges.push_back({ juce::String("SUSTAIN ") + (sustainPedalDown ? "ON" : "OFF"),
                        sustainPedalDown ? juce::Colours::deeppink : juce::Colours::grey, sustainPedalDown, 120, false, 13.0f });

    // NOTE: CHROMATIC/DRUM badge -- the virtual keyboard/drum grid has no
    // held modifier of its own anymore (Enter toggles between them
    // instead), so this is the only always-visible way to tell which one
    // pressing a mapped key right now would trigger.
    badges.push_back({ juce::String("NOTE: ") + (drumGridModeActive ? "DRUM" : "CHROMATIC"),
                        drumGridModeActive ? juce::Colours::mediumpurple : juce::Colours::mediumseagreen, true, 126, false, 13.0f });

    // AUTO: READ/TOUCH badge -- same filled-badge treatment as NOTE:
    // CHROMATIC/DRUM above (two states, not a plain on/off). Touch mode
    // still plays back existing plugin-parameter automation exactly like
    // Read does -- the only difference is that physically moving a
    // plugin's own knob while Touch is on records that gesture (while
    // playing) or previews it (while stopped) into an automation lane.
    // Bright red while Touch is on since it's a record-adjacent state --
    // same reasoning REC's own badge colors use ("オートメーションの
    // Read／Writeがわからない見分けがつかない").
    badges.push_back({ juce::String("AUTO: ") + (automationTouchModeEnabled ? "TOUCH" : "READ"),
                        automationTouchModeEnabled ? juce::Colours::red : juce::Colours::grey,
                        automationTouchModeEnabled, 120, false, 13.0f });

    // Trailing free-form status line -- pendingNoteDurationSteps tracks the
    // current commit-duration preset (Shift+Z/Shift+X) continuously
    // (MainEditorComponent::updatePendingNoteDisplays() pushes it on every
    // refresh regardless of whether a chord is actually pending), so this
    // name is valid to show ALWAYS, not just while a chord happens to be
    // held ("今指定されている音価がわからない。わかりやすいところに
    // 表示したい"). Flows as one more badge for wrapping purposes (its
    // width is measured, not fixed, since NOTE: a held chord's name makes
    // it grow/shrink) but draws as plain left-aligned text, not a chip --
    // see isPlainText's declaration.
    auto durationName = [](int steps) -> juce::String
    {
        return steps == 240 ? "1/16"
             : steps == 320 ? "1/8T"
             : steps == 480 ? "1/8"
             : steps == 960 ? "1/4"
                             : juce::String(steps) + " steps";
    };

    juce::String text;
    text << (countingIn ? "... COUNT IN" : playing ? "> PLAYING" : "|| STOPPED")
         << "    BPM " << juce::String(bpmValue, 0)
         << "    OCT: " << (octaveShift >= 0 ? "+" : "") << octaveShift
         << "    VEL: " << (int) std::round(virtualKeyboardVelocity * 100.0f) << "%"
         << "    DUR: " << durationName(pendingNoteDurationSteps)
         << "    QUANT: " << quantizeAmountPercent << "%" << (quantizeTripletMode ? " (TRIPLET)" : "")
         << "    COUNT-IN: " << (countInEnabled ? "ON" : "OFF");

    if (!pendingNotePitches.empty())
    {
        juce::StringArray names;
        for (auto pitch : pendingNotePitches)
            names.add(juce::MidiMessage::getMidiNoteName(pitch, true, true, 3));

        text << "    NOTE: " << names.joinIntoString("+") << " (f to commit)";
    }

    juce::Font statusFont(16.0f);
    auto textWidth = (int) std::ceil(statusFont.getStringWidthFloat(text)) + 8;
    badges.push_back({ text, juce::Colours::white, false, textWidth, true, 16.0f });

    return badges;
}

std::vector<std::pair<TransportBarComponent::Badge, juce::Rectangle<int>>>
    TransportBarComponent::layoutBadges(const std::vector<Badge>& badges, int availableWidth) const
{
    std::vector<std::pair<Badge, juce::Rectangle<int>>> positioned;
    positioned.reserve(badges.size());

    int x = 0;
    int y = 0;
    for (auto& badge : badges)
    {
        // Never wrap the very first badge of a row, even if it alone is
        // wider than availableWidth -- an empty row followed by an
        // overflowing one is still more useful than an infinite-height
        // component from a badge that can never "fit".
        if (x > 0 && x + badge.width > availableWidth)
        {
            x = 0;
            y += badgeRowHeight + badgeRowGap;
        }

        positioned.push_back({ badge, juce::Rectangle<int>(x, y, badge.width, badgeRowHeight) });
        x += badge.width + badgeHorizontalGap;
    }

    return positioned;
}

int TransportBarComponent::getRequiredHeightForWidth(int width) const
{
    auto badges = buildBadges();
    auto positioned = layoutBadges(badges, juce::jmax(1, width - 16)); // matches paint()'s reduced(8, 0) margin
    if (positioned.empty())
        return badgeRowHeight;
    return positioned.back().second.getBottom();
}

void TransportBarComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    auto bounds = getLocalBounds().reduced(8, 0);
    auto badges = buildBadges();
    auto positioned = layoutBadges(badges, juce::jmax(1, bounds.getWidth()));

    for (auto& [badge, area] : positioned)
    {
        auto drawArea = area.translated(bounds.getX(), bounds.getY());

        if (badge.isPlainText)
        {
            g.setColour(badge.colour);
            g.setFont(juce::FontOptions(badge.fontSize));
            g.drawText(badge.text, drawArea, juce::Justification::centredLeft);
            continue;
        }

        auto chipArea = drawArea.reduced(0, 7);
        if (badge.filled)
        {
            g.setColour(badge.colour);
            g.fillRoundedRectangle(chipArea.toFloat(), 4.0f);
            g.setColour(juce::Colours::black);
        }
        else
        {
            g.setColour(badge.colour.withAlpha(0.5f));
            g.drawRoundedRectangle(chipArea.toFloat(), 4.0f, 1.0f);
            g.setColour(badge.colour);
        }
        g.setFont(juce::FontOptions(badge.fontSize, juce::Font::bold));
        g.drawText(badge.text, chipArea, juce::Justification::centred);
    }
}
