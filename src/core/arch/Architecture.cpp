#include "core/arch/Architecture.h"

#include "core/dsp/Registry.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace forge::arch {
namespace {

using nlohmann::json;

// --- builder helpers -------------------------------------------------------

struct Builder {
    ir::Instrument inst;

    void node(const char* id, const char* type, Scope scope, json settings = json::object()) {
        ir::NodeSpec n;
        n.id = id; n.type = type; n.scope = scope; n.settings = std::move(settings);
        inst.nodes.push_back(std::move(n));
    }

    void conn(const char* from, const char* to, float gain = 1.0f) {
        inst.audio.push_back({from, to, gain});
    }

    void mod(const char* source, const char* node, const char* param, float depth,
             Taper curve = Taper::Linear, bool bipolar = false) {
        inst.mod.push_back({source, node, param, depth, curve, bipolar});
    }

    void param(const char* id, const char* label, const char* unit,
               float mn, float mx, float def, Taper taper,
               const char* node, const char* target, const char* group) {
        ir::ParamSpec p;
        p.id = id; p.label = label; p.unit = unit;
        p.min = mn; p.max = mx; p.def = def; p.taper = taper;
        p.control = "knob";
        // "tab/PANEL" - the panel is what gives a label like "Wow" the context
        // that it belongs to a tape machine.
        splitGroup(group, p.group, p.panel);
        p.bind.push_back({node, target, 1.0f, 0.0f});
        inst.params.push_back(std::move(p));
    }

    void sw(const char* id, const char* label, const char* node, const char* setting,
            const char* group) {
        ir::SwitchSpec s;
        s.id = id; s.label = label; s.node = node; s.setting = setting;
        splitGroup(group, s.group, s.panel);
        inst.switches.push_back(std::move(s));
    }

    static void splitGroup(const char* combined, std::string& tab, std::string& panel) {
        const std::string g(combined);
        const auto slash = g.find('/');
        if (slash == std::string::npos) { tab = g; panel.clear(); return; }
        tab   = g.substr(0, slash);
        panel = g.substr(slash + 1);
    }

    void macro(const char* id, const char* label, float def,
               std::vector<ir::MacroRoute> routes) {
        ir::MacroSpec m;
        m.id = id; m.label = label; m.def = def; m.routes = std::move(routes);
        inst.macros.push_back(std::move(m));
    }
};

/// Adds the five knobs and two selectors every ADSR gets. Four envelopes with
/// identical layouts is exactly the repetition a producer relies on.
void addEnvelope(Builder& b, const char* id, const char* label, const char* group,
                 float attack, float decay, float sustain, float release) {
    b.node(id, "env.adsr", Scope::Voice, {{"curve", "exp"}});
    const std::string p = std::string(id) + "_";
    b.param((p + "a").c_str(),   "Attack",  "ms", 0.2f,  8000.0f, attack,  Taper::Log,    id, "attack_ms",       group);
    b.param((p + "d").c_str(),   "Decay",   "ms", 0.5f,  8000.0f, decay,   Taper::Log,    id, "decay_ms",        group);
    b.param((p + "s").c_str(),   "Sustain", "",   0.0f,     1.0f, sustain, Taper::Linear, id, "sustain",         group);
    b.param((p + "r").c_str(),   "Release", "ms", 0.5f, 12000.0f, release, Taper::Log,    id, "release_ms",      group);
    b.param((p + "vel").c_str(), "Vel",     "",   0.0f,     1.0f, 0.35f,   Taper::Linear, id, "velocity_amount", group);
    b.sw((p + "curve").c_str(), "Curve", id, "curve", group);
    (void) label;
}

void addLfo(Builder& b, const char* id, const char* group, Scope scope,
            const char* wave, float rate, bool sync) {
    b.node(id, "lfo", scope, {{"wave", wave}, {"bipolar", true},
                              {"sync", sync}, {"division", "1/4"}, {"retrigger", false}});
    const std::string p = std::string(id) + "_";
    b.param((p + "rate").c_str(),   "Rate",   "Hz", 0.005f, 60.0f, rate, Taper::Log,    id, "rate_hz",      group);
    b.param((p + "phase").c_str(),  "Phase",  "",   0.0f,    1.0f, 0.0f, Taper::Linear, id, "phase_offset", group);
    b.param((p + "smooth").c_str(), "Smooth", "",   0.0f,    1.0f, 0.0f, Taper::Linear, id, "smooth",       group);
    b.sw((p + "wave").c_str(), "Shape", id, "wave",     group);
    b.sw((p + "sync").c_str(), "Sync",  id, "sync",     group);
    b.sw((p + "div").c_str(),  "Div",   id, "division", group);
}

} // namespace

// ---------------------------------------------------------------------------

const std::vector<Section>& sections() {
    static const std::vector<Section> s = {
        {"osc", "OSC", false},   // sources, filters and shaping
        {"mod", "MOD", false},   // envelopes, LFOs, sequencer
        {"fx",  "FX",  true},    // effects, space, output and macros
    };
    return s;
}

