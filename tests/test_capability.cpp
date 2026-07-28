// ---------------------------------------------------------------------------
// Everything the engine can do must be reachable from a prompt.
//
// This project has now produced the same bug four times:
//
//   - `macros` had no field in the patch schema, so "four macros named Weight,
//     Bite, Motion, Space" was impossible rather than ignored.
//   - `osc.karplus` was in the module library and not in the rack, so every
//     request for a guitar or a bass came back a synthesiser.
//   - `osc_fm.ratio` had no control, so the non-integer ratios that make a
//     glockenspiel sound like struck metal could not be selected.
//   - `pitch_mod_semis` had no knob, so "pitch movement at the start of each
//     note" had nowhere to route to.
//
// Every one of them presented as the model ignoring an instruction. None of
// them were. A capability that exists in the engine and cannot be reached from
// a prompt is invisible, and invisible capability is indistinguishable from a
// disobedient model.
//
// So: this file fails the build when something is added to the engine and not
// wired up. Anything genuinely internal goes on the exemption list WITH A
// REASON, which forces the decision to be deliberate rather than forgotten.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/arch/Architecture.h"
#include "core/dsp/Registry.h"
#include "core/ir/IrRepair.h"
#include "core/llm/PromptBuilder.h"

#include <nlohmann/json.hpp>

#include <set>
#include <string>

using namespace forge;

namespace {

struct Exemption { const char* what; const char* why; };

/// Modules deliberately absent from the fixed rack.
const Exemption kModuleExemptions[] = {
    {"env.multi",        "Needs an authored envelope asset; the four ADSRs cover the "
                         "shapes a patch can currently describe."},
    {"filter.crossover", "Only useful as a pair feeding two parallel chains, which the "
                         "fixed serial rack has no way to express."},
    {"fx.freq_shifter",  "Non-harmonic frequency shifting is a special effect, not an "
                         "instrument-shaping tool; it would be dead weight on every page."},
    {"util.const",       "An offset source. The macro system covers the same ground and "
                         "is directly meaningful to a musician."},
    {"util.smooth",      "Slew is available on the LFOs themselves via `smooth`."},
};

/// Node parameters with no knob, on purpose.
const Exemption kParamExemptions[] = {
    {"amp.gain",        "Driven by the amplitude envelope. A knob here would fight it."},
    {"amp.pan",         "The pan node downstream owns this."},
    {"m_vel.bias",      "Modulation source offset; shaped by route depth instead."},
    {"m_key.bias",      "As above."},
    {"m_wheel.bias",    "As above."},
    {"m_press.bias",    "As above."},
    {"amt_fenv.a",      "This IS the modulation input of the amount stage."},
    {"amt_vib.a",       "As above."},
    {"osc_sub.detune_cents", "The sub is a single sine an octave down; detuning it just "
                             "makes the low end beat."},
    {"osc_sub.pulse_width",  "Only meaningful for pulse waves; the sub is a sine."},
};

/// Settings with no selector, on purpose.
const Exemption kSettingExemptions[] = {
    {"osc_wt.table",    "The wavetable is authored through the patch's `wavetable` block, "
                        "not chosen from a list."},
    {"osc_a.retrigger", "Free-running versus retriggered phase is a design decision the "
                        "rack makes once; exposing it invites confusion, not control."},
    {"osc_b.retrigger", "As above."},
    {"osc_wt.retrigger","As above."},
    {"osc_sub.retrigger","As above."},
    {"osc_sub.unison",  "Unison on a sine sub thickens nothing and costs voices."},
    {"osc_sub.semitones","The sub tracks the note; transposing it off the octave defeats "
                         "the point of a sub."},
    {"lfo_1.bipolar",   "Route polarity is set by the modulation depth's sign."},
    {"lfo_2.bipolar",   "As above."},
    {"lfo_3.bipolar",   "As above."},
    {"lfo_4.bipolar",   "As above."},
    {"lfo_1.retrigger", "Per-voice LFOs retrigger; global ones free-run. That is what "
                        "the scope already means."},
    {"lfo_2.retrigger", "As above."},
    {"lfo_3.retrigger", "As above."},
    {"lfo_4.retrigger", "As above."},
    {"seq.sync",        "The sequencer is always tempo-synced; free-running steps are a "
                        "different module."},
    {"amt_fenv.op",     "The amount stages multiply. Changing the operator would silently "
                        "change what every route through them means."},
    {"amt_vib.op",      "As above."},
    {"shaper.curve",    "Only meaningful with the 'curve' shaper type, which needs an "
                        "authored curve asset."},
};

bool exempt(const Exemption* list, size_t n, const std::string& key) {
    for (size_t i = 0; i < n; ++i) if (key == list[i].what) return true;
    return false;
}

} // namespace

