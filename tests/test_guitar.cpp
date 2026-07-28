// ---------------------------------------------------------------------------
// Why a physically-modelled guitar still sounded like a synthesiser.
//
// The routing was right - `str_level` 0.85, oscillators pulled back, body
// engaged - and it still did not sound like a guitar. That is because a bare
// Karplus-Strong string IS very close to a filtered sawtooth: perfectly
// harmonic partials, every one of them present, no excitation character, and
// full-bandwidth output up to Nyquist.
//
// Four things separate that from an instrument, and each is measured here
// rather than asserted:
//
//   1. PICK POSITION. Plucking a fraction B along a string cannot excite any
//      harmonic with a node there, so every 1/B-th harmonic is missing. That
//      comb of notches is the most recognisable signature of a plucked string.
//   2. STIFFNESS. Real strings are stiff, so upper partials run progressively
//      SHARP. A perfectly harmonic spectrum is what an oscillator produces.
//   3. EXCITATION CHARACTER. A pick is a band-limited scrape whose brightness
//      tracks how hard you hit it, not flat white noise.
//   4. A SPEAKER. A guitar cabinet dies above ~5 kHz. Distortion produces
//      harmonics to Nyquist, and that fizz - which no real amp can make - is
//      what the ear reads as synthetic.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/arch/Architecture.h"
#include "core/dsp/GraphBuilder.h"
#include "core/dsp/Registry.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "core/llm/InstrumentCorrection.h"
#include "core/llm/Compliance.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace forge;
using json = nlohmann::json;

namespace {

constexpr double kSr = 48000.0;

/// Exact energy at one frequency. The only pitch/level measurement in this
/// project that has never lied.
double energyAt(const std::vector<float>& x, size_t from, double hz) {
    if (from >= x.size()) return 0.0;
    const double w = 2.0 * 3.14159265358979 * hz / kSr, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = from; i < x.size(); ++i) { const double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    return std::sqrt(std::abs(s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(x.size() - from);
}

std::vector<float> renderString(const char* values, int midiNote = 52, double seconds = 1.2,
                                float velocity = 0.9f) {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;

    json patch = json::parse(R"({"values":{
        "str_level":0.9,"str_damp":0.20,"str_bright":8000,"str_decay":0.992,
        "str_pick":0.14,"str_stiff":0.0,
        "osc_a_level":0.0,"osc_b_level":0.0,"osc_sub_level":0.0,"osc_noise_level":0.0,
        "f1_cutoff":19000,"f1_reso":0.0,"f2_cutoff":19000,"body_mix":0.0,"vow_mix":0.0,
        "shp_mix":0.0,"dr_drive":1.0,"cab_mix":0.0,"rv_mix":0.0,"dl_mix":0.0,
        "env_1_a":0.5,"env_1_d":6000,"env_1_s":1.0,"env_1_r":2000}})");
    const auto extra = json::parse(values);
    for (auto it = extra.begin(); it != extra.end(); ++it) patch["values"][it.key()] = it.value();

    arch::applyPatch(inst, patch, r);
    ir::repair(inst, r);
    ir::applySafety(inst, r, 0.35f);

    std::vector<float> out;
    ir::IrReport build;
    auto graph = GraphBuilder::build(inst, kSr, build);
    if (graph == nullptr) return out;

    float l[256] = {}, rr[256] = {};
    float* chans[2] = {l, rr};
    graph->noteOn(midiNote, velocity);
    const int blocks = static_cast<int>(kSr * seconds / 256.0);
    for (int b = 0; b < blocks; ++b) {
        graph->process(chans, 256);
        for (int i = 0; i < 256; ++i) out.push_back(l[i]);
    }
    return out;
}

double midiToHz(int n) { return 440.0 * std::pow(2.0, (n - 69) / 12.0); }

} // namespace

TEST_CASE(the_string_gained_the_controls_a_guitar_needs) {
    const auto rack = arch::buildFullArchitecture();
    bool pick = false, stiff = false, cab = false;
    for (const auto& p : rack.params) {
        if (p.id == "str_pick")  pick  = true;
        if (p.id == "str_stiff") stiff = true;
        if (p.id == "cab_mix")   cab   = true;
    }
    CHECK(pick);
    CHECK(stiff);
    CHECK(cab);

    // And the cabinet is really in the signal path, after the drive.
    bool wired = false;
    for (const auto& c : rack.audio) if (c.from == "drive" && c.to == "cab") wired = true;
    CHECK(wired);
}

