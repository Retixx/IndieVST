#include "TestUtil.h"

#include "core/dsp/Registry.h"
#include "core/ir/Ir.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "core/ir/IrValidator.h"
#include "core/llm/CannedLibrary.h"

using namespace forge;

namespace {

std::string dumpErrors(const ir::IrReport& r) {
    std::string s;
    for (const auto& i : r.issues)
        if (i.level == ir::IrIssue::Level::Error)
            s += "\n      [" + i.path + "] " + i.message;
    return s;
}

/// A minimal instrument that validates, used as a base for mutation tests.
const char* kMinimal = R"({
  "ir_version": "0.1",
  "name": "Minimal",
  "voicing": "poly",
  "polyphony": 8,
  "nodes": [
    {"id": "osc", "type": "osc.analog", "scope": "voice", "settings": {"wave": "saw"}},
    {"id": "amp", "type": "vca", "scope": "voice"},
    {"id": "env", "type": "env.adsr", "scope": "voice"},
    {"id": "master", "type": "out.master", "scope": "global"}
  ],
  "audio": [
    {"from": "osc", "to": "amp", "gain": 1.0},
    {"from": "amp", "to": "master", "gain": 1.0}
  ],
  "mod": [{"source": "env", "target": "amp.gain", "depth": 1.0}],
  "params": [
    {"id": "attack", "label": "Attack", "unit": "ms", "min": 1, "max": 2000,
     "default": 5, "taper": "log", "group": "Amp",
     "bind": [{"node": "env", "param": "attack_ms"}]}
  ]
})";

bool validates(const std::string& json, ir::IrReport& report) {
    ir::Instrument inst;
    if (!ir::parse(json, inst, report)) return false;
    return ir::validate(inst, report);
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE(registry_is_populated) {
    const auto& reg = Registry::instance();
    CHECK(reg.all().size() >= 20);
    // Every module the prompt and the repairer depend on by name must exist.
    for (const char* type : {"osc.analog", "osc.wavetable", "osc.noise", "osc.fm2",
                             "env.adsr", "env.multi", "lfo", "mod.velocity",
                             "filter.svf", "filter.ladder", "filter.comb",
                             "shaper.wave", "vca", "mixer", "util.math",
                             "fx.delay", "fx.reverb", "fx.chorus", "fx.drive",
                             "out.master"}) {
        const bool found = reg.find(type) != nullptr;
        if (!found) test::note(std::string("missing module type: ") + type);
        CHECK(found);
    }
    // Every module must be instantiable and have a factory.
    for (const auto& m : reg.all()) {
        CHECK(m.factory != nullptr);
        CHECK(reg.create(m.type) != nullptr);
        CHECK(static_cast<int>(m.params.size()) <= kMaxParamsPerModule);
    }
}

TEST_CASE(manifest_json_is_wellformed) {
    const auto j = Registry::instance().capabilityJson();
    REQUIRE(j.contains("modules"));
    REQUIRE(j["modules"].is_array());
    CHECK(j["modules"].size() == Registry::instance().all().size());
    // Round-trips through a dump/parse without loss - it is embedded in a prompt.
    const std::string dumped = j.dump();
    const auto reparsed = nlohmann::json::parse(dumped, nullptr, false);
    CHECK(!reparsed.is_discarded());
}

TEST_CASE(minimal_instrument_validates) {
    ir::IrReport report;
    const bool ok = validates(kMinimal, report);
    if (!ok) test::note("errors:" + dumpErrors(report));
    CHECK(ok);
}

TEST_CASE(canned_library_all_validate) {
    // These are the few-shot examples in the prompt AND the offline fallback.
    // If one of them is wrong, every generation is poisoned.
    for (const auto& canned : forge::llm::cannedLibrary()) {
        ir::IrReport report;
        ir::Instrument inst;
        const bool parsed = ir::parse(canned.json, inst, report);
        if (!parsed) test::note(std::string(canned.id) + " parse failed:" + dumpErrors(report));
        REQUIRE(parsed);

        const bool valid = ir::validate(inst, report);
        if (!valid) test::note(std::string(canned.id) + " validation failed:" + dumpErrors(report));
        CHECK(valid);

        // And it must survive the safety pass without being rejected or
        // silently gutted by the CPU budget.
        ir::IrReport safetyReport;
        const bool safe = ir::applySafety(inst, safetyReport, 0.35f);
        if (!safe) test::note(std::string(canned.id) + " over budget:" + dumpErrors(safetyReport));
        CHECK(safe);

        const auto cost = ir::estimateCost(inst);
        test::note(std::string(canned.id) + " est. cpu "
                   + std::to_string(static_cast<int>(cost.cpuFraction * 100.0f)) + "%");
        CHECK(inst.polyphony >= 1);
    }
}

TEST_CASE(rejects_unknown_module_type) {
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.quantum","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"master"}],"params":[]})", report));
    CHECK(report.hasErrors());
}

TEST_CASE(rejects_bad_identifiers) {
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"Osc-Main","type":"osc.analog","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[],"params":[]})", report));
}

TEST_CASE(rejects_duplicate_node_ids) {
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"osc","type":"osc.noise","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[],"params":[]})", report));
}

TEST_CASE(rejects_illegal_feedback_loop) {
    // Two filters feeding each other with no delay in the loop.
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"f1","type":"filter.svf","scope":"voice"},
        {"id":"f2","type":"filter.svf","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"f1"},{"from":"f1","to":"f2"},
                 {"from":"f2","to":"f1"},{"from":"f2","to":"master"}],
        "params":[]})", report));
}