TEST_CASE(every_module_is_reachable_or_deliberately_exempt) {
    const auto rack = arch::buildFullArchitecture();
    std::set<std::string> inRack;
    for (const auto& n : rack.nodes) inRack.insert(n.type);

    for (const auto& m : Registry::instance().all()) {
        if (inRack.count(m.type)) continue;
        const bool justified =
            exempt(kModuleExemptions, std::size(kModuleExemptions), m.type);
        // If this fires, either put the module in the rack or add it to
        // kModuleExemptions with a reason. Silently leaving it out is how a
        // Karplus-Strong string sat unused while every guitar came back a synth.
        CHECK(justified);
    }

    // And the exemption list must not rot: everything on it should still exist
    // and still be absent.
    for (const auto& e : kModuleExemptions) {
        CHECK(Registry::instance().find(e.what) != nullptr);
        CHECK(inRack.count(e.what) == 0);
        CHECK(std::string(e.why).size() > 20);   // a real reason, not a shrug
    }
}

TEST_CASE(every_node_parameter_has_a_knob_or_a_reason) {
    const auto rack = arch::buildFullArchitecture();
    const auto& registry = Registry::instance();

    for (const auto& node : rack.nodes) {
        const auto* man = registry.find(node.type);
        if (man == nullptr) continue;
        for (const auto& pd : man->params) {
            bool bound = false;
            for (const auto& p : rack.params)
                for (const auto& b : p.bind)
                    if (b.node == node.id && b.param == pd.id) bound = true;
            if (bound) continue;

            const auto key = node.id + "." + pd.id;
            CHECK(exempt(kParamExemptions, std::size(kParamExemptions), key));
        }
    }
}

TEST_CASE(every_setting_has_a_selector_or_a_reason) {
    const auto rack = arch::buildFullArchitecture();
    const auto& registry = Registry::instance();

    for (const auto& node : rack.nodes) {
        const auto* man = registry.find(node.type);
        if (man == nullptr) continue;
        for (const auto& sd : man->settings) {
            bool exposed = false;
            for (const auto& sw : rack.switches)
                if (sw.node == node.id && sw.setting == sd.id) exposed = true;
            if (exposed) continue;

            const auto key = node.id + "." + sd.id;
            CHECK(exempt(kSettingExemptions, std::size(kSettingExemptions), key));
        }
    }
}

TEST_CASE(every_control_in_the_rack_is_offered_to_the_model) {
    // A knob the prompt never mentions may as well not exist: the model cannot
    // set what it has not been shown.
    const auto rack   = arch::buildFullArchitecture();
    const auto system = llm::buildPatchSystemPrompt();

    for (const auto& p : rack.params)  CHECK(system.find(p.id) != std::string::npos);
    for (const auto& sw : rack.switches) CHECK(system.find(sw.id) != std::string::npos);

    // And every panel name, since those are what `layout` addresses.
    std::set<std::string> panels;
    for (const auto& p : rack.params)  if (!p.panel.empty()) panels.insert(p.panel);
    for (const auto& sw : rack.switches) if (!sw.panel.empty()) panels.insert(sw.panel);
    for (const auto& panel : panels) CHECK(system.find(panel) != std::string::npos);
}