TEST_CASE(pick_position_notches_the_harmonic_it_should) {
    // Picking at exactly 1/4 of the string cannot excite the 4th harmonic,
    // because that harmonic has a node at the pick point. This is physics, and
    // it is the most audible single difference between a plucked string and a
    // sawtooth.
    const auto quarter = renderString(R"({"str_pick":0.25})");
    // NOT the midpoint. sin(k*pi*beta) is zero for the 4th harmonic at beta =
    // 0.5 as well as at 0.25, so comparing those two measures one null against
    // another - two noise-floor numbers whose ratio means nothing. 0.14 excites
    // the 4th harmonic almost fully, so it is a real reference level.
    const auto elsewhere = renderString(R"({"str_pick":0.14})");
    CHECK(!quarter.empty());
    if (quarter.empty()) return;

    const double f0 = midiToHz(52);
    const size_t from = static_cast<size_t>(kSr * 0.05);

    const double h4Quarter = energyAt(quarter, from, f0 * 4.0);
    const double h3Quarter = energyAt(quarter, from, f0 * 3.0);

    // The 4th harmonic is suppressed relative to its neighbour...
    CHECK(h4Quarter < h3Quarter);
    // ...and relative to the same harmonic picked where it is not on a node.
    CHECK(h4Quarter < energyAt(elsewhere, from, f0 * 4.0));
}

TEST_CASE(a_plucked_note_is_led_by_its_fundamental) {
    // The bug the user could hear and no test could see. Exciting the string
    // with flat white noise starts every harmonic at full strength, which is
    // the spectrum of a filtered sawtooth - and once the body resonator and the
    // pickup comb had taken their share, the FUNDAMENTAL was 13 dB below the
    // second and third partials. The ear resolves that as the wrong note in the
    // wrong octave, which is exactly what "it sounds like a synth, not a
    // guitar" means.
    //
    // A real pluck starts from a triangular displacement, whose harmonics fall
    // away as 1/k^2, and a pickup reads velocity rather than position, which
    // lifts that back to about 1/k. Either way the fundamental leads.
    const double f0 = midiToHz(52);
    const size_t from = static_cast<size_t>(kSr * 0.05);

    for (const char* pick : {R"({"str_pick":0.10})", R"({"str_pick":0.14})",
                             R"({"str_pick":0.25})", R"({"str_pick":0.35})"}) {
        const auto out = renderString(pick);
        CHECK(!out.empty());
        if (out.empty()) continue;

        const double h1 = energyAt(out, from, f0);

        // Nothing above the second partial may outrank the fundamental. The
        // second is allowed to come close - it does on a real guitar too, and
        // how close depends on where you pick.
        for (int h = 3; h <= 8; ++h)
            CHECK(energyAt(out, from, f0 * h) < h1);

        // And the fundamental is never buried: it stays within 3 dB of
        // whichever partial is loudest. It used to sit 13 dB under.
        double loudest = h1;
        for (int h = 2; h <= 8; ++h) loudest = std::max(loudest, energyAt(out, from, f0 * h));
        CHECK(h1 > loudest * 0.707);
    }
}

TEST_CASE(the_harmonics_of_a_pluck_fall_away_rather_than_sitting_flat) {
    // "Every harmonic present at full strength" is the definition of an
    // oscillator, not of a string. Averaged over the low partials the spectrum
    // has to slope down, and the top of it has to be gone entirely.
    const auto out = renderString(R"({"str_pick":0.14,"str_stiff":0.3})");
    CHECK(!out.empty());
    if (out.empty()) return;

    const double f0 = midiToHz(52);
    const size_t from = static_cast<size_t>(kSr * 0.05);

    const double low  = energyAt(out, from, f0) + energyAt(out, from, f0 * 2.0);
    const double high = energyAt(out, from, f0 * 7.0) + energyAt(out, from, f0 * 8.0);
    CHECK(high < low * 0.5);
}

