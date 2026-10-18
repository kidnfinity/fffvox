#include "PluginEditor.h"

using namespace juce;
using Coeff = dsp::IIR::Coefficients<float>;

namespace
{
const Colour cVocal    (0xff35d0ff);
const Colour cBeat     (0xffff4fd8);
const Colour cBg       (0xff14161c);
const Colour cPanel    (0xff1d2029);
const Colour cConflict (0xffff3b3b);
const Colour cCurve    (0xffffd84a);
}

VocalLiftEditor::VocalLiftEditor (VocalLiftAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    mainDb.fill (-100.f);
    scDb.fill (-100.f);

    modeBox.addItem ("Vocal-Modus (Insert auf Gesang, Beat in Sidechain)", 1);
    modeBox.addItem ("Beat-Modus (Insert auf Beat, Gesang in Sidechain)", 2);
    addAndMakeVisible (modeBox);
    modeAtt = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "mode", modeBox);

    smartBtn.setButtonText ("Smart (reagiert auf Sidechain)");
    addAndMakeVisible (smartBtn);
    smartAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "smart", smartBtn);

    autoBox.addItem ("Auto-EQ: Aus", 1);
    autoBox.addItem ("Auto-EQ: Live (lernt mit)", 2);
    autoBox.addItem ("Auto-EQ: Freeze (Lernstand halten)", 3);
    addAndMakeVisible (autoBox);
    autoAtt = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "autoMode", autoBox);

    addAndMakeVisible (resetBtn);
    resetBtn.onClick = [this] { proc.resetRequest = true; };

    statusLabel.setFont (FontOptions (12.f));
    statusLabel.setColour (Label::textColourId, Colours::white.withAlpha (0.8f));
    addAndMakeVisible (statusLabel);

    addKnob ("autoAmount", "Auto Amount", 0);
    addKnob ("hpf",       "Low Cut",     0);
    addKnob ("mud",       "Mud Cut",     0);
    addKnob ("presence",  "Presence",    0);
    addKnob ("air",       "Air",         0);
    addKnob ("deessThr",  "De-Ess Thr",  0);
    addKnob ("deessMax",  "De-Ess Max",  0);
    addKnob ("compThr",   "Comp Thr",    0);
    addKnob ("compRatio", "Comp Ratio",  0);
    addKnob ("carve",     "Vocal Pocket", 1);
    addKnob ("carveFreq", "Pocket Freq",  1);
    addKnob ("depth",     "FRONT <-> BACK", 2);

    setSize (1000, 620);
    startTimerHz (30);
}

VocalLiftEditor::~VocalLiftEditor() { stopTimer(); }

void VocalLiftEditor::addKnob (const String& id, const String& name, int side)
{
    auto k = std::make_unique<Knob>();
    k->side = side;
    k->slider.setTextBoxStyle (Slider::TextBoxBelow, false, 70, 18);
    k->label.setText (name, dontSendNotification);
    k->label.setJustificationType (Justification::centred);
    k->label.setFont (FontOptions (12.f));
    k->att = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k->slider);
    addAndMakeVisible (k->slider);
    addAndMakeVisible (k->label);
    knobs.push_back (std::move (k));
}

void VocalLiftEditor::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (40).reduced (10, 7);
    modeBox.setBounds (top.removeFromLeft (470));
    top.removeFromLeft (10);
    smartBtn.setBounds (top.removeFromLeft (260));

    auto row2 = r.removeFromTop (34).reduced (10, 4);
    autoBox.setBounds (row2.removeFromLeft (300));
    row2.removeFromLeft (10);
    resetBtn.setBounds (row2.removeFromLeft (170));
    row2.removeFromLeft (10);
    statusLabel.setBounds (row2);

    auto bottom = r.removeFromBottom (170);
    spectrumArea = r.reduced (10, 4);

    const int w = bottom.getWidth() / (int) knobs.size();
    for (auto& k : knobs)
    {
        auto cell = bottom.removeFromLeft (w);
        k->label.setBounds (cell.removeFromTop (20));
        k->slider.setBounds (cell.reduced (4, 0));
    }
}

