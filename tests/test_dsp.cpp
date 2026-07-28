#include "TestUtil.h"

#include "core/dsp/AssetBaker.h"
#include "core/dsp/GraphBuilder.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"
#include "core/llm/CannedLibrary.h"

#include <cmath>
#include <vector>

using namespace forge;

namespace {

constexpr double kSr = 48000.0;

struct RenderResult {
    float peak = 0.0f;
    float rms  = 0.0f;
    float dc   = 0.0f;
    bool  finite = true;
    std::vector<float> left;

    /// Ratio of first-difference energy to total energy. A crude but reliable
    /// spectral-centroid proxy - and unlike RMS it is not defeated by the
    /// master limiter, which flattens level differences on purpose.
    float brightness() const {
        if (left.size() < 3) return 0.0f;
        double sq = 0.0, dsq = 0.0;
        for (size_t i = 1; i < left.size(); ++i) {
            const double d = static_cast<double>(left[i]) - left[i - 1];
            dsq += d * d;
            sq  += static_cast<double>(left[i]) * left[i];
        }
        return static_cast<float>(std::sqrt(dsq / std::max(sq, 1.0e-12)));
    }
};

std::unique_ptr<GraphInstance> buildFromJson(const std::string& json, ir::IrReport& report) {
    ir::Instrument inst;
    if (!ir::parse(json, inst, report)) return nullptr;
    ir::repair(inst, report);
    ir::applySafety(inst, report, 0.35f);
    return GraphBuilder::build(inst, kSr, report);
}

/// Plays a chord and renders `seconds` of audio in irregular block sizes,
/// because a bug that only shows up when the host changes buffer size is
/// exactly the bug that ruins a live demo.
RenderResult render(GraphInstance& g, const std::vector<int>& notes, double seconds,
                    double releaseAt = -1.0, bool keepTail = true) {
    RenderResult r;
    const int total = static_cast<int>(kSr * seconds);
    const int blockSizes[] = {64, 128, 37, 512, 16, 256, 1};
    int blockCursor = 0;

    std::vector<float> l(1024), rch(1024);
    float* chans[2] = { l.data(), rch.data() };

    for (int n : notes) g.noteOn(n, 0.85f);

    bool released = false;
    int done = 0;
    double sum = 0.0, sumSq = 0.0;

    while (done < total) {
        const int block = std::min(blockSizes[blockCursor++ % 7], total - done);
        if (block <= 0) break;

        if (!released && releaseAt >= 0.0
            && static_cast<double>(done) / kSr >= releaseAt) {
            for (int n : notes) g.noteOff(n);
            released = true;
        }

        g.process(chans, block);

        for (int i = 0; i < block; ++i) {
            const float s = l[static_cast<size_t>(i)];
            const float s2 = rch[static_cast<size_t>(i)];
            if (!isFinite(s) || !isFinite(s2)) r.finite = false;
            r.peak = std::max(r.peak, std::max(std::abs(s), std::abs(s2)));
            sum += s;
            sumSq += static_cast<double>(s) * s;
            if (r.left.size() < 96000) r.left.push_back(s);
        }
        done += block;
    }

    (void) keepTail;
    r.rms = static_cast<float>(std::sqrt(sumSq / std::max(1, done)));
    r.dc  = static_cast<float>(sum / std::max(1, done));
    return r;
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE(every_canned_instrument_builds_and_sounds) {
    for (const auto& canned : forge::llm::cannedLibrary()) {
        ir::IrReport report;
        auto graph = buildFromJson(canned.json, report);
        if (graph == nullptr) {
            std::string errs;
            for (const auto& i : report.issues)
                if (i.level == ir::IrIssue::Level::Error) errs += "\n      " + i.message;
            test::note(std::string(canned.id) + " failed to build:" + errs);
        }
        REQUIRE(graph != nullptr);

        const auto res = render(*graph, {48, 55, 60}, 1.5, 1.0);
        CHECK(res.finite);
        // It must actually make a sound...
        if (res.rms <= 1.0e-4f) test::note(std::string(canned.id) + " was silent");
        CHECK(res.rms > 1.0e-4f);
        // ...and it must not exceed the master ceiling.
        if (res.peak > 1.0f) test::note(std::string(canned.id) + " peak "
                                        + std::to_string(res.peak));
        CHECK(res.peak <= 1.0f);
        // ...without a DC offset riding under it.
        CHECK(std::abs(res.dc) < 0.02f);

        CHECK(!graph->nanGuardTripped());
        test::note(std::string(canned.id) + " peak " + std::to_string(res.peak)
                   + " rms " + std::to_string(res.rms)
                   + " dc " + std::to_string(res.dc));
    }
}

// KNOWN FAILURE - regressed during the Karplus/macro work, cause not isolated.
//
// `porcelain_pluck` holds its voice indefinitely after noteOff: the output sits
// at a flat 0.4665 for at least seven seconds instead of releasing over its
// 300 ms release time. Established so far:
//
//   - It is the VOICE, not the effects. Zeroing delay_mix, delay_fb and
//     reverb_mix changes nothing (0.4665 either way).
//   - It is NOT the macro change. The failure reproduces identically with the
//     old additive-from-zero macro semantics and with the new neutral-at-rest
//     ones, verified by rebuilding both ways.
//   - This instrument contains no Karplus string, so the string rewrite cannot
//     be reaching it directly.
//
// A flat, non-decaying level means the amplitude envelope is stuck in SUSTAIN,
// not in a slow release: `amp.gain` has a modBase of 0 and is driven solely by
// `env_amp`, so a constant output can only mean a constant envelope value.
// `noteOff` is therefore not reaching that envelope.
//
// Reading of GraphInstance::noteOff has not found the reason. It matches on
// `v.active && v.note == midiNote && v.held`, all of which should hold here,
// and `sustain_` is properly initialised to false so the pedal early-out is not
// firing. The next step is to print `v.held`, `v.note` and `v.ctx.gate` at the
// moment noteOff is called - a five-line change that was not worth starting
// with no room left to act on the answer.
//
// Left failing on purpose: a voice that is never handed back means polyphony
// leaks and the instrument stops responding after N notes.
TEST_CASE(voices_are_released_and_reclaimed) {
    ir::IrReport report;
    auto graph = buildFromJson(forge::llm::cannedLibrary()[2].json, report);  // pluck
    REQUIRE(graph != nullptr);

    graph->noteOn(60, 1.0f);
    render(*graph, {}, 0.05);
    CHECK(graph->activeVoiceCount() >= 1);

    graph->noteOff(60);
    render(*graph, {}, 6.0);
    // After a long silence every voice must have been handed back, otherwise
    // polyphony leaks and the instrument stops responding after N notes.
    CHECK(graph->activeVoiceCount() == 0);
}

TEST_CASE(silence_in_silence_out) {
    ir::IrReport report;
    auto graph = buildFromJson(forge::llm::cannedLibrary()[0].json, report);
    REQUIRE(graph != nullptr);

    const auto res = render(*graph, {}, 0.5);
    CHECK(res.finite);
    CHECK(res.peak < 1.0e-5f);
}

TEST_CASE(output_is_deterministic) {
    // Same instrument, same seed, same notes, same block sizes -> identical
    // samples. Without this, nothing about the engine is debuggable.
    ir::IrReport r1, r2;
    auto a = buildFromJson(forge::llm::cannedLibrary()[1].json, r1);
    auto b = buildFromJson(forge::llm::cannedLibrary()[1].json, r2);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    const auto ra = render(*a, {60, 64, 67}, 0.75);
    const auto rb = render(*b, {60, 64, 67}, 0.75);
    REQUIRE(ra.left.size() == rb.left.size());

    size_t mismatches = 0;
    for (size_t i = 0; i < ra.left.size(); ++i)
        if (ra.left[i] != rb.left[i]) ++mismatches;
    CHECK(mismatches == 0);
}

TEST_CASE(master_limiter_holds_under_abuse) {
    // Six hot oscillators, unity gains, no envelope decay: the graph itself is
    // trying to clip. The output stage must not let it.
    const char* json = R"({
      "name":"Abuse","voicing":"poly","polyphony":8,
      "nodes":[
        {"id":"o1","type":"osc.analog","scope":"voice","settings":{"wave":"saw","unison":5}},
        {"id":"o2","type":"osc.analog","scope":"voice","settings":{"wave":"square"}},
        {"id":"o3","type":"osc.analog","scope":"voice","settings":{"wave":"saw","octave":1}},
        {"id":"o4","type":"osc.fm2","scope":"voice","settings":{"ratio":1.5}},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"drv","type":"fx.drive","scope":"global"},
        {"id":"master","type":"out.master","scope":"global"}],
      "audio":[
        {"from":"o1","to":"amp","gain":2.0},{"from":"o2","to":"amp","gain":2.0},
        {"from":"o3","to":"amp","gain":2.0},{"from":"o4","to":"amp","gain":2.0},
        {"from":"amp","to":"drv","gain":2.0},{"from":"drv","to":"master","gain":2.0}],
      "mod":[{"source":"env","target":"amp.gain","depth":1.0}],
      "params":[
        {"id":"sus","label":"S","min":0,"max":1,"default":1.0,
         "bind":[{"node":"env","param":"sustain"}]},
        {"id":"drive","label":"D","min":1,"max":20,"default":20,"taper":"log",
         "bind":[{"node":"drv","param":"drive"}]}]})";

    ir::IrReport report;
    auto graph = buildFromJson(json, report);
    REQUIRE(graph != nullptr);

    const auto res = render(*graph, {36, 43, 48, 55, 60, 64, 67, 72}, 2.0);
    CHECK(res.finite);
    test::note("abuse peak " + std::to_string(res.peak));
    CHECK(res.peak <= 1.0f);
    CHECK(!graph->nanGuardTripped());
}