TEST_CASE(pick_position_changes_the_timbre_audibly) {
    // Bridge versus neck. If these measure the same, the control is decorative.
    const auto bridge = renderString(R"({"str_pick":0.04})");
    const auto neck   = renderString(R"({"str_pick":0.45})");
    CHECK(!bridge.empty() && !neck.empty());
    if (bridge.empty() || neck.empty()) return;

    const double f0 = midiToHz(52);
    const size_t from = static_cast<size_t>(kSr * 0.05);

    // Picking near the bridge leaves more upper-harmonic energy relative to
    // the fundamental - that is what "thin and nasal" means.
    const double bridgeRatio = energyAt(bridge, from, f0 * 6.0)
                             / std::max(1e-9, energyAt(bridge, from, f0));
    const double neckRatio   = energyAt(neck, from, f0 * 6.0)
                             / std::max(1e-9, energyAt(neck, from, f0));
    CHECK(bridgeRatio > neckRatio);
}

/// Where the energy of the n-th partial actually sits, in cents relative to
/// the exact harmonic. Scans a small window and takes the peak.
double partialOffsetCents(const std::vector<float>& x, size_t from, double f0, int harmonic) {
    // 0.0002 is a resolution of about a third of a cent. At 0.001 the grid was
    // 1.7 cents wide, which is coarser than the shift being measured at the 8th
    // partial - it read as exactly zero, which is not a result, it is a ruler
    // with no marks on it.
    double best = 0.0, bestHz = f0 * harmonic;
    for (double mult = 0.985; mult <= 1.016; mult += 0.0002) {
        const double e = energyAt(x, from, f0 * harmonic * mult);
        if (e > best) { best = e; bestHz = f0 * harmonic * mult; }
    }
    return 1200.0 * std::log2(bestHz / (f0 * harmonic));
}

TEST_CASE(stiffness_makes_the_upper_partials_run_sharp) {
    // A perfectly harmonic spectrum is what an oscillator makes; it is a large
    // part of why the string still read as a synth. A real string is stiff, so
    // its high partials travel faster and sit progressively SHARP of the
    // harmonic series - while the fundamental stays exactly where it was
    // played, because a guitar that detunes when you add realism is useless.
    const auto pure  = renderString(R"({"str_stiff":0.0})");
    const auto stiff = renderString(R"({"str_stiff":0.9})");
    CHECK(!pure.empty() && !stiff.empty());
    if (pure.empty() || stiff.empty()) return;

    const double f0 = midiToHz(52);
    const size_t from = static_cast<size_t>(kSr * 0.08);

    // A pure delay line is harmonic to within the measurement resolution.
    CHECK(std::abs(partialOffsetCents(pure, from, f0, 1))  < 2.0);
    CHECK(std::abs(partialOffsetCents(pure, from, f0, 8))  < 2.0);
    CHECK(std::abs(partialOffsetCents(pure, from, f0, 12)) < 2.0);

    // The stiff string keeps its fundamental...
    CHECK(std::abs(partialOffsetCents(stiff, from, f0, 1)) < 2.0);

    // ...and its upper partials climb, progressively.
    //
    // Measured AGAINST the pure string rather than against zero. The peak
    // search has a resolution of about a cent and the high partials are quiet -
    // the pluck spectrum falls away as 1/k - so the absolute reading carries a
    // bias of its own. It is the same bias in both renders, and subtracting it
    // is what leaves the physics.
    const double h8  = partialOffsetCents(stiff, from, f0, 8)
                     - partialOffsetCents(pure,  from, f0, 8);
    const double h12 = partialOffsetCents(stiff, from, f0, 12)
                     - partialOffsetCents(pure,  from, f0, 12);
    // Measured: h8 +0.35 cents, h12 +7.95. The dispersion of a first-order
    // allpass grows steeply with frequency, so the low partials barely move and
    // the high ones move a lot - which is the shape of real string stiffness,
    // and is what the ear is actually hearing. An earlier version of this case
    // claimed +3.5 cents at h8; that was read off a peak search a full 1.7
    // cents wide against a much louder 8th partial, and it was measuring the
    // grid rather than the string.
    test::note("stiffness sharpens h8 by " + std::to_string(h8)
               + " cents, h12 by " + std::to_string(h12));
    CHECK(h8  > 0.2);
    CHECK(h12 > 4.0);
    CHECK(h12 > h8);
}

