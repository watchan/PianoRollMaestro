#include "MainEditorComponent.h"

MainEditorComponent::MainEditorComponent()
{
    setSize(900, 600);
}

void MainEditorComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey);
}

void MainEditorComponent::resized()
{
}