TEST_CASE(resonant_filters_do_not_explode) {
    const char* json = R"({
      "name":"Screaming","voicing":"poly","polyphony":4,
      "nodes":[
        {"id":"o1","type":"osc.analog","scope":"voice","settings":{"wave":"square"}},
        {"id":"lad","type":"filter.ladder","scope":"voice"},
        {"id":"svf","type":"filter.svf","scope":"voice","settings":{"mode":"bp","slope":"24"}},
        {"id":"cmb","type":"filter.comb","scope":"voice"},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
      "audio":[{"from":"o1","to":"lad"},{"from":"lad","to":"svf"},
               {"from":"svf","to":"cmb"},{"from":"cmb","to":"amp"},
               {"from":"amp","to":"master"}],
      "mod":[{"source":"env","target":"amp.gain","depth":1.0}],
      "params":[
        {"id":"r1","label":"R1","min":0,"max":0.95,"default":0.95,
         "bind":[{"node":"lad","param":"resonance"}]},
        {"id":"r2","label":"R2","min":0,"max":0.98,"default":0.98,
         "bind":[{"node":"svf","param":"resonance"}]},
        {"id":"r3","label":"R3","min":0,"max":0.95,"default":0.95,
         "bind":[{"node":"cmb","param":"feedback"}]},
        {"id":"d","label":"D","min":1,"max":6,"default":6,
         "bind":[{"node":"lad","param":"drive"}]},
        {"id":"s","label":"S","min":0,"max":1,"default":1,
         "bind":[{"node":"env","param":"sustain"}]}]})";

    ir::IrReport report;
    auto graph = buildFromJson(json, report);
    REQUIRE(graph != nullptr);

    const auto res = render(*graph, {24, 36, 48, 84}, 4.0);
    CHECK(res.finite);
    CHECK(res.peak <= 1.0f);
    CHECK(!graph->nanGuardTripped());
}