TEST_CASE(velocity_changes_the_pick_brightness_not_just_the_level) {
    // A hard pick is BRIGHTER, not merely louder. If only the level changes,
    // the instrument has no dynamic life.
    const auto soft = renderString("{}", 52, 1.2, 0.15f);
    const auto hard = renderString("{}", 52, 1.2, 1.0f);
    CHECK(!soft.empty() && !hard.empty());
    if (soft.empty() || hard.empty()) return;

    const double f0 = midiToHz(52);
    const size_t from = static_cast<size_t>(kSr * 0.02);

    const double softRatio = energyAt(soft, from, f0 * 8.0)
                           / std::max(1e-9, energyAt(soft, from, f0));
    const double hardRatio = energyAt(hard, from, f0 * 8.0)
                           / std::max(1e-9, energyAt(hard, from, f0));
    CHECK(hardRatio > softRatio);
}

/// Drives a module directly with white noise and returns its response in dB at
/// a set of frequencies. Measuring the cabinet through the instrument chain is
/// meaningless - the string is already dark, so the "before" level above 6 kHz
/// is the measurement floor rather than any real signal, and the ratio is
/// noise divided by noise.
std::vector<double> cabinetResponseDb(float topHz, float mix,
                                      const std::vector<double>& atHz) {
    const auto* man = Registry::instance().find("fx.cabinet");
    std::vector<double> out(atHz.size(), 0.0);
    if (man == nullptr) return out;

    auto m = man->factory();
    ModulePrepareInfo info;
    info.sampleRate = kSr; info.maxBlock = 16; info.seed = 1; info.scope = Scope::Global;
    m->prepare(info);
    m->setParam(0, 85.0f); m->setParam(1, topHz); m->setParam(2, mix);

    Rng rng;
    rng.reseed(7);
    std::vector<float> in, wet;
    float bi[2][16], bo[2][16];
    const float* ip[2] = {bi[0], bi[1]};
    float*       op[2] = {bo[0], bo[1]};

    for (int block = 0; block < 3000; ++block) {
        for (int i = 0; i < 16; ++i) {
            const float n = rng.nextBipolar() * 0.3f;
            bi[0][i] = bi[1][i] = n;
            bo[0][i] = bo[1][i] = 0.0f;
        }
        ProcessArgs a;
        a.in = ip; a.out = op; a.numSamples = 16; a.g = nullptr; a.v = nullptr;
        m->process(a);
        for (int i = 0; i < 16; ++i) { in.push_back(bi[0][i]); wet.push_back(bo[0][i]); }
    }

    for (size_t k = 0; k < atHz.size(); ++k) {
        const double dry = energyAt(in,  0, atHz[k]);
        const double amp = energyAt(wet, 0, atHz[k]);
        out[k] = 20.0 * std::log10(amp / std::max(dry, 1e-12));
    }
    return out;
}

TEST_CASE(the_cabinet_behaves_like_a_real_speaker) {
    // A guitar speaker is not remotely flat, and its shape is the difference
    // between "amp in a room" and "distorted oscillator".
    const auto db = cabinetResponseDb(4800.0f, 1.0f,
                                      {200.0, 1000.0, 3000.0, 6000.0, 10000.0, 16000.0});

    CHECK(db[2] > 0.0);        // presence peak around 3 kHz, where guitars live
    CHECK(db[3] < -6.0);       // already dying at 6 kHz
    CHECK(db[4] < -20.0);      // gone by 10 kHz
    CHECK(db[5] < -35.0);      // and the fizz no speaker can make is not there
    CHECK(db[4] < db[3]);      // monotonically steeper
    CHECK(db[5] < db[4]);
}

TEST_CASE(the_cabinet_top_control_moves_the_rolloff) {
    const auto dark   = cabinetResponseDb(2000.0f, 1.0f, {4000.0});
    const auto bright = cabinetResponseDb(9000.0f, 1.0f, {4000.0});
    CHECK(bright[0] > dark[0] + 10.0);
}

TEST_CASE(the_cabinet_is_bypassed_at_zero_mix) {
    // It sits in every instrument's chain, so at zero it must be inaudible.
    const auto off = cabinetResponseDb(4800.0f, 0.0f, {200.0, 3000.0, 12000.0});
    for (double db : off) CHECK(std::abs(db) < 0.1);
}

