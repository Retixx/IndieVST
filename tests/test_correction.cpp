// ---------------------------------------------------------------------------
// The guarantee.
//
// Three separate mechanisms already existed to stop a request for a guitar
// coming back as a synthesiser: the prompt teaches the recipe, the compliance
// checker catches the miss, and the retry sends it back with instructions. All
// three are REQUESTS. It still came back a synth.
//
// This file covers the mechanism that is not a request. If the musician named a
// plucked, struck, bowed or reed instrument and the patch still is not built
// that way, the source section is rebuilt deterministically. Everything else
// the model chose is left alone.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/arch/Architecture.h"
#include "core/dsp/GraphBuilder.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "core/ir/IrValidator.h"
#include "core/llm/Compliance.h"
#include "core/llm/InstrumentCorrection.h"
#include "core/llm/PromptBuilder.h"

#include <nlohmann/json.hpp>

#include <cmath>

using namespace forge;
using json = nlohmann::json;

namespace {

/// The exact shape that shipped: saws, a lowpass, no physical model anywhere.
const char* kSynthPatch = R"({
    "name":"Cloudchrome Riff",
    "values":{"osc_a_level":0.78,"osc_b_level":0.40,"osc_a_detune":10,
              "f1_cutoff":2600,"f1_reso":0.22,"dr_drive":2.2,
              "env_1_a":6,"env_1_d":500,"env_1_s":0.55,"env_1_r":320}})";

ir::Instrument patched(const char* patch) {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, json::parse(patch), r);
    ir::repair(inst, r);
    return inst;
}

float valueOf(const ir::Instrument& inst, const char* id) {
    for (const auto& p : inst.params) if (p.id == id) return p.def;
    return -1.0f;
}

bool physicalOk(const char* prompt, const ir::Instrument& inst) {
    for (const auto& r : llm::check(prompt, inst).requirements)
        if (r.id == "instrument.physical") return r.met;
    return true;
}

} // namespace

TEST_CASE(a_guitar_request_answered_with_saws_is_corrected) {
    auto inst = patched(kSynthPatch);
    CHECK(!physicalOk("a driven electric guitar riff", inst));

    ir::IrReport report;
    CHECK(llm::enforceInstrumentFamily(inst, "a driven electric guitar riff", report));

    CHECK(valueOf(inst, "str_level") > 0.7f);        // the string is now the voice
    CHECK(valueOf(inst, "osc_a_level") <= 0.12f);    // the saws stepped back
    // ...and it comes out of a BOX. For an electric that box is the speaker,
    // not a wooden body: a solid-body guitar has no resonant cavity, and the
    // comb that was standing in for one reinforced a fixed 200/400/600 Hz
    // regardless of the note, which on a low E put an unrequested peak straight
    // onto the fourth harmonic.
    CHECK(valueOf(inst, "cab_mix") > 0.5f);
    CHECK(valueOf(inst, "body_mix") < 0.05f);
    CHECK(physicalOk("a driven electric guitar riff", inst));

    // The musician is told it happened.
    bool explained = false;
    for (const auto& i : report.issues)
        if (i.message.find("physical string model") != std::string::npos) explained = true;
    CHECK(explained);
}

TEST_CASE(the_bass_request_that_started_this_is_corrected) {
    const char* prompt = "a bass instrument like the ones Steve Lacy and Thundercat use";
    auto inst = patched(kSynthPatch);
    ir::IrReport report;
    CHECK(llm::enforceInstrumentFamily(inst, prompt, report));

    CHECK(valueOf(inst, "str_level") > 0.7f);
    // Bass register: a darker pick tone, not a bright steel one.
    CHECK(valueOf(inst, "str_bright") < 3500.0f);
    // And a bass cabinet, tuned lower than a guitar one. Thundercat plays an
    // electric bass, so the resonator is the rig, not a soundbox.
    CHECK(valueOf(inst, "cab_mix") > 0.5f);
    CHECK(valueOf(inst, "cab_top") <= 3600.0f);
    CHECK(physicalOk(prompt, inst));
}