TEST_CASE(parameters_and_macros_change_the_sound) {
    ir::IrReport report;
    ir::Instrument inst;
    REQUIRE(ir::parse(forge::llm::cannedLibrary()[0].json, inst, report));
    ir::repair(inst, report);
    ir::applySafety(inst, report, 0.35f);

    int cutoffIndex = -1;
    for (size_t i = 0; i < inst.params.size(); ++i)
        if (inst.params[i].id == "cutoff") cutoffIndex = static_cast<int>(i);
    REQUIRE(cutoffIndex >= 0);

    auto dark = GraphBuilder::build(inst, kSr, report);
    auto bright = GraphBuilder::build(inst, kSr, report);
    REQUIRE(dark != nullptr);
    REQUIRE(bright != nullptr);

    dark->setExposedParam(cutoffIndex, 0.0f);
    bright->setExposedParam(cutoffIndex, 1.0f);

    const auto rd = render(*dark, {40}, 0.5);
    const auto rb = render(*bright, {40}, 0.5);
    CHECK(rd.finite);
    CHECK(rb.finite);
    // Opening a low-pass filter must make the sound measurably brighter.
    // (RMS is the wrong measure here: the master limiter deliberately evens
    // out level, so a closed and an open filter can land at the same loudness.)
    test::note("dark brightness " + std::to_string(rd.brightness())
               + "  bright brightness " + std::to_string(rb.brightness()));
    CHECK(rb.brightness() > rd.brightness() * 1.5f);

    // And the macro must move things too.
    if (!inst.macros.empty()) {
        auto m0 = GraphBuilder::build(inst, kSr, report);
        auto m1 = GraphBuilder::build(inst, kSr, report);
        REQUIRE(m0 != nullptr);
        REQUIRE(m1 != nullptr);
        m0->setMacro(0, 0.0f);
        m1->setMacro(0, 1.0f);
        const auto r0 = render(*m0, {40}, 0.5);
        const auto r1 = render(*m1, {40}, 0.5);
        test::note("macro off " + std::to_string(r0.brightness())
                   + "  macro on " + std::to_string(r1.brightness()));
        CHECK(std::abs(r0.brightness() - r1.brightness()) > 1.0e-3f);
    }
}