TEST_CASE(the_cabinet_is_audible_in_the_instrument) {
    // And it really is in the signal path, not just in the registry.
    const auto open   = renderString(R"({"dr_drive":4.0,"cab_mix":0.0})");
    const auto amped  = renderString(R"({"dr_drive":4.0,"cab_mix":1.0,"cab_top":1500})");
    CHECK(!open.empty() && !amped.empty());
    if (open.empty() || amped.empty()) return;

    // Measured at 2 kHz, where the string genuinely has energy - not up in the
    // noise floor where the numbers mean nothing. 3 kHz used to qualify and no
    // longer does: with the pickup response in place the string is 8 dB down by
    // then and heading for the floor, and by 6 kHz the "ratio" is 1.00, which
    // is noise divided by noise.
    //
    // So the floor is established first rather than assumed - this is the
    // measurement that produced a false "the cabinet does nothing" once
    // already.
    // Measured as a TILT - how much 3 kHz sits below 1 kHz, with the speaker
    // and without it - rather than as an absolute level at one frequency.
    //
    // A level comparison keeps having to be re-aimed, because it only means
    // anything where the string itself still has energy, and how far up that
    // reaches is exactly what the rest of this file keeps changing. A ratio of
    // ratios does not care: whatever the string is doing, it is doing it in
    // both renders, so what is left is the speaker.
    const size_t from = static_cast<size_t>(kSr * 0.05);
    const double floorLevel = energyAt(open, from, 9000.0);
    const double open1k = energyAt(open,  from, 1000.0);
    const double open3k = energyAt(open,  from, 3000.0);
    const double amp1k  = energyAt(amped, from, 1000.0);
    const double amp3k  = energyAt(amped, from, 3000.0);

    // Both 3 kHz readings are well clear of the floor, so neither ratio below
    // is noise divided by noise - the mistake that once produced a confident
    // "the cabinet does nothing".
    CHECK(open3k > floorLevel * 4.0);
    CHECK(amp3k  > floorLevel * 2.0);

    // Through a cabinet closed down to 1.5 kHz, the top must fall away much
    // faster than it does without one.
    CHECK((amp3k / amp1k) < (open3k / open1k) * 0.5);
}

TEST_CASE(a_full_electric_guitar_patch_renders_cleanly) {
    const auto out = renderString(R"({
        "str_level":0.85,"str_damp":0.22,"str_bright":7000,"str_decay":0.915,
        "str_pick":0.11,"str_stiff":0.35,
        "body_mix":0.22,"body_freq":190,
        "dr_drive":3.4,"cab_mix":0.85,"cab_top":4600,
        "f1_cutoff":6000,"dl_mix":0.15,"rv_mix":0.10})");
    CHECK(!out.empty());
    if (out.empty()) return;

    float peak = 0.0f;
    bool  finite = true;
    for (float v : out) {
        if (!std::isfinite(v)) finite = false;
        peak = std::max(peak, std::abs(v));
    }
    CHECK(finite);
    CHECK(peak > 0.01f);
    CHECK(peak <= 1.0f);

    // And it is genuinely band-limited like an amp, not open to Nyquist.
    const size_t from = static_cast<size_t>(kSr * 0.05);
    CHECK(energyAt(out, from, 14000.0) < energyAt(out, from, midiToHz(52) * 3.0) * 0.1);
    CHECK(energyAt(out, from, midiToHz(52)) > 0.0);
}

TEST_CASE(a_string_without_playing_character_is_caught) {
    // The exact patch from the report: str_level 0.85, oscillators pulled back,
    // body engaged - passes "is it a physical model" and still sounds like a
    // synth because nothing was done about HOW it is played.
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, json::parse(R"({"values":{
        "str_level":0.85,"str_damp":0.28,"str_bright":6200,"str_decay":0.96,
        "osc_a_level":0.15,"body_mix":0.30,"dr_drive":1.8}})"), r);
    ir::repair(inst, r);

    bool physicalOk = true, characterFlagged = false, cabinetFlagged = false;
    for (const auto& q : llm::check("a reverb rock electric guitar", inst).requirements) {
        if (q.id == "instrument.physical")    physicalOk       = q.met;
        if (q.id == "tone.pluckDecay")        characterFlagged = !q.met;
        if (q.id == "tone.cabinet")           cabinetFlagged   = !q.met;
    }
    CHECK(physicalOk);          // it really is built on the string...
    CHECK(characterFlagged);    // ...but it rings for 13 seconds, like a pad
    CHECK(cabinetFlagged);      // ...and it is a DI, not an amp
}