TEST_CASE(every_modulation_source_is_offered_to_the_model) {
    const auto rack   = arch::buildFullArchitecture();
    const auto system = llm::buildPatchSystemPrompt();

    for (const auto& node : rack.nodes) {
        const auto* man = Registry::instance().find(node.type);
        if (man == nullptr || !man->isModSource) continue;
        CHECK(system.find(node.id) != std::string::npos);
    }
}

TEST_CASE(every_control_referenced_in_code_actually_exists) {
    // The offline fallback spent weeks writing to `dly_mix`, `drv_amount`,
    // `rev_mix`, `tape_amount` and `wid_amount` - none of which are real control
    // ids. `applyPatch` warns and moves on, so the whole effects section of the
    // safety-net instrument silently did nothing.
    //
    // Any patch built inside this codebase must resolve completely.
    const auto rack = arch::buildFullArchitecture();
    std::set<std::string> known;
    for (const auto& p : rack.params)   known.insert(p.id);
    for (const auto& sw : rack.switches) known.insert(sw.id);

    for (const char* prompt : {"warm analog bass", "nylon guitar", "a huge cinematic pad",
                               "gritty psych-rock lead", "deep sub bass", "bright bell",
                               "", "a cello"}) {
        const auto patch = arch::heuristicPatch(prompt);

        for (auto it = patch["values"].begin(); it != patch["values"].end(); ++it)
            CHECK(known.count(it.key()) == 1);
        for (auto it = patch["switches"].begin(); it != patch["switches"].end(); ++it)
            CHECK(known.count(it.key()) == 1);

        // Macro routes and panel names have to resolve too.
        if (patch.contains("macros"))
            for (const auto& m : patch["macros"])
                for (const auto& r : m.value("routes", nlohmann::json::array()))
                    CHECK(known.count(r.value("param", std::string())) == 1);
    }
}

TEST_CASE(the_offline_fallback_actually_changes_the_sound) {
    // The real symptom of the id bug: the patch applied cleanly and changed
    // nothing, because every effect id was wrong. Two very different requests
    // must produce measurably different instruments.
    auto build = [](const char* prompt) {
        auto inst = arch::buildFullArchitecture();
        ir::IrReport r;
        arch::applyPatch(inst, arch::heuristicPatch(prompt), r);
        return inst;
    };
    auto valueOf = [](const ir::Instrument& inst, const char* id) {
        for (const auto& p : inst.params) if (p.id == id) return p.def;
        return -1.0f;
    };

    const auto dry = build("a dry punchy sub bass");
    const auto wet = build("a huge cinematic ambient pad");
    CHECK(valueOf(wet, "rv_mix") > valueOf(dry, "rv_mix"));
    CHECK(valueOf(wet, "env_1_a") > valueOf(dry, "env_1_a"));

    const auto clean = build("a soft gentle bell");
    const auto dirty = build("a gritty distorted lead");
    CHECK(valueOf(dirty, "dr_drive") > valueOf(clean, "dr_drive"));
}

TEST_CASE(every_control_shape_in_a_generated_patch_addresses_a_real_control) {
    // The dead-reference trap, once more: styling a control that the layout
    // pass then removes is silently a no-op. The offline patch must only shape
    // controls that survive into the instrument it actually builds.
    for (const char* prompt : {"a warm bass", "nylon guitar", "a huge cinematic pad",
                               "bright glockenspiel", "gritty techno stab", "a cello"}) {
        auto inst = arch::buildFullArchitecture();
        ir::IrReport r;
        const auto patch = arch::heuristicPatch(prompt);
        arch::applyPatch(inst, patch, r);

        std::set<std::string> present;
        for (const auto& p : inst.params) present.insert(p.id);

        int shaped = 0;
        if (patch.contains("controls"))
            for (auto it = patch["controls"].begin(); it != patch["controls"].end(); ++it) {
                CHECK(present.count(it.key()) == 1);
                ++shaped;
            }
        CHECK(shaped >= 3);

        // And they really took effect.
        int nonKnob = 0;
        for (const auto& p : inst.params) if (p.control != "knob") ++nonKnob;
        CHECK(nonKnob >= 3);
    }
}
