// ---------------------------------------------------------------------------
// Can it actually build every instrument?
//
// "Make sure you can create basically every instrument - glockenspiel, viola,
// cello, etc." So this file builds one of each, from the recipes the prompt
// teaches, and holds each to the same bar: it validates, it renders real audio,
// it does not clip or produce NaNs, and the compliance checker agrees it is the
// instrument that was asked for rather than a synthesiser wearing its name.
//
// If a recipe in the prompt cannot be expressed here, the prompt is lying to
// the model - which is exactly how the FM ratio went missing.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/arch/Architecture.h"
#include "core/dsp/GraphBuilder.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "core/ir/IrValidator.h"
#include "core/llm/Compliance.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>

using namespace forge;
using json = nlohmann::json;

namespace {

struct Rendered {
    bool  built  = false;
    bool  finite = true;
    float peak   = 0.0f;
    float rms    = 0.0f;
};

/// Builds the patch, plays a note, and reports what came out.
Rendered render(const ir::Instrument& source, int midiNote) {
    ir::Instrument inst = source;
    ir::IrReport report;
    ir::repair(inst, report);
    ir::applySafety(inst, report, 0.35f);

    Rendered out;
    ir::IrReport buildReport;
    auto graph = GraphBuilder::build(inst, 48000.0, buildReport);
    if (graph == nullptr) return out;
    out.built = true;

    float left[256] = {}, right[256] = {};
    float* chans[2] = {left, right};

    graph->noteOn(midiNote, 0.85f);
    double sumSq = 0.0;
    int    counted = 0;
    for (int block = 0; block < 60; ++block) {
        graph->process(chans, 256);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 256; ++i) {
                const float v = chans[c][i];
                if (!std::isfinite(v)) out.finite = false;
                out.peak = std::max(out.peak, std::abs(v));
                sumSq += static_cast<double>(v) * v;
                ++counted;
            }
    }
    out.rms = counted > 0 ? static_cast<float>(std::sqrt(sumSq / counted)) : 0.0f;
    return out;
}

ir::Instrument patched(const json& patch) {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;
    arch::applyPatch(inst, patch, r);
    return inst;
}

bool physicalOk(const char* prompt, const ir::Instrument& inst) {
    for (const auto& r : llm::check(prompt, inst).requirements)
        if (r.id == "instrument.physical") return r.met;
    return true;   // not classified as a named instrument
}

struct Recipe {
    const char* prompt;
    int         note;
    const char* patch;
};