TEST_CASE(an_acoustic_guitar_is_not_noise) {
    // "It gave me filtered white noise." Whatever the model sends, the noise
    // oscillator must not be the voice of a named acoustic instrument.
    auto inst = patched(R"({"values":{"osc_noise_level":0.85,"osc_a_level":0.0,
                                      "f1_cutoff":1800}})");
    ir::IrReport report;
    CHECK(llm::enforceInstrumentFamily(inst, "a warm acoustic guitar", report));

    CHECK(valueOf(inst, "str_level") > 0.7f);
    CHECK(valueOf(inst, "osc_noise_level") <= 0.06f);   // demoted to texture
    CHECK(physicalOk("a warm acoustic guitar", inst));
}

TEST_CASE(a_glockenspiel_gets_an_inharmonic_ratio) {
    auto inst = patched(kSynthPatch);
    ir::IrReport report;
    CHECK(llm::enforceInstrumentFamily(inst, "a bright glockenspiel", report));

    CHECK(valueOf(inst, "osc_fm_level") > 0.7f);
    const auto* fm = inst.findNode("osc_fm");
    CHECK(fm != nullptr);
    if (fm != nullptr) {
        const double ratio = fm->settings.value("ratio", 0.0);
        CHECK(std::abs(ratio - std::round(ratio)) > 0.1);   // metallic, not organ-like
    }
    CHECK(valueOf(inst, "env_1_s") < 0.05f);                // struck, so it decays
}

TEST_CASE(a_marimba_stays_wooden_and_a_piano_stays_harmonic) {
    for (auto pair : {std::make_pair("a wooden marimba", 4.0),
                      std::make_pair("an acoustic piano", 1.0)}) {
        auto inst = patched(kSynthPatch);
        ir::IrReport report;
        llm::enforceInstrumentFamily(inst, pair.first, report);
        const auto* fm = inst.findNode("osc_fm");
        CHECK(fm != nullptr);
        if (fm != nullptr) CHECK(std::abs(fm->settings.value("ratio", 0.0) - pair.second) < 0.01);
    }
}

TEST_CASE(a_cello_gets_a_body_and_a_bowed_attack) {
    auto inst = patched(kSynthPatch);
    ir::IrReport report;
    CHECK(llm::enforceInstrumentFamily(inst, "a warm cello", report));

    CHECK(valueOf(inst, "body_mix") > 0.05f);
    CHECK(valueOf(inst, "env_1_a") > 25.0f);     // bowed, not plucked
    CHECK(valueOf(inst, "env_1_s") > 0.5f);      // and held
    CHECK(physicalOk("a warm cello", inst));
}

TEST_CASE(correction_does_nothing_when_the_patch_is_already_right) {
    // body_freq is part of "already right". A box tuned to 220 Hz reinforces
    // A3 and its multiples under every note played, so a nylon guitar with one
    // is not a correctly voiced instrument - it is the thing the corrector
    // exists to catch.
    auto inst = patched(R"({"values":{"str_level":0.85,"str_damp":0.4,"body_mix":0.3,
                                      "body_freq":100,"osc_a_level":0.0}})");
    const auto before = inst;
    ir::IrReport report;
    CHECK(!llm::enforceInstrumentFamily(inst, "a nylon guitar", report));
    CHECK(valueOf(inst, "str_level") == valueOf(before, "str_level"));
    CHECK(valueOf(inst, "str_damp")  == valueOf(before, "str_damp"));
}

TEST_CASE(correction_never_fires_on_a_synth_request) {
    for (const char* prompt : {"a synth bass", "warm analog pad", "303 acid line",
                               "supersaw lead", "something dark and heavy",
                               "808 sub", "a reese bass"}) {
        auto inst = patched(kSynthPatch);
        ir::IrReport report;
        CHECK(!llm::enforceInstrumentFamily(inst, prompt, report));
        CHECK(valueOf(inst, "str_level") < 0.05f);
    }
}

TEST_CASE(correction_respects_the_mood_words_around_it) {
    ir::IrReport report;

    auto dark = patched(kSynthPatch);
    llm::enforceInstrumentFamily(dark, "a warm muted lofi guitar", report);
    auto bright = patched(kSynthPatch);
    llm::enforceInstrumentFamily(bright, "a bright crisp steel guitar", report);

    // A corrected instrument still has to sound like what was asked for.
    CHECK(dark.findNode("osc_string") != nullptr);
    CHECK(valueOf(dark, "str_damp") > valueOf(bright, "str_damp"));
    CHECK(valueOf(bright, "str_bright") > valueOf(dark, "str_bright"));
}

