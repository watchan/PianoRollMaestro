#pragma once
#include <vector>
#include <JuceHeader.h>

// Visual-only readout; nothing here is mouse-interactive.
class TransportBarComponent : public juce::Component
{
public:
    void setPlaying(bool isPlaying) { playing = isPlaying; repaint(); }
    // Real-time REC's 4-beat pre-roll (PlaybackEngine::startWithCountIn())
    // -- shown in place of the usual play/stop text so the silence-except-
    // clicks pre-roll doesn't look like nothing is happening.
    void setCountingIn(bool isCountingIn) { countingIn = isCountingIn; repaint(); }
    void setBpm(double bpm) { bpmValue = bpm; repaint(); }
    void setOctaveShift(int octaves) { octaveShift = octaves; repaint(); }
    // Ctrl+Shift+Z/X -- fixed velocity used by both PC-keyboard note
    // sources (the melodic virtual keyboard and the drum grid), since
    // neither can report a real physical press-force the way a MIDI
    // keyboard's velocity byte does.
    void setVirtualKeyboardVelocity(float velocity) { virtualKeyboardVelocity = velocity; repaint(); }

    // Cmd+U cycles this (25/50/75/100) -- how far '1'/'2'/'3' pulls a note
    // toward the grid line, see MainEditorComponent::quantizeAmountPercent's
    // declaration.
    void setQuantizeAmountPercent(int percent) { quantizeAmountPercent = percent; repaint(); }

    // Cmd+4 toggles this -- see MainEditorComponent::quantizeTripletMode's
    // declaration. Previously had no visible indicator at all, so it was
    // easy to leave on by accident and then be confused why '1'/'2'/'3'
    // weren't snapping to the expected straight grid (traced to triplet
    // mode having been left on with no way to tell).
    void setQuantizeTripletMode(bool isEnabled) { quantizeTripletMode = isEnabled; repaint(); }

    // '1'/'2'/'4' select noteRepeatGridSteps and turn this on, '5' toggles
    // noteRepeatTripletMode -- see MainEditorComponent::
    // updateNoteRepeat()'s declaration. gridSteps follows the same 12/6/3
    // (quarter/eighth/sixteenth) convention as setQuantizeAmountPercent's
    // neighbors.
    void setNoteRepeat(bool isEnabled, int gridSteps, bool tripletMode)
    {
        noteRepeatEnabled = isEnabled;
        noteRepeatGridSteps = gridSteps;
        noteRepeatTripletMode = tripletMode;
        repaint();
    }

    // Shift+W toggles this -- whether starting Real-time REC gives a 4-beat
    // count-in first, see MainEditorComponent::toggleCountIn()'s declaration.
    void setCountInEnabled(bool isEnabled) { countInEnabled = isEnabled; repaint(); }

    // 'r' cycles this -- 0 = Off, 1 = Manual, 2 = Auto, 3 = Realtime -- see
    // MainEditorComponent::RecMode's declaration. Combined with `playing`
    // to show BROWSE/MANUAL/STEP/ARMED/LIVE. Drawn as a filled badge (or
    // outlined while nothing can currently be written), not just text, so
    // the current state is readable at a glance.
    void setRecMode(int newRecMode) { recMode = newRecMode; repaint(); }

    // 'c' toggles this -- whether the loop region (set with Shift+C/Cmd+C,
    // drawn in stepGrid) actually makes playback wrap. Same filled-badge
    // treatment as the HUM indicator.
    void setLoopEnabled(bool isEnabled) { loopEnabled = isEnabled; repaint(); }

    // 'w' toggles this -- whether PlaybackEngine::renderMetronomeClicks()
    // actually sounds during playback. Same filled-badge treatment.
    void setMetronomeEnabled(bool isEnabled) { metronomeEnabled = isEnabled; repaint(); }

    // Cmd+Shift+U toggles this -- whether Real-time REC auto-quantizes
    // every note it commits, see MainEditorComponent::
    // autoQuantizeOnRecordEnabled's declaration. Same filled-badge
    // treatment as LOOP/METRONOME.
    void setAutoQuantizeOnRecordEnabled(bool isEnabled) { autoQuantizeOnRecordEnabled = isEnabled; repaint(); }

    // Enter toggles this -- the virtual keyboard/drum grid keys have no
    // held modifier of their own anymore (see MainEditorComponent::
    // drumGridModeActive), so this badge is the only way to tell which of
    // the two is currently live without actually pressing a key.
    void setDrumGridMode(bool isDrumGridActive) { drumGridModeActive = isDrumGridActive; repaint(); }

    // Cmd+Ctrl+W toggles (MainEditorComponent::toggleAutomationTouchMode())
    // -- see the badge's own drawing comment for what Touch actually does.
    void setAutomationTouchMode(bool isTouchEnabled) { automationTouchModeEnabled = isTouchEnabled; repaint(); }