TEST_CASE(envelope_actually_shapes_amplitude) {
    const char* json = R"({
      "name":"EnvTest","voicing":"poly","polyphony":1,
      "nodes":[
        {"id":"o1","type":"osc.analog","scope":"voice","settings":{"wave":"sine"}},
        {"id":"amp","type":"vca","scope":"voice"},
        {"id":"env","type":"env.adsr","scope":"voice"},
        {"id":"master","type":"out.master","scope":"global"}],
      "audio":[{"from":"o1","to":"amp"},{"from":"amp","to":"master"}],
      "mod":[{"source":"env","target":"amp.gain","depth":1.0}],
      "params":[
        {"id":"a","label":"A","unit":"ms","min":1,"max":4000,"default":400,"taper":"log",
         "bind":[{"node":"env","param":"attack_ms"}]},
        {"id":"s","label":"S","min":0,"max":1,"default":1.0,
         "bind":[{"node":"env","param":"sustain"}]},
        {"id":"v","label":"V","min":0,"max":1,"default":0.0,
         "bind":[{"node":"env","param":"velocity_amount"}]}]})";

    ir::IrReport report;
    auto graph = buildFromJson(json, report);
    REQUIRE(graph != nullptr);

    graph->noteOn(60, 1.0f);
    std::vector<float> l(256), r(256);
    float* chans[2] = { l.data(), r.data() };

    // First 10 ms of a 400 ms attack should be much quieter than 300 ms in.
    float earlyPeak = 0.0f, latePeak = 0.0f;
    for (int block = 0; block < 4; ++block) {
        graph->process(chans, 128);
        for (int i = 0; i < 128; ++i) earlyPeak = std::max(earlyPeak, std::abs(l[static_cast<size_t>(i)]));
    }
    for (int block = 0; block < 110; ++block) graph->process(chans, 128);
    for (int block = 0; block < 4; ++block) {
        graph->process(chans, 128);
        for (int i = 0; i < 128; ++i) latePeak = std::max(latePeak, std::abs(l[static_cast<size_t>(i)]));
    }
    test::note("early " + std::to_string(earlyPeak) + " late " + std::to_string(latePeak));
    CHECK(latePeak > earlyPeak * 3.0f);
}

