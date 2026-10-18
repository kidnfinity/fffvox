#include "PluginProcessor.h"
#include "PluginEditor.h"

using Coeff = juce::dsp::IIR::Coefficients<float>;

// Biquad-Koeffizienten (RBJ, identisch zu JUCE) ohne Speicher-Allokation im Audio-Thread.
// Geschrieben wird IN das geteilte State-Objekt des ProcessorDuplicators, so sehen alle Kanaele die Aenderung.
namespace Bq
{
    using Dup = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    constexpr double twoPi = 6.283185307179586;

    inline void write (Dup& d, double b0, double b1, double b2, double a0, double a1, double a2)
    {
        auto* r = d.state->getRawCoefficients();
        const double inv = 1.0 / a0;
        r[0] = (float) (b0 * inv);
        r[1] = (float) (b1 * inv);
        r[2] = (float) (b2 * inv);
        r[3] = (float) (a1 * inv);
        r[4] = (float) (a2 * inv);
    }

    inline void peak (Dup& d, double sr, double f, double q, double gain)
    {
        const double A = std::sqrt (std::max (1.0e-6, gain));
        const double w = twoPi * std::max (f, 2.0) / sr;
        const double alpha = std::sin (w) / (q * 2.0), c2 = -2.0 * std::cos (w);
        write (d, 1.0 + alpha * A, c2, 1.0 - alpha * A, 1.0 + alpha / A, c2, 1.0 - alpha / A);
    }

    inline void highShelf (Dup& d, double sr, double f, double q, double gain)
    {
        const double A = std::sqrt (std::max (1.0e-6, gain));
        const double am1 = A - 1.0, ap1 = A + 1.0;
        const double w = twoPi * std::max (f, 2.0) / sr;
        const double co = std::cos (w), beta = std::sin (w) * std::sqrt (A) / q;
        const double am1co = am1 * co;
        write (d, A * (ap1 + am1co + beta), A * -2.0 * (am1 + ap1 * co), A * (ap1 + am1co - beta),
                  ap1 - am1co + beta, 2.0 * (am1 - ap1 * co), ap1 - am1co - beta);
    }

    inline void highPass (Dup& d, double sr, double f, double q)
    {
        const double n = 1.0 / std::tan (3.141592653589793 * f / sr), n2 = n * n, invQ = 1.0 / q;
        const double c1 = 1.0 / (1.0 + invQ * n + n2);
        write (d, c1, c1 * -2.0, c1, 1.0, c1 * 2.0 * (1.0 - n2), c1 * (1.0 - invQ * n + n2));
    }

    inline void bandPass (Dup& d, double sr, double f, double q)
    {
        const double w = twoPi * f / sr, alpha = 1.0 / (2.0 * q), c2 = -2.0 * std::cos (w);
        write (d, alpha, 0.0, -alpha, 1.0 + alpha, c2, 1.0 - alpha);
    }
}

VocalLiftAudioProcessor::VocalLiftAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (auto& a : autoDisp) a.store (0.f);
}

juce::AudioProcessorValueTreeState::ParameterLayout VocalLiftAudioProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto F = [&] (const juce::String& id, const juce::String& name, float lo, float hi, float def, float skew = 1.f)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (lo, hi, 0.1f, skew), def));
    };

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "mode", 1 }, "Mode", juce::StringArray { "Vocal", "Beat" }, 0));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "smart", 1 }, "Smart", true));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "autoMode", 1 }, "Auto EQ", juce::StringArray { "Off", "Live", "Freeze" }, 1));
    F ("autoAmount", "Auto Amount", 0.f, 100.f, 60.f);

    // Vocal-Modus
    F ("hpf",       "Low Cut",     20.f,  300.f, 90.f, 0.5f);
    F ("mud",       "Mud Cut",     -9.f,  0.f,   -1.f);
    F ("presence",  "Presence",    0.f,   8.f,   2.f);
    F ("air",       "Air",         0.f,   6.f,   2.f);
    F ("deessThr",  "De-Ess Thr",  -50.f, -15.f, -32.f);
    F ("deessMax",  "De-Ess Max",  0.f,   12.f,  5.f);
    F ("compThr",   "Comp Thr",    -40.f, 0.f,   -18.f);
    F ("compRatio", "Comp Ratio",  1.f,   8.f,   3.f);

    // Beat-Modus
    F ("carve",     "Vocal Pocket", 0.f,   9.f,   4.f);
    F ("carveFreq", "Pocket Freq",  1000.f, 5000.f, 2500.f, 0.5f);

    // Tiefe: -100 = ganz vorne, +100 = ganz hinten (beide Modi)
    F ("depth", "Front <-> Back", -100.f, 100.f, 0.f);

    return layout;
}

