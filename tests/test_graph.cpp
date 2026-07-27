#include "TestUtil.h"

#include "core/dsp/GraphBuilder.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "core/ir/IrValidator.h"
#include "core/llm/CannedLibrary.h"
#include "core/llm/PromptBuilder.h"
#include "core/llm/ResponseExtract.h"

using namespace forge;

namespace {

std::unique_ptr<GraphInstance> build(const std::string& json, ir::IrReport& report) {
    ir::Instrument inst;
    if (!ir::parse(json, inst, report)) return nullptr;
    ir::repair(inst, report);
    ir::applySafety(inst, report, 0.35f);
    return GraphBuilder::build(inst, 48000.0, report);
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE(topo_sort_respects_signal_order) {
    ir::Instrument inst;
    ir::IrReport report;
    REQUIRE(ir::parse(R"({"name":"x","nodes":[
        {"id":"c","type":"vca","scope":"voice"},
        {"id":"a","type":"osc.analog","scope":"voice"},
        {"id":"b","type":"filter.svf","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
        "audio":[{"from":"a","to":"b"},{"from":"b","to":"c"},{"from":"c","to":"master"}],
        "params":[]})", inst, report));

    std::vector<int> order;
    REQUIRE(ir::topoSort(inst, Scope::Voice, order));
    REQUIRE(order.size() == 3);

    // a must come before b, which must come before c, regardless of the order
    // they were declared in.
    int posA = -1, posB = -1, posC = -1;
    for (size_t i = 0; i < order.size(); ++i) {
        const std::string& id = inst.nodes[static_cast<size_t>(order[i])].id;
        if (id == "a") posA = static_cast<int>(i);
        if (id == "b") posB = static_cast<int>(i);
        if (id == "c") posC = static_cast<int>(i);
    }
    CHECK(posA < posB);
    CHECK(posB < posC);
}

TEST_CASE(global_lfo_can_modulate_voice_parameters) {
    // The one cross-scope route that is legal, and the reason pads breathe.
    const char* json = R"({
      "name":"Drift","voicing":"poly","polyphony":4,
      "nodes":[
        {"id":"o1","type":"osc.analog","scope":"voice","settings":{"wave":"saw"}},
        {"id":"filt","type":"filter.svf","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"lfo1","type":"lfo","scope":"global","settings":{"wave":"sine"}},
        {"id":"master","type":"out.master","scope":"global"}],
      "audio":[{"from":"o1","to":"filt"},{"from":"filt","to":"amp"},
               {"from":"amp","to":"master"}],
      "mod":[{"source":"env","target":"amp.gain","depth":1.0},
             {"source":"lfo1","target":"filt.cutoff","depth":0.6}],
      "params":[
        {"id":"rate","label":"Rate","unit":"Hz","min":0.1,"max":20,"default":8,"taper":"log",
         "bind":[{"node":"lfo1","param":"rate_hz"}]},
        {"id":"sus","label":"S","min":0,"max":1,"default":1,
         "bind":[{"node":"env","param":"sustain"}]}]})";

    ir::IrReport report;
    auto graph = build(json, report);
    REQUIRE(graph != nullptr);

    graph->noteOn(60, 1.0f);
    std::vector<float> l(512), r(512);
    float* chans[2] = { l.data(), r.data() };

    // A fast LFO on the cutoff should make the block-to-block energy vary.
    float minRms = 1.0e9f, maxRms = 0.0f;
    for (int block = 0; block < 60; ++block) {
        graph->process(chans, 512);
        double sq = 0.0;
        for (int i = 0; i < 512; ++i) sq += static_cast<double>(l[static_cast<size_t>(i)]) * l[static_cast<size_t>(i)];
        const float rms = static_cast<float>(std::sqrt(sq / 512.0));
        if (block < 8) continue;   // let the attack settle
        minRms = std::min(minRms, rms);
        maxRms = std::max(maxRms, rms);
    }
    test::note("lfo rms range " + std::to_string(minRms) + " .. " + std::to_string(maxRms));
    CHECK(maxRms > minRms * 1.1f);
}

TEST_CASE(polyphony_and_voice_stealing_stay_bounded) {
    ir::IrReport report;
    auto graph = build(forge::llm::cannedLibrary()[1].json, report);   // pad, poly
    REQUIRE(graph != nullptr);

    std::vector<float> l(256), r(256);
    float* chans[2] = { l.data(), r.data() };

    // Far more notes than voices, held down together.
    for (int n = 24; n < 96; ++n) {
        graph->noteOn(n, 0.9f);
        graph->process(chans, 64);
    }
    CHECK(graph->activeVoiceCount() <= kMaxVoices);

    float peak = 0.0f;
    for (int block = 0; block < 200; ++block) {
        graph->process(chans, 256);
        for (int i = 0; i < 256; ++i) peak = std::max(peak, std::abs(l[static_cast<size_t>(i)]));
    }
    CHECK(peak <= 1.0f);
    CHECK(!graph->nanGuardTripped());

    graph->allNotesOff(false);
    for (int block = 0; block < 2000; ++block) graph->process(chans, 256);
    CHECK(graph->activeVoiceCount() == 0);
}

TEST_CASE(block_size_does_not_change_the_result) {
    // The engine chops host blocks into 16-sample control blocks. If that
    // splitting is wrong, the sound changes with the host's buffer size - a
    // bug that only appears on someone else's machine.
    ir::IrReport r1, r2;
    auto a = build(forge::llm::cannedLibrary()[0].json, r1);
    auto b = build(forge::llm::cannedLibrary()[0].json, r2);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    std::vector<float> al(4096), ar(4096), bl(4096), br(4096);
    float* ac[2] = { al.data(), ar.data() };
    float* bc[2] = { bl.data(), br.data() };

    a->noteOn(45, 0.8f);
    b->noteOn(45, 0.8f);

    std::vector<float> aOut, bOut;
    for (int i = 0; i < 16; ++i) {         // 16 x 256
        a->process(ac, 256);
        aOut.insert(aOut.end(), al.begin(), al.begin() + 256);
    }
    for (int i = 0; i < 4; ++i) {          // 4 x 1024, same total
        b->process(bc, 1024);
        bOut.insert(bOut.end(), bl.begin(), bl.begin() + 1024);
    }

    REQUIRE(aOut.size() == bOut.size());
    double worst = 0.0;
    for (size_t i = 0; i < aOut.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::abs(aOut[i] - bOut[i])));
    test::note("max block-size difference " + std::to_string(worst));
    CHECK(worst < 1.0e-6);
}

TEST_CASE(prompt_contains_every_module_and_stays_reasonable) {
    const std::string system = forge::llm::buildSystemPrompt();

    for (const auto& m : Registry::instance().all()) {
        const bool present = system.find(m.type) != std::string::npos;
        if (!present) test::note("module missing from prompt: " + m.type);
        CHECK(present);
    }

    CHECK(system.find("out.master") != std::string::npos);
    CHECK(system.find("util.math") != std::string::npos);

    const int tokens = forge::llm::estimateTokens(system);
    test::note("system prompt ~" + std::to_string(tokens) + " tokens");
    // Sending this on every generation, so keep an eye on it.
    CHECK(tokens < 22000);

    const auto spec = forge::llm::buildGenerationPrompt("warm analog bass");
    CHECK(spec.user.find("warm analog bass") != std::string::npos);

    const auto edit = forge::llm::buildGenerationPrompt("make it darker", "{\"name\":\"x\"}");
    CHECK(edit.user.find("currently loaded") != std::string::npos);
    CHECK(edit.user.find("make it darker") != std::string::npos);
}

TEST_CASE(schema_is_valid_json) {
    const auto parsed = nlohmann::json::parse(forge::llm::irJsonSchema(), nullptr, false);
    CHECK(!parsed.is_discarded());
    CHECK(parsed.contains("properties"));
}

TEST_CASE(response_extraction_handles_real_world_shapes) {
    using forge::llm::extractJsonObject;

    CHECK(extractJsonObject("{\"a\":1}") == "{\"a\":1}");
    CHECK(extractJsonObject("Here you go:\n```json\n{\"a\":1}\n```\nEnjoy!") == "{\"a\":1}");
    CHECK(extractJsonObject("{\"a\":\"}\"}") == "{\"a\":\"}\"}");
    CHECK(extractJsonObject("{\"a\":{\"b\":[1,2]},\"c\":3} trailing") == "{\"a\":{\"b\":[1,2]},\"c\":3}");
    CHECK(extractJsonObject("no json here").empty());
    CHECK(extractJsonObject("{\"unbalanced\": ").empty());
    CHECK(extractJsonObject("{\"esc\":\"a\\\"}\"}") == "{\"esc\":\"a\\\"}\"}");

    std::string err;
    const std::string anthropic = R"({"content":[{"type":"text","text":"{\"name\":\"X\"}"}]})";
    CHECK(forge::llm::extractAnthropicContent(anthropic, err) == "{\"name\":\"X\"}");
    CHECK(err.empty());

    const std::string toolUse = R"({"content":[{"type":"tool_use","input":{"name":"Y"}}]})";
    CHECK(forge::llm::extractAnthropicContent(toolUse, err).find("\"Y\"") != std::string::npos);

    const std::string apiError = R"({"error":{"message":"invalid x-api-key"}})";
    forge::llm::extractAnthropicContent(apiError, err);
    CHECK(err.find("invalid x-api-key") != std::string::npos);

    const std::string openai = R"({"choices":[{"message":{"content":"{\"name\":\"Z\"}"}}]})";
    CHECK(forge::llm::extractOpenAiContent(openai, err) == "{\"name\":\"Z\"}");

    const std::string ollama = R"({"message":{"content":"{\"name\":\"O\"}"}})";
    CHECK(forge::llm::extractOllamaContent(ollama, err) == "{\"name\":\"O\"}");
}