// One per family, built the way the prompt says to build it.
const Recipe kOrchestra[] = {
    {"a nylon acoustic guitar", 52, R"({"voicing":"poly","polyphony":8,"values":{
        "str_level":0.85,"str_damp":0.30,"str_bright":5200,"str_decay":0.990,
        "osc_a_level":0.0,"body_mix":0.30,"body_freq":190,"body_res":0.50,
        "env_1_a":0.5,"env_1_d":900,"env_1_s":0.10,"env_1_r":700,"f1_cutoff":6500}})"},

    {"an electric bass like Thundercat", 33, R"({"voicing":"poly","polyphony":6,"values":{
        "str_level":0.85,"str_damp":0.42,"str_bright":2400,"str_decay":0.985,
        "osc_a_level":0.0,"osc_sub_level":0.22,"body_mix":0.25,"body_freq":85,
        "dr_drive":1.9,"f1_cutoff":2200,
        "env_1_a":1,"env_1_d":700,"env_1_s":0.18,"env_1_r":400}})"},

    {"a gritty electric guitar lead", 55, R"({"voicing":"poly","polyphony":8,"values":{
        "str_level":0.80,"str_damp":0.22,"str_bright":7000,"str_decay":0.988,
        "osc_a_level":0.0,"dr_drive":3.2,"f1_cutoff":3000,"dl_mix":0.18,"dl_time":120,
        "env_1_a":1,"env_1_d":800,"env_1_s":0.30,"env_1_r":500}})"},

    {"a warm cello", 45, R"({"voicing":"poly","polyphony":6,"values":{
        "osc_a_level":0.75,"osc_b_level":0.45,"osc_b_detune":9,
        "body_mix":0.28,"body_freq":110,"f1_cutoff":2400,
        "env_1_a":140,"env_1_d":600,"env_1_s":0.80,"env_1_r":420,
        "lfo_1_rate":5.2},
        "switches":{"osc_a_wave":"saw","osc_b_wave":"saw"},
        "mod":[{"source":"env_1","target":"amp.gain","depth":1.0},
               {"source":"lfo_1","target":"amt_vib.a","depth":0.25}]})"},

    {"a bright violin", 76, R"({"voicing":"poly","polyphony":6,"values":{
        "osc_a_level":0.75,"osc_b_level":0.40,"osc_b_detune":7,
        "body_mix":0.24,"body_freq":420,"f1_cutoff":7000,
        "env_1_a":90,"env_1_d":500,"env_1_s":0.85,"env_1_r":320}})"},

    {"a viola line", 60, R"({"voicing":"poly","polyphony":6,"values":{
        "osc_a_level":0.75,"osc_b_level":0.42,"osc_b_detune":8,
        "body_mix":0.26,"body_freq":260,"f1_cutoff":4200,
        "env_1_a":120,"env_1_d":550,"env_1_s":0.82,"env_1_r":380}})"},

    {"a bright glockenspiel", 84, R"({"voicing":"poly","polyphony":8,"values":{
        "osc_fm_level":0.85,"osc_fm_index":4.0,"osc_a_level":0.0,
        "env_1_a":0.2,"env_1_d":1800,"env_1_s":0.0,"env_1_r":1500,
        "f1_cutoff":14000,"rv_mix":0.18},
        "switches":{"osc_fm_ratio":3.5}})"},

    {"a vibraphone", 72, R"({"voicing":"poly","polyphony":8,"values":{
        "osc_fm_level":0.80,"osc_fm_index":2.6,"osc_a_level":0.0,
        "env_1_a":0.5,"env_1_d":2400,"env_1_s":0.0,"env_1_r":1800,
        "lfo_1_rate":5.0,"rv_mix":0.22},
        "switches":{"osc_fm_ratio":2.41}})"},

    {"a wooden marimba", 64, R"({"voicing":"poly","polyphony":8,"values":{
        "osc_fm_level":0.78,"osc_fm_index":1.6,"osc_a_level":0.0,
        "body_mix":0.30,"body_freq":320,
        "env_1_a":0.3,"env_1_d":420,"env_1_s":0.0,"env_1_r":320},
        "switches":{"osc_fm_ratio":4.0}})"},

    {"an acoustic piano", 60, R"({"voicing":"poly","polyphony":12,"values":{
        "osc_fm_level":0.80,"osc_fm_index":3.0,"osc_fm_fine":4,"osc_a_level":0.0,
        "env_1_a":0.5,"env_1_d":1600,"env_1_s":0.15,"env_1_r":600,
        "f1_cutoff":9000},
        "switches":{"osc_fm_ratio":1.0},
        "mod":[{"source":"env_1","target":"amp.gain","depth":1.0},
               {"source":"m_vel","target":"osc_fm.index","depth":0.5}]})"},

    {"a rhodes electric piano", 60, R"({"voicing":"poly","polyphony":10,"values":{
        "osc_fm_level":0.78,"osc_fm_index":1.8,"osc_a_level":0.0,
        "ch_mix":0.30,"ch_depth":0.40,"tp_drive":0.25,
        "env_1_a":1,"env_1_d":1400,"env_1_s":0.25,"env_1_r":700},
        "switches":{"osc_fm_ratio":2.0}})"},

    {"a church organ", 55, R"({"voicing":"poly","polyphony":12,"values":{
        "osc_a_level":0.55,"osc_b_level":0.45,"osc_sub_level":0.35,
        "body_mix":0.15,"env_1_a":8,"env_1_d":100,"env_1_s":1.0,"env_1_r":90},
        "switches":{"osc_a_wave":"sine","osc_b_wave":"sine","osc_b_oct":1}})"},

    {"a breathy flute", 72, R"({"voicing":"mono","polyphony":1,"glide_ms":30,"values":{
        "osc_a_level":0.70,"osc_noise_level":0.10,"vow_mix":0.25,"vow_morph":0.4,
        "f1_cutoff":4200,"env_1_a":70,"env_1_d":300,"env_1_s":0.85,"env_1_r":260},
        "switches":{"osc_a_wave":"sine"}})"},

    {"a bright brass section", 58, R"({"voicing":"poly","polyphony":8,"values":{
        "osc_a_level":0.70,"osc_b_level":0.50,"vow_mix":0.18,
        "f1_cutoff":3400,"dr_drive":1.6,
        "env_1_a":45,"env_1_d":400,"env_1_s":0.80,"env_1_r":260},
        "switches":{"osc_a_wave":"saw"},
        "mod":[{"source":"env_1","target":"amp.gain","depth":1.0},
               {"source":"m_vel","target":"f1_cutoff","depth":0.6}]})"},

    {"a plucked harp", 67, R"({"voicing":"poly","polyphony":12,"values":{
        "str_level":0.82,"str_damp":0.20,"str_bright":6800,"str_decay":0.992,
        "osc_a_level":0.0,"body_mix":0.22,"body_freq":260,"rv_mix":0.20,
        "env_1_a":0.3,"env_1_d":1200,"env_1_s":0.05,"env_1_r":900}})"},

    {"a kalimba", 69, R"({"voicing":"poly","polyphony":8,"values":{
        "str_level":0.78,"str_damp":0.45,"str_bright":3800,"str_decay":0.975,
        "osc_a_level":0.0,"body_mix":0.35,"body_freq":300,
        "env_1_a":0.2,"env_1_d":500,"env_1_s":0.0,"env_1_r":400}})"},
};

} // namespace

