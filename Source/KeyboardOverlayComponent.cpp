#include "KeyboardOverlayComponent.h"
#include "VirtualKeyboardMaps.h"
#include <algorithm>
#include <vector>

namespace
{
    struct KeySpec { char key; float xUnits; };
    struct KeyRow { std::vector<KeySpec> keys; float xOffsetUnits; };

    // Physical QWERTY stagger, in key-widths from each row's own left edge
    // -- matches real keyboard rows closely enough to read at a glance.
    const std::vector<KeyRow>& keyboardRows()
    {
        static const std::vector<KeyRow> rows = {
            { { { '1', 0 }, { '2', 1 }, { '3', 2 }, { '4', 3 }, { '5', 4 }, { '6', 5 }, { '7', 6 },
                { '8', 7 }, { '9', 8 }, { '0', 9 }, { '-', 10 }, { '=', 11 } }, 0.0f },
            { { { 'Q', 0 }, { 'W', 1 }, { 'E', 2 }, { 'R', 3 }, { 'T', 4 }, { 'Y', 5 }, { 'U', 6 },
                { 'I', 7 }, { 'O', 8 }, { 'P', 9 }, { '[', 10 }, { ']', 11 } }, 0.5f },
            { { { 'A', 0 }, { 'S', 1 }, { 'D', 2 }, { 'F', 3 }, { 'G', 4 }, { 'H', 5 }, { 'J', 6 },
                { 'K', 7 }, { 'L', 8 }, { ';', 9 }, { '\'', 10 } }, 0.75f },
            { { { 'Z', 0 }, { 'X', 1 }, { 'C', 2 }, { 'V', 3 }, { 'B', 4 }, { 'N', 5 }, { 'M', 6 },
                { ',', 7 }, { '.', 8 }, { '/', 9 } }, 1.25f },
        };
        return rows;
    }

    juce::String noteNameFor(int pitch)
    {
        return juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
    }

    // Specific (modifier tier, key) combos macOS/JUCE intercepts before this
    // app ever sees them -- discovered by direct testing over the course of
    // this app's development (this is the exhaustive, exact list; every
    // instance is called out by name in MainEditorComponent::keyPressed()'s
    // own comments explaining why that combo was avoided). Kept here purely
    // so the overlay can show them greyed out with why, instead of looking
    // exactly like every other plain unassigned key on the board.
    // needsCtrl/needsCmd/needsAlt/needsShift must
    // ALL match the currently-held modifiers exactly -- these combos are
    // only actually blocked at that one specific tier, not at every tier
    // that happens to include the key.
    struct BlockedCombo
    {
        bool needsCtrl, needsCmd, needsAlt, needsShift;
        char key;
        const char* reason;
    };

    const std::vector<BlockedCombo>& blockedCombos()
    {
        static const std::vector<BlockedCombo> combos = {
            // Cmd+Shift+3 -- macOS's system-wide full-screen screenshot
            // shortcut, never reaches any app.
            { false, true, false, true, '3', "Screenshot\n(macOS)" },
            // Shift+5 -- macOS/JUCE deliver this as the '%' character
            // instead of a raw keypress this app can intercept.
            { false, false, false, true, '5', "Types '%'\n(macOS)" },
            // Option+E -- a dead key (accent composition), swallows the
            // keypress waiting for a second character instead of firing.
            { false, false, true, false, 'E', "Dead key\n(macOS)" },
        };
        return combos;
    }

    const BlockedCombo* findBlockedCombo(const juce::ModifierKeys& mods, char key)
    {
        for (auto& combo : blockedCombos())
            if (combo.key == key && combo.needsCtrl == mods.isCtrlDown() && combo.needsCmd == mods.isCommandDown()
                && combo.needsAlt == mods.isAltDown() && combo.needsShift == mods.isShiftDown())
                return &combo;
        return nullptr;
    }