TEST_CASE(every_module_is_safe_in_isolation) {
    // Builds a minimal instrument around each registered module in turn and
    // hammers its parameters. This is the test that has to keep passing as the
    // library grows - a new effect with an unclamped feedback path would sail
    // past every other test in the suite.
    const auto& registry = Registry::instance();
    Rng rng(0xA11CE5ull);
    int covered = 0;

    for (const auto& man : registry.all()) {
        if (man.type == "out.master") continue;   // implicit, always present

        const bool isSource = man.audioIns == 0 && man.audioOuts > 0;
        const bool isMod    = man.isModSource;
        const bool voice    = man.allowVoice;
        const char* scope   = voice ? "voice" : "global";

        nlohmann::json nodes = nlohmann::json::array();
        nlohmann::json audio = nlohmann::json::array();
        nlohmann::json mod   = nlohmann::json::array();

        // Assets so wavetable/envelope/curve-dependent modules can be built.
        const auto assets = nlohmann::json::parse(R"([
          {"id":"wt","kind":"wavetable","spec":{"method":"shape","shape":"saw"}},
          {"id":"cv","kind":"curve","spec":{"method":"breakpoints",
              "points":[[-1,-0.9],[0,0],[1,0.9]]}},
          {"id":"ev","kind":"envelope","spec":{"method":"stages","stages":[
              {"level":1,"time_ms":5},{"level":0,"time_ms":400}]}}
        ])");

        nlohmann::json settings = nlohmann::json::object();
        for (const auto& s : man.settings) {
            if (s.type == SettingDesc::Type::AssetWavetable) settings[s.id] = "wt";
            if (s.type == SettingDesc::Type::AssetCurve)     settings[s.id] = "cv";
            if (s.type == SettingDesc::Type::AssetEnvelope)  settings[s.id] = "ev";
        }

        nodes.push_back({{"id", "src"}, {"type", "osc.analog"}, {"scope", "voice"},
                         {"settings", {{"wave", "saw"}}}});
        nodes.push_back({{"id", "amp"}, {"type", "vca"}, {"scope", "voice"}});
        nodes.push_back({{"id", "env"}, {"type", "env.adsr"}, {"scope", "voice"}});
        nodes.push_back({{"id", "unit"}, {"type", man.type}, {"scope", scope},
                         {"settings", settings}});
        mod.push_back({{"source", "env"}, {"target", "amp.gain"}, {"depth", 1.0}});

        if (isSource) {
            audio.push_back({{"from", "src"},  {"to", "amp"}});
            audio.push_back({{"from", "unit"}, {"to", "amp"}});
            audio.push_back({{"from", "amp"},  {"to", "master"}});
        } else if (man.audioIns > 0) {
            if (voice) {
                audio.push_back({{"from", "src"},  {"to", "unit"}});
                audio.push_back({{"from", "unit"}, {"to", "amp"}});
                audio.push_back({{"from", "amp"},  {"to", "master"}});
            } else {
                audio.push_back({{"from", "src"},  {"to", "amp"}});
                audio.push_back({{"from", "amp"},  {"to", "unit"}});
                audio.push_back({{"from", "unit"}, {"to", "master"}});
            }
        } else {
            audio.push_back({{"from", "src"}, {"to", "amp"}});
            audio.push_back({{"from", "amp"}, {"to", "master"}});
        }
        nodes.push_back({{"id", "master"}, {"type", "out.master"}, {"scope", "global"}});

        // Expose every parameter of the module under test so the sweep can
        // drive all of them to both extremes.
        nlohmann::json params = nlohmann::json::array();
        int exposed = 0;
        for (const auto& pd : man.params) {
            if (exposed >= 12) break;
            params.push_back({{"id", "p" + std::to_string(exposed)},
                              {"label", pd.id},
                              {"min", pd.min}, {"max", pd.max}, {"default", pd.def},
                              {"taper", toString(pd.taper)},
                              {"bind", nlohmann::json::array({
                                  {{"node", "unit"}, {"param", pd.id}}})}});
            ++exposed;
        }

        if (isMod && !man.params.empty()) {
            // Point the modulator at something audible.
            mod.push_back({{"source", "unit"},
                           {"target", voice ? "amp.pan" : "master.volume"},
                           {"depth", 1.0}});
        }

        nlohmann::json j = {
            {"ir_version", "0.1"}, {"name", man.type}, {"voicing", "poly"},
            {"polyphony", 4}, {"assets", assets}, {"nodes", nodes},
            {"audio", audio}, {"mod", mod}, {"params", params}
        };

        ir::Instrument inst;
        ir::IrReport report;
        if (!ir::parse(j.dump(), inst, report)) { test::note(man.type + ": parse failed"); CHECK(false); continue; }
        ir::repair(inst, report);
        ir::applySafety(inst, report, 0.35f);

        ir::IrReport buildReport;
        auto graph = GraphBuilder::build(inst, kSr, buildReport);
        if (graph == nullptr) {
            std::string errs;
            for (const auto& i : buildReport.issues)
                if (i.level == ir::IrIssue::Level::Error) errs += " " + i.message;
            test::note(man.type + ": build failed:" + errs);
            CHECK(false);
            continue;
        }

        std::vector<float> l(128), r(128);
        float* chans[2] = { l.data(), r.data() };
        graph->noteOn(45, 1.0f);
        graph->noteOn(69, 0.8f);

        float peak = 0.0f;
        bool finite = true;
        for (int block = 0; block < 260; ++block) {
            // Sweep every exposed parameter, including to both hard extremes.
            for (int p = 0; p < exposed; ++p) {
                const float v = (block % 40 == 0) ? 1.0f
                              : (block % 40 == 20) ? 0.0f
                                                   : rng.nextUnipolar();
                graph->setExposedParam(p, v);
            }
            if (block == 200) graph->allNotesOff(false);
            graph->process(chans, 128);
            for (int i = 0; i < 128; ++i) {
                if (!isFinite(l[static_cast<size_t>(i)]) || !isFinite(r[static_cast<size_t>(i)]))
                    finite = false;
                peak = std::max(peak, std::abs(l[static_cast<size_t>(i)]));
            }
        }

        if (!finite || peak > 1.0f || graph->nanGuardTripped())
            test::note(man.type + ": peak " + std::to_string(peak)
                       + (finite ? "" : " NON-FINITE")
                       + (graph->nanGuardTripped() ? " NAN-GUARD" : ""));
        CHECK(finite);
        CHECK(peak <= 1.0f);
        CHECK(!graph->nanGuardTripped());
        ++covered;
    }

    test::note("modules exercised: " + std::to_string(covered));
    CHECK(covered >= 30);
}

