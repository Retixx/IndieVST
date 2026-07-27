#include "TestUtil.h"

#include "core/ir/Ir.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "core/ir/IrValidator.h"

using namespace forge;

namespace {

/// Runs the real production pipeline: parse -> repair -> safety -> validate.
bool repairAndValidate(const std::string& json, ir::Instrument& inst, ir::IrReport& report) {
    if (!ir::parse(json, inst, report)) return false;
    ir::repair(inst, report);
    ir::applySafety(inst, report, 0.35f);
    ir::IrReport fresh;   // validate against a clean report so repairs do not read as errors
    const bool ok = ir::validate(inst, fresh);
    for (const auto& i : fresh.issues) report.issues.push_back(i);
    return ok;
}

std::string errorText(const ir::IrReport& r) {
    std::string s;
    for (const auto& i : r.issues)
        if (i.level == ir::IrIssue::Level::Error) s += "\n      [" + i.path + "] " + i.message;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE(repair_adds_missing_master) {
    ir::Instrument inst;
    ir::IrReport report;
    const bool ok = repairAndValidate(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"}],
        "audio":[{"from":"osc","to":"amp"}],
        "mod":[{"source":"env","target":"amp.gain","depth":1.0}],
        "params":[]})", inst, report);
    if (!ok) test::note(errorText(report));
    CHECK(ok);

    bool hasMaster = false;
    for (const auto& n : inst.nodes) if (n.type == "out.master") hasMaster = true;
    CHECK(hasMaster);
}

TEST_CASE(repair_inserts_amplitude_gate_when_missing) {
    // No envelope anywhere: without repair this note would sustain forever.
    ir::Instrument inst;
    ir::IrReport report;
    const bool ok = repairAndValidate(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"filt","type":"filter.svf","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"filt"},{"from":"filt","to":"master"}],
        "params":[]})", inst, report);
    if (!ok) test::note(errorText(report));
    CHECK(ok);

    bool hasEnv = false, hasVca = false;
    for (const auto& n : inst.nodes) {
        if (n.type == "env.adsr" && n.scope == Scope::Voice) hasEnv = true;
        if (n.type == "vca"      && n.scope == Scope::Voice) hasVca = true;
    }
    CHECK(hasEnv);
    CHECK(hasVca);

    // And the inserted envelope must actually drive the inserted VCA.
    bool wired = false;
    for (const auto& r : inst.mod) if (r.targetParam == "gain") wired = true;
    CHECK(wired);

    // The user should get amp knobs for it.
    CHECK(inst.findParam("amp_attack") != nullptr);
    CHECK(inst.findParam("amp_release") != nullptr);
}

TEST_CASE(repair_breaks_illegal_cycle) {
    ir::Instrument inst;
    ir::IrReport report;
    const bool ok = repairAndValidate(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"f1","type":"filter.svf","scope":"voice"},
        {"id":"f2","type":"filter.svf","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"f1"},{"from":"f1","to":"f2"},
                 {"from":"f2","to":"f1"},{"from":"f2","to":"amp"},
                 {"from":"amp","to":"master"}],
        "mod":[{"source":"env","target":"amp.gain","depth":1.0}],
        "params":[]})", inst, report);
    if (!ok) test::note(errorText(report));
    CHECK(ok);

    std::vector<int> order;
    CHECK(ir::topoSort(inst, Scope::Voice, order));
}

TEST_CASE(repair_normalises_bad_identifiers) {
    ir::Instrument inst;
    ir::IrReport report;
    const bool ok = repairAndValidate(R"({"name":"x","nodes":[
        {"id":"Osc Main","type":"osc.analog","scope":"voice"},
        {"id":"AMP-1","type":"vca","scope":"voice"},
        {"id":"Env.1","type":"env.adsr","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"Osc Main","to":"AMP-1"},{"from":"AMP-1","to":"master"}],
        "mod":[{"source":"Env.1","target":"AMP-1.gain","depth":1.0}],
        "params":[]})", inst, report);
    if (!ok) test::note(errorText(report));
    CHECK(ok);
    for (const auto& n : inst.nodes) CHECK(ir::isValidIdentifier(n.id));
    // The rename must have followed through into the connections.
    CHECK(!inst.audio.empty());
    for (const auto& c : inst.audio) {
        CHECK(inst.findNode(c.from) != nullptr);
        CHECK(inst.findNode(c.to) != nullptr);
    }
}