TEST_CASE(allows_feedback_through_delay) {
    ir::IrReport report;
    ir::Instrument inst;
    REQUIRE(ir::parse(R"({"name":"x","voicing":"poly","polyphony":4,"nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"mix","type":"mixer","scope":"global"},
        {"id":"dly","type":"fx.delay","scope":"global"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"mix"},
                 {"from":"mix","to":"dly"},{"from":"dly","to":"mix"},
                 {"from":"dly","to":"master"}],
        "mod":[{"source":"env","target":"amp.gain","depth":1.0}],
        "params":[]})", inst, report));
    const bool ok = ir::validate(inst, report);
    if (!ok) test::note("errors:" + dumpErrors(report));
    CHECK(ok);
}

TEST_CASE(rejects_global_feeding_voice) {
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"verb","type":"fx.reverb","scope":"global"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"verb"},
                 {"from":"verb","to":"amp"},{"from":"verb","to":"master"}],
        "params":[]})", report));
}

TEST_CASE(rejects_voice_modulator_on_global_target) {
    // One reverb, sixteen voices: which voice's LFO would win?
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"lfo1","type":"lfo","scope":"voice"},
        {"id":"verb","type":"fx.reverb","scope":"global"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"verb"},
                 {"from":"verb","to":"master"}],
        "mod":[{"source":"env","target":"amp.gain","depth":1.0},
               {"source":"lfo1","target":"verb.mix","depth":0.5}],
        "params":[]})", report));
}

TEST_CASE(rejects_modulating_a_non_modulator_source) {
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"master"}],
        "mod":[{"source":"osc","target":"amp.gain","depth":1.0}],
        "params":[]})", report));
}

TEST_CASE(rejects_out_of_range_setting) {
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice","settings":{"unison":99}},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"master"}],
        "params":[]})", report));
}

TEST_CASE(rejects_missing_required_asset) {
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.wavetable","scope":"voice","settings":{"table":"nope"}},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"master"}],
        "params":[]})", report));
}

TEST_CASE(rejects_log_taper_with_zero_min) {
    ir::IrReport report;
    CHECK(!validates(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"master"}],
        "params":[{"id":"lvl","label":"L","min":0,"max":1,"default":0.5,
                   "taper":"log","bind":[{"node":"osc","param":"level"}]}]})", report));
}

TEST_CASE(parser_rejects_oversized_and_deep_input) {
    {
        ir::IrReport report;
        nlohmann::json out;
        CHECK(!ir::parseJsonSafely(std::string(ir::kMaxIrBytes + 10, 'x'), out, report));
    }
    {
        ir::IrReport report;
        nlohmann::json out;
        std::string deep;
        for (int i = 0; i < 500; ++i) deep += "[";
        for (int i = 0; i < 500; ++i) deep += "]";
        CHECK(!ir::parseJsonSafely(deep, out, report));
    }
    {
        ir::IrReport report;
        nlohmann::json out;
        CHECK(!ir::parseJsonSafely("not json at all", out, report));
        CHECK(!ir::parseJsonSafely("", out, report));
        CHECK(!ir::parseJsonSafely("[1,2,3]", out, report));   // top level must be an object
    }
}

TEST_CASE(safety_clamps_hot_gain_staging) {
    ir::Instrument inst;
    ir::IrReport report;
    REQUIRE(ir::parse(kMinimal, inst, report));
    // Six sources at 3x each summing into one node.
    for (int i = 0; i < 6; ++i) inst.audio.push_back({"osc", "amp", 3.0f});
    ir::IrReport safety;
    ir::applySafety(inst, safety, 0.35f);

    float total = 0.0f;
    for (const auto& c : inst.audio) if (c.to == "amp") total += std::abs(c.gain);
    CHECK(total <= 8.01f);
    CHECK(safety.count(ir::IrIssue::Level::Fixed) > 0);
}

TEST_CASE(safety_reduces_polyphony_when_over_budget) {
    ir::Instrument inst;
    ir::IrReport report;
    REQUIRE(ir::parse(kMinimal, inst, report));
    inst.polyphony = 32;
    for (int i = 0; i < 6; ++i) {
        ir::NodeSpec n;
        n.id = "osc" + std::to_string(i);
        n.type = "osc.analog";
        n.scope = Scope::Voice;
        n.settings = nlohmann::json{{"unison", 7}};
        inst.nodes.push_back(n);
        inst.audio.push_back({n.id, "amp", 0.5f});
    }
    ir::IrReport safety;
    ir::applySafety(inst, safety, 0.35f);
    CHECK(inst.polyphony < 32);
    CHECK(ir::estimateCost(inst).cpuFraction <= 0.36f);
}

TEST_CASE(taper_round_trips) {
    struct { float mn, mx; Taper t; } cases[] = {
        {20.0f, 20000.0f, Taper::Log},
        {0.0f, 1.0f, Taper::Linear},
        {0.0f, 12.0f, Taper::Exp},
        {-100.0f, 100.0f, Taper::Linear},
    };
    for (const auto& c : cases) {
        for (float n = 0.0f; n <= 1.0f; n += 0.125f) {
            const float v  = applyTaper(n, c.mn, c.mx, c.t);
            const float n2 = removeTaper(v, c.mn, c.mx, c.t);
            CHECK(std::abs(n - n2) < 1.0e-3f);
        }
    }
}