    // The single source of truth for "what does pressing this key do right
    // now" -- mirrors MainEditorComponent::keyPressed()'s dispatch exactly
    // (same modifier-tier priority: Ctrl, then Cmd, then Shift+Option, then
    // Option, then Shift, then plain/mode-aware), but as a pure lookup that
    // never executes anything. NOTE: this has to be kept in sync by hand
    // whenever keyPressed() changes -- same accepted tradeoff as
    // ShortcutHelpBarComponent's static help text.
    std::map<char, juce::String> computeKeyLabels(const juce::ModifierKeys& mods, bool isSessionView,
                                                   int recMode, int transposeSemitones, bool isDrumGridActive)
    {
        std::map<char, juce::String> labels;

        // Ctrl+Z/X (transpose), Ctrl+Shift+Z/X (velocity), Ctrl+T/G
        // (tie/bar-jump), and Cmd+Ctrl+T/P/N (track add/prev/next) +
        // Cmd+Ctrl+A/L/S/Z/X/I/D (automation editing) are the only Ctrl
        // combos left -- the note maps themselves have no modifier of
        // their own anymore (Enter toggles between them instead, see the
        // plain tier below), so Ctrl+S sustain and these stay the only
        // reason to still check Ctrl at all.
        if (mods.isCtrlDown())
        {
            if (mods.isCommandDown())
            {
                labels['T'] = "Add Track";
                labels['P'] = "Prev Trk";
                labels['N'] = "Next Trk";
                labels['C'] = "Loop End";
                labels['E'] = "Clip End";
                labels['B'] = "Toggle Loop";
                labels['5'] = "Range Start";
                labels['R'] = "Range End";
                labels['H'] = "Chord Trk";
                labels['A'] = "Automation Mode";
                labels['L'] = "Cycle Lane";
                labels['W'] = "Auto Read/Touch";
                labels['S'] = "Sustain Point";
                labels['I'] = "Insert Point";
                labels['D'] = "Delete Point";
                labels['V'] = "Toggle Curve/Step";
                labels['Z'] = mods.isShiftDown() ? "Curve Amt-- " : "Curve Amt-";
                labels['X'] = mods.isShiftDown() ? "Curve Amt++" : "Curve Amt+";
            }
            else if (mods.isShiftDown())
            {
                labels['Z'] = "Vel-";
                labels['X'] = "Vel+";
            }
            else
            {
                labels['Z'] = "Transp-";
                labels['X'] = "Transp+";
                labels['S'] = "Sustain";
                labels['T'] = "Tie";
                labels['G'] = "Jump Fwd";
                labels['V'] = "Commit";
            }
            return labels;
        }

        if (mods.isCommandDown())
        {
            if (mods.isAltDown())
            {
                labels['G'] = "Scroll Up";
                labels['B'] = "Scroll Dn";
            }
            else if (mods.isShiftDown())
            {
                labels['G'] = "Zoom V+";
                labels['B'] = "Zoom V-";
                labels['D'] = "Zoom H-";
                labels['F'] = "Zoom H+";
                labels['S'] = "Save As";
                labels['Z'] = "Redo";
                labels['U'] = "Auto-Quant";
                labels['C'] = "Loop Start";
            }
            else
            {
                labels['S'] = isSessionView ? "Save" : "Unquantize";
                labels['O'] = "Open";
                labels['0'] = "Save";
                labels['N'] = "New";
                labels['G'] = "Sel Top Note";
                labels['Y'] = "Instrument";
                labels['P'] = "Plugin Ed.";
                labels[','] = "Audio Set.";
                labels['B'] = "Sel Note Dn";
                labels['M'] = "Scale";
                labels['C'] = "Copy";
                labels['V'] = "Paste";
                labels['D'] = "Dup Range";
                labels['Z'] = "Undo";
                labels['K'] = "Keyboard";
                labels['X'] = isSessionView ? "Capture" : "Del+Back";
                labels['U'] = "Quant Amt";
                labels['5'] = "Raise Low(Oct)";
                labels['R'] = "Lower High(Oct)";
                if (!isSessionView)
                {
                    labels['1'] = "Quant 1/4";
                    labels['2'] = "Quant 1/8";
                    labels['3'] = "Quant 1/16";
                    labels['4'] = "Triplet";
                    labels['A'] = "Select All";
                }
            }
            return labels;
        }

        if (mods.isShiftDown() && mods.isAltDown())
        {
            auto browseModePitchNudge = !isSessionView && recMode == 0;
            labels['G'] = browseModePitchNudge ? "Pitch Up" : "Prev Trk";
            labels['B'] = browseModePitchNudge ? "Pitch Dn" : "Next Trk";
            return labels;
        }

        if (mods.isAltDown())
        {
            labels['Z'] = "Tempo-";
            labels['X'] = "Tempo+";
            if (!isSessionView)
            {
                labels['D'] = "Nudge Left";
                labels['F'] = "Nudge Right";
            }
            return labels;
        }

        if (mods.isShiftDown())
        {
            labels['Z'] = "Duration-";
            labels['X'] = "Duration+";
            labels['F'] = "Sel Ext Fwd";
            labels['D'] = "Sel Ext Back";
            labels['Q'] = "Sel Up+";
            labels['R'] = "Sel Dn+";
            labels['A'] = "Jump Start";
            labels['G'] = "Oct Up";
            labels['B'] = "Oct Dn";
            labels['W'] = "Count-In";
            if (!isSessionView)
            {
                labels['C'] = "Jump Back 1 Bar";
                labels['V'] = "Jump Fwd 1 Bar";
            }
            return labels;
        }

        // Melodic keyboard / drum grid -- no modifier needed anymore, Enter
        // toggles which of the two is live (see isDrumGridActive). Filled
        // in first so the editing labels below can still take priority for
        // any key that happens to also be a mapped note (shouldn't happen
        // by design -- see VirtualKeyboardMaps.h's comment on why B/G/T/5
        // and '0' were kept out of/left in the note maps specifically to
        // avoid colliding with the plain-key editing commands below).
        if (isDrumGridActive)
        {
            std::vector<std::pair<char, int>> sorted(virtualDrumKeyMap().begin(), virtualDrumKeyMap().end());
            std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second < b.second; });
            for (size_t i = 0; i < sorted.size(); ++i)
                labels[sorted[i].first] = "Pad " + juce::String((int) i + 1);
        }
        else
        {
            for (auto& [ch, offset] : virtualKeyboardKeyMap())
                labels[ch] = noteNameFor(juce::jlimit(0, 127, 60 + transposeSemitones + offset));
        }

        // Plain, mode-aware.
        if (isSessionView)
        {
            labels['D'] = "Prev Slot";
            labels['F'] = "Next Slot";
            labels['A'] = "Del Clip";
            labels['T'] = "Load Slot";
            labels['Z'] = "Stop Trk";
            labels['X'] = "Launch";
            labels['B'] = "Duplicate";
        }
        else
        {
            labels['D'] = "Prev Note";
            labels['F'] = "Next Note";
            labels['B'] = "Pitch Dn";
            labels['A'] = "Clear Step";
            labels['Z'] = "Oct Dn";
            labels['X'] = "Oct Up";
            labels['G'] = "Pitch Up";
            labels['Q'] = "Dup Range";
            labels['S'] = "Jump Back";
        }
        // 3/e now mean prev/next track in both views (used to be Session-
        // View-only, with Piano Roll repurposing '3' for quantize -- see
        // the Cmd block above for where quantize moved).
        labels['3'] = "Prev Trk";
        labels['E'] = "Next Trk";
        labels['C'] = "Retreat";
        labels['V'] = "Advance";
        labels['W'] = "Metronome";
        labels['R'] = recMode == 0 ? "Rec: Browse" : recMode == 1 ? "Rec: Manual" : recMode == 2 ? "Rec: Auto" : "Rec: Realtime";
        // Note Repeat rate -- see MainEditorComponent::updateNoteRepeat()'s
        // declaration. '0' is a note-map key and '3' is Prev Track, so the
        // rate keys skip straight from '2' to '4'.
        labels['1'] = "Repeat 1/4";
        labels['2'] = "Repeat 1/8";
        labels['4'] = "Repeat 1/16";
        labels['5'] = "Repeat Triplet";

        return labels;
    }
}