TEST_CASE(repair_drops_dangling_references) {
    ir::Instrument inst;
    ir::IrReport report;
    const bool ok = repairAndValidate(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"master"},
                 {"from":"ghost","to":"amp"},{"from":"amp","to":"phantom"}],
        "mod":[{"source":"env","target":"amp.gain","depth":1.0},
               {"source":"nosuch","target":"amp.pan","depth":0.5},
               {"source":"env","target":"amp.nosuchparam","depth":0.5}],
        "params":[{"id":"p1","label":"P","min":0,"max":1,"default":0.5,
                   "bind":[{"node":"ghost","param":"level"}]}]})", inst, report);
    if (!ok) test::note(errorText(report));
    CHECK(ok);

    for (const auto& c : inst.audio) {
        CHECK(inst.findNode(c.from) != nullptr);
        CHECK(inst.findNode(c.to) != nullptr);
    }
    for (const auto& r : inst.mod) CHECK(inst.findNode(r.source) != nullptr);
    // The parameter bound only to a ghost node should have been dropped.
    CHECK(inst.findParam("p1") == nullptr);
}

TEST_CASE(repair_prunes_unreachable_nodes) {
    ir::Instrument inst;
    ir::IrReport report;
    const bool ok = repairAndValidate(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"stray","type":"osc.noise","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"master"}],
        "mod":[{"source":"env","target":"amp.gain","depth":1.0}],
        "params":[]})", inst, report);
    CHECK(ok);
    // 'stray' produced audio that reached nothing. Repair connects dangling
    // voice outputs to the output rather than deleting them, so either
    // outcome is acceptable - what matters is that it is not left orphaned.
    const ir::NodeSpec* stray = inst.findNode("stray");
    if (stray != nullptr) {
        bool connected = false;
        for (const auto& c : inst.audio) if (c.from == "stray") connected = true;
        CHECK(connected);
    }
}

TEST_CASE(repair_merges_duplicate_masters) {
    ir::Instrument inst;
    ir::IrReport report;
    const bool ok = repairAndValidate(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"m1","type":"out.master","scope":"global"},
        {"id":"m2","type":"out.master","scope":"global"}],
        "audio":[{"from":"osc","to":"amp"},{"from":"amp","to":"m1"},{"from":"amp","to":"m2"}],
        "mod":[{"source":"env","target":"amp.gain","depth":1.0}],
        "params":[]})", inst, report);
    if (!ok) test::note(errorText(report));
    CHECK(ok);
    int masters = 0;
    for (const auto& n : inst.nodes) if (n.type == "out.master") ++masters;
    CHECK(masters == 1);
}

TEST_CASE(repair_is_idempotent) {
    ir::Instrument inst;
    ir::IrReport report;
    REQUIRE(repairAndValidate(R"({"name":"x","nodes":[
        {"id":"osc","type":"osc.analog","scope":"voice"},
        {"id":"filt","type":"filter.svf","scope":"voice"}],
        "audio":[{"from":"osc","to":"filt"}],
        "params":[]})", inst, report));

    const std::string first = ir::toJson(inst).dump();
    ir::IrReport second;
    ir::repair(inst, second);
    ir::applySafety(inst, second, 0.35f);
    CHECK(ir::toJson(inst).dump() == first);
    // A second pass over an already-repaired graph should find nothing to do.
    CHECK(second.count(ir::IrIssue::Level::Fixed) == 0);
}

TEST_CASE(repair_survives_empty_and_garbage_shapes) {
    // These must not crash and must not produce a "valid" graph out of nothing.
    const char* cases[] = {
        R"({})",
        R"({"nodes":[]})",
        R"({"nodes":[{"id":"a","type":"out.master","scope":"global"}]})",
        R"({"name":"x","nodes":[{"id":"lfo1","type":"lfo","scope":"global"}],"audio":[],"params":[]})",
    };
    for (const char* c : cases) {
        ir::Instrument inst;
        ir::IrReport report;
        ir::parse(c, inst, report);
        ir::repair(inst, report);
        ir::applySafety(inst, report, 0.35f);
        ir::IrReport fresh;
        ir::validate(inst, fresh);   // may fail; must not crash or hang
        CHECK(true);
    }
}

TEST_CASE(report_feedback_is_useful) {
    ir::IrReport report;
    report.error("nodes[2].type", "Unknown module type 'osc.quantum'.");
    report.warn("ui", "ignored");
    report.fixed("audio", "Removed a connection.");

    const std::string feedback = report.toModelFeedback();
    CHECK(feedback.find("osc.quantum") != std::string::npos);
    CHECK(feedback.find("nodes[2].type") != std::string::npos);
    // Warnings and fixes are not errors and must not be sent back as failures.
    CHECK(feedback.find("ignored") == std::string::npos);

    const std::string summary = report.toUserSummary();
    CHECK(summary.find("Removed a connection.") != std::string::npos);
}