    // Cmd+M toggles Auto/Off (MainEditorComponent::cycleScale()) --
    // rootPitchClass/isMinor are KeyEstimator's whole-piece guess, kept
    // synced whenever Auto is on (see updateStepGridScale()). isShown ==
    // false (Off) hides the badge entirely rather than showing a stale or
    // meaningless key.
    void setEstimatedKey(int rootPitchClass, bool isMinor, bool isShown)
    {
        estimatedKeyRootPitchClass = rootPitchClass;
        estimatedKeyIsMinor = isMinor;
        estimatedKeyShown = isShown;
        repaint();
    }

    // Empty = nothing's been heard yet, otherwise the last note(s) held on
    // the MIDI/PC keyboard (possibly a chord) -- kept even after the
    // note(s) stop sounding, so 'f' can commit at your own pace -- see
    // MainEditorComponent::handleMidiNoteChange(). durationSteps is the
    // Shift+Z/Shift+X commit-duration preset.
    void setPendingNoteStatus(const std::vector<int>& pitches, int durationSteps)
    {
        pendingNotePitches = pitches;
        pendingNoteDurationSteps = durationSteps;
        repaint();
    }

    // Pushed every tick from MainEditorComponent::timerCallback() (not just
    // on edits) with the DEBOUNCED pedal state -- see its
    // pendingSustainCrossingMs's declaration -- so the badge visibly tracks
    // a real sustain pedal live, rather than only being inferable after the
    // fact from a recorded note's length.
    void setSustainPedalDown(bool isDown) { sustainPedalDown = isDown; repaint(); }

    void paint(juce::Graphics& g) override;

    // How tall this component needs to be to show every badge/status text
    // WITHOUT clipping anything, for the given width -- wraps onto as many
    // rows as it takes rather than running off the right edge, which is
    // what used to happen once enough badges accumulated that they no
    // longer fit in one row at any reasonable window width.
    // MainEditorComponent::resized() calls
    // this BEFORE sizing this component (this component's own width is
    // always the full window width, decided before anything else divides
    // it up), then sizes it to exactly the height this returns, shifting
    // everything below it down/up to match. Must use the exact same
    // layout (buildBadges()/layoutBadges()) paint() itself draws from, or
    // the reported height and the actual drawn content would drift apart.
    int getRequiredHeightForWidth(int width) const;

private:
    // One drawn unit -- either a filled/outlined rounded-rect "chip" (every
    // ON/OFF badge) or the trailing free-form status line, which behaves
    // like one more flow item for wrapping purposes but draws as plain
    // left-aligned text instead of a chip (isPlainText). width is fixed for
    // chips, and measured from the actual string for the status line (its
    // length varies -- e.g. NOTE: a held chord's name -- so a fixed width
    // wouldn't fit it).
    struct Badge
    {
        juce::String text;
        juce::Colour colour;
        bool filled = true;
        int width = 0;
        bool isPlainText = false;
        float fontSize = 13.0f;
    };
    // Resolves every badge's current text/colour/filled state (and the
    // trailing status line's current text/measured width) from this
    // component's own member state -- the single source of truth both
    // paint() and getRequiredHeightForWidth() read from, so what's
    // reported as "how tall this needs to be" can never drift from what
    // actually gets drawn.
    std::vector<Badge> buildBadges() const;
    // Flows badges left to right, wrapping to a new row (x back to 0, y
    // down by rowHeight + rowGap) whenever the next one wouldn't fit in
    // availableWidth -- never wraps the very first badge of a row even if
    // it alone is wider than availableWidth (nothing useful would come of
    // an infinite-height component). Returns each badge paired with the
    // Rectangle it should be drawn into.
    std::vector<std::pair<Badge, juce::Rectangle<int>>> layoutBadges(const std::vector<Badge>& badges, int availableWidth) const;

    static constexpr int badgeRowHeight = 28;
    static constexpr int badgeRowGap = 4;
    static constexpr int badgeHorizontalGap = 12;

    bool playing = false;
    bool countingIn = false;
    double bpmValue = 120.0;
    int octaveShift = 0;
    float virtualKeyboardVelocity = 0.8f;
    int quantizeAmountPercent = 100;
    bool quantizeTripletMode = false;
    bool noteRepeatEnabled = false;
    int noteRepeatGridSteps = 12;
    bool noteRepeatTripletMode = false;
    bool countInEnabled = true;
    int recMode = 0;
    bool loopEnabled = false;
    bool metronomeEnabled = false;
    bool autoQuantizeOnRecordEnabled = false;
    bool drumGridModeActive = false;
    bool automationTouchModeEnabled = false;
    bool sustainPedalDown = false;
    int estimatedKeyRootPitchClass = 0;
    bool estimatedKeyIsMinor = false;
    bool estimatedKeyShown = true;

    std::vector<int> pendingNotePitches;
    int pendingNoteDurationSteps = 1;
};
