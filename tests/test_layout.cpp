// ---------------------------------------------------------------------------
// Model-authored layout.
//
// The model designs the front panel: which panels exist, how they group into
// pages, their order, colour and prominence. That is a lot of authority handed
// to untrusted input, so the interesting cases here are the hostile ones - a
// layout must never be able to produce an instrument that is silent, empty, or
// missing its output stage.
// ---------------------------------------------------------------------------
#include "core/arch/Architecture.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrValidator.h"
#include "TestUtil.h"

#include <nlohmann/json.hpp>
#include <set>

using namespace forge;
using json = nlohmann::json;

namespace {

struct Built {
    ir::Instrument inst;
    ir::IrReport   report;
};

Built build(const char* patchText) {
    Built b;
    b.inst = arch::buildFullArchitecture();
    arch::applyPatch(b.inst, json::parse(patchText), b.report);
    ir::repair(b.inst, b.report);
    return b;
}

std::set<std::string> panelsOf(const ir::Instrument& inst) {
    std::set<std::string> out;
    for (const auto& p : inst.params)   if (!p.panel.empty()) out.insert(p.panel);
    for (const auto& sw : inst.switches) if (!sw.panel.empty()) out.insert(sw.panel);
    return out;
}

bool hasNodeType(const ir::Instrument& inst, const std::string& prefix) {
    for (const auto& n : inst.nodes)
        if (n.type.rfind(prefix, 0) == 0) return true;
    return false;
}

} // namespace

TEST_CASE(layout_authors_pages_in_the_order_given) {
    auto b = build(R"({"layout":{"pages":[
        {"title":"body","panels":["OSC A","FILTER 1","ENV 1"]},
        {"title":"room","panels":["REVERB","DELAY","OUTPUT"]}]}})");

    CHECK(b.inst.ui.sections.size() == size_t(2));
    CHECK(b.inst.ui.sections[0].title == "BODY");   // uppercased for the tab bar
    CHECK(b.inst.ui.sections[1].title == "ROOM");
    // Controls follow their panel onto the page the model put it on.
    for (const auto& p : b.inst.params) {
        if (p.panel == "OSC A")  CHECK(p.group == "BODY");
        if (p.panel == "REVERB") CHECK(p.group == "ROOM");
    }
}

TEST_CASE(layout_removes_panels_it_does_not_place) {
    const auto full = arch::buildFullArchitecture();
    auto b = build(R"({"layout":{"pages":[
        {"title":"core","panels":["OSC A","FILTER 1","ENV 1","OUTPUT"]}]}})");

    CHECK(b.inst.nodes.size() < full.nodes.size());       // it really shrank
    CHECK(panelsOf(b.inst).size() < panelsOf(full).size());
    CHECK(panelsOf(b.inst).count("OSC A") == 1);
    CHECK(panelsOf(b.inst).count("PHASER") == 0);          // never placed
}

TEST_CASE(layout_cannot_remove_the_output_stage) {
    auto b = build(R"({"layout":{"pages":[
        {"title":"only","panels":["OSC A","FILTER 1","ENV 1","EQ"]}]}})");
    CHECK(hasNodeType(b.inst, "out."));
    CHECK(hasNodeType(b.inst, "vca"));
}

TEST_CASE(layout_cannot_remove_every_sound_source) {
    // Four legal panels, none of which makes a sound.
    auto b = build(R"({"layout":{"pages":[
        {"title":"silent","panels":["EQ","DRIVE","REVERB","DELAY"]}]}})");
    CHECK(hasNodeType(b.inst, "osc."));
}

TEST_CASE(near_empty_layout_falls_back_to_the_full_rack) {
    const auto full = arch::buildFullArchitecture();
    auto b = build(R"({"layout":{"pages":[{"title":"x","panels":["EQ"]}]}})");
    // A one-panel layout is a malformed response, not a minimal design.
    CHECK(b.inst.ui.sections.size() == full.ui.sections.size());
    CHECK(panelsOf(b.inst).size() == panelsOf(full).size());
}