void VocalLiftEditor::updateSpectrum (Analyzer& a, SpecArray& outDb)
{
    if (! a.ready.load()) return;

    std::copy (a.block.begin(), a.block.end(), fftData.begin());
    std::fill (fftData.begin() + Analyzer::size, fftData.end(), 0.f);
    a.ready.store (false);

    window.multiplyWithWindowingTable (fftData.data(), (size_t) Analyzer::size);
    fft.performFrequencyOnlyForwardTransform (fftData.data());

    for (int i = 0; i < bins; ++i)
    {
        const float db = Decibels::gainToDecibels (fftData[(size_t) i] / (Analyzer::size * 0.25f), -100.f);
        auto& o = outDb[(size_t) i];
        o = db > o ? 0.5f * o + 0.5f * db : 0.85f * o + 0.15f * db;
    }
}

void VocalLiftEditor::timerCallback()
{
    updateSpectrum (proc.mainAnalyzer, mainDb);
    updateSpectrum (proc.scAnalyzer, scDb);

    {
        const double secPerFrame = (double) BandAnalyzer::size / jmax (1.0, proc.getSR());
        int best = 0; float bg = 0.f;
        for (int b = 0; b < BandAnalyzer::numBands; ++b)
        {
            const float gv = proc.autoDisp[(size_t) b].load();
            if (std::abs (gv) > std::abs (bg)) { bg = gv; best = b; }
        }
        String t = "Stimme: " + String (proc.vocalFrames.load() * secPerFrame, 1) + " s analysiert  |  Beat: "
                   + String (proc.beatFrames.load() * secPerFrame, 1) + " s";
        if (std::abs (bg) > 0.5f)
        {
            const float f = kBandHz[(size_t) best];
            t << "  |  Groesste Korrektur: " << (bg > 0 ? "+" : "") << String (bg, 1) << " dB @ "
              << (f >= 1000.f ? String (f / 1000.f, 1) + "k" : String ((int) f) + " Hz");
        }
        statusLabel.setText (t, dontSendNotification);
    }

    const bool beat = modeBox.getSelectedId() == 2;
    for (auto& k : knobs)
    {
        const bool en = k->side == 2 ? true : (k->side == 0 ? ! beat : beat);
        k->slider.setEnabled (en);
        k->label.setEnabled (en);
        k->slider.setAlpha (en ? 1.f : 0.35f);
        k->label.setAlpha (en ? 1.f : 0.35f);
    }
    repaint (spectrumArea);
}