TEST_CASE(a_properly_voiced_guitar_passes_every_tone_check) {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, json::parse(R"({"values":{
        "str_level":0.85,"str_damp":0.22,"str_bright":7000,"str_decay":0.915,
        "str_pick":0.11,"str_stiff":0.35,"osc_a_level":0.0,
        "body_mix":0.22,"dr_drive":3.4,"cab_mix":0.85,"cab_top":4800}})"), r);
    ir::repair(inst, r);

    for (const auto& q : llm::check("a reverb rock electric guitar", inst).requirements)
        CHECK(q.met);
}

TEST_CASE(the_corrector_gives_a_guitar_its_voice_as_well_as_its_engine) {
    // Correcting a synth into a string is not enough on its own.
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, json::parse(R"({"values":{
        "osc_a_level":0.8,"f1_cutoff":2600,"dr_drive":2.2}})"), r);
    ir::repair(inst, r);

    ir::IrReport fix;
    CHECK(llm::enforceInstrumentFamily(inst, "a driven electric guitar riff", fix));

    auto valueOf = [&](const char* id) {
        for (const auto& p : inst.params) if (p.id == id) return p.def;
        return -1.0f;
    };
    CHECK(valueOf("str_level") > 0.7f);
    CHECK(valueOf("str_stiff") > 0.05f);        // it is a string, not a comb
    CHECK(valueOf("cab_mix")   > 0.5f);         // and it goes through a speaker
    for (const auto& q : llm::check("a driven electric guitar riff", inst).requirements)
        CHECK(q.met);
}

TEST_CASE(the_exact_reported_patch_is_repaired_end_to_end) {
    // "Reverb Rock Guitar": str_level 0.85, a body, sensible filtering - and it
    // still sounded like a synth. The engine was already right, so the engine
    // corrector had nothing to do and returned early. What was wrong was the
    // VOICING: a thirteen-second ring straight into the output with no speaker.
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, json::parse(R"({"values":{
        "str_level":0.85,"str_damp":0.28,"str_bright":6200,"str_decay":0.96,
        "osc_a_level":0.15,"body_mix":0.30,"dr_drive":1.8}})"), r);
    ir::repair(inst, r);

    const char* prompt = "a reverb rock electric guitar";
    CHECK(!llm::check(prompt, inst).allMet());          // caught...

    ir::IrReport fix;
    CHECK(llm::enforceInstrumentFamily(inst, prompt, fix));   // ...and fixed
    ir::repair(inst, fix);

    auto valueOf = [&](const char* id) {
        for (const auto& p : inst.params) if (p.id == id) return p.def;
        return -1.0f;
    };
    CHECK(valueOf("str_level") > 0.7f);      // the engine was left alone
    CHECK(valueOf("str_decay") <= 0.955f);   // the pad-length ring is gone
    CHECK(valueOf("cab_mix")   > 0.5f);      // and it goes through a speaker now
    CHECK(llm::check(prompt, inst).allMet());
}

TEST_CASE(tone_correction_leaves_a_well_voiced_guitar_alone) {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, json::parse(R"({"values":{
        "str_level":0.85,"str_decay":0.905,"str_pick":0.10,"str_stiff":0.35,
        "cab_mix":0.9,"cab_top":4700,"dr_drive":3.0}})"), r);
    ir::repair(inst, r);

    ir::IrReport fix;
    CHECK(!llm::enforceInstrumentFamily(inst, "a driven electric guitar", fix));
}

TEST_CASE(an_acoustic_guitar_is_not_forced_through_an_amp) {
    // The cabinet belongs on electric instruments. A nylon guitar through a
    // 4x12 is its own kind of wrong.
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, json::parse(R"({"values":{"osc_a_level":0.8}})"), r);
    ir::repair(inst, r);

    ir::IrReport fix;
    llm::enforceInstrumentFamily(inst, "a soft nylon acoustic guitar", fix);
    for (const auto& p : inst.params)
        if (p.id == "cab_mix") CHECK(p.def < 0.25f);
}
