#pragma once
#include <JuceHeader.h>

// Wraps a hosted plugin's own native editor (patch browser, etc.) in a
// window. Deliberately mouse-driven -- third-party plugin GUIs are out of
// scope for the app's keyboard-only editing philosophy.
// Note: juce::Component (an ancestor via DocumentWindow) already IS-A
// juce::MouseListener, so this doesn't need (and mustn't add) a second,
// separate MouseListener base -- that would make the conversion from
// PluginEditorWindow* to MouseListener* ambiguous. mouseUp() below simply
// overrides the one Component already provides.
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    // onInteractionEnd: called once each time the mouse is released
    // somewhere inside this window's content -- see mouseUp()'s comment.
    // Kept as defense-in-depth alongside getDesktopWindowStyleFlags()'s
    // windowIgnoresKeyPresses override below, which is the real fix.
    PluginEditorWindow(juce::AudioProcessor& processor, const juce::String& name, std::function<void()> onInteractionEndIn = nullptr)
        // addToDesktop=false: the peer must NOT be created here, inside the
        // base class's own constructor -- a virtual call to
        // getDesktopWindowStyleFlags() made from there runs with only
        // DocumentWindow's own vtable (ordinary C++ "virtual dispatch during
        // construction" rules), so this class's override below would never
        // actually be seen. addToDesktop() is called explicitly further
        // down instead, once this object is fully constructed.
        : DocumentWindow(name, juce::Colours::darkgrey, DocumentWindow::closeButton, false),
          onInteractionEnd(std::move(onInteractionEndIn))
    {
        setUsingNativeTitleBar(true);
        addToDesktop();

        if (processor.hasEditor())
        {
            if (auto* editor = processor.createEditorIfNeeded())
            {
                setContentOwned(editor, true);
                setResizable(editor->isResizable(), false);
            }
        }

        centreWithSize(juce::jmax(getWidth(), 200), juce::jmax(getHeight(), 100));
        setVisible(true);

        // wantsEventsForAllNestedChildComponents=true: catches a mouseUp
        // anywhere in the plugin's own editor tree, not just directly on
        // this window's own content component.
        if (auto* content = getContentComponent())
            content->addMouseListener(this, true);
    }

    ~PluginEditorWindow() override
    {
        if (auto* content = getContentComponent())
            content->removeMouseListener(this);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

    // The actual fix: ComponentPeer::windowIgnoresKeyPresses is JUCE's own
    // documented mechanism for exactly this scenario -- "can be used for
    // things like plugin windows, to stop them interfering with the host's
    // shortcut keys." On macOS this maps directly to the native window's
    // canBecomeKeyWindow() returning NO (see NSViewComponentPeer's
    // canBecomeKeyWindow()), meaning this window can never become the real
    // OS key window at all, ever -- mouse clicks/drags still land on it
    // normally (turning a knob doesn't require key-window status on macOS,
    // the standard "non-activating panel" pattern many apps use for
    // inspector/tool windows), but every keyDown always keeps going to
    // whichever window WAS already key (the main editor), so PC-keyboard
    // shortcuts/notes are never interrupted and macOS never gets a chance
    // to beep at an unhandled key landing on this window in the first
    // place. Two earlier, weaker attempts both lived in
    // MainEditorComponent instead of here: reclaiming focus on a JUCE host-
    // automation gesture-end callback (only fires for plugins that
    // cooperate with that API, most bespoke GUIs don't), then a ~30Hz poll that
    // force-reclaimed focus every tick -- which made things WORSE, not
    // better (PC-keyboard notes stopped sounding entirely while the plugin
    // window was active), almost certainly because
    // repeatedly forcing OS window-activation back and forth many times a
    // second raced whatever key-state tracking
    // juce::KeyPress::isKeyCurrentlyDown() relies on. Preventing the
    // window from ever taking key status in the first place sidesteps
    // both failure modes entirely, rather than reacting after the fact.
    int getDesktopWindowStyleFlags() const override
    {
        return DocumentWindow::getDesktopWindowStyleFlags() | juce::ComponentPeer::windowIgnoresKeyPresses;
    }

private:
    // Now purely a defense-in-depth fallback (see
    // getDesktopWindowStyleFlags() above for the actual fix) for whatever
    // platform this window ever DOES somehow end up with real keyboard
    // focus on. Fires once per actual mouse release inside the plugin's
    // own editor tree -- covers any editor actually built as a
    // juce::Component tree (true of most JUCE-based plugins). A plugin
    // whose editor is a fully native/opaque view that swallows mouse
    // events below JUCE's own dispatch is the one case this can't see,
    // same caveat MainEditorComponent::audioProcessorParameterChangeGestureEnd()
    // already has for host-automation-uncooperative plugins.
    void mouseUp(const juce::MouseEvent&) override
    {
        if (onInteractionEnd)
            onInteractionEnd();
    }

    std::function<void()> onInteractionEnd;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorWindow)
};