TEST_CASE(every_instrument_family_builds_validates_and_sounds) {
    for (const auto& recipe : kOrchestra) {
        const auto inst = patched(json::parse(recipe.patch));

        ir::Instrument checked = inst;
        ir::IrReport   repairReport;
        ir::repair(checked, repairReport);

        ir::IrReport validation;
        const bool valid = ir::validate(checked, validation);
        CHECK(valid);

        const auto audio = render(inst, recipe.note);
        CHECK(audio.built);
        CHECK(audio.finite);          // no NaN or inf ever reaches the monitors
        CHECK(audio.peak > 0.001f);   // it actually makes a sound
        CHECK(audio.peak <= 1.0f);    // and the limiter holds
        CHECK(audio.rms > 0.00005f);  // not just a click at the start
    }
}

TEST_CASE(every_named_instrument_is_recognised_as_itself) {
    // The recipe for each must satisfy the checker for its own prompt. If it
    // does not, either the recipe is wrong or the checker is - and the model
    // would be sent round in circles chasing an unsatisfiable requirement.
    for (const auto& recipe : kOrchestra) {
        auto inst = patched(json::parse(recipe.patch));
        ir::IrReport r;
        ir::repair(inst, r);
        CHECK(physicalOk(recipe.prompt, inst));
    }
}

TEST_CASE(mallet_instruments_can_actually_be_inharmonic) {
    // The whole reason a glockenspiel sounds like struck metal is a non-integer
    // modulator ratio, and until the float-setting branch existed there was no
    // way to select one - the prompt asked for something impossible.
    auto inst = patched(json::parse(R"({"switches":{"osc_fm_ratio":3.5},
                                        "values":{"osc_fm_level":0.8}})"));
    const auto* fm = inst.findNode("osc_fm");
    CHECK(fm != nullptr);
    if (fm == nullptr) return;

    const auto ratio = fm->settings.value("ratio", 0.0);
    CHECK(ratio > 3.4);
    CHECK(ratio < 3.6);
    CHECK(std::abs(ratio - std::round(ratio)) > 0.1);   // genuinely inharmonic
}

TEST_CASE(a_struck_and_a_bowed_instrument_do_not_sound_the_same) {
    // Two instruments from different families must differ measurably, or the
    // recipes are decorative.
    const auto glock = render(patched(json::parse(kOrchestra[6].patch)), 84);
    const auto cello = render(patched(json::parse(kOrchestra[3].patch)), 45);

    CHECK(glock.built && cello.built);
    // A struck bell rings out and dies; a bowed note is held. Over a fixed
    // window that shows up as a real difference in sustained energy.
    CHECK(std::abs(glock.rms - cello.rms) > 0.001f);
}

TEST_CASE(instrument_names_are_matched_as_words_not_substrings) {
    // Two silent failures found this way: "flute" contains "lute", so a breathy
    // flute was told to build a plucked string; "harpsichord" contains "harp"
    // for the same reason.
    const auto plucked = patched(json::parse(R"({"values":{"str_level":0.8}})"));
    const auto fm      = patched(json::parse(R"({"values":{"osc_fm_level":0.8,"body_mix":0.2}})"));

    CHECK(physicalOk("a breathy flute", fm));         // wind, not plucked
    CHECK(physicalOk("a harpsichord", fm));           // struck, not plucked
    CHECK(physicalOk("a plucked harp", plucked));     // genuinely plucked

    // And a word inside another word never classifies at all.
    for (const auto& r : llm::check("embarrassed", fm).requirements)
        CHECK(r.id != "instrument.physical");         // not "brass"
}

TEST_CASE(the_whole_orchestra_is_classifiable) {
    // Every instrument the request named, plus the ones a producer would reach
    // for next. Each must be recognised as a real instrument family.
    const auto generic = patched(json::parse(
        R"({"values":{"str_level":0.7,"osc_fm_level":0.5,"body_mix":0.3,"vow_mix":0.2}})"));

    for (const char* name : {
            "acoustic guitar", "electric guitar", "bass", "ukulele", "mandolin",
            "sitar", "banjo", "harp", "koto", "lute",
            "violin", "viola", "cello", "double bass", "fiddle", "erhu",
            "piano", "rhodes", "wurlitzer", "clav", "harpsichord", "celesta",
            "glockenspiel", "vibraphone", "marimba", "xylophone", "kalimba",
            "bell", "chime", "gamelan", "steel drum",
            "flute", "clarinet", "oboe", "bassoon", "trumpet", "trombone",
            "sax", "brass", "voice", "choir",
            "organ", "hammond", "accordion", "harmonium", "bagpipe",
        }) {
        bool classified = false;
        for (const auto& r : llm::check(name, generic).requirements)
            if (r.id == "instrument.physical") classified = true;
        CHECK(classified);
    }
}