void VocalLiftAudioProcessor::prepareToPlay (double sr, int samplesPerBlock)
{
    sampleRate = sr;
    juce::dsp::ProcessSpec spec { sr, (juce::uint32) samplesPerBlock, 2 };
    for (auto* p : { &hpf, &mud, &presence, &air, &deess, &carve, &carveLow, &detector, &sibDetector, &depthShelf })
    {
        *p->state = *Coeff::makeAllPass (sr, 1000.f);
        p->prepare (spec);
        p->reset();
    }
    for (auto& b : autoBands) { *b.state = *Coeff::makeAllPass (sr, 1000.f); b.prepare (spec); b.reset(); }
    setLatencySamples (0); // alle Filter sind IIR ohne Lookahead, die FFT dient nur der Analyse
    autoS.fill (0.f);
    autoApplied.fill (0.f);
    scratch.setSize (2, samplesPerBlock);
    sibScratch.setSize (2, samplesPerBlock);
    scEnv = sibEnv = compEnv = 0.f;
    presS = carveS = deS = depthS = 0.f;
    reverb.setSampleRate (sr);
    reverb.reset();
}

bool VocalLiftAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet();
    if (in != l.getMainOutputChannelSet()) return false;
    if (in != juce::AudioChannelSet::stereo() && in != juce::AudioChannelSet::mono()) return false;
    const auto sc = l.getChannelSet (true, 1);
    return sc.isDisabled() || sc.size() <= 2;
}

void VocalLiftAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    auto main = getBusBuffer (buffer, true, 0);
    auto sc   = getBusBuffer (buffer, true, 1);
    const bool hasSC = getBus (true, 1) != nullptr && getBus (true, 1)->isEnabled() && sc.getNumChannels() > 0;
    const int chs = main.getNumChannels();
    const float sr = (float) sampleRate;

    auto P = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    const bool beatMode = P ("mode") > 0.5f;
    const bool smart    = P ("smart") > 0.5f;

    // ---------- 0) Auto-EQ Analyse (Stimme + Beat, vor jeder Bearbeitung) ----------
    const int autoMode = (int) P ("autoMode"); // 0 = Off, 1 = Live, 2 = Freeze
    if (resetRequest.exchange (false)) { mainAn.reset(); scAn.reset(); }
    if (autoMode > 0)
    {
        for (int i = 0; i < n; ++i)
        {
            float m = 0.f, s = 0.f;
            for (int c = 0; c < chs; ++c) m += main.getSample (c, i);
            m /= (float) juce::jmax (1, chs);
            if (hasSC)
            {
                for (int c = 0; c < sc.getNumChannels(); ++c) s += sc.getSample (c, i);
                s /= (float) sc.getNumChannels();
            }
            const bool frameDone = mainAn.push (m);
            scAn.push (s);
            if (frameDone)
            {
                const bool learn = autoMode == 1;
                mainAn.analyse (sampleRate, beatMode ? -60.f : -50.f, learn);
                scAn.analyse   (sampleRate, beatMode ? -50.f : -60.f, learn);
                computeAutoTargets (beatMode, hasSC);
            }
        }
    }

    // ---------- 1) Sidechain-Detektor: wie viel Energie liegt im kritischen Band? ----------
    float mask = hasSC ? 0.f : 0.6f;
    if (hasSC)
    {
        scratch.setSize (sc.getNumChannels(), n, false, false, true);
        for (int c = 0; c < sc.getNumChannels(); ++c)
            scratch.copyFrom (c, 0, sc, c, 0, n);

        Bq::bandPass (detector, sr, beatMode ? P ("carveFreq") : 3200.f, 1.0f);
        juce::dsp::AudioBlock<float> sb (scratch);
        detector.process (juce::dsp::ProcessContextReplacing<float> (sb));

        const float att = std::exp (-1.f / (0.005f * sr));
        const float rel = std::exp (-1.f / (0.15f * sr));
        for (int i = 0; i < n; ++i)
        {
            float x = 0.f;
            for (int c = 0; c < scratch.getNumChannels(); ++c) x += std::abs (scratch.getSample (c, i));
            x /= (float) scratch.getNumChannels();
            const float k = x > scEnv ? att : rel;
            scEnv = k * scEnv + (1.f - k) * x;
        }
        const float scDb = juce::Decibels::gainToDecibels (scEnv, -100.f);
        mask = juce::jlimit (0.f, 1.f, (scDb + 42.f) / 20.f);
    }

    // ---------- 2) Zielwerte + Glaettung ----------
    float tPres = 0.f, tCarve = 0.f;
    if (! beatMode) tPres  = P ("presence") * (smart ? 0.35f + 0.65f * mask : 1.f);
    else            tCarve = P ("carve")    * (smart ? mask : 1.f);

    const float a = 1.f - std::exp (-(float) n / (0.05f * sr));
    presS  += (tPres  - presS)  * a;
    carveS += (tCarve - carveS) * a;

    juce::dsp::AudioBlock<float> mb (main);
    juce::dsp::ProcessContextReplacing<float> ctx (mb);
    float maxGr = 0.f;

    // Auto-EQ: 11 Peak-Filter, Gains werden sanft zum Ziel geglaettet
    auto applyAuto = [&]
    {
        const float sm = 1.f - std::exp (-(float) n / (0.08f * sr));
        for (size_t b = 0; b < 11; ++b)
        {
            const float tgt = autoMode > 0 ? autoTarget[b] : 0.f;
            autoS[b] += (tgt - autoS[b]) * sm;
            autoDisp[b].store (autoS[b]);
            if (std::abs (autoS[b]) > 0.05f)
            {
                if (std::abs (autoS[b] - autoApplied[b]) > 0.02f)
                {
                    Bq::peak (autoBands[b], sr, kBandHz[b], 1.4f,
                                                                juce::Decibels::decibelsToGain (autoS[b]));
                    autoApplied[b] = autoS[b];
                }
                autoBands[b].process (ctx);
            }
        }
    };

    if (! beatMode)
    {
        // ---------- VOCAL-KETTE ----------
        Bq::highPass (hpf, sr, P ("hpf"), 0.707f);
        hpf.process (ctx);
        applyAuto();

        Bq::peak (mud, sr, 280.f, 1.0f, juce::Decibels::decibelsToGain (P ("mud")));
        mud.process (ctx);

        Bq::peak (presence, sr, 3200.f, 0.9f, juce::Decibels::decibelsToGain (presS));
        presence.process (ctx);

        Bq::highShelf (air, sr, 10000.f, 0.7f, juce::Decibels::decibelsToGain (P ("air")));
        air.process (ctx);

        // De-Esser (Detektor 7 kHz -> dynamischer Peak-Cut)
        sibScratch.setSize (chs, n, false, false, true);
        for (int c = 0; c < chs; ++c) sibScratch.copyFrom (c, 0, main, c, 0, n);
        Bq::bandPass (sibDetector, sr, 7000.f, 1.2f);
        juce::dsp::AudioBlock<float> sbk (sibScratch);
        sibDetector.process (juce::dsp::ProcessContextReplacing<float> (sbk));

        const float sa = std::exp (-1.f / (0.001f * sr)), sbR = std::exp (-1.f / (0.05f * sr));
        for (int i = 0; i < n; ++i)
        {
            float x = 0.f;
            for (int c = 0; c < chs; ++c) x += std::abs (sibScratch.getSample (c, i));
            x /= (float) chs;
            const float k = x > sibEnv ? sa : sbR;
            sibEnv = k * sibEnv + (1.f - k) * x;
        }
        const float sibDb  = juce::Decibels::gainToDecibels (sibEnv, -100.f);
        const float excess = juce::jlimit (0.f, 1.f, (sibDb - P ("deessThr")) / 10.f);
        deS += (-P ("deessMax") * excess - deS) * (1.f - std::exp (-(float) n / (0.005f * sr)));
        Bq::peak (deess, sr, 7000.f, 1.5f, juce::Decibels::decibelsToGain (deS));
        deess.process (ctx);

        // Kompressor (gleichmaessige Lautstaerke = bessere Verstaendlichkeit)
        const float thr = P ("compThr"), ratio = P ("compRatio");
        const float ca = std::exp (-1.f / (0.008f * sr)), cr = std::exp (-1.f / (0.09f * sr));
        const float makeup = -thr * (1.f - 1.f / ratio) * 0.4f;
        for (int i = 0; i < n; ++i)
        {
            float lvl = 0.f;
            for (int c = 0; c < chs; ++c) lvl = juce::jmax (lvl, std::abs (main.getSample (c, i)));
            const float k = lvl > compEnv ? ca : cr;
            compEnv = k * compEnv + (1.f - k) * lvl;
            const float over = juce::Decibels::gainToDecibels (compEnv, -100.f) - thr;
            const float gr = over > 0.f ? over * (1.f - 1.f / ratio) : 0.f;
            maxGr = juce::jmax (maxGr, gr);
            const float g = juce::Decibels::decibelsToGain (makeup - gr);
            for (int c = 0; c < chs; ++c) main.setSample (c, i, main.getSample (c, i) * g);
        }
    }
    else
    {
        // ---------- BEAT-KETTE: macht Platz fuer die Stimme, wenn sie singt ----------
        if (autoMode == 0) // ohne Auto-EQ: einfacher fester Pocket-Cut
        {
            Bq::peak (carve, sr, P ("carveFreq"), 0.9f, juce::Decibels::decibelsToGain (-carveS));
            Bq::peak (carveLow, sr, 300.f,           0.8f, juce::Decibels::decibelsToGain (-carveS * 0.4f));
            carve.process (ctx);
            carveLow.process (ctx);
        }
        applyAuto();
        deS = 0.f;
    }

    // ---------- DEPTH: Vorne <-> Hinten ----------
    // Hinten = dunkler, leiser, mehr Raum.  Vorne = heller, lauter, trocken.
    const float tDepth = P ("depth") / 100.f;
    depthS += (tDepth - depthS) * a;
    const float back  = juce::jmax (0.f,  depthS);
    const float front = juce::jmax (0.f, -depthS);
    const float shelfDb = -9.f * back + 3.f * front;
    const float levelDb = -4.f * back + 2.f * front;

    Bq::highShelf (depthShelf, sr, 5000.f, 0.7f, juce::Decibels::decibelsToGain (shelfDb));
    depthShelf.process (ctx);

    if (back > 0.001f)
    {
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.45f + 0.25f * back;
        rp.damping  = 0.6f;
        rp.wetLevel = 0.35f * back;
        rp.dryLevel = 1.f - 0.3f * back;
        rp.width    = 1.f;
        rp.freezeMode = 0.f;
        reverb.setParameters (rp);
        if (chs >= 2) reverb.processStereo (main.getWritePointer (0), main.getWritePointer (1), n);
        else          reverb.processMono (main.getWritePointer (0), n);
    }
    main.applyGain (juce::Decibels::decibelsToGain (levelDb));
    curDepthShelfDb = shelfDb;

    curPresenceDb = presS;
    curCarveDb    = carveS;
    curDeessDb    = deS;
    compGrDb      = maxGr;
    maskAmount    = mask;

    // ---------- Analyzer fuettern ----------
    for (int i = 0; i < n; ++i)
    {
        float m = 0.f, s = 0.f;
        for (int c = 0; c < chs; ++c) m += main.getSample (c, i);
        mainAnalyzer.push (m / (float) juce::jmax (1, chs));
        if (hasSC)
        {
            for (int c = 0; c < sc.getNumChannels(); ++c) s += sc.getSample (c, i);
            s /= (float) sc.getNumChannels();
        }
        scAnalyzer.push (s);
    }
}