KeyboardOverlayComponent::KeyboardOverlayComponent(std::function<bool()> isSessionViewIn,
                                                     std::function<int()> recModeIn,
                                                     std::function<int()> transposeSemitonesIn,
                                                     std::function<std::vector<int>()> highlightedKeyCodesIn,
                                                     std::function<bool()> isDrumGridActiveIn,
                                                     std::function<int()> keyRootPitchClassIn,
                                                     std::function<bool()> keyShownIn)
    : isSessionView(std::move(isSessionViewIn)),
      recMode(std::move(recModeIn)),
      transposeSemitones(std::move(transposeSemitonesIn)),
      highlightedKeyCodes(std::move(highlightedKeyCodesIn)),
      isDrumGridActive(std::move(isDrumGridActiveIn)),
      keyRootPitchClass(std::move(keyRootPitchClassIn)),
      keyShown(std::move(keyShownIn))
{
    setSize(640, 300);
    startTimerHz(15);
}

void KeyboardOverlayComponent::timerCallback()
{
    auto mods = juce::ModifierKeys::getCurrentModifiers();
    auto session = isSessionView();
    auto rec = recMode();
    auto highlighted = highlightedKeyCodes();
    auto drumGrid = isDrumGridActive();
    auto rootPitchClass = keyRootPitchClass();
    auto shown = keyShown();

    if (mods != lastMods || session != lastSessionView || rec != lastRecMode || highlighted != lastHighlighted
        || drumGrid != lastDrumGridActive || rootPitchClass != lastKeyRootPitchClass || shown != lastKeyShown)
    {
        lastMods = mods;
        lastSessionView = session;
        lastRecMode = rec;
        lastHighlighted = std::move(highlighted);
        lastDrumGridActive = drumGrid;
        lastKeyRootPitchClass = rootPitchClass;
        lastKeyShown = shown;
        repaint();
    }
}

void KeyboardOverlayComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);

    auto mods = juce::ModifierKeys::getCurrentModifiers();
    auto plainTier = !mods.isCtrlDown() && !mods.isCommandDown() && !mods.isAltDown() && !mods.isShiftDown();
    auto drumGrid = isDrumGridActive();
    auto labels = computeKeyLabels(mods, isSessionView(), recMode(), transposeSemitones(), drumGrid);
    auto highlighted = highlightedKeyCodes(); // fresh, not the cached lastHighlighted (that's purely for timerCallback()'s change-detection)

    constexpr float keyUnit = 46.0f;
    constexpr float keySize = 42.0f;
    constexpr float topMargin = 10.0f;

    g.setFont(juce::FontOptions(10.0f));

    for (size_t rowIndex = 0; rowIndex < keyboardRows().size(); ++rowIndex)
    {
        auto& row = keyboardRows()[rowIndex];
        auto y = topMargin + (float) rowIndex * keyUnit;

        for (auto& spec : row.keys)
        {
            auto x = 10.0f + (row.xOffsetUnits + spec.xUnits) * keyUnit;
            auto bounds = juce::Rectangle<float>(x, y, keySize, keySize);

            // No more shift-remapping needed -- the drum map now stores
            // unshifted characters directly (matching this grid's own key
            // labels), since it no longer needs Ctrl+Shift to disambiguate
            // from the melodic map.
            auto it = labels.find(spec.key);
            auto hasLabel = it != labels.end();

            // A combo macOS intercepts before this app ever sees it (see
            // blockedCombos()) takes priority over everything else below --
            // greyed out and labelled with why, instead of looking exactly
            // like a plain unassigned key.
            if (auto* blocked = findBlockedCombo(mods, spec.key))
            {
                g.setColour(juce::Colours::grey.withAlpha(0.12f));
                g.fillRoundedRectangle(bounds, 4.0f);
                g.setColour(juce::Colours::grey.withAlpha(0.4f));
                g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
                g.setColour(juce::Colours::grey.withAlpha(0.7f));
                g.drawFittedText(blocked->reason, bounds.toNearestInt().reduced(2), juce::Justification::centred, 3, 0.7f);
                continue;
            }

            // Currently-highlighted keys -- a distinct bright colour
            // overriding the normal tier colour, so it's obvious at a
            // glance what you're playing/just pressed. Can be MORE THAN
            // ONE at once: a held chord on the virtual keyboard highlights
            // every one of its keys simultaneously (see
            // MainEditorComponent's highlightedKeyCodes getter, which
            // unions currently-held note/drum keys with the last plain
            // editing-command keypress -- the latter stays lit until the
            // next one, same "persists until overwritten" convention
            // ShortcutHelpBarComponent's last-action text uses).
            auto isLastPressed = std::find(highlighted.begin(), highlighted.end(), (int) spec.key) != highlighted.end();

            // Note-performance colouring only applies in the plain tier --
            // that's the only tier computeKeyLabels() actually fills in
            // note labels for (see its comment), even though the note maps
            // themselves are polled regardless of modifiers.
            auto isNoteKey = plainTier && ((drumGrid ? virtualDrumKeyMap() : virtualKeyboardKeyMap()).count(spec.key) > 0);

            // Root-of-key highlight: melodic keyboard only (drum pads
            // aren't pitched notes) -- a distinct colour on every key whose
            // note is the estimated key's root, so the root is visible at
            // a glance while playing.
            auto isRootKey = false;
            if (isNoteKey && !drumGrid && keyShown())
            {
                auto offsetIt = virtualKeyboardKeyMap().find(spec.key);
                if (offsetIt != virtualKeyboardKeyMap().end())
                {
                    auto pitch = juce::jlimit(0, 127, 60 + transposeSemitones() + offsetIt->second);
                    isRootKey = ((pitch % 12) + 12) % 12 == keyRootPitchClass();
                }
            }

            if (isLastPressed)
            {
                g.setColour(juce::Colours::yellow);
            }
            else if (isRootKey)
            {
                g.setColour(juce::Colours::hotpink);
            }
            else if (isNoteKey)
            {
                g.setColour(drumGrid ? juce::Colours::mediumpurple : juce::Colours::mediumseagreen);
            }
            else
            {
                g.setColour(hasLabel ? juce::Colours::dodgerblue.withAlpha(0.55f) : juce::Colours::grey.withAlpha(0.15f));
            }
            g.fillRoundedRectangle(bounds, 4.0f);

            g.setColour(juce::Colours::white.withAlpha(0.25f));
            g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

            if (hasLabel)
            {
                g.setColour(isLastPressed ? juce::Colours::black : juce::Colours::white);
                g.drawFittedText(it->second, bounds.toNearestInt().reduced(2), juce::Justification::centred, 3, 0.7f);
            }
            else
            {
                g.setColour(isLastPressed ? juce::Colours::black : juce::Colours::grey);
                g.drawText(juce::String::charToString((juce::juce_wchar) spec.key),
                           bounds, juce::Justification::centred);
            }
        }
    }

    // Space / Tab -- always the same meaning regardless of modifier tier or
    // mode, so drawn separately rather than routed through computeKeyLabels().
    auto bottomY = topMargin + (float) keyboardRows().size() * keyUnit + 6.0f;
    auto spaceBounds = juce::Rectangle<float>(10.0f + 1.25f * keyUnit, bottomY, keyUnit * 5.0f, keySize);
    auto tabBounds = juce::Rectangle<float>(10.0f, bottomY, keyUnit * 1.0f, keySize);
    auto enterBounds = juce::Rectangle<float>(10.0f + 6.25f * keyUnit, bottomY, keyUnit * 1.5f, keySize);

    const std::vector<std::pair<juce::Rectangle<float>, juce::String>> extraKeys = {
        { spaceBounds, "Play/Stop" }, { tabBounds, "Session" },
        { enterBounds, drumGrid ? "-> Chromatic" : "-> Drum" },
    };
    for (auto& [bounds, label] : extraKeys)
    {
        g.setColour(juce::Colours::dodgerblue.withAlpha(0.55f));
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
        g.setColour(juce::Colours::white);
        g.drawFittedText(label, bounds.toNearestInt().reduced(2), juce::Justification::centred, 2, 0.7f);
    }

    // Modifier status strip -- filled when currently held, so it's obvious
    // at a glance which tier the labels above belong to.
    auto modY = bottomY + keySize + 10.0f;
    struct ModifierBadge { const char* name; bool held; };
    std::vector<ModifierBadge> badges = {
        { "SHIFT", mods.isShiftDown() }, { "CTRL", mods.isCtrlDown() },
        { "OPTION", mods.isAltDown() }, { "CMD", mods.isCommandDown() },
    };

    auto badgeX = 10.0f;
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    for (auto& badge : badges)
    {
        auto width = 80.0f;
        auto bounds = juce::Rectangle<float>(badgeX, modY, width, 26.0f);

        if (badge.held)
        {
            g.setColour(juce::Colours::orange);
            g.fillRoundedRectangle(bounds, 4.0f);
            g.setColour(juce::Colours::black);
        }
        else
        {
            g.setColour(juce::Colours::grey.withAlpha(0.5f));
            g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
            g.setColour(juce::Colours::grey);
        }
        g.drawText(badge.name, bounds, juce::Justification::centred);
        badgeX += width + 8.0f;
    }

    g.setColour(juce::Colours::lightgrey);
    g.setFont(juce::FontOptions(12.0f));
    auto recModeSuffix = recMode() == 0 ? "" : recMode() == 1 ? "  |  REC MANUAL" : recMode() == 2 ? "  |  REC AUTO" : "  |  REC REALTIME";
    auto noteModeSuffix = juce::String("  |  NOTE ") + (drumGrid ? "DRUM" : "CHROMATIC");
    g.drawText(juce::String(isSessionView() ? "Session View" : "Piano Roll") + recModeSuffix + noteModeSuffix,
               juce::Rectangle<float>(badgeX, modY, 260.0f, 26.0f), juce::Justification::centredLeft);
}