TEST_CASE(asset_baker_produces_safe_tables) {
    {
        Wavetable wt;
        bakeWavetable(nlohmann::json::parse(R"({"method":"harmonics","frames":4,
            "morph":"lowpass_sweep",
            "partials":[{"n":1,"amp":1},{"n":3,"amp":0.5},{"n":7,"amp":0.25}]})"), wt);
        CHECK(wt.numFrames == 4);
        float peak = 0.0f;
        double sum = 0.0;
        for (float v : wt.data) { CHECK(isFinite(v)); peak = std::max(peak, std::abs(v)); }
        for (int i = 0; i < Wavetable::kFrameSize; ++i) sum += wt.frameData(0, 0)[i];
        CHECK(peak <= 1.001f);
        CHECK(std::abs(sum / Wavetable::kFrameSize) < 1.0e-3);   // DC removed
    }
    {
        // Garbage in: must still produce something usable, not a crash.
        Wavetable wt;
        bakeWavetable(nlohmann::json::parse(R"({"method":"harmonics","frames":9999,
            "partials":[{"n":-5,"amp":1e9},{"n":100000,"amp":"x"}]})"), wt);
        CHECK(wt.numFrames >= 1 && wt.numFrames <= 8);
        for (float v : wt.data) CHECK(isFinite(v));
    }
    {
        Curve c;
        bakeCurve(nlohmann::json::parse(R"({"method":"breakpoints","interp":"catmullrom",
            "points":[[-1,-0.9],[0,0],[1,0.9]]})"), c);
        for (float v : c.table) { CHECK(isFinite(v)); CHECK(std::abs(v) <= 1.0f); }
        CHECK(c.lookup(-2.0f) >= -1.0f);
        CHECK(c.lookup(2.0f) <= 1.0f);
    }
    {
        Curve c;
        bakeCurve(nlohmann::json::parse(R"({"method":"polynomial","coeffs":[0,1e9,-1e9]})"), c);
        for (float v : c.table) CHECK(std::abs(v) <= 1.0f);
    }
    {
        EnvelopeShape e;
        bakeEnvelope(nlohmann::json::parse(R"({"method":"stages","stages":[]})"), e);
        CHECK(!e.stages.empty());   // never leaves a voice with no way to end
    }
}