TEST_CASE(a_corrected_instrument_is_still_valid_and_still_plays) {
    for (const char* prompt : {"a driven electric guitar riff", "a bass like Thundercat",
                               "a bright glockenspiel", "a warm cello",
                               "a warm acoustic guitar", "a church organ"}) {
        auto inst = patched(kSynthPatch);
        ir::IrReport report;
        llm::enforceInstrumentFamily(inst, prompt, report);
        ir::repair(inst, report);
        ir::applySafety(inst, report, 0.35f);

        ir::IrReport check;
        CHECK(ir::validate(inst, check));

        ir::IrReport buildReport;
        auto graph = GraphBuilder::build(inst, 48000.0, buildReport);
        CHECK(graph != nullptr);
        if (graph == nullptr) continue;

        float l[256] = {}, r[256] = {};
        float* chans[2] = {l, r};
        graph->noteOn(48, 0.85f);
        float peak = 0.0f;
        bool finite = true;
        for (int block = 0; block < 40; ++block) {
            graph->process(chans, 256);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 256; ++i) {
                    if (!std::isfinite(chans[c][i])) finite = false;
                    peak = std::max(peak, std::abs(chans[c][i]));
                }
        }
        CHECK(finite);
        CHECK(peak > 0.001f);
        CHECK(peak <= 1.0f);
    }
}

TEST_CASE(correction_leaves_the_models_own_choices_alone) {
    // It corrects the SOURCE, not the design. Filter, effects and layout are
    // the model's to decide, and overwriting them would make every corrected
    // instrument identical.
    auto inst = patched(R"({"values":{"osc_a_level":0.8,"f1_cutoff":900,"f1_reso":0.44,
                                      "rv_mix":0.31,"dl_mix":0.22,"dr_drive":2.6}})");
    ir::IrReport report;
    llm::enforceInstrumentFamily(inst, "a nylon guitar", report);

    CHECK(std::abs(valueOf(inst, "f1_cutoff") - 900.0f) < 1.0f);
    CHECK(std::abs(valueOf(inst, "f1_reso")   - 0.44f)  < 0.01f);
    CHECK(std::abs(valueOf(inst, "rv_mix")    - 0.31f)  < 0.01f);
    CHECK(std::abs(valueOf(inst, "dl_mix")   - 0.22f)  < 0.01f);
}

TEST_CASE(naming_an_artist_requires_actually_citing_them) {
    // "It had no mention of Steve Lacy besides that being in the generation."
    // There was no way to tell whether the reference was used or ignored.
    const char* prompt = "a bass like the ones Steve Lacy and Thundercat use";

    auto silent = patched(R"({"name":"Warm Pick Bass","values":{"str_level":0.8}})");
    bool flagged = false;
    for (const auto& r : llm::check(prompt, silent).requirements)
        if (r.id == "references.cited") flagged = !r.met;
    CHECK(flagged);

    auto cited = patched(R"({"name":"Warm Pick Bass","values":{"str_level":0.8},
        "references":["Steve Lacy - short scratchy bass tone, close-mic'd, little sustain",
                      "Thundercat - six-string finger technique, high register"]})");
    for (const auto& r : llm::check(prompt, cited).requirements)
        if (r.id == "references.cited") CHECK(r.met);
}

TEST_CASE(a_simile_without_a_name_is_not_treated_as_a_citation) {
    auto inst = patched(R"({"values":{"osc_a_level":0.8}})");
    for (const char* prompt : {"something like a warm pad", "a bass that sounds like water",
                               "dark vibes"}) {
        for (const auto& r : llm::check(prompt, inst).requirements)
            CHECK(r.id != "references.cited");
    }
}

TEST_CASE(the_prompt_asks_for_the_reference_in_the_name) {
    // "I asked for a Kanye West kind of viola; it really should be telling the
    // user confirmation that it's a Kanye West Viola." The name is the first
    // place anyone looks; a tooltip is not good enough.
    const auto system = llm::buildPatchSystemPrompt();
    CHECK(system.find("names an ARTIST") != std::string::npos);
    CHECK(system.find("Kanye Viola") != std::string::npos);
    // ...but only when a real name was given.
    CHECK(system.find("never invent an") != std::string::npos);
}