TEST_CASE(layout_ignores_panels_that_do_not_exist) {
    auto b = build(R"({"layout":{"pages":[
        {"title":"a","panels":["OSC A","GRANULAR CLOUD","FILTER 1","ENV 1","OUTPUT"]}]}})");
    CHECK(b.inst.ui.sections.size() == size_t(1));
    CHECK(panelsOf(b.inst).count("GRANULAR CLOUD") == 0);
}

TEST_CASE(panel_names_are_matched_loosely) {
    // "filter 1", "FILTER 1" and "Filter1" all mean the same panel.
    auto b = build(R"({"layout":{"pages":[
        {"title":"a","panels":["osc a","Filter1","env 1","OuTpUt"]}],
        "panel_accent":{"filter 1":"#C9A227"},
        "featured":["osc  a"]}})");
    CHECK(panelsOf(b.inst).count("FILTER 1") == 1);
    CHECK(b.inst.ui.panelAccent.count("FILTER 1") == 1);
    CHECK(b.inst.ui.featuredPanels.size() == size_t(1));
    CHECK(b.inst.ui.featuredPanels[0] == "OSC A");
}

TEST_CASE(layout_caps_the_page_count) {
    auto b = build(R"({"layout":{"pages":[
        {"title":"p1","panels":["OSC A"]},   {"title":"p2","panels":["OSC B"]},
        {"title":"p3","panels":["FILTER 1"]},{"title":"p4","panels":["FILTER 2"]},
        {"title":"p5","panels":["ENV 1"]},   {"title":"p6","panels":["ENV 2"]},
        {"title":"p7","panels":["LFO 1"]},   {"title":"p8","panels":["REVERB"]}]}})");
    CHECK(b.inst.ui.sections.size() <= size_t(6));
}

TEST_CASE(a_laid_out_instrument_still_validates_and_builds) {
    auto b = build(R"({"layout":{"pages":[
        {"title":"cloud","panels":["WAVETABLE","OSC A","OSC B","FILTER 1","ENV 1"]},
        {"title":"drift","panels":["LFO 1","LFO 2","ENV 2","SEQUENCER"]},
        {"title":"air","panels":["REVERB","DELAY","EQ","OUTPUT","MACROS"]}],
        "featured":["WAVETABLE","REVERB"],
        "panel_accent":{"WAVETABLE":"#8FB6D8","REVERB":"#6E7F94"}}})");

    ir::IrReport vr;
    CHECK(ir::validate(b.inst, vr));
    CHECK(!vr.hasErrors());
    CHECK(b.inst.ui.sections.size() == size_t(3));
    // Every param the sections reference must still exist after the drop pass;
    // a dangling id would build a tab with holes in it.
    for (const auto& sec : b.inst.ui.sections)
        for (const auto& id : sec.params)
            CHECK(b.inst.findParam(id) != nullptr);
}

TEST_CASE(a_page_with_too_few_controls_is_folded_away) {
    // Exactly the shape that shipped an OSCILLATORS tab holding two selectors.
    auto b = build(R"({"layout":{"pages":[
        {"title":"engine","panels":["OSC A","OSC B","FILTER 1","ENV 1"]},
        {"title":"stray","panels":["SUB"]},
        {"title":"air","panels":["REVERB","DELAY","OUTPUT"]}]}})");

    for (const auto& sec : b.inst.ui.sections)
        CHECK(sec.title != "STRAY");

    // Folded, not deleted: SUB's controls still exist somewhere.
    bool sawSub = false;
    for (const auto& p : b.inst.params) if (p.panel == "SUB") sawSub = true;
    CHECK(sawSub);

    // And every surviving page carries a worthwhile number of controls.
    for (const auto& sec : b.inst.ui.sections) CHECK(sec.params.size() >= size_t(6));
}

TEST_CASE(references_are_kept_so_the_ui_can_show_them_back) {
    auto b = build(R"({"name":"Gorgeous Lead","references":[
        "Kanye West, Gorgeous (2010) - the driven mid-forward guitar motif",
        "Late-60s psych-rock slapback"]})");

    auto it = b.inst.meta.find("references");
    CHECK(it != b.inst.meta.end());
    CHECK(it->is_array());
    CHECK(it->size() == size_t(2));
    CHECK((*it)[0].get<std::string>().find("Kanye") != std::string::npos);
}