ir::Instrument buildFullArchitecture() {
    Builder b;
    b.inst.name        = "Init";
    b.inst.description = "Full architecture.";
    b.inst.voicing     = "poly";
    b.inst.polyphony   = 12;

    // --- generated assets the rack always carries -------------------------
    b.inst.assets.push_back({"wt_main", "wavetable",
        json::parse(R"({"method":"harmonics","frames":4,"morph":"lowpass_sweep",
                        "partials":[{"n":1,"amp":1.0},{"n":2,"amp":0.5},{"n":3,"amp":0.33},
                                    {"n":4,"amp":0.25},{"n":5,"amp":0.2},{"n":7,"amp":0.14}]})")});

    // ======================= OSCILLATORS ==================================
    b.node("osc_a", "osc.analog", Scope::Voice,
           {{"wave", "saw"}, {"unison", 1}, {"octave", 0}, {"semitones", 0}, {"retrigger", true}});
    b.param("osc_a_level",  "Level",  "",      0.0f,   1.0f,  0.75f, Taper::Linear, "osc_a", "level",           "osc/OSC A");
    b.param("osc_a_detune", "Detune", "cents", 0.0f,  60.0f,  8.0f,  Taper::Linear, "osc_a", "detune_cents",    "osc/OSC A");
    b.param("osc_a_fine",   "Fine",   "cents", -100.0f, 100.0f, 0.0f, Taper::Linear, "osc_a", "fine_cents",     "osc/OSC A");
    b.param("osc_a_pmod", "A Pitch", "st", -48.0f, 48.0f, 0.0f, Taper::Linear, "osc_a", "pitch_mod_semis", "osc/OSC A");
    b.param("osc_a_pw",     "Width",  "",      0.02f,  0.98f, 0.5f,  Taper::Linear, "osc_a", "pulse_width",     "osc/OSC A");
    b.sw("osc_a_wave",   "A Wave",   "osc_a", "wave",      "osc/OSC A");
    b.sw("osc_a_uni",    "A Voices", "osc_a", "unison",    "osc/OSC A");
    b.sw("osc_a_oct",    "A Oct",    "osc_a", "octave",    "osc/OSC A");
    b.sw("osc_a_semi",   "A Semi",   "osc_a", "semitones", "osc/OSC A");

    b.node("osc_b", "osc.analog", Scope::Voice,
           {{"wave", "square"}, {"unison", 1}, {"octave", 0}, {"semitones", 0}, {"retrigger", true}});
    b.param("osc_b_level",  "Level",  "",      0.0f,   1.0f,  0.0f,  Taper::Linear, "osc_b", "level",        "osc/OSC B");
    b.param("osc_b_detune", "Detune", "cents", 0.0f,  60.0f,  12.0f, Taper::Linear, "osc_b", "detune_cents", "osc/OSC B");
    b.param("osc_b_fine",   "Fine",   "cents", -100.0f, 100.0f, 0.0f, Taper::Linear, "osc_b", "fine_cents",  "osc/OSC B");
    b.param("osc_b_pmod", "B Pitch", "st",   -48.0f, 48.0f, 0.0f, Taper::Linear, "osc_b", "pitch_mod_semis", "osc/OSC B");
    b.param("osc_b_pw",   "B Width", "",     0.02f,  0.98f, 0.5f, Taper::Linear, "osc_b", "pulse_width",     "osc/OSC B");
    b.sw("osc_b_wave", "B Wave",   "osc_b", "wave",   "osc/OSC B");
    b.sw("osc_b_uni",  "B Voices", "osc_b", "unison", "osc/OSC B");
    b.sw("osc_b_oct",  "B Oct",    "osc_b", "octave",    "osc/OSC B");
    b.sw("osc_b_semi", "B Semi",   "osc_b", "semitones", "osc/OSC B");

    b.node("osc_wt", "osc.wavetable", Scope::Voice,
           {{"table", "wt_main"}, {"octave", 0}, {"semitones", 0}, {"retrigger", true}});
    b.param("osc_wt_level", "WT Level", "",      0.0f,   1.0f, 0.0f, Taper::Linear, "osc_wt", "level", "osc/WAVETABLE");
    b.param("osc_wt_pmod", "WT Pitch", "st", -48.0f, 48.0f, 0.0f, Taper::Linear, "osc_wt", "pitch_mod_semis", "osc/WAVETABLE");
    b.param("osc_wt_pos",   "Position", "",      0.0f,   1.0f, 0.0f, Taper::Linear, "osc_wt", "position", "osc/WAVETABLE");
    b.param("osc_wt_fine",  "WT Fine",  "cents", -100.0f, 100.0f, 0.0f, Taper::Linear, "osc_wt", "fine_cents", "osc/WAVETABLE");
    b.sw("osc_wt_oct",  "WT Oct",  "osc_wt", "octave",    "osc/WAVETABLE");
    b.sw("osc_wt_semi", "WT Semi", "osc_wt", "semitones", "osc/WAVETABLE");

    b.node("osc_fm", "osc.fm2", Scope::Voice, {{"ratio", 2.0}, {"octave", 0}, {"semitones", 0}});
    b.param("osc_fm_level", "FM Level", "", 0.0f,  1.0f, 0.0f, Taper::Linear, "osc_fm", "level",    "osc/FM");
    b.param("osc_fm_index", "FM Index", "", 0.0f, 12.0f, 2.0f, Taper::Linear, "osc_fm", "index",    "osc/FM");
    b.param("osc_fm_fb",    "Feedback", "", 0.0f,  0.9f, 0.0f, Taper::Linear, "osc_fm", "feedback", "osc/FM");
    // RATIO was the missing control that made bells impossible.
    //
    // The modulator-to-carrier ratio is the single most important parameter in
    // FM: integer ratios give harmonic tones (organs, basses), NON-INTEGER
    // ratios give inharmonic ones - which is exactly and only what makes a
    // glockenspiel, a bell or a vibraphone sound like struck metal. The prompt
    // has been telling the model to use non-integer ratios for months with no
    // way to set one, so every mallet instrument came out harmonic and wrong.
    b.sw("osc_fm_ratio", "FM Ratio", "osc_fm", "ratio", "osc/FM");
    b.param("osc_fm_fine",  "FM Fine",  "cents", -100.0f, 100.0f, 0.0f, Taper::Linear, "osc_fm", "fine_cents", "osc/FM");
    b.param("osc_fm_pmod",  "FM Pitch", "st",     -48.0f,  48.0f, 0.0f, Taper::Linear, "osc_fm", "pitch_mod_semis", "osc/FM");
    b.sw("osc_fm_oct",  "FM Oct",  "osc_fm", "octave",    "osc/FM");
    b.sw("osc_fm_semi", "FM Semi", "osc_fm", "semitones", "osc/FM");

    b.node("osc_sub", "osc.analog", Scope::Voice,
           {{"wave", "sine"}, {"octave", -1}, {"unison", 1}, {"retrigger", true}});
    b.param("osc_sub_level", "Sub",       "",   0.0f,  1.0f, 0.0f, Taper::Linear, "osc_sub", "level",           "osc/SUB");
    b.param("osc_sub_fine",  "Sub Fine",  "cents", -100.0f, 100.0f, 0.0f, Taper::Linear, "osc_sub", "fine_cents", "osc/SUB");
    b.param("osc_sub_pmod",  "Sub Pitch", "st", -48.0f, 48.0f, 0.0f, Taper::Linear, "osc_sub", "pitch_mod_semis", "osc/SUB");
    b.sw("osc_sub_wave", "Sub Wave", "osc_sub", "wave",   "osc/SUB");
    b.sw("osc_sub_oct",  "Sub Oct",  "osc_sub", "octave", "osc/SUB");

    b.node("osc_noise", "osc.noise", Scope::Voice, {{"color", "white"}});
    b.param("osc_noise_level", "Noise", "", 0.0f, 1.0f, 0.0f, Taper::Linear, "osc_noise", "level", "osc/NOISE");
    b.sw("osc_noise_color", "Noise Type", "osc_noise", "color", "osc/NOISE");

    // --- physical modelling -------------------------------------------------
    //
    // Added because someone asked for a Thundercat bass and got a synthesiser,
    // twice. The Karplus-Strong string was already in the module library,
    // tested and safe - it simply was not in the RACK, so no prompt could ever
    // reach it. Every request for a guitar, a bass, a harp or a koto had
    // nothing but saws and FM to work with, and dutifully came back sounding
    // like a synth.
    //
    // A plucked string is not an oscillator with a fast envelope. It is an
    // excitation decaying inside a resonator, and this is the module that does
    // that. Off by default, so nothing else changes.
    b.node("osc_string", "osc.karplus", Scope::Voice);
    b.param("str_level",  "String",    "",   0.0f,     1.0f,   0.0f,   Taper::Linear, "osc_string", "level",      "osc/STRING");
    b.param("str_damp",   "Damping",   "",   0.0f,     1.0f,   0.35f,  Taper::Linear, "osc_string", "damping",    "osc/STRING");
    b.param("str_bright", "Pick Tone", "Hz", 500.0f, 16000.0f, 4200.0f, Taper::Log,   "osc_string", "brightness", "osc/STRING");
    // 0.90, not 0.985. The old default mapped to a TWELVE SECOND ring, so
    // every string instrument arrived sustaining like a pad - one of the surest
    // ways to sound synthetic no matter how good the model underneath is. This
    // is about four seconds, which is a real guitar.
    b.param("str_decay",  "Sustain",   "",   0.8f,     0.999f, 0.90f,  Taper::Linear, "osc_string", "decay",      "osc/STRING");
    b.param("str_pick",   "Pick Pos",  "",   0.02f,    0.5f,   0.14f,  Taper::Linear, "osc_string", "pick_pos",   "osc/STRING");
    b.param("str_stiff",  "Stiffness", "",   0.0f,     1.0f,   0.30f,  Taper::Linear, "osc_string", "stiffness",  "osc/STRING");

    b.node("osc_mix", "mixer", Scope::Voice);
    b.param("osc_drive", "Osc Gain", "", 0.0f, 2.0f, 1.0f, Taper::Linear, "osc_mix", "gain", "osc/MIX");

    for (const char* src : {"osc_a", "osc_b", "osc_wt", "osc_fm", "osc_sub", "osc_noise", "osc_string"})
        b.conn(src, "osc_mix", 1.0f);

    // ======================= FILTERS ======================================
    b.node("filt_1", "filter.svf", Scope::Voice, {{"mode", "lp"}, {"slope", "24"}});
    b.param("f1_cutoff", "Cutoff",  "Hz", 20.0f, 20000.0f, 3000.0f, Taper::Log,    "filt_1", "cutoff",    "osc/FILTER 1");
    b.param("f1_reso",   "Reso",    "",    0.0f,     0.9f,   0.15f, Taper::Linear, "filt_1", "resonance", "osc/FILTER 1");
    b.param("f1_track",  "Track",   "",    0.0f,     1.0f,   0.0f,  Taper::Linear, "filt_1", "keytrack",  "osc/FILTER 1");
    b.param("f1_drive",  "Drive",   "",    1.0f,     4.0f,   1.0f,  Taper::Linear, "filt_1", "drive",     "osc/FILTER 1");
    b.sw("f1_mode",  "Mode",  "filt_1", "mode",  "osc/FILTER 1");
    b.sw("f1_slope", "Slope", "filt_1", "slope", "osc/FILTER 1");

    b.node("filt_2", "filter.ladder", Scope::Voice);
    b.param("f2_cutoff", "Cutoff 2", "Hz", 20.0f, 20000.0f, 18000.0f, Taper::Log,    "filt_2", "cutoff",    "osc/FILTER 2");
    b.param("f2_reso",   "Reso 2",   "",    0.0f,    0.95f,    0.0f,  Taper::Linear, "filt_2", "resonance", "osc/FILTER 2");
    b.param("f2_drive",  "Drive 2",  "",    1.0f,     6.0f,    1.0f,  Taper::Linear, "filt_2", "drive",     "osc/FILTER 2");
    b.param("f2_track",  "Track 2",  "",    0.0f,     1.0f,    0.0f,  Taper::Linear, "filt_2", "keytrack",  "osc/FILTER 2");

    b.conn("osc_mix", "filt_1");
    b.conn("filt_1",  "filt_2");

    // ======================= SHAPE ========================================
    // --- body -----------------------------------------------------------
    //
    // What separates an instrument from a tone generator is that the sound
    // comes out of a BOX: a guitar body, a piano soundboard, a cello's belly.
    // A comb resonator gives that hollow wooden reinforcement, and a formant
    // bank gives the vowel-like character of a resonant cavity. Both sit in
    // the voice chain at zero mix, so they cost nothing until asked for.
    // 110 Hz, not 220. The comb reinforces its frequency and every multiple of
    // it under whatever note is playing, so a default up in the playing
    // register is a trap: at 220 Hz a low E came back with its FOURTH harmonic
    // 12 dB above the fundamental, which the ear resolves as the wrong note two
    // octaves up. Real boxes resonate below the register they support.
    b.node("body", "filter.comb", Scope::Voice, {{"freq", 110.0f}});
    b.param("body_freq", "Body Size", "Hz", 20.0f, 5000.0f, 110.0f, Taper::Log,    "body", "freq",     "osc/BODY");
    b.param("body_res",  "Resonance", "",    0.0f,   0.95f,   0.55f, Taper::Linear, "body", "feedback", "osc/BODY");
    b.param("body_mix",  "Body",      "",    0.0f,   1.0f,    0.0f,  Taper::Linear, "body", "mix",      "osc/BODY");

    b.node("vowel", "filter.formant", Scope::Voice);
    b.param("vow_morph", "Vowel",     "", 0.0f, 1.0f, 0.30f, Taper::Linear, "vowel", "morph",      "osc/VOWEL");
    b.param("vow_res",   "Vowel Res", "", 0.0f, 1.0f, 0.50f, Taper::Linear, "vowel", "resonance",  "osc/VOWEL");
    b.param("vow_mix",   "Vowel Mix", "", 0.0f, 1.0f, 0.0f,  Taper::Linear, "vowel", "mix",        "osc/VOWEL");

    b.node("shaper", "shaper.wave", Scope::Voice, {{"type", "tanh"}});
    b.param("shp_drive", "Shape Drive", "", 1.0f, 20.0f, 1.0f, Taper::Log,    "shaper", "drive", "osc/SHAPER");
    b.param("shp_mix",   "Shape Mix",   "", 0.0f,  1.0f, 0.0f, Taper::Linear, "shaper", "mix",   "osc/SHAPER");
    b.param("shp_bits",  "Bits",        "", 1.0f, 16.0f, 12.0f, Taper::Linear, "shaper", "bits", "osc/SHAPER");
    b.sw("shp_type", "Shaper", "shaper", "type", "osc/SHAPER");
    b.conn("filt_2", "body");
    b.conn("body",   "vowel");
    b.conn("vowel",  "shaper");

    // ======================= AMP ==========================================
    b.node("amp",   "vca", Scope::Voice);
    b.node("pan_v", "pan", Scope::Voice);
    b.param("v_pan",   "Pan",   "", -1.0f, 1.0f, 0.0f, Taper::Linear, "pan_v", "pan",   "osc/MIX");
    b.param("v_width", "Width", "",  0.0f, 2.0f, 1.0f, Taper::Linear, "pan_v", "width", "osc/MIX");
    b.conn("shaper", "amp");
    b.conn("amp",    "pan_v");

    // ======================= ENVELOPES ====================================
    addEnvelope(b, "env_1", "Amp",    "mod/ENV 1", 5.0f,   400.0f, 0.8f,  350.0f);
    addEnvelope(b, "env_2", "Filter", "mod/ENV 2", 5.0f,   600.0f, 0.3f,  400.0f);
    addEnvelope(b, "env_3", "Mod 3",  "mod/ENV 3", 50.0f,  800.0f, 0.5f,  500.0f);
    addEnvelope(b, "env_4", "Mod 4",  "mod/ENV 4", 200.0f, 1200.0f, 0.6f, 800.0f);

    // ======================= LFOs =========================================
    addLfo(b, "lfo_1", "mod/LFO 1", Scope::Voice,  "sine",   5.0f,  false);
    addLfo(b, "lfo_2", "mod/LFO 2", Scope::Voice,  "tri",    0.5f,  false);
    addLfo(b, "lfo_3", "mod/LFO 3", Scope::Global, "sine",   0.15f, false);
    addLfo(b, "lfo_4", "mod/LFO 4", Scope::Global, "square", 2.0f,  true);

    b.node("seq", "mod.sequencer", Scope::Global,
           {{"steps", 8}, {"pattern", "random"}, {"sync", true}, {"division", "1/8"}});
    b.param("seq_rate",   "Seq Rate",   "Hz", 0.05f, 40.0f, 4.0f, Taper::Log,    "seq", "rate_hz", "mod/SEQUENCER");
    b.param("seq_smooth", "Seq Smooth", "",   0.0f,   1.0f, 0.0f, Taper::Linear, "seq", "smooth",  "mod/SEQUENCER");
    b.param("seq_depth",  "Seq Depth",  "",   0.0f,   1.0f, 0.0f, Taper::Linear, "seq", "depth",   "mod/SEQUENCER");
    b.sw("seq_steps",   "Steps",   "seq", "steps",    "mod/SEQUENCER");
    b.sw("seq_pattern", "Pattern", "seq", "pattern",  "mod/SEQUENCER");
    b.sw("seq_div",     "Seq Div", "seq", "division", "mod/SEQUENCER");

    // MIDI-derived sources, always available to the matrix.
    b.node("m_vel",   "mod.velocity",   Scope::Voice);
    b.node("m_key",   "mod.keytrack",   Scope::Voice);
    b.node("m_wheel", "mod.modwheel",   Scope::Voice);
    b.node("m_press", "mod.aftertouch", Scope::Voice);

    // Amount controls, so modulation depth is a knob rather than a constant.
    b.node("amt_fenv", "util.math", Scope::Voice, {{"op", "mul"}});
    b.param("f1_env_amt", "Env Amt", "", 0.0f, 1.0f, 0.4f, Taper::Linear, "amt_fenv", "b", "osc/FILTER 1");
    b.node("amt_vib", "util.math", Scope::Voice, {{"op", "mul"}});
    b.param("vib_amt", "Vibrato", "", 0.0f, 1.0f, 0.0f, Taper::Linear, "amt_vib", "b", "mod/AMOUNTS");

    // ======================= DEFAULT MATRIX ===============================
    b.mod("env_1", "amp", "gain", 1.0f);
    b.mod("env_2", "amt_fenv", "a", 1.0f);
    b.mod("amt_fenv", "filt_1", "cutoff", 1.0f, Taper::Exp);
    b.mod("lfo_1", "amt_vib", "a", 1.0f);
    b.mod("m_wheel", "amt_vib", "b", 1.0f);
    b.mod("amt_vib", "osc_a", "pitch_mod_semis", 0.02f);
    b.mod("amt_vib", "osc_b", "pitch_mod_semis", 0.02f);
    b.mod("lfo_3", "filt_1", "cutoff", 0.0f);
    b.mod("m_vel", "amp", "gain", 0.0f);

    // ======================= GLOBAL FX ====================================
    b.node("eq",     "fx.eq3",         Scope::Global);
    b.param("eq_low",  "Low",     "dB",  -18.0f,    18.0f,    0.0f, Taper::Linear, "eq", "low_gain_db",  "fx/EQ");
    b.param("eq_lowf", "Lo Freq", "Hz",   30.0f,  1000.0f,  120.0f, Taper::Log,    "eq", "low_freq",     "fx/EQ");
    b.param("eq_mid",  "Mid",     "dB",  -18.0f,    18.0f,    0.0f, Taper::Linear, "eq", "mid_gain_db",  "fx/EQ");
    b.param("eq_midf", "Mid Frq", "Hz",  100.0f, 12000.0f, 1000.0f, Taper::Log,    "eq", "mid_freq",     "fx/EQ");
    b.param("eq_midq", "Mid Q",   "",      0.2f,     8.0f,    0.9f, Taper::Log,    "eq", "mid_q",        "fx/EQ");
    b.param("eq_high", "High",    "dB",  -18.0f,    18.0f,    0.0f, Taper::Linear, "eq", "high_gain_db", "fx/EQ");
    b.param("eq_highf","Hi Freq", "Hz", 1000.0f, 18000.0f, 6000.0f, Taper::Log,    "eq", "high_freq",    "fx/EQ");

    b.node("drive", "fx.drive", Scope::Global);
    b.param("dr_drive", "Drive",  "",     1.0f,    20.0f,    1.0f, Taper::Log,    "drive", "drive",   "fx/DRIVE");
    b.param("dr_tone",  "Tone",   "Hz", 500.0f, 18000.0f, 12000.0f, Taper::Log,   "drive", "tone_hz", "fx/DRIVE");
    b.param("dr_mix",   "Dr Mix", "",     0.0f,     1.0f,    1.0f, Taper::Linear, "drive", "mix",     "fx/DRIVE");
    b.param("dr_out",   "Dr Out", "",     0.0f,     1.0f,    0.8f, Taper::Linear, "drive", "output",  "fx/DRIVE");

    // The speaker, immediately after the drive - which is where a real amp
    // puts it, and the reason a modelled guitar stops sounding like a
    // distorted oscillator.
    b.node("cab", "fx.cabinet", Scope::Global);
    b.param("cab_low",  "Low Cut", "Hz",   40.0f,   400.0f,   85.0f, Taper::Log,    "cab", "low_hz",  "fx/CABINET");
    b.param("cab_top",  "Top",     "Hz", 1500.0f, 12000.0f, 4800.0f, Taper::Log,    "cab", "high_hz", "fx/CABINET");
    b.param("cab_mix",  "Cabinet", "",     0.0f,     1.0f,    0.0f,  Taper::Linear, "cab", "mix",     "fx/CABINET");

    b.node("tape", "fx.tape", Scope::Global);
    b.param("tp_drive", "Tape",    "",     1.0f,    12.0f,   1.0f, Taper::Log,    "tape", "drive",   "fx/TAPE");
    b.param("tp_wow",   "Wow",     "",     0.0f,     1.0f,   0.2f, Taper::Linear, "tape", "wow",     "fx/TAPE");
    b.param("tp_flut",  "Flutter", "",     0.0f,     1.0f,   0.2f, Taper::Linear, "tape", "flutter", "fx/TAPE");
    b.param("tp_tone",  "Tp Tone", "Hz", 1500.0f, 18000.0f, 12000.0f, Taper::Log, "tape", "tone_hz", "fx/TAPE");
    b.param("tp_mix",   "Tp Mix",  "",     0.0f,     1.0f,   0.0f, Taper::Linear, "tape", "mix",     "fx/TAPE");

    b.node("trans", "fx.transient", Scope::Global);
    b.param("tr_att", "Attack+",  "", -1.0f, 1.0f, 0.0f, Taper::Linear, "trans", "attack",  "fx/TRANSIENT");
    b.param("tr_sus", "Sustain+", "", -1.0f, 1.0f, 0.0f, Taper::Linear, "trans", "sustain", "fx/TRANSIENT");
    b.param("tr_mix", "Tr Mix",   "",  0.0f, 1.0f, 1.0f, Taper::Linear, "trans", "mix",     "fx/TRANSIENT");

    b.node("comp", "dyn.compressor", Scope::Global);
    b.param("cp_thr", "Thresh",  "dB", -60.0f,    0.0f, -6.0f, Taper::Linear, "comp", "threshold_db", "fx/COMPRESSOR");
    b.param("cp_rat", "Ratio",   ":1",   1.0f,   20.0f,  2.0f, Taper::Log,    "comp", "ratio",        "fx/COMPRESSOR");
    b.param("cp_att", "C Att",   "ms",   0.1f,  200.0f, 20.0f, Taper::Log,    "comp", "attack_ms",    "fx/COMPRESSOR");
    b.param("cp_rel", "C Rel",   "ms",   1.0f, 1000.0f, 150.0f, Taper::Log,   "comp", "release_ms",   "fx/COMPRESSOR");
    b.param("cp_mak", "Makeup",  "dB",   0.0f,   24.0f,  0.0f, Taper::Linear, "comp", "makeup_db",    "fx/COMPRESSOR");

    // ======================= SPACE ========================================
    //
    // Chorus was in the module library and not in the rack, so "Rhodes with
    // chorus" and "ensemble strings" - both of which the recipes call for -
    // had nothing to reach for.
    // Octave doubling: a 12-string guitar, an octave bass, a celesta doubling
    // a piano. Another module that existed and could not be reached.
    b.node("oct", "fx.pitch", Scope::Global);
    b.param("oc_semis", "Octave Up", "st", -24.0f, 24.0f, 12.0f, Taper::Linear, "oct", "semitones", "fx/DOUBLER");
    b.param("oc_mix",   "Doubler",   "",     0.0f,  1.0f,  0.0f, Taper::Linear, "oct", "mix",       "fx/DOUBLER");
    b.param("oc_fb",    "Dbl Fdbk",  "",     0.0f,  0.7f,  0.0f, Taper::Linear, "oct", "feedback",  "fx/DOUBLER");

    b.node("chor", "fx.chorus", Scope::Global);
    b.param("ch_rate",  "Ch Rate",  "Hz", 0.01f, 8.0f,  0.6f,  Taper::Log,    "chor", "rate_hz",  "fx/CHORUS");
    b.param("ch_depth", "Ch Depth", "",   0.0f,  1.0f,  0.35f, Taper::Linear, "chor", "depth",    "fx/CHORUS");
    b.param("ch_delay", "Ch Delay", "ms", 1.0f,  40.0f, 12.0f, Taper::Log,    "chor", "delay_ms", "fx/CHORUS");
    b.param("ch_fb",    "Ch Fdbk",  "",   0.0f,  0.7f,  0.0f,  Taper::Linear, "chor", "feedback", "fx/CHORUS");
    b.param("ch_mix",   "Ch Mix",   "",   0.0f,  1.0f,  0.0f,  Taper::Linear, "chor", "mix",      "fx/CHORUS");

    b.node("dim", "fx.dimension", Scope::Global);
    b.param("dm_depth", "Dim Depth", "",   0.0f, 1.0f, 0.5f, Taper::Linear, "dim", "depth", "fx/DIMENSION");
    b.param("dm_rate",  "Dim Rate",  "Hz", 0.05f, 3.0f, 0.5f, Taper::Log,   "dim", "rate",  "fx/DIMENSION");
    b.param("dm_mix",   "Dim Mix",   "",   0.0f, 1.0f, 0.0f, Taper::Linear, "dim", "mix",   "fx/DIMENSION");

    b.node("phs", "fx.phaser", Scope::Global);
    b.param("ph_rate",  "Ph Rate", "Hz",   0.01f,    8.0f,   0.3f, Taper::Log,    "phs", "rate_hz",   "fx/PHASER");
    b.param("ph_depth", "Ph Depth","",     0.0f,     1.0f,   0.7f, Taper::Linear, "phs", "depth",     "fx/PHASER");
    b.param("ph_fbk",   "Ph Fbk",  "",     0.0f,     0.9f,   0.3f, Taper::Linear, "phs", "feedback",  "fx/PHASER");
    b.param("ph_mix",   "Ph Mix",  "",     0.0f,     1.0f,   0.0f, Taper::Linear, "phs", "mix",       "fx/PHASER");
    b.param("ph_cent",  "Ph Freq", "Hz", 100.0f,  8000.0f, 700.0f, Taper::Log,    "phs", "center_hz", "fx/PHASER");

    b.node("dly", "fx.delay", Scope::Global, {{"sync", true}, {"division", "1/8"}, {"pingpong", false}});
    b.param("dl_time", "Dl Time", "ms",   1.0f,  2000.0f, 375.0f, Taper::Log,    "dly", "time_ms",    "fx/DELAY");
    b.param("dl_fbk",  "Dl Fbk",  "",     0.0f,     0.92f,  0.3f, Taper::Linear, "dly", "feedback",   "fx/DELAY");
    b.param("dl_mix",  "Dl Mix",  "",     0.0f,     1.0f,   0.0f, Taper::Linear, "dly", "mix",        "fx/DELAY");
    b.param("dl_damp", "Dl Damp", "Hz", 200.0f, 18000.0f, 6000.0f, Taper::Log,   "dly", "damping_hz", "fx/DELAY");
    b.param("dl_sprd", "Dl Sprd", "",     0.0f,     1.0f,   0.2f, Taper::Linear, "dly", "spread",     "fx/DELAY");
    b.sw("dl_sync", "Dl Sync", "dly", "sync",     "fx/DELAY");
    b.sw("dl_div",  "Dl Div",  "dly", "division", "fx/DELAY");
    b.sw("dl_ping", "Ping",    "dly", "pingpong", "fx/DELAY");

    b.node("verb", "fx.reverb", Scope::Global);
    b.param("rv_size", "Size",     "",   0.0f,   1.0f,  0.6f, Taper::Linear, "verb", "size",        "fx/REVERB");
    b.param("rv_damp", "Rv Damp",  "",   0.0f,   1.0f,  0.4f, Taper::Linear, "verb", "damping",     "fx/REVERB");
    b.param("rv_mix",  "Reverb",   "",   0.0f,   1.0f,  0.15f, Taper::Linear,"verb", "mix",         "fx/REVERB");
    b.param("rv_wide", "Rv Width", "",   0.0f,   1.0f,  1.0f, Taper::Linear, "verb", "width",       "fx/REVERB");
    b.param("rv_pre",  "Predelay", "ms", 0.0f, 200.0f, 10.0f, Taper::Linear, "verb", "predelay_ms", "fx/REVERB");

    b.node("exc", "fx.exciter", Scope::Global);
    b.param("ex_freq", "Freq",   "Hz", 500.0f, 12000.0f, 3000.0f, Taper::Log,    "exc", "freq_hz", "fx/EXCITER");
    b.param("ex_amt",  "Amount", "",     1.0f,    20.0f,    4.0f, Taper::Log,    "exc", "amount",  "fx/EXCITER");
    b.param("ex_mix",  "Mix",    "",     0.0f,     1.0f,    0.0f, Taper::Linear, "exc", "mix",     "fx/EXCITER");

    b.node("gate", "fx.gate", Scope::Global);
    b.param("gt_thr", "Thresh",  "dB", -80.0f,    0.0f, -80.0f, Taper::Linear, "gate", "threshold_db", "fx/GATE");
    b.param("gt_att", "Attack",  "ms",   0.1f,  100.0f,   1.0f, Taper::Log,    "gate", "attack_ms",    "fx/GATE");
    b.param("gt_hld", "Hold",    "ms",   0.0f,  500.0f,  20.0f, Taper::Linear,    "gate", "hold_ms",      "fx/GATE");
    b.param("gt_rel", "Release", "ms",   1.0f, 2000.0f, 120.0f, Taper::Log,    "gate", "release_ms",   "fx/GATE");

    b.node("ring", "fx.ringmod", Scope::Global);
    b.param("rg_freq",  "Freq",  "Hz", 1.0f, 8000.0f, 220.0f, Taper::Log,    "ring", "freq_hz", "fx/RING MOD");
    b.param("rg_mix",   "Mix",   "",   0.0f,    1.0f,   0.0f, Taper::Linear, "ring", "mix",     "fx/RING MOD");
    b.param("rg_track", "Track", "",   0.0f,    1.0f,   0.0f, Taper::Linear, "ring", "track",   "fx/RING MOD");

    b.node("flg", "fx.flanger", Scope::Global);
    b.param("fl_rate",  "Rate",     "Hz",  0.01f,  8.0f, 0.2f, Taper::Log,    "flg", "rate_hz",  "fx/FLANGER");
    b.param("fl_depth", "Depth",    "",    0.0f,   1.0f, 0.7f, Taper::Linear, "flg", "depth",    "fx/FLANGER");
    b.param("fl_fbk",   "Feedback", "",   -0.95f, 0.95f, 0.5f, Taper::Linear, "flg", "feedback", "fx/FLANGER");
    b.param("fl_mix",   "Mix",      "",    0.0f,   1.0f, 0.0f, Taper::Linear, "flg", "mix",      "fx/FLANGER");
    b.param("fl_dly",   "Delay",    "ms",  0.2f,  12.0f, 2.0f, Taper::Log,    "flg", "delay_ms", "fx/FLANGER");

    b.node("apan", "fx.autopan", Scope::Global);
    b.param("ap_rate",  "Rate",  "Hz", 0.01f, 20.0f, 1.0f, Taper::Log,    "apan", "rate_hz", "fx/AUTOPAN");
    b.param("ap_depth", "Depth", "",   0.0f,   1.0f, 0.0f, Taper::Linear, "apan", "depth",   "fx/AUTOPAN");
    b.param("ap_shape", "Shape", "",   0.0f,   1.0f, 0.0f, Taper::Linear, "apan", "shape",   "fx/AUTOPAN");

    b.node("width", "fx.width", Scope::Global);
    b.param("wd_width", "Stereo",    "",   0.0f,   2.0f,   1.0f, Taper::Linear, "width", "width",        "fx/OUTPUT");
    b.param("wd_mono",  "Mono Below","Hz", 20.0f, 500.0f, 120.0f, Taper::Log,   "width", "bass_mono_hz", "fx/OUTPUT");
    b.param("wd_tilt",  "Tilt",      "",  -1.0f,   1.0f,   0.0f, Taper::Linear, "width", "tilt",         "fx/OUTPUT");

    b.node("master", "out.master", Scope::Global);
    b.param("out_vol", "Volume", "", 0.0f, 1.2f, 0.85f, Taper::Linear, "master", "volume", "fx/OUTPUT");

    // Global chain order.
    b.conn("pan_v", "eq");
    b.conn("eq",    "drive");
    b.conn("drive", "cab");
    b.conn("cab",   "tape");
    b.conn("tape",  "trans");
    b.conn("trans", "comp");
    b.conn("comp",  "exc");
    b.conn("exc",   "gate");
    b.conn("gate",  "ring");
    b.conn("ring",  "oct");
    b.conn("oct",   "chor");
    b.conn("chor",  "dim");
    b.conn("dim",   "flg");
    b.conn("flg",   "phs");
    b.conn("phs",   "apan");
    b.conn("apan",  "dly");
    b.conn("dly",   "verb");
    b.conn("verb",  "width");
    b.conn("width", "master");

    // ======================= MACROS =======================================
    b.macro("m_bright", "Bright", 0.5f, {{"f1_cutoff", 0.45f, Taper::Linear},
                                          {"eq_high",   0.3f,  Taper::Linear}});
    b.macro("m_body",   "Body",   0.4f, {{"osc_sub_level", 0.5f, Taper::Linear},
                                          {"eq_low",        0.35f, Taper::Linear}});
    b.macro("m_drive",  "Drive",  0.2f, {{"dr_drive", 0.6f, Taper::Linear},
                                          {"f1_reso",  0.2f, Taper::Linear}});
    b.macro("m_space",  "Space",  0.3f, {{"rv_mix", 0.5f, Taper::Linear},
                                          {"dl_mix", 0.3f, Taper::Linear}});
    b.macro("m_motion", "Motion", 0.2f, {{"lfo_1_rate", 0.4f, Taper::Linear},
                                          {"dm_mix",     0.4f, Taper::Linear}});

    // ======================= UI LAYOUT ====================================
    for (const auto& s : sections()) {
        ir::UiSection ui;
        ui.title = s.title;
        // Group by panel so a tab's controls arrive already clustered.
        std::vector<std::string> panels;
        for (const auto& p : b.inst.params)
            if (p.group == s.id
                && std::find(panels.begin(), panels.end(), p.panel) == panels.end())
                panels.push_back(p.panel);
        for (const auto& panel : panels)
            for (const auto& p : b.inst.params)
                if (p.group == s.id && p.panel == panel) ui.params.push_back(p.id);
        if (!ui.params.empty()) b.inst.ui.sections.push_back(std::move(ui));
    }
    for (const auto& m : b.inst.macros) b.inst.ui.macroRow.push_back(m.id);

    return b.inst;
}

// ---------------------------------------------------------------------------

namespace {

/// Remove a set of nodes and everything that referenced them, bridging the
/// serial audio chain across each hole so the signal still reaches the output.
void dropNodes(ir::Instrument& inst, const std::unordered_set<std::string>& doomed) {
    if (doomed.empty()) return;

    for (const auto& id : doomed) {
        std::vector<ir::AudioConn> bridged;
        for (const auto& in : inst.audio)
            if (in.to == id)
                for (const auto& out : inst.audio)
                    if (out.from == id) bridged.push_back({in.from, out.to, in.gain * out.gain});
        inst.audio.erase(std::remove_if(inst.audio.begin(), inst.audio.end(),
                                        [&](const ir::AudioConn& c) {
                                            return c.from == id || c.to == id;
                                        }), inst.audio.end());
        for (auto& c : bridged) inst.audio.push_back(c);
    }

    inst.nodes.erase(std::remove_if(inst.nodes.begin(), inst.nodes.end(),
                                    [&](const ir::NodeSpec& n) { return doomed.count(n.id) > 0; }),
                     inst.nodes.end());
    inst.mod.erase(std::remove_if(inst.mod.begin(), inst.mod.end(),
                                  [&](const ir::ModRoute& r) {
                                      return doomed.count(r.source) || doomed.count(r.targetNode);
                                  }), inst.mod.end());
    inst.switches.erase(std::remove_if(inst.switches.begin(), inst.switches.end(),
                                       [&](const ir::SwitchSpec& sw) { return doomed.count(sw.node) > 0; }),
                        inst.switches.end());
    for (auto& p : inst.params)
        p.bind.erase(std::remove_if(p.bind.begin(), p.bind.end(),
                                    [&](const ir::ParamBind& bd) { return doomed.count(bd.node) > 0; }),
                     p.bind.end());
    inst.params.erase(std::remove_if(inst.params.begin(), inst.params.end(),
                                     [](const ir::ParamSpec& p) { return p.bind.empty(); }),
                      inst.params.end());
}

/// Panel titles arrive from a language model, so "Filter 1", "FILTER 1" and
/// "filter1" all have to resolve to the same panel.
std::string panelKey(const std::string& in) {
    std::string out;
    for (char c : in)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

void pruneDisabledSections(ir::Instrument& inst, const std::vector<std::string>& enabled) {
    // NOTE: with the pages fused, "fx" carries the output stage and macros too,
    // so it can no longer be dropped wholesale. Section pruning is now driven by
    // panel names rather than pages.
    std::unordered_set<std::string> keep(enabled.begin(), enabled.end());
    if (keep.empty()) return;

    // Collect the nodes belonging to optional sections that were not enabled,
    // via the controls that reference them.
    std::unordered_set<std::string> doomed;
    for (const auto& s : sections()) {
        if (!s.optional || keep.count(s.id)) continue;
        for (const auto& p : inst.params)
            if (p.group == s.id)
                for (const auto& bind : p.bind) doomed.insert(bind.node);
    }
    if (doomed.empty()) return;

    dropNodes(inst, doomed);
}

// ---------------------------------------------------------------------------


namespace {

/// The model authors the page structure itself: which panels survive, how they
/// group into pages, in what order, what colour each block is, and which ones
/// are large enough to carry the identity of the sound.
///
/// This is the thing that makes two instruments look like two instruments. A
/// fixed rack renders every patch through the same furniture, so a glass bell
/// and a distorted sub arrive on screen as the same picture with different
/// numbers under the knobs. Here the furniture is generated too.
///
/// The safety position is unchanged: the model chooses arrangement, never
/// capability. Everything it can place is a precompiled module that already
/// passed the sweep tests, and the output stage cannot be designed away.
void applyLayout(ir::Instrument& inst, const json& layout, ir::IrReport& report) {
    // Real panels, in the order the architecture declared them.
    std::unordered_map<std::string, std::string> byKey;   // key -> real title
    auto note = [&](const std::string& title) {
        if (!title.empty()) byKey.emplace(panelKey(title), title);
    };
    for (const auto& p  : inst.params)   note(p.panel);
    for (const auto& sw : inst.switches) note(sw.panel);
    if (byKey.empty()) return;

    // Panels the instrument cannot lose: the output stage, the amplifier, and
    // whatever drives its envelope. Removing those is not a design decision,
    // it is silence.
    std::unordered_set<std::string> protectedNodes;
    for (const auto& n : inst.nodes)
        if (n.type.rfind("out.", 0) == 0 || n.type == "vca") protectedNodes.insert(n.id);
    for (const auto& r : inst.mod)
        if (protectedNodes.count(r.targetNode)) protectedNodes.insert(r.source);

    std::unordered_set<std::string> protectedPanels;
    for (const auto& p : inst.params)
        for (const auto& b : p.bind)
            if (protectedNodes.count(b.node)) protectedPanels.insert(panelKey(p.panel));

    // --- pages ------------------------------------------------------------
    auto pagesIt = layout.find("pages");
    if (pagesIt == layout.end() || !pagesIt->is_array() || pagesIt->empty()) return;

    std::vector<std::pair<std::string, std::vector<std::string>>> pages;  // title, panel keys
    std::unordered_set<std::string> claimed;
    for (const auto& pg : *pagesIt) {
        if (!pg.is_object()) continue;
        auto title = pg.value("title", std::string());
        if (title.empty()) continue;
        if (title.size() > 14) title.resize(14);
        for (auto& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        std::vector<std::string> keys;
        if (auto pl = pg.find("panels"); pl != pg.end() && pl->is_array())
            for (const auto& v : *pl) {
                if (!v.is_string()) continue;
                const auto k = panelKey(v.get<std::string>());
                if (byKey.count(k) && claimed.insert(k).second) keys.push_back(k);
            }
        if (!keys.empty()) pages.emplace_back(std::move(title), std::move(keys));
        if (pages.size() >= 6) break;   // more than six tabs is a filing cabinet
    }
    if (pages.empty()) return;

    // A layout naming one or two panels is not a minimal design, it is a
    // malformed response - and honouring it would put a two-knob synth on
    // stage. Below the floor, discard the layout entirely and keep the
    // architecture's own arrangement, which is always complete.
    if (claimed.size() < 4) {
        report.warn("layout", "Layout named only " + std::to_string(claimed.size())
                              + " known panel(s); kept the default arrangement.");
        return;
    }

    // Panels the sound cannot exist without, that the model did not name, join
    // the last page rather than being dropped.
    for (const auto& k : protectedPanels)
        if (claimed.insert(k).second) pages.back().second.push_back(k);

    // An instrument needs something to make a sound. If every oscillator was
    // left off the pages, keep the panel holding the first one.
    {
        std::unordered_set<std::string> oscNodes;
        for (const auto& n : inst.nodes)
            if (n.type.rfind("osc.", 0) == 0) oscNodes.insert(n.id);
        bool haveSource = false;
        std::string firstOscPanel;
        for (const auto& p : inst.params)
            for (const auto& b : p.bind)
                if (oscNodes.count(b.node)) {
                    const auto k = panelKey(p.panel);
                    if (firstOscPanel.empty()) firstOscPanel = k;
                    if (claimed.count(k)) haveSource = true;
                }
        if (!haveSource && !firstOscPanel.empty() && claimed.insert(firstOscPanel).second) {
            pages.front().second.insert(pages.front().second.begin(), firstOscPanel);
            report.fixed("layout", "No oscillator was placed; kept one so the instrument sounds.");
        }
    }

    {
        std::unordered_set<std::string> doomed;
        for (const auto& p : inst.params)
            if (!claimed.count(panelKey(p.panel)))
                for (const auto& b : p.bind) doomed.insert(b.node);
        // A node is only really unwanted if nothing that survived still uses it.
        for (const auto& p : inst.params)
            if (claimed.count(panelKey(p.panel)))
                for (const auto& b : p.bind) doomed.erase(b.node);
        for (const auto& sw : inst.switches)
            if (claimed.count(panelKey(sw.panel))) doomed.erase(sw.node);
        for (const auto& id : protectedNodes) doomed.erase(id);
        dropNodes(inst, doomed);
    }

    // --- fold thin pages ---------------------------------------------------
    // A tab holding two selectors is not a page, it is a mistake with a name.
    // Rather than trust the model to get the balance right, merge anything too
    // thin into whichever neighbour is smaller.
    {
        auto controlsOn = [&](const std::vector<std::string>& keys) {
            int n = 0;
            for (const auto& p : inst.params)   if (std::count(keys.begin(), keys.end(), panelKey(p.panel)))  ++n;
            for (const auto& sw : inst.switches) if (std::count(keys.begin(), keys.end(), panelKey(sw.panel))) ++n;
            return n;
        };
        constexpr int kMinControlsPerPage = 9;
        bool merged = true;
        while (merged && pages.size() > 1) {
            merged = false;
            for (size_t i = 0; i < pages.size(); ++i) {
                if (controlsOn(pages[i].second) >= kMinControlsPerPage) continue;
                // Prefer the smaller neighbour so pages stay balanced.
                size_t into = (i == 0) ? 1 : i - 1;
                if (i > 0 && i + 1 < pages.size()
                    && controlsOn(pages[i + 1].second) < controlsOn(pages[i - 1].second))
                    into = i + 1;
                auto& dst = pages[into].second;
                dst.insert(dst.end(), pages[i].second.begin(), pages[i].second.end());
                pages.erase(pages.begin() + static_cast<long>(i));
                report.fixed("layout", "Folded a page with too few controls into its neighbour.");
                merged = true;
                break;
            }
        }
    }

    // --- regroup and rebuild the section list -----------------------------
    std::unordered_map<std::string, std::string> pageOf;   // panel key -> page title
    for (const auto& [title, keys] : pages)
        for (const auto& k : keys) pageOf[k] = title;

    for (auto& p : inst.params)
        if (auto it = pageOf.find(panelKey(p.panel)); it != pageOf.end()) p.group = it->second;
    for (auto& sw : inst.switches)
        if (auto it = pageOf.find(panelKey(sw.panel)); it != pageOf.end()) sw.group = it->second;

    // Panel order inside a page follows the model's list, not the
    // architecture's - that ordering IS the arrangement it designed.
    std::vector<ir::UiSection> sections;
    for (const auto& [title, keys] : pages) {
        ir::UiSection sec;
        sec.title = title;
        for (const auto& k : keys)
            for (const auto& p : inst.params)
                if (panelKey(p.panel) == k) sec.params.push_back(p.id);
        if (!sec.params.empty() || !keys.empty()) sections.push_back(std::move(sec));
    }
    if (!sections.empty()) inst.ui.sections = std::move(sections);

    // --- per-panel colour and prominence -----------------------------------
    if (auto pa = layout.find("panel_accent"); pa != layout.end() && pa->is_object())
        for (auto it = pa->begin(); it != pa->end(); ++it) {
            if (!it.value().is_string()) continue;
            if (auto real = byKey.find(panelKey(it.key())); real != byKey.end())
                inst.ui.panelAccent[real->second] = it.value().get<std::string>();
        }
    if (auto ft = layout.find("featured"); ft != layout.end() && ft->is_array())
        for (const auto& v : *ft) {
            if (!v.is_string()) continue;
            if (auto real = byKey.find(panelKey(v.get<std::string>())); real != byKey.end())
                inst.ui.featuredPanels.push_back(real->second);
        }

    report.fixed("layout", "Laid out " + std::to_string(claimed.size()) + " panels across "
                           + std::to_string(pages.size()) + " pages.");
}

} // namespace

bool applyPatch(ir::Instrument& inst, const json& patch, ir::IrReport& report) {
    if (!patch.is_object()) {
        report.error("", "Patch must be a JSON object.");
        return false;
    }

    auto str = [&](const char* key, const std::string& fallback) {
        auto it = patch.find(key);
        return (it != patch.end() && it->is_string()) ? it->get<std::string>() : fallback;
    };

    inst.name        = str("name", "Generated");
    inst.description = str("description", "");

    // What the model recognised, kept so the UI can show it back. This lives in
    // `meta` rather than as a new field on Instrument: an IR struct change means
    // a full clean rebuild for everyone, and this is presentation, not schema.
    if (auto it = patch.find("references"); it != patch.end() && it->is_array()) {
        auto list = json::array();
        for (const auto& v : *it) {
            if (!v.is_string()) continue;
            auto line = v.get<std::string>();
            if (line.size() > 160) line.resize(160);
            if (!line.empty()) list.push_back(line);
            if (list.size() >= 6) break;
        }
        if (!list.empty()) inst.meta["references"] = list;
    }
    inst.ui.accent   = str("accent", inst.ui.accent);
    inst.ui.mood     = str("mood", inst.ui.mood);
    inst.voicing     = str("voicing", inst.voicing);

    if (auto it = patch.find("polyphony"); it != patch.end() && it->is_number())
        inst.polyphony = clampT(static_cast<int>(it->get<double>()), 1, kMaxVoices);
    if (auto it = patch.find("glide_ms"); it != patch.end() && it->is_number())
        inst.glideMs = clampT(static_cast<float>(it->get<double>()), 0.0f, 5000.0f);

    // --- sections ---------------------------------------------------------
    std::vector<std::string> enabled;
    for (const auto& s : sections()) if (!s.optional) enabled.push_back(s.id);
    if (auto it = patch.find("sections"); it != patch.end() && it->is_array()) {
        for (const auto& v : *it)
            if (v.is_string()) enabled.push_back(v.get<std::string>());
    } else {
        // No guidance: keep everything, rather than silently shipping a
        // stripped-down instrument.
        for (const auto& s : sections()) enabled.push_back(s.id);
    }

    // --- wavetable content ------------------------------------------------
    // The harmonic recipe is one of the strongest character levers the model
    // has, so it is authored per instrument rather than fixed.
    if (auto it = patch.find("wavetable"); it != patch.end() && it->is_object()) {
        for (auto& asset : inst.assets)
            if (asset.id == "wt_main") asset.spec = *it;
    }

    // --- parameter values -------------------------------------------------
    int applied = 0, unknown = 0;
    if (auto it = patch.find("values"); it != patch.end() && it->is_object()) {
        for (auto v = it->begin(); v != it->end(); ++v) {
            if (!v.value().is_number()) continue;
            bool found = false;
            for (auto& p : inst.params) {
                if (p.id != v.key()) continue;
                p.def = clampT(static_cast<float>(v.value().get<double>()), p.min, p.max);
                found = true;
                ++applied;
                break;
            }
            if (!found) ++unknown;
        }
    }
    if (unknown > 0)
        report.warn("values", std::to_string(unknown)
                              + " value(s) referenced controls that do not exist; ignored.");

    // --- selector positions ------------------------------------------------
    if (auto it = patch.find("switches"); it != patch.end() && it->is_object()) {
        for (auto v = it->begin(); v != it->end(); ++v) {
            for (const auto& sw : inst.switches) {
                if (sw.id != v.key()) continue;
                if (ir::NodeSpec* node = inst.findNode(sw.node)) {
                    if (!node->settings.is_object()) node->settings = json::object();
                    node->settings[sw.setting] = v.value();
                }
                break;
            }
        }
    }

    // --- visual style ---------------------------------------------------------
    //
    // Read here rather than inside applyLayout, because the style of an
    // instrument does not depend on whether the model also authored pages: a
    // patch that only sets values still deserves to look like itself.
    if (auto lay = patch.find("layout"); lay != patch.end() && lay->is_object()) {
        if (auto st = lay->find("style"); st != lay->end() && st->is_string()) {
            const auto want = st->get<std::string>();
            for (const char* ok : {"sharp", "soft", "vintage", "minimal"})
                if (want == ok) { inst.ui.style = want; break; }
        }
    }

    // --- control types -------------------------------------------------------
    //
    // A pitch fader is vertical because pitch is up and down. A blend reads as
    // a horizontal travel. An on/off is a switch, not a knob turned to one end.
    // Every control being a circle is what made these instruments look
    // interchangeable however different they sounded.
    if (auto it = patch.find("controls"); it != patch.end() && it->is_object()) {
        static const char* kKinds[] = {"knob", "vslider", "hslider", "toggle"};
        int changed = 0;
        for (auto v = it->begin(); v != it->end(); ++v) {
            if (!v.value().is_string()) continue;
            const auto kind = v.value().get<std::string>();
            bool legal = false;
            for (const char* k : kKinds) if (kind == k) legal = true;
            if (!legal) continue;
            for (auto& p : inst.params)
                if (p.id == v.key()) { p.control = kind; ++changed; break; }
        }
        if (changed > 0)
            report.fixed("controls", "Set " + std::to_string(changed)
                                     + " control(s) to a non-default shape.");
    }

    // --- macros -------------------------------------------------------------
    //
    // Added because a musician asked for "four macro controls: Weight, Bite,
    // Motion, Space" and got the five stock macros instead. It read as the
    // model ignoring a direct instruction; in fact there was no way to express
    // it - the schema had no macros field at all, so compliance was impossible.
    //
    // A macro is the single highest-leverage control on a synth: it is the one
    // knob a producer actually performs with. Letting the model name and wire
    // them is what turns a patch into an instrument.
    if (auto it = patch.find("macros"); it != patch.end() && it->is_array()) {
        // Every exposed control id, so a route to something that does not exist
        // is dropped rather than silently doing nothing.
        std::unordered_set<std::string> known;
        for (const auto& p : inst.params) known.insert(p.id);

        std::vector<ir::MacroSpec> macros;
        int index = 0;
        for (const auto& m : *it) {
            if (!m.is_object() || macros.size() >= static_cast<size_t>(kMaxMacros)) break;
            ir::MacroSpec spec;
            spec.id    = "m_user_" + std::to_string(index++);
            spec.label = m.value("label", std::string("Macro"));
            if (spec.label.size() > 14) spec.label.resize(14);
            spec.def   = clampT(static_cast<float>(m.value("default", 0.0)), 0.0f, 1.0f);

            if (auto routes = m.find("routes"); routes != m.end() && routes->is_array()) {
                for (const auto& r : *routes) {
                    if (!r.is_object() || spec.routes.size() >= 12) continue;
                    ir::MacroRoute route;
                    route.param = r.value("param", std::string());
                    if (!known.count(route.param)) continue;   // unknown target
                    route.depth = clampT(static_cast<float>(r.value("depth", 0.0)), -1.0f, 1.0f);
                    Taper curve = Taper::Linear;
                    parseTaper(r.value("curve", std::string("linear")), curve);
                    route.curve = curve;
                    if (std::abs(route.depth) > 1e-4f) spec.routes.push_back(route);
                }
            }
            // A macro wired to nothing is a knob that lies. Drop it.
            if (!spec.routes.empty()) macros.push_back(std::move(spec));
        }

        if (!macros.empty()) {
            inst.macros = std::move(macros);
            inst.ui.macroRow.clear();
            for (const auto& m : inst.macros) inst.ui.macroRow.push_back(m.id);
        }
    }

    // --- modulation matrix -------------------------------------------------
    // Routing is where an instrument's character actually lives, so the model
    // authors it outright rather than inheriting a fixed set.
    if (auto it = patch.find("mod"); it != patch.end() && it->is_array()) {
        std::vector<ir::ModRoute> kept;
        // Keep the amp envelope route: losing it means silence.
        for (const auto& r : inst.mod)
            if (r.targetNode == "amp" && r.targetParam == "gain" && r.source == "env_1")
                kept.push_back(r);

        // A modulation target can be written either way, and both are accepted.
        //
        // The prompt shows the model a list of CONTROL ids (`f1_cutoff`,
        // `dr_drive`, `lfo_1_rate`) but modulation addresses NODE.PARAM
        // (`filt_1.cutoff`, `drive.drive`, `lfo_1.rate_hz`). Two namespaces for
        // the same thing, only one of them ever shown - so the model wrote
        // `lfo_1.depth`, the route was silently dropped as invalid, and the
        // musician was left looking at a mod wheel that did nothing while the
        // description claimed otherwise.
        //
        // Resolving control ids here closes that trap for good.
        auto resolveTarget = [&](const std::string& target, ir::ModRoute& out) {
            if (ir::splitTarget(target, out.targetNode, out.targetParam)) {
                // Only accept it if the node really has that parameter.
                if (const auto* node = inst.findNode(out.targetNode))
                    if (const auto* man = Registry::instance().find(node->type))
                        for (const auto& pd : man->params)
                            if (pd.id == out.targetParam) return true;
            }
            // Fall back to reading it as an exposed control id.
            for (const auto& p : inst.params) {
                if (p.id != target || p.bind.empty()) continue;
                out.targetNode  = p.bind.front().node;
                out.targetParam = p.bind.front().param;
                return true;
            }
            return false;
        };

        for (const auto& r : *it) {
            if (!r.is_object()) continue;
            ir::ModRoute route;
            route.source = r.value("source", std::string());
            const auto target = r.value("target", std::string());
            if (!resolveTarget(target, route)) continue;
            route.depth = clampT(static_cast<float>(r.value("depth", 0.0)), -1.0f, 1.0f);
            Taper curve = Taper::Linear;
            parseTaper(r.value("curve", std::string("linear")), curve);
            route.curve   = curve;
            route.bipolar = r.value("bipolar", false);
            if (!route.source.empty()) kept.push_back(route);
        }
        if (!kept.empty()) inst.mod = std::move(kept);
    }

    pruneDisabledSections(inst, enabled);

    // Layout runs last: it works on whatever survived the section pass.
    if (auto it = patch.find("layout"); it != patch.end() && it->is_object())
        applyLayout(inst, *it, report);

    report.fixed("patch", "Applied " + std::to_string(applied) + " values across "
                          + std::to_string(inst.params.size()) + " controls.");
    return true;
}

std::string patchSchemaDoc() {
    std::string doc =
        "Return ONE JSON object with this shape:\n\n"
        "{\n"
        "  \"name\": \"Two or three plain words\",\n"
        "  \"description\": \"One sentence, as a sound designer would hand it over.\",\n"
        "  \"accent\": \"#RRGGBB\",\n"
        "  \"voicing\": \"poly\" | \"mono\" | \"legato\",\n"
        "  \"polyphony\": 1-32,\n"
        "  \"glide_ms\": 0-5000,\n"
        "  \"sections\": [\"shape\",\"fx\",\"space\",\"mix\"],\n"
        "  \"wavetable\": {\"method\":\"harmonics\",\"frames\":4,\"morph\":\"lowpass_sweep\",\n"
        "                  \"partials\":[{\"n\":1,\"amp\":1.0},{\"n\":3,\"amp\":0.4}]},\n"
        "  \"switches\": {\"osc_a_wave\": \"saw\", \"f1_mode\": \"lp\"},\n"
        "  \"values\": {\"osc_a_level\": 0.8, \"f1_cutoff\": 2400},\n"
        "  \"mod\": [{\"source\":\"env_2\",\"target\":\"amt_fenv.a\",\"depth\":1.0}],\n"
        "  \"controls\": {\"osc_a_pmod\":\"vslider\", \"wd_width\":\"hslider\",\n"
        "                \"lfo_1_sync\":\"toggle\"},\n"
        "  \"macros\": [{\"label\":\"Weight\", \"default\":0.4,\n"
        "               \"routes\":[{\"param\":\"osc_sub_level\",\"depth\":0.6},\n"
        "                         {\"param\":\"f1_cutoff\",\"depth\":-0.2}]}],\n"
        "  \"references\": [\"Kanye West, Gorgeous (2010) - the driven, mid-forward\n"
        "                    guitar motif and its short slapback\"],\n"
        "  \"layout\": {\n"
        "    \"pages\": [\n"
        "      {\"title\":\"CORE\", \"panels\":[\"WAVETABLE\",\"OSC A\",\"FILTER 1\",\"AMP ENV\"]},\n"
        "      {\"title\":\"MOTION\", \"panels\":[\"LFO 1\",\"MOD ENV\",\"SEQUENCER\"]},\n"
        "      {\"title\":\"AIR\", \"panels\":[\"REVERB\",\"DELAY\",\"EQ\",\"OUTPUT\",\"MACROS\"]}\n"
        "    ],\n"
        "    \"style\": \"sharp\" | \"soft\" | \"vintage\" | \"minimal\",\n"
        "    \"featured\": [\"WAVETABLE\",\"FILTER 1\"],\n"
        "    \"panel_accent\": {\"WAVETABLE\":\"#8FB6D8\",\"FILTER 1\":\"#C9A227\",\n"
        "                      \"REVERB\":\"#5C6B7A\",\"AMP ENV\":\"#B8843F\"}\n"
        "  }\n"
        "}\n\n"
        "- `values` are in ENGINEERING units and are clamped to each control's range.\n"
        "- `sections` lists the OPTIONAL sections to keep. Omit one and its modules\n"
        "  are removed entirely - that is how you leave out effects that do not suit\n"
        "  the sound. Core sections (osc, filter, env, lfo, macros) are always kept.\n"
        "- `mod` REPLACES the default matrix. This is where character lives: decide\n"
        "  what moves what. The amp envelope route is preserved automatically.\n"
        "- Set every value that matters. Anything you omit keeps its default, which\n"
        "  is a neutral init patch, not a finished instrument.\n"
        "\n"
        "## macros - the knobs they will actually perform with\n"
        "\n"
        "REPLACES the stock macro row. Give each one a `label` and a list of\n"
        "`routes`, where every `param` is an exposed control id from the list above\n"
        "and `depth` is -1..1 in that control's normalised range.\n"
        "\n"
        "- If the musician names macros, use EXACTLY those names, in that order,\n"
        "  and that many. \"Four macros: Weight, Bite, Motion, Space\" means four\n"
        "  macros with those four labels - not five stock ones.\n"
        "- One macro should move SEVERAL things at once. That is the whole point:\n"
        "  a \"Weight\" that only changes sub level is a duplicate knob, while one\n"
        "  that raises the sub, drops the cutoff a little and slows the attack is\n"
        "  an instrument voice.\n"
        "- A macro routed to nothing is discarded, so never emit an empty one.\n"
        "\n"
        "## controls - not everything is a knob\n"
        "\n"
        "Every control being a circle is the fastest way to make two instruments\n"
        "look interchangeable however different they sound. Choose the shape that\n"
        "matches what the control DOES:\n"
        "\n"
        "  vslider   things that move up and down: pitch, transpose, a filter\n"
        "            sweep, anything a musician thinks of as high and low. A\n"
        "            vertical pitch fader is standard on real instruments and\n"
        "            almost every good plugin has one.\n"
        "  hslider   blends, mixes, morphs, positions - a travel between two\n"
        "            things. Wide and shallow, so it reads as a journey.\n"
        "  toggle    genuinely two-state: sync on/off, bypass, mono/stereo.\n"
        "  knob      everything else, which is still most things.\n"
        "\n"
        "Use 3-8 non-knob controls on a typical instrument. All knobs is dull; all\n"
        "sliders is unreadable.\n"
        "\n"
        "## style - the visual language\n"
        "\n"
        "  sharp     angular pointers, tick marks, tight rectangles. Clinical and\n"
        "            utilitarian - a mastering tool, a modular rack, techno.\n"
        "  soft      thick rounded arcs, no ticks, generous spacing. Warm and\n"
        "            hand-made - soul, reggae, bedroom pop, anything groovy.\n"
        "  vintage   cream faces, chunky caps, engraved lettering. Tape, valve\n"
        "            gear, 60s and 70s.\n"
        "  minimal   hairline indicators, almost no chrome. Ambient, modern\n"
        "            classical, anything restrained.\n"
        "\n"
        "Pick the one that matches the MUSIC, and let the palette agree with it. A\n"
        "cold technical request wants `sharp` with teals, slates and steel; a warm\n"
        "Jamaican groove wants `soft` with orange, green and gold. Those two must\n"
        "not arrive looking like the same product in different colours.\n"
        "\n"
        "## layout - you are designing the front panel\n"
        "\n"
        "This is not decoration. It is the difference between an instrument someone\n"
        "wants to open and a spreadsheet of knobs.\n"
        "\n"
        "- `pages` are the tabs, in reading order. Use 2-4. Name them for what this\n"
        "  instrument actually does - CORE / MOTION / AIR, BODY / BOW / ROOM,\n"
        "  ENGINE / GRIT / MIX - not OSC / MOD / FX. The names are part of the design.\n"
        "- A panel listed on a page appears there. A panel listed on NO page is\n"
        "  REMOVED from the instrument entirely, module and all. That is how you build\n"
        "  a four-panel plucked string rather than a wall of everything.\n"
        "- Panel order inside a page is the order they are drawn, left to right and\n"
        "  wrapping. Put the panels that define the sound first.\n"
        "- `featured` panels are drawn large. Choose 1-3: the ones a player would\n"
        "  reach for first. A page where everything is the same size has no face.\n"
        "- `panel_accent` colours individual blocks. Work in ONE family - a set of\n"
        "  related hues at different depths, not a rainbow - and let the family carry\n"
        "  the feeling: cold blues and slates for glass and air, ember oranges and\n"
        "  browns for tape and drive, deep violets for something nocturnal. Two or\n"
        "  three panels much brighter than the rest is what makes a panel look\n"
        "  composed rather than tiled.\n"
        "- Aim for 5-9 panels per page. Fewer looks empty; more looks like a menu.\n"
        "- Every panel name must be copied EXACTLY from the control listing.\n";
    return doc;
}


// ---------------------------------------------------------------------------

namespace {

bool mentions(const std::string& text, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (text.find(w) != std::string::npos) return true;
    return false;
}

} // namespace

nlohmann::json heuristicPatch(const std::string& prompt) {
    std::string t;
    for (char c : prompt) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // --- what kind of thing is it -----------------------------------------
    const bool bass    = mentions(t, {"bass", "sub", "808", "low end"});
    const bool pad     = mentions(t, {"pad", "drone", "ambient", "atmosphere", "texture", "cloud"});
    const bool pluck   = mentions(t, {"pluck", "guitar", "harp", "koto", "string", "bell",
                                      "mallet", "marimba", "kalimba", "stab"});
    // A named acoustic or electric instrument gets the physical model, even
    // offline. The floor should not be a synth when a guitar was asked for.
    const bool synthy  = mentions(t, {"synth", "moog", "juno", "303", "808", "analog",
                                      "analogue", "reese", "supersaw", "wavetable"});
    const bool acoustic = !synthy && mentions(t, {"guitar", "bass", "harp", "koto", "banjo",
                                                  "violin", "viola", "cello", "fiddle",
                                                  "acoustic", "nylon", "upright"});
    const bool lead    = mentions(t, {"lead", "riff", "motif", "melody", "hook", "arp"});
    const bool keys    = mentions(t, {"piano", "rhodes", "wurli", "organ", "clav", "keys"});
    // Anything that goes through an amplifier needs the speaker after the
    // drive. Without it a driven signal keeps its harmonics all the way to
    // Nyquist, and that fizz is the clearest single tell of a modelled guitar.
    const bool amped   = acoustic && mentions(t, {"electric", "amp", "pickup", "overdriv",
                                                  "distort", "driven", "fuzz", "crunch",
                                                  "gritty", "rock", "strat", "tele",
                                                  "les paul", "jazzmaster", "blues", "metal"});

    // --- how should it feel ------------------------------------------------
    const bool dark    = mentions(t, {"dark", "warm", "mellow", "dusty", "muffled", "deep",
                                      "moody", "nocturnal", "sad", "melancholy"});
    const bool bright  = mentions(t, {"bright", "glassy", "crisp", "shiny", "icy", "airy",
                                      "crystal", "sparkl", "neon"});
    const bool gritty  = mentions(t, {"gritty", "dirty", "distort", "drive", "fuzz", "aggressive",
                                      "harsh", "crunch", "saturated", "gnarly", "hard"});
    const bool soft    = mentions(t, {"soft", "gentle", "calm", "float", "dreamy", "delicate",
                                      "smooth", "fragile", "intimate", "still"});
    const bool huge    = mentions(t, {"huge", "wide", "cinematic", "epic", "massive", "vast",
                                      "enormous", "big"});
    const bool tape    = mentions(t, {"vintage", "tape", "analog", "analogue", "retro", "lofi",
                                      "lo-fi", "nostalg", "dusty", "70s", "80s"});

    json patch;
    patch["name"]        = acoustic ? "Plain String" : "Starting Point";
    patch["description"] = "Built without the model - a full instrument shaped by the words "
                           "in your request, ready to edit.";

    // --- voicing ----------------------------------------------------------
    // Poly unless the request is unambiguously a single-line source. Getting
    // this wrong makes chords collapse, and the musician cannot fix it from
    // the panel.
    const bool monoSource = bass && !pad && !keys && !pluck
                          && mentions(t, {"mono", "303", "acid", "sub bass", "monosynth"});
    patch["voicing"]   = monoSource ? "mono" : "poly";
    patch["polyphony"] = monoSource ? 1 : (pad || huge ? 12 : 8);
    if (monoSource) patch["glide_ms"] = 40;

    // --- envelope ----------------------------------------------------------
    // On the physical model the STRING is what decays, so the amplifier stays
    // largely out of the way and lets it. Shutting the VCA to 0.10 after 420 ms
    // over the top of a ringing string is the envelope of a synth pluck, and it
    // throws away the one thing the model was built to provide.
    // A plucked string cannot fade in. "soft" used to win this, so "a soft
    // acoustic guitar" was given a 420 ms swell - and an instrument that swells
    // is not a guitar however good the string underneath it is. Soft means
    // gentler and darker on a plucked instrument, never slower.
    const double attack  = acoustic ? 2.0
                         : (pad || soft ? (huge ? 900.0 : 420.0)
                                        : (pluck ? 3.0 : 12.0));
    const double decay   = acoustic ? 2500.0 : (pluck ? 420.0 : (bass ? 260.0 : 700.0));
    const double sustain = acoustic ? 0.62 : (pluck ? 0.10 : (bass ? 0.55 : 0.80));
    const double release = pad || huge ? 2200.0
                                       : (acoustic ? 380.0 : (pluck ? 700.0 : 500.0));

    // --- filter ------------------------------------------------------------
    double cutoff = 2600.0;
    if (bass)         cutoff =  620.0;
    else if (dark)    cutoff = 1200.0;
    else if (bright)  cutoff = 7200.0;
    else if (pad)     cutoff = 2200.0;
    // A plucked string has already shaped its own spectrum, and it decays from
    // the top down on its own. Closing a 24 dB lowpass at 2.6 kHz over the top
    // of it just makes it dull - measured centroid 587 Hz against 1424 Hz for a
    // real recording of the same instrument.
    else if (acoustic) cutoff = 8000.0;
    const double reso = soft ? 0.12 : (gritty ? 0.42 : 0.24);

    patch["values"] = {
        {"osc_a_level",   acoustic ? 0.0 : 0.80},
        {"str_level",     acoustic ? 0.85 : 0.0},
        // The loop damping decides how fast the top of the string dies, and it
        // is applied once per period, so it bites hard. At 0.28 damping and a
        // 4.2 kHz pick tone the offline guitar measured 9% of its energy above
        // 2 kHz against 23% for a real recording of one - dull, in a way no
        // downstream control could put back.
        {"str_damp",      bass ? 0.38 : (dark ? 0.30 : 0.20)},
        {"str_bright",    bright ? 8000.0 : (bass ? 2600.0 : (dark ? 4200.0 : 6500.0))},
        // 0.90, not 0.984. Through the T60 mapping 0.984 is an ELEVEN second
        // ring - a pad with a fast attack. The rack default and the corrector
        // were both fixed for this; the offline floor was left behind writing
        // the old value straight over the top of the fixed default.
        {"str_decay",     bass ? 0.93 : (pluck ? 0.905 : 0.94)},
        // Where along the string it is picked, and how stiff that string is.
        // Neither was ever set here, so the offline instrument had no picking
        // character at all - and those two are most of what separates a plucked
        // string from a comb filter with a fast envelope.
        {"str_pick",      bass ? 0.16 : 0.13},
        {"str_stiff",     bass ? 0.22 : 0.32},
        // A solid-body electric has no box to speak of - its resonator is the
        // speaker cabinet, and running both smears the pick attack across the
        // comb's delay for no gain. The body belongs on the instruments that
        // actually have one.
        {"body_mix",      (acoustic && !amped) ? 0.22 : 0.0},
        // The air resonance of a real box, not a fixed 220 Hz.
        //
        // 220 Hz is A3, so on an E3 the comb put a loud unrequested peak on the
        // FOURTH harmonic - measured 12 dB above the fundamental, which the ear
        // resolves as the wrong note two octaves up. A guitar body resonates
        // near 100 Hz and a bass cabinet lower still; down there the resonance
        // reinforces the low end instead of inventing a partial.
        {"body_freq",     bass ? 62.0 : 100.0},
        {"osc_a_detune",  huge ? 22.0 : (pad ? 12.0 : 6.0)},
        {"osc_b_level",   pad || huge ? 0.55 : 0.30},
        {"osc_b_detune",  huge ? 30.0 : 14.0},
        {"osc_sub_level", bass ? 0.70 : (pad ? 0.12 : 0.0)},
        {"f1_cutoff",     cutoff},
        {"f1_reso",       reso},
        {"f1_track",      pluck || keys ? 0.60 : 0.30},
        {"f1_drive",      gritty ? 2.60 : 1.10},
        {"env_1_a",       attack},
        {"env_1_d",       decay},
        {"env_1_s",       sustain},
        {"env_1_r",       release},
        {"env_2_a",       pluck ? 1.0 : 40.0},
        {"env_2_d",       pluck ? 220.0 : 900.0},
        {"env_2_s",       0.0},
        {"env_2_r",       600.0},
        {"lfo_1_rate",    soft ? 0.35 : 2.20},
        {"rv_mix",       pad || huge ? 0.42 : (soft ? 0.26 : (bass ? 0.04 : 0.16))},
        {"rv_size",      pad || huge ? 0.80 : 0.45},
        {"dl_mix",       lead || pluck ? 0.20 : 0.08},
        {"dl_time",      320.0},
        {"dl_fbk",  0.28},
        {"dr_drive",    gritty ? 3.20 : 1.15},
        // The speaker. Only on an instrument that is actually plugged in - a
        // nylon guitar through a 4x12 is its own kind of wrong.
        {"cab_mix",     amped ? 0.85 : 0.0},
        {"cab_top",     bass ? 3500.0 : 4800.0},
        {"tp_drive",    tape ? 0.45 : 0.0},
        {"wd_width",    huge || pad ? 0.70 : 0.35},
    };

    patch["switches"] = {
        {"osc_a_wave", bright || gritty ? "saw" : (soft || pad ? "triangle" : "saw")},
        {"osc_b_wave", pad || huge ? "saw" : "square"},
        {"f1_mode",    bright && !bass ? "lp" : "lp"},
        {"f1_slope",   gritty || bass ? "24" : "12"},
        {"osc_a_uni",  huge || pad ? 4 : 2},
    };

    // Cutoff follows the note envelope: that movement is what separates an
    // instrument from a tone, and it costs one route.
    patch["mod"] = json::array({
        {{"source", "env_2"}, {"target", "amt_fenv.a"}, {"depth", pluck ? 0.85 : 0.45}},
        {{"source", "m_vel"}, {"target", "amp.gain"},   {"depth", 0.35}},
    });

    // --- layout ------------------------------------------------------------
    // Three balanced pages. The fallback has to look designed too; an
    // instrument that arrives ugly reads as a broken product, whatever the
    // reason it arrived.
    json core   = acoustic
        ? json::array({"STRING", "BODY", "SUB", "MIX", "FILTER 1", "ENV 1"})
        : json::array({"OSC A", "OSC B", "SUB", "MIX", "FILTER 1", "ENV 1"});
    json motion = json::array({"ENV 2", "LFO 1", "LFO 2", "AMOUNTS", "SEQUENCER"});
    json space  = json::array({"EQ", "DRIVE", "DELAY", "REVERB", "OUTPUT", "MACROS"});
    // The cabinet has to be ON A PAGE, not merely set.
    //
    // A panel that appears on no page is removed from the instrument entirely,
    // node and all - so the offline fallback could set `cab_mix` to anything it
    // liked and the value went straight in the bin, exactly like the earlier
    // round of effects that wrote to control ids which did not exist. Every
    // offline electric guitar has been a DI into a distortion box.
    if (amped)  space.insert(space.begin() + 2, "CABINET");
    if (tape)   space.insert(space.begin() + (amped ? 3 : 2), "TAPE");
    if (huge || pad) space.insert(space.begin() + 3, "DIMENSION");

    // Even offline, a warm request and a cold one should not look identical.
    const bool warm = dark || tape || mentions(t, {"soul", "reggae", "groove", "jam",
                                                   "funk", "vintage", "motown", "dusty"});
    const bool cold = bright || mentions(t, {"techno", "clinical", "industrial", "digital",
                                             "cold", "steel", "chrome", "utilitarian"});
    // Shape the controls this instrument actually HAS. A plucked patch has no
    // OSC A panel, so styling `osc_a_pmod` there would silently do nothing -
    // the same dead-reference trap that has bitten this codebase repeatedly.
    patch["controls"] = acoustic
        ? json{{"str_bright", "vslider"},   // pick tone reads as high and low
               {"str_damp",   "vslider"},
               {"wd_width",   "hslider"},   // width is a travel
               {"rv_mix",     "hslider"}}
        : json{{"osc_a_pmod", "vslider"},   // pitch is up and down
               {"osc_b_pmod", "vslider"},
               {"wd_width",   "hslider"},
               {"rv_mix",     "hslider"}};

    patch["layout"] = {
        {"style", tape ? "vintage" : (warm ? "soft" : (soft && !cold ? "minimal" : "sharp"))},
        {"pages", json::array({
            {{"title", bass ? "LOW END" : "CORE"},   {"panels", core}},
            {{"title", "MOTION"},                     {"panels", motion}},
            {{"title", pad || huge ? "AIR" : "OUT"},  {"panels", space}},
        })},
        {"featured", json::array({"FILTER 1", "ENV 1"})},
    };

    // A palette that at least matches the mood, rather than the default orange
    // on everything.
    // The palette has to agree with the style, or a "soft" instrument in cold
    // teal is just as incoherent as the uniform orange it replaced.
    if      (tape)   { patch["accent"] = "#C98A3E"; }   // vintage: amber
    else if (warm)   { patch["accent"] = "#D2702F"; }   // soft: burnt orange
    else if (cold)   { patch["accent"] = "#3FA9A0"; }   // sharp: teal
    else if (dark)   { patch["accent"] = "#8A6B4F"; }
    else if (bright) { patch["accent"] = "#7FB2D8"; }
    else if (gritty) { patch["accent"] = "#C1502E"; }
    else if (soft)   { patch["accent"] = "#8E93B8"; }
    else             { patch["accent"] = "#9A8577"; }

    // Per-panel colours drawn from the same family, so the page has variety
    // without becoming a rainbow.
    if (warm || tape) {
        patch["layout"]["panel_accent"] = {{"FILTER 1", "#C7833A"}, {"ENV 1", "#8FA45C"},
                                           {"REVERB",   "#B4622C"}};
    } else if (cold) {
        patch["layout"]["panel_accent"] = {{"FILTER 1", "#4FBFB4"}, {"ENV 1", "#6E8FA8"},
                                           {"REVERB",   "#3C7C86"}};
    }

    return patch;
}

} // namespace forge::arch