void VocalLiftAudioProcessor::computeAutoTargets (bool beatMode, bool hasSC)
{
    // Zielprofil einer klaren Stimme, Bandpegel relativ zu ~1 kHz (dB)
    static constexpr std::array<float, 11> T { -10.f, -6.f, -2.5f, -1.5f, -1.f, 0.f, -2.f, -4.f, -7.f, -11.f, -17.f };
    // Wie wichtig ist das Band fuer Verstaendlichkeit (Konsonanten/Formanten)?
    static constexpr std::array<float, 11> W { 0.f, 0.f, 0.1f, 0.3f, 0.6f, 0.8f, 1.f, 1.f, 1.f, 0.6f, 0.2f };

    const auto& V = beatMode ? scAn : mainAn;   // Stimme
    const auto& B = beatMode ? mainAn : scAn;   // Beat
    vocalFrames = V.count;
    beatFrames  = B.count;

    const float amount   = apvts.getRawParameterValue ("autoAmount")->load() / 100.f;
    const float carveMax = apvts.getRawParameterValue ("carve")->load();
    vocalAct = V.frameDb > -50.f ? 1.f : juce::jmax (0.f, vocalAct - 0.12f);

    for (size_t b = 0; b < 11; ++b)
    {
        float g = 0.f;
        if (! beatMode)
        {
            // Stimme: Abweichung vom Zielprofil korrigieren + je nach Beat Verstaendlichkeits-Baender anheben,
            // unwichtige Baender absenken, wo der Beat die Stimme ueberdeckt
            if (V.count >= 20)
            {
                const float ref  = (V.slow[4] + V.slow[5] + V.slow[6]) / 3.f;
                const float corr = juce::jlimit (-6.f, 4.f, 0.6f * (T[b] - (V.slow[b] - ref)));
                float m = 0.f;
                if (hasSC && B.count >= 20)
                    m = juce::jlimit (0.f, 1.f, (B.slow[b] - V.slow[b] + 6.f) / 12.f);
                g = juce::jlimit (-9.f, 6.f, corr + W[b] * m * 4.f - (1.f - W[b]) * m * 3.f) * amount;
            }
        }
        else if (hasSC)
        {
            // Beat: dort absenken, wo die Stimme gerade singt und der Beat dominiert
            const float m = juce::jlimit (0.f, 1.f, (B.fast[b] - V.fast[b] + 6.f) / 18.f);
            g = -W[b] * m * carveMax * vocalAct;
        }
        autoTarget[b] = g;
    }
}

juce::AudioProcessorEditor* VocalLiftAudioProcessor::createEditor() { return new VocalLiftEditor (*this); }

void VocalLiftAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, dest);
}

void VocalLiftAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VocalLiftAudioProcessor(); }
