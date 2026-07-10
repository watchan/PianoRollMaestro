#pragma once
#include <JuceHeader.h>

class MainEditorComponent : public juce::Component
{
public:
    MainEditorComponent();

    void paint(juce::Graphics& g) override;
    void resized() override;
};
