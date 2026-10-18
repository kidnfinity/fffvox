#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>

// Lock-free Sammler fuer die Spektrum-Anzeige (Audio-Thread schreibt, GUI liest)
struct Analyzer
{
    static constexpr int order = 11;
    static constexpr int size  = 1 << order;

    void push (float s) noexcept
    {
        fifo[(size_t) pos++] = s;
        if (pos >= size)
        {
            pos = 0;
            if (! ready.load())
            {
                block = fifo;
                ready.store (true);
            }
        }
    }

    std::array<float, size> fifo {}, block {};
    int pos = 0;
    std::atomic<bool> ready { false };
};

// 11 Analyse-/EQ-Baender (ca. 2/3 Oktave Abstand)
inline constexpr std::array<float, 11> kBandHz { 100.f, 160.f, 250.f, 400.f, 630.f, 1000.f,
                                                 1600.f, 2500.f, 4000.f, 6300.f, 10000.f };

// Misst das Langzeit-Spektrum (Stimme bzw. Beat) in 11 Baendern
struct BandAnalyzer
{
    static constexpr int order = 11, size = 1 << order, numBands = 11;

    BandAnalyzer() { reset(); }

    void reset()
    {
        fast.fill (-100.f);
        slow.fill (-100.f);
        count = 0;
        pos = 0;
        frameDb = -100.f;
    }

    bool push (float s) noexcept
    {
        frame[(size_t) pos++] = s;
        if (pos < size) return false;
        pos = 0;
        return true;
    }

    // learn = true: Langzeitmittel (slow) wird aktualisiert, solange Signal ueber dem Gate liegt
    void analyse (double sr, float gateDb, bool learn)
    {
        float sum = 0.f;
        for (auto v : frame) sum += v * v;
        frameDb = juce::Decibels::gainToDecibels (std::sqrt (sum / (float) size), -100.f);

        if (frameDb <= gateDb)
        {
            for (auto& f : fast) f = juce::jmax (-100.f, f - 3.f);
            return;
        }

        std::copy (frame.begin(), frame.end(), data.begin());
        std::fill (data.begin() + size, data.end(), 0.f);
        window.multiplyWithWindowingTable (data.data(), (size_t) size);
        fft.performFrequencyOnlyForwardTransform (data.data());

        const float binHz = (float) sr / (float) size;
        for (int b = 0; b < numBands; ++b)
        {
            const float lo = kBandHz[(size_t) b] / 1.414f, hi = kBandHz[(size_t) b] * 1.414f;
            const int i0 = juce::jmax (1, (int) std::ceil (lo / binHz));
            const int i1 = juce::jmax (i0, juce::jmin (size / 2 - 1, (int) std::floor (hi / binHz)));
            float e = 0.f;
            for (int i = i0; i <= i1; ++i)
            {
                const float m = data[(size_t) i] / ((float) size * 0.25f);
                e += m * m;
            }
            e = e / (float) (i1 - i0 + 1) * ((hi - lo) / binHz); // Band-Pegel, unabhaengig von der Bin-Anzahl
            const float db = 10.f * std::log10 (e + 1.0e-12f);

            auto& fs = fast[(size_t) b];
            fs = fs <= -99.f ? db : 0.6f * fs + 0.4f * db;

            if (learn)
            {
                auto& sl = slow[(size_t) b];
                const float alpha = juce::jmax (0.03f, 1.f / (float) (count + 1));
                sl = sl <= -99.f ? db : sl + alpha * (db - sl);
            }
        }
        if (learn) ++count;
    }

    std::array<float, size> frame {};
    std::array<float, size * 2> data {};
    std::array<float, numBands> fast {}, slow {};
    int pos = 0, count = 0;
    float frameDb = -100.f;
    juce::dsp::FFT fft { order };
    juce::dsp::WindowingFunction<float> window { (size_t) size, juce::dsp::WindowingFunction<float>::hann };
};

class VocalLiftAudioProcessor : public juce::AudioProcessor
{
public:
    VocalLiftAudioProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "FINIFICKFACKVOX"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    double getSR() const { return sampleRate; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    // Fuer die GUI
    Analyzer mainAnalyzer, scAnalyzer;
    std::atomic<float> curPresenceDb { 0.f }, curCarveDb { 0.f }, curDeessDb { 0.f },
                       compGrDb { 0.f }, maskAmount { 0.f }, curDepthShelfDb { 0.f };
    std::array<std::atomic<float>, 11> autoDisp;      // aktuelle Auto-EQ-Gains pro Band (fuer die GUI)
    std::atomic<int> vocalFrames { 0 }, beatFrames { 0 };
    std::atomic<bool> resetRequest { false };

private:
    using Duplicator = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                                      juce::dsp::IIR::Coefficients<float>>;
    Duplicator hpf, mud, presence, air, deess, carve, carveLow, detector, sibDetector, depthShelf;
    juce::Reverb reverb;

    // Auto-EQ
    BandAnalyzer mainAn, scAn;
    std::array<Duplicator, 11> autoBands;
    std::array<float, 11> autoTarget {}, autoS {}, autoApplied {};
    float vocalAct = 0.f;
    void computeAutoTargets (bool beatMode, bool hasSC);

    juce::AudioBuffer<float> scratch, sibScratch;
    double sampleRate = 44100.0;
    float scEnv = 0.f, sibEnv = 0.f, compEnv = 0.f;
    float presS = 0.f, carveS = 0.f, deS = 0.f, depthS = 0.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalLiftAudioProcessor)
};
