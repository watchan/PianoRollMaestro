#pragma once
#include <JuceHeader.h>
#include "PluginHost.h"

// Setup panel for assigning an instrument plugin to one track. Keyboard-
// first: an incremental search box filters the plugin list by name or
// manufacturer as you type, Up/Down move the highlight, Enter loads it.
// (Scan/Show Editor/Remove stay mouse buttons -- they're rare one-off
// actions, not part of the search-and-load loop.)
class InstrumentPanelComponent : public juce::Component,
                                  private juce::ListBoxModel
{
public:
    InstrumentPanelComponent(PluginHost& pluginHostIn,
                              juce::String trackNameIn,
                              juce::String currentInstrumentNameIn,
                              std::function<void(const juce::PluginDescription&)> onLoadIn,
                              std::function<void()> onShowEditorIn,
                              std::function<void()> onRemoveIn,
                              std::function<void()> onRequestCloseIn);

    void resized() override;
    void paint(juce::Graphics& g) override;
    void visibilityChanged() override;

    // Called explicitly by the owning window once it (and not just this
    // component) is actually on screen -- visibilityChanged() alone fires
    // too early (when setContentOwned() first marks this visible, before
    // the parent DocumentWindow itself is shown) for grabKeyboardFocus() to
    // reliably stick.
    void focusSearchBox() { searchEditor.grabKeyboardFocus(); }

    int getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

private:
    // Intercepts navigation before it becomes typed text; every other key
    // falls through to normal text editing. Up/Down arrow keys move the
    // highlight -- a deliberate, explicit exception to the app's usual
    // hands-stay-on-home-position rule, made specifically for this
    // candidate-list picker (a plain search/select UI where arrow keys are
    // the conventional, expected control) -- and Cmd+G/Cmd+B do the same,
    // matching this app's usual prev/next mnemonic everywhere else
    // (Cmd rather than Ctrl to match every other modifier shortcut in the
    // app). Previously Cmd+3/Cmd+E (the 3/e track-switch mnemonic) -- moved
    // to G/B since nothing else in this isolated component needs 3/e for
    // anything, and G/B is the more standard pairing elsewhere in the app.
    class SearchEditor : public juce::TextEditor
    {
    public:
        std::function<void(int)> onMoveHighlight;
        std::function<void()> onEnterKey;
        std::function<void()> onEscapeKey;

        bool keyPressed(const juce::KeyPress& key) override;
    };

    void scanClicked();
    void updateFilter();
    void moveHighlight(int delta);
    void loadHighlighted();

    PluginHost& pluginHost;
    juce::String trackName;
    juce::String currentInstrumentName;
    std::function<void(const juce::PluginDescription&)> onLoad;
    std::function<void()> onShowEditor;
    std::function<void()> onRemove;
    std::function<void()> onRequestClose;

    juce::Label headerLabel;
    SearchEditor searchEditor;
    juce::TextButton scanButton{ "Scan" };
    juce::TextButton showEditorButton{ "Show Editor" };
    juce::TextButton removeButton{ "Remove" };
    juce::ListBox listBox{ "instruments", this };

    juce::Array<juce::PluginDescription> filteredInstruments;
    int highlightedRow = 0;
};