TEST_CASE(references_are_bounded_and_type_checked) {
    // Untrusted input: a hostile or confused response must not be able to put
    // a megabyte of text, or a nested object, into a tooltip.
    auto b = build(R"({"references":[
        "a", "b", "c", "d", "e", "f", "g", "h", 42, {"nested":true}, null]})");
    auto it = b.inst.meta.find("references");
    CHECK(it != b.inst.meta.end());
    CHECK(it->size() <= size_t(6));
    for (const auto& r : *it) CHECK(r.is_string());
}

TEST_CASE(no_references_leaves_meta_clean) {
    auto b = build(R"({"name":"Plain"})");
    CHECK(b.inst.meta.find("references") == b.inst.meta.end());
}

// --- the offline floor ------------------------------------------------------
//
// When the model cannot be reached or its answer is unusable, this is what the
// musician sees. It has to be a complete, laid-out, valid instrument every
// time - the previous behaviour dropped a hand-authored graph with a dozen
// knobs on screen, which reads as a broken product rather than a degraded one.

namespace {

Built buildHeuristic(const char* prompt) {
    Built b;
    b.inst = arch::buildFullArchitecture();
    arch::applyPatch(b.inst, arch::heuristicPatch(prompt), b.report);
    ir::repair(b.inst, b.report);
    return b;
}

} // namespace

TEST_CASE(the_offline_fallback_is_always_a_complete_instrument) {
    for (const char* prompt : {"warm analog bass", "floating through the clouds violin",
                               "gritty psych-rock guitar lead", "glassy bell", "",
                               "asdfgh qwerty", "a huge cinematic pad", "808 sub"}) {
        auto b = buildHeuristic(prompt);

        ir::IrReport check;
        CHECK(ir::validate(b.inst, check));
        CHECK(!check.hasErrors());

        // Laid out, not a wall - and never the three-knob page.
        CHECK(b.inst.ui.sections.size() >= size_t(2));
        for (const auto& sec : b.inst.ui.sections) CHECK(sec.params.size() >= size_t(6));
        CHECK(b.inst.params.size() >= size_t(25));
        CHECK(hasNodeType(b.inst, "osc."));
        CHECK(hasNodeType(b.inst, "out."));
    }
}

TEST_CASE(the_offline_fallback_is_deterministic) {
    const auto a = arch::heuristicPatch("warm dusty analog bass");
    const auto b = arch::heuristicPatch("warm dusty analog bass");
    CHECK(a == b);
}

TEST_CASE(the_offline_fallback_reads_the_prompt) {
    const auto bass   = arch::heuristicPatch("deep sub bass");
    const auto bright = arch::heuristicPatch("bright glassy bell");
    const auto pad    = arch::heuristicPatch("a huge cinematic pad");

    // Darker material really is filtered lower.
    CHECK(bass["values"]["f1_cutoff"].get<double>()
          < bright["values"]["f1_cutoff"].get<double>());
    // A pad swells; a bell does not.
    CHECK(pad["values"]["env_1_a"].get<double>()
          > bright["values"]["env_1_a"].get<double>());
    // A dry bass does not arrive drenched in reverb.
    CHECK(bass["values"]["rv_mix"].get<double>()
          < pad["values"]["rv_mix"].get<double>());
}

TEST_CASE(the_offline_fallback_does_not_make_chordal_things_mono) {
    for (const char* prompt : {"electric guitar lead", "rhodes piano", "warm string pad",
                               "bright bell melody", "organ riff"}) {
        CHECK(arch::heuristicPatch(prompt)["voicing"] == "poly");
    }
    // But a genuine monosynth line still gets glide and one voice.
    const auto acid = arch::heuristicPatch("a squelchy 303 acid bass");
    CHECK(acid["voicing"] == "mono");
    CHECK(acid["polyphony"] == 1);
}
