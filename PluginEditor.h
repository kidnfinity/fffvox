#pragma once
#include "PluginProcessor.h"

class VocalLiftEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VocalLiftEditor (VocalLiftAudioProcessor&);
    ~VocalLiftEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int bins = Analyzer::size / 2;
    using SpecArray = std::array<float, bins>;

    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        int side = 0; // 0 = nur Vocal-Modus, 1 = nur Beat-Modus, 2 = immer
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };

    void addKnob (const juce::String& id, const juce::String& name, int side);
    void timerCallback() override;
    void updateSpectrum (Analyzer&, SpecArray&);

    VocalLiftAudioProcessor& proc;

    juce::ComboBox modeBox;
    juce::ToggleButton smartBtn;
    juce::ComboBox autoBox;
    juce::TextButton resetBtn { "Analyse zuruecksetzen" };
    juce::Label statusLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> autoAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> smartAtt;
    std::vector<std::unique_ptr<Knob>> knobs;

    juce::dsp::FFT fft { Analyzer::order };
    juce::dsp::WindowingFunction<float> window { (size_t) Analyzer::size,
                                                 juce::dsp::WindowingFunction<float>::hann };
    std::array<float, Analyzer::size * 2> fftData {};
    SpecArray mainDb, scDb;
    juce::Rectangle<int> spectrumArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalLiftEditor)
};