void VocalLiftEditor::paint (Graphics& g)
{
    g.fillAll (cBg);
    g.setColour (cCurve);
    g.setFont (FontOptions (16.f, Font::bold));
    g.drawText ("FINIFICKFACKVOX", Rectangle<int> (getWidth() - 260, 8, 250, 24), Justification::centredRight);

    const auto area = spectrumArea.toFloat();
    g.setColour (cPanel);
    g.fillRoundedRectangle (area, 8.f);

    const bool beatMode = modeBox.getSelectedId() == 2;
    const double sr = proc.getSR();

    auto xF = [&] (float f) { return area.getX() + area.getWidth() * std::log (f / 20.f) / std::log (1000.f); };
    auto yD = [&] (float db) { return jmap (jlimit (-90.f, 0.f, db), -90.f, 0.f, area.getBottom(), area.getY()); };
    auto dbAt = [&] (const SpecArray& a, float f)
    {
        const int idx = jlimit (1, bins - 1, (int) std::round (f / (sr * 0.5) * bins));
        return a[(size_t) idx] + 3.f * std::log2 (f / 1000.f) + 12.f; // +3 dB/Okt Tilt fuer ruhigere Anzeige
    };

    Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (area.toNearestInt());

    // ---- Frequenzzonen ----
    struct Zone { const char* name; float lo, hi; };
    const Zone zones[] = { { "Sub", 20.f, 80.f }, { "Mud", 200.f, 400.f }, { "Presence", 2000.f, 5000.f },
                           { "S-Laute", 5000.f, 9000.f }, { "Air", 10000.f, 20000.f } };
    g.setFont (FontOptions (11.f));
    for (auto& z : zones)
    {
        const float x1 = xF (z.lo), x2 = xF (z.hi);
        g.setColour (Colours::white.withAlpha (0.05f));
        g.fillRect (x1, area.getY(), x2 - x1, area.getHeight());
        g.setColour (Colours::white.withAlpha (0.45f));
        g.drawText (z.name, Rectangle<float> (x1, area.getY() + 2.f, x2 - x1, 14.f), Justification::centred);
    }

    // ---- Raster ----
    for (float f : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
    {
        g.setColour (Colours::white.withAlpha (0.08f));
        g.drawVerticalLine ((int) xF (f), area.getY(), area.getBottom());
        g.setColour (Colours::white.withAlpha (0.4f));
        g.drawText (f >= 1000.f ? String (f / 1000.f, 0) + "k" : String ((int) f),
                    Rectangle<float> (xF (f) - 20.f, area.getBottom() - 16.f, 40.f, 14.f), Justification::centred);
    }

    const SpecArray& vocalArr = beatMode ? scDb : mainDb;
    const SpecArray& beatArr  = beatMode ? mainDb : scDb;

    // ---- Konflikt-Zonen (Beat ueberdeckt Stimme) ----
    for (float x = area.getX(); x <= area.getRight(); x += 2.f)
    {
        const float f = 20.f * std::pow (1000.f, (x - area.getX()) / area.getWidth());
        if (f < 180.f || f > 9000.f) continue;
        const float vb = dbAt (vocalArr, f), bb = dbAt (beatArr, f);
        if (vb > -62.f && bb > vb - 3.f)
        {
            const float alpha = jmap (jlimit (0.f, 12.f, bb - (vb - 3.f)), 0.f, 12.f, 0.12f, 0.5f);
            g.setColour (cConflict.withAlpha (alpha));
            g.fillRect (x, area.getY() + 18.f, 2.f, area.getHeight() - 36.f);
        }
    }

    // ---- Spektren ----
    auto makePath = [&] (const SpecArray& a)
    {
        Path p;
        p.startNewSubPath (area.getX(), area.getBottom());
        for (float x = area.getX(); x <= area.getRight(); x += 2.f)
        {
            const float f = 20.f * std::pow (1000.f, (x - area.getX()) / area.getWidth());
            p.lineTo (x, yD (dbAt (a, f)));
        }
        p.lineTo (area.getRight(), area.getBottom());
        p.closeSubPath();
        return p;
    };
    auto drawSpec = [&] (const SpecArray& a, Colour c)
    {
        auto p = makePath (a);
        g.setColour (c.withAlpha (0.22f));
        g.fillPath (p);
        g.setColour (c);
        g.strokePath (p, PathStrokeType (1.5f));
    };
    drawSpec (beatArr, cBeat);
    drawSpec (vocalArr, cVocal);

    // ---- EQ-Kurve (was das Plugin gerade tut, +-12 dB um die Mittellinie) ----
    auto Pv = [this] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
    const float fs = (float) sr;
    std::vector<Coeff::Ptr> cs;
    if (! beatMode)
    {
        cs.push_back (Coeff::makeHighPass (fs, Pv ("hpf"), 0.707f));
        cs.push_back (Coeff::makePeakFilter (fs, 280.f, 1.0f, Decibels::decibelsToGain (Pv ("mud"))));
        cs.push_back (Coeff::makePeakFilter (fs, 3200.f, 0.9f, Decibels::decibelsToGain (proc.curPresenceDb.load())));
        cs.push_back (Coeff::makeHighShelf (fs, 10000.f, 0.7f, Decibels::decibelsToGain (Pv ("air"))));
        cs.push_back (Coeff::makePeakFilter (fs, 7000.f, 1.5f, Decibels::decibelsToGain (proc.curDeessDb.load())));
    }
    else if (Pv ("autoMode") < 0.5f)
    {
        const float c = proc.curCarveDb.load();
        cs.push_back (Coeff::makePeakFilter (fs, Pv ("carveFreq"), 0.9f, Decibels::decibelsToGain (-c)));
        cs.push_back (Coeff::makePeakFilter (fs, 300.f, 0.8f, Decibels::decibelsToGain (-c * 0.4f)));
    }

    for (int b = 0; b < BandAnalyzer::numBands; ++b)
        cs.push_back (Coeff::makePeakFilter (fs, kBandHz[(size_t) b], 1.4f,
                                             Decibels::decibelsToGain (proc.autoDisp[(size_t) b].load())));
    cs.push_back (Coeff::makeHighShelf (fs, 5000.f, 0.7f, Decibels::decibelsToGain (proc.curDepthShelfDb.load())));

    g.setColour (Colours::white.withAlpha (0.15f));
    g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

    Path curve;
    bool first = true;
    for (float x = area.getX(); x <= area.getRight(); x += 2.f)
    {
        const float f = 20.f * std::pow (1000.f, (x - area.getX()) / area.getWidth());
        double mag = 1.0;
        for (auto& c : cs) mag *= c->getMagnitudeForFrequency ((double) f, sr);
        const float db = jlimit (-24.f, 24.f, (float) Decibels::gainToDecibels (mag));
        const float y = area.getCentreY() - db * (area.getHeight() / 48.f);
        if (first) { curve.startNewSubPath (x, y); first = false; } else curve.lineTo (x, y);
    }
    g.setColour (cCurve);
    g.strokePath (curve, PathStrokeType (2.f));

    // ---- Auto-EQ Balken pro Band (gruen = Boost, orange = Cut) ----
    for (int b = 0; b < BandAnalyzer::numBands; ++b)
    {
        const float gv = proc.autoDisp[(size_t) b].load();
        if (std::abs (gv) < 0.1f) continue;
        const float x = xF (kBandHz[(size_t) b]);
        const float h = gv * (area.getHeight() / 48.f);
        g.setColour ((gv > 0 ? Colour (0xff4be37a) : Colour (0xffff9a3c)).withAlpha (0.55f));
        g.fillRect (x - 7.f, h > 0 ? area.getCentreY() - h : area.getCentreY(), 14.f, std::abs (h));
        g.setColour (Colours::white.withAlpha (0.9f));
        g.setFont (FontOptions (10.f));
        g.drawText (String (gv, 1), Rectangle<float> (x - 20.f, h > 0 ? area.getCentreY() - h - 13.f : area.getCentreY() - h + 1.f, 40.f, 12.f),
                    Justification::centred);
    }

    // ---- Infozeile + Legende ----
    String info;
    if (! beatMode)
        info << "Presence +" << String (proc.curPresenceDb.load(), 1) << " dB   De-Ess "
             << String (proc.curDeessDb.load(), 1) << " dB   Comp GR -" << String (proc.compGrDb.load(), 1) << " dB";
    else
        info << "Vocal Pocket -" << String (proc.curCarveDb.load(), 1) << " dB";
    info << "   Sidechain-Energie " << roundToInt (proc.maskAmount.load() * 100.f) << "%";

    g.setFont (FontOptions (12.f));
    g.setColour (Colours::white);
    g.drawText (info, Rectangle<float> (area.getX() + 10.f, area.getBottom() - 34.f, area.getWidth() - 20.f, 16.f),
                Justification::centredRight);

    float lx = area.getX() + 10.f;
    const float ly = area.getBottom() - 34.f;
    auto legend = [&] (Colour c, const String& t, float w)
    {
        g.setColour (c);
        g.fillRect (lx, ly + 4.f, 10.f, 8.f);
        g.setColour (Colours::white.withAlpha (0.85f));
        g.drawText (t, Rectangle<float> (lx + 14.f, ly, w, 16.f), Justification::centredLeft);
        lx += w + 20.f;
    };
    legend (cVocal, "Gesang", 50.f);
    legend (cBeat, "Beat", 36.f);
    legend (cConflict, "Konflikt (Beat verdeckt Stimme)", 190.f);
    legend (cCurve, "EQ-Kurve (+-12 dB)", 110.f);

    // ---- Front <-> Back Anzeige ----
    {
        const float d = Pv ("depth") / 100.f;
        const float cx = area.getCentreX(), y = area.getY() + 34.f;
        g.setColour (cPanel.withAlpha (0.8f));
        g.fillRoundedRectangle (cx - 150.f, y - 12.f, 300.f, 24.f, 12.f);
        g.setColour (Colours::white.withAlpha (0.5f));
        g.drawLine (cx - 90.f, y, cx + 90.f, y, 2.f);
        g.setFont (FontOptions (11.f, Font::bold));
        g.drawText ("VORNE", Rectangle<float> (cx - 148.f, y - 8.f, 54.f, 16.f), Justification::centred);
        g.drawText ("HINTEN", Rectangle<float> (cx + 94.f, y - 8.f, 54.f, 16.f), Justification::centred);
        g.setColour (cCurve);
        g.fillEllipse (cx + d * 90.f - 7.f, y - 7.f, 14.f, 14.f);
    }
}