TEST_CASE(canned_matching_routes_prompts_sensibly) {
    struct { const char* prompt; const char* expect; } cases[] = {
        {"warm analog bass with a slow filter sweep", "molten_sub"},
        {"a wide lush pad, ambient and airy",         "glass_drift"},
        {"short percussive pluck for arps",           "porcelain_pluck"},
        {"screaming acid lead solo",                  "neon_lead"},
        {"glassy metallic bell",                      "cathedral_bell"},
        {"dark cinematic drone texture",              "tectonic_drone"},
    };
    for (const auto& c : cases) {
        const auto* match = forge::llm::bestCannedMatch(c.prompt);
        REQUIRE(match != nullptr);
        if (std::string(match->id) != c.expect)
            test::note(std::string("'") + c.prompt + "' -> " + match->id
                       + " (expected " + c.expect + ")");
        CHECK(std::string(match->id) == c.expect);
    }
    // Never returns nullptr, even for nonsense.
    CHECK(forge::llm::bestCannedMatch("") != nullptr);
    CHECK(forge::llm::bestCannedMatch("zzzzz") != nullptr);
}

TEST_CASE(ir_round_trips_through_json) {
    for (const auto& canned : forge::llm::cannedLibrary()) {
        ir::Instrument a;
        ir::IrReport report;
        REQUIRE(ir::parse(canned.json, a, report));

        const std::string serialised = ir::toJson(a).dump();
        ir::Instrument b;
        ir::IrReport report2;
        REQUIRE(ir::parse(serialised, b, report2));

        CHECK(a.nodes.size()  == b.nodes.size());
        CHECK(a.audio.size()  == b.audio.size());
        CHECK(a.mod.size()    == b.mod.size());
        CHECK(a.params.size() == b.params.size());
        CHECK(a.macros.size() == b.macros.size());
        CHECK(a.name == b.name);
        CHECK(a.polyphony == b.polyphony);
        // And the re-parsed copy must still validate.
        ir::IrReport v;
        CHECK(ir::validate(b, v));
    }
}
