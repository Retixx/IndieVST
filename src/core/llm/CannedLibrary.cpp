#include "core/llm/CannedLibrary.h"

#include <algorithm>
#include <cctype>

namespace forge::llm {
namespace {

// ---------------------------------------------------------------------------
// 1. Bass - subtractive, mono, filter envelope, drive.
//    Demonstrates: util.math used to make a modulation DEPTH user-controllable,
//    which is the idiomatic way to build an "Env Amount" knob.
// ---------------------------------------------------------------------------
const char* kMoltenSub = R"JSON({
  "ir_version": "0.1",
  "name": "Molten Sub",
  "description": "Warm analog bass. Detuned saw over a sine sub, through a saturating ladder filter with a slow envelope sweep and output drive.",
  "voicing": "mono",
  "glide_ms": 40,
  "polyphony": 1,
  "assets": [],
  "nodes": [
    {"id": "osc_saw", "type": "osc.analog", "scope": "voice", "settings": {"wave": "saw", "unison": 2, "octave": -1}},
    {"id": "osc_sub", "type": "osc.analog", "scope": "voice", "settings": {"wave": "sine", "octave": -2}},
    {"id": "filt",    "type": "filter.ladder", "scope": "voice"},
    {"id": "amp",     "type": "vca", "scope": "voice"},
    {"id": "env_amp", "type": "env.adsr", "scope": "voice"},
    {"id": "env_filt","type": "env.adsr", "scope": "voice"},
    {"id": "env_amt", "type": "util.math", "scope": "voice", "settings": {"op": "mul"}},
    {"id": "drive",   "type": "fx.drive", "scope": "global"},
    {"id": "master",  "type": "out.master", "scope": "global"}
  ],
  "audio": [
    {"from": "osc_saw", "to": "filt",   "gain": 1.0},
    {"from": "osc_sub", "to": "filt",   "gain": 1.0},
    {"from": "filt",    "to": "amp",    "gain": 1.0},
    {"from": "amp",     "to": "drive",  "gain": 1.0},
    {"from": "drive",   "to": "master", "gain": 1.0}
  ],
  "mod": [
    {"source": "env_amp",  "target": "amp.gain",     "depth": 1.0},
    {"source": "env_filt", "target": "env_amt.a",    "depth": 1.0},
    {"source": "env_amt",  "target": "filt.cutoff",  "depth": 1.0, "curve": "exp"}
  ],
  "params": [
    {"id": "saw_level",  "label": "Saw",     "min": 0, "max": 1, "default": 0.7,  "group": "Oscillators", "bind": [{"node": "osc_saw", "param": "level"}]},
    {"id": "sub_level",  "label": "Sub",     "min": 0, "max": 1, "default": 0.75, "group": "Oscillators", "bind": [{"node": "osc_sub", "param": "level"}]},
    {"id": "detune",     "label": "Detune",  "unit": "cents", "min": 0, "max": 30, "default": 6, "group": "Oscillators", "bind": [{"node": "osc_saw", "param": "detune_cents"}]},
    {"id": "cutoff",     "label": "Cutoff",  "unit": "Hz", "min": 40, "max": 12000, "default": 420, "taper": "log", "group": "Filter", "bind": [{"node": "filt", "param": "cutoff"}]},
    {"id": "reso",       "label": "Reso",    "min": 0, "max": 0.9, "default": 0.22, "group": "Filter", "bind": [{"node": "filt", "param": "resonance"}]},
    {"id": "filter_env", "label": "Env Amt", "min": 0, "max": 1, "default": 0.55, "group": "Filter", "bind": [{"node": "env_amt", "param": "b"}]},
    {"id": "sweep",      "label": "Sweep",   "unit": "ms", "min": 20, "max": 4000, "default": 700, "taper": "log", "group": "Filter", "bind": [{"node": "env_filt", "param": "decay_ms"}]},
    {"id": "drive_amt",  "label": "Drive",   "min": 1, "max": 12, "default": 2.4, "taper": "log", "group": "Output", "bind": [{"node": "drive", "param": "drive"}]},
    {"id": "amp_attack", "label": "Attack",  "unit": "ms", "min": 0.5, "max": 2000, "default": 3,   "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "attack_ms"}]},
    {"id": "amp_decay",  "label": "Decay",   "unit": "ms", "min": 10, "max": 4000, "default": 600, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "decay_ms"}]},
    {"id": "amp_sustain","label": "Sustain", "min": 0, "max": 1, "default": 0.55, "group": "Amp", "bind": [{"node": "env_amp", "param": "sustain"}]},
    {"id": "amp_release","label": "Release", "unit": "ms", "min": 10, "max": 4000, "default": 250, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "release_ms"}]}
  ],
  "macros": [
    {"id": "warmth", "label": "Warmth", "default": 0.35,
     "routes": [{"param": "cutoff", "depth": -0.35}, {"param": "drive_amt", "depth": 0.5}, {"param": "reso", "depth": 0.15}]}
  ],
  "ui": {
    "theme": {"accent": "#E4572E", "mood": "warm"},
    "sections": [
      {"title": "Oscillators", "params": ["saw_level", "sub_level", "detune"]},
      {"title": "Filter", "params": ["cutoff", "reso", "filter_env", "sweep"]},
      {"title": "Amp", "params": ["amp_attack", "amp_decay", "amp_sustain", "amp_release"]},
      {"title": "Output", "params": ["drive_amt"]}
    ],
    "macro_row": ["warmth"]
  }
})JSON";

// ---------------------------------------------------------------------------
// 2. Pad - wide unison, slow envelopes, global LFO drift, chorus + reverb.
//    Demonstrates: a GLOBAL modulator driving a VOICE parameter, and a global
//    effects chain sitting after the voice sum.
// ---------------------------------------------------------------------------
const char* kGlassDrift = R"JSON({
  "ir_version": "0.1",
  "name": "Glass Drift",
  "description": "Wide evolving pad. Heavily detuned saw stack with a square underneath, a slow filter drift, chorus and a long reverb.",
  "voicing": "poly",
  "glide_ms": 0,
  "polyphony": 16,
  "assets": [],
  "nodes": [
    {"id": "osc_a",   "type": "osc.analog", "scope": "voice", "settings": {"wave": "saw", "unison": 5, "retrigger": false}},
    {"id": "osc_b",   "type": "osc.analog", "scope": "voice", "settings": {"wave": "square", "unison": 3, "octave": -1, "retrigger": false}},
    {"id": "filt",    "type": "filter.svf", "scope": "voice", "settings": {"mode": "lp", "slope": "24"}},
    {"id": "amp",     "type": "vca", "scope": "voice"},
    {"id": "env_amp", "type": "env.adsr", "scope": "voice"},
    {"id": "lfo_drift","type": "lfo", "scope": "global", "settings": {"wave": "sine", "bipolar": true}},
    {"id": "chorus",  "type": "fx.chorus", "scope": "global"},
    {"id": "verb",    "type": "fx.reverb", "scope": "global"},
    {"id": "master",  "type": "out.master", "scope": "global"}
  ],
  "audio": [
    {"from": "osc_a",  "to": "filt",   "gain": 0.8},
    {"from": "osc_b",  "to": "filt",   "gain": 0.6},
    {"from": "filt",   "to": "amp",    "gain": 1.0},
    {"from": "amp",    "to": "chorus", "gain": 1.0},
    {"from": "chorus", "to": "verb",   "gain": 1.0},
    {"from": "verb",   "to": "master", "gain": 1.0}
  ],
  "mod": [
    {"source": "env_amp",   "target": "amp.gain",    "depth": 1.0},
    {"source": "lfo_drift", "target": "filt.cutoff", "depth": 0.18}
  ],
  "params": [
    {"id": "cutoff",     "label": "Cutoff",  "unit": "Hz", "min": 100, "max": 16000, "default": 2400, "taper": "log", "group": "Filter", "bind": [{"node": "filt", "param": "cutoff"}]},
    {"id": "reso",       "label": "Reso",    "min": 0, "max": 0.7, "default": 0.12, "group": "Filter", "bind": [{"node": "filt", "param": "resonance"}]},
    {"id": "detune",     "label": "Detune",  "unit": "cents", "min": 0, "max": 45, "default": 18, "group": "Oscillators", "bind": [{"node": "osc_a", "param": "detune_cents"}, {"node": "osc_b", "param": "detune_cents", "scale": 0.6}]},
    {"id": "sub_level",  "label": "Body",    "min": 0, "max": 1, "default": 0.45, "group": "Oscillators", "bind": [{"node": "osc_b", "param": "level"}]},
    {"id": "drift_rate", "label": "Drift",   "unit": "Hz", "min": 0.02, "max": 2, "default": 0.12, "taper": "log", "group": "Motion", "bind": [{"node": "lfo_drift", "param": "rate_hz"}]},
    {"id": "chorus_mix", "label": "Chorus",  "min": 0, "max": 1, "default": 0.45, "group": "Space", "bind": [{"node": "chorus", "param": "mix"}]},
    {"id": "reverb_mix", "label": "Reverb",  "min": 0, "max": 1, "default": 0.42, "group": "Space", "bind": [{"node": "verb", "param": "mix"}]},
    {"id": "reverb_size","label": "Size",    "min": 0, "max": 1, "default": 0.82, "group": "Space", "bind": [{"node": "verb", "param": "size"}]},
    {"id": "amp_attack", "label": "Attack",  "unit": "ms", "min": 1, "max": 8000, "default": 900, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "attack_ms"}]},
    {"id": "amp_release","label": "Release", "unit": "ms", "min": 10, "max": 12000, "default": 2200, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "release_ms"}]},
    {"id": "amp_sustain","label": "Sustain", "min": 0, "max": 1, "default": 0.85, "group": "Amp", "bind": [{"node": "env_amp", "param": "sustain"}]}
  ],
  "macros": [
    {"id": "air",    "label": "Air",    "default": 0.4, "routes": [{"param": "cutoff", "depth": 0.4}, {"param": "reverb_mix", "depth": 0.25}]},
    {"id": "motion", "label": "Motion", "default": 0.3, "routes": [{"param": "drift_rate", "depth": 0.5}, {"param": "chorus_mix", "depth": 0.3}]}
  ],
  "ui": {
    "theme": {"accent": "#6C8AE4", "mood": "cold"},
    "sections": [
      {"title": "Oscillators", "params": ["detune", "sub_level"]},
      {"title": "Filter", "params": ["cutoff", "reso"]},
      {"title": "Amp", "params": ["amp_attack", "amp_sustain", "amp_release"]},
      {"title": "Space", "params": ["chorus_mix", "reverb_mix", "reverb_size", "drift_rate"]}
    ],
    "macro_row": ["air", "motion"]
  }
})JSON";

// ---------------------------------------------------------------------------
// 3. Pluck - FM, fast envelopes on both amplitude and timbre, synced delay.
// ---------------------------------------------------------------------------
const char* kPorcelainPluck = R"JSON({
  "ir_version": "0.1",
  "name": "Porcelain Pluck",
  "description": "Bright FM pluck with a fast timbral decay and a tempo-synced delay.",
  "voicing": "poly",
  "polyphony": 16,
  "nodes": [
    {"id": "osc_fm",  "type": "osc.fm2", "scope": "voice", "settings": {"ratio": 3.0}},
    {"id": "filt",    "type": "filter.svf", "scope": "voice", "settings": {"mode": "lp", "slope": "12"}},
    {"id": "amp",     "type": "vca", "scope": "voice"},
    {"id": "env_amp", "type": "env.adsr", "scope": "voice"},
    {"id": "env_idx", "type": "env.adsr", "scope": "voice"},
    {"id": "vel",     "type": "mod.velocity", "scope": "voice"},
    {"id": "dly",     "type": "fx.delay", "scope": "global", "settings": {"sync": true, "division": "1/8", "pingpong": true}},
    {"id": "verb",    "type": "fx.reverb", "scope": "global"},
    {"id": "master",  "type": "out.master", "scope": "global"}
  ],
  "audio": [
    {"from": "osc_fm", "to": "filt",   "gain": 1.0},
    {"from": "filt",   "to": "amp",    "gain": 1.0},
    {"from": "amp",    "to": "dly",    "gain": 1.0},
    {"from": "dly",    "to": "verb",   "gain": 1.0},
    {"from": "verb",   "to": "master", "gain": 1.0}
  ],
  "mod": [
    {"source": "env_amp", "target": "amp.gain",    "depth": 1.0},
    {"source": "env_idx", "target": "osc_fm.index","depth": 0.75, "curve": "exp"},
    {"source": "vel",     "target": "filt.cutoff", "depth": 0.25}
  ],
  "params": [
    {"id": "brightness", "label": "Bright",  "min": 0, "max": 12, "default": 4.5, "taper": "exp", "group": "Tone", "bind": [{"node": "osc_fm", "param": "index"}]},
    {"id": "fm_fall",    "label": "Fall",    "unit": "ms", "min": 5, "max": 2000, "default": 120, "taper": "log", "group": "Tone", "bind": [{"node": "env_idx", "param": "decay_ms"}]},
    {"id": "cutoff",     "label": "Cutoff",  "unit": "Hz", "min": 200, "max": 18000, "default": 7000, "taper": "log", "group": "Filter", "bind": [{"node": "filt", "param": "cutoff"}]},
    {"id": "reso",       "label": "Reso",    "min": 0, "max": 0.8, "default": 0.1, "group": "Filter", "bind": [{"node": "filt", "param": "resonance"}]},
    {"id": "amp_decay",  "label": "Decay",   "unit": "ms", "min": 20, "max": 4000, "default": 420, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "decay_ms"}]},
    {"id": "amp_release","label": "Release", "unit": "ms", "min": 10, "max": 4000, "default": 300, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "release_ms"}]},
    {"id": "delay_mix",  "label": "Delay",   "min": 0, "max": 1, "default": 0.24, "group": "Space", "bind": [{"node": "dly", "param": "mix"}]},
    {"id": "delay_fb",   "label": "Repeats", "min": 0, "max": 0.85, "default": 0.4, "group": "Space", "bind": [{"node": "dly", "param": "feedback"}]},
    {"id": "reverb_mix", "label": "Reverb",  "min": 0, "max": 1, "default": 0.2, "group": "Space", "bind": [{"node": "verb", "param": "mix"}]}
  ],
  "macros": [
    {"id": "sparkle", "label": "Sparkle", "default": 0.4, "routes": [{"param": "brightness", "depth": 0.5}, {"param": "cutoff", "depth": 0.3}]}
  ],
  "ui": {
    "theme": {"accent": "#39C6A0", "mood": "bright"},
    "sections": [
      {"title": "Tone", "params": ["brightness", "fm_fall"]},
      {"title": "Filter", "params": ["cutoff", "reso"]},
      {"title": "Amp", "params": ["amp_decay", "amp_release"]},
      {"title": "Space", "params": ["delay_mix", "delay_fb", "reverb_mix"]}
    ],
    "macro_row": ["sparkle"]
  }
})JSON";

// ---------------------------------------------------------------------------
// 4. Lead - mono legato with glide, vibrato from a per-voice LFO.
// ---------------------------------------------------------------------------
const char* kNeonLead = R"JSON({
  "ir_version": "0.1",
  "name": "Neon Lead",
  "description": "Cutting mono lead. Detuned saws into a ladder filter, glide between notes, vibrato on the mod wheel, drive and a slap delay.",
  "voicing": "legato",
  "glide_ms": 60,
  "polyphony": 1,
  "nodes": [
    {"id": "osc_saw", "type": "osc.analog", "scope": "voice", "settings": {"wave": "saw", "unison": 3}},
    {"id": "osc_pls", "type": "osc.analog", "scope": "voice", "settings": {"wave": "pulse", "octave": -1}},
    {"id": "filt",    "type": "filter.ladder", "scope": "voice"},
    {"id": "amp",     "type": "vca", "scope": "voice"},
    {"id": "env_amp", "type": "env.adsr", "scope": "voice"},
    {"id": "lfo_vib", "type": "lfo", "scope": "voice", "settings": {"wave": "sine", "bipolar": true, "retrigger": true}},
    {"id": "wheel",   "type": "mod.modwheel", "scope": "voice"},
    {"id": "vib_amt", "type": "util.math", "scope": "voice", "settings": {"op": "mul"}},
    {"id": "drive",   "type": "fx.drive", "scope": "global"},
    {"id": "dly",     "type": "fx.delay", "scope": "global", "settings": {"sync": true, "division": "1/8D"}},
    {"id": "master",  "type": "out.master", "scope": "global"}
  ],
  "audio": [
    {"from": "osc_saw", "to": "filt",   "gain": 1.0},
    {"from": "osc_pls", "to": "filt",   "gain": 0.5},
    {"from": "filt",    "to": "amp",    "gain": 1.0},
    {"from": "amp",     "to": "drive",  "gain": 1.0},
    {"from": "drive",   "to": "dly",    "gain": 1.0},
    {"from": "dly",     "to": "master", "gain": 1.0}
  ],
  "mod": [
    {"source": "env_amp", "target": "amp.gain", "depth": 1.0},
    {"source": "lfo_vib", "target": "vib_amt.a", "depth": 1.0},
    {"source": "wheel",   "target": "vib_amt.b", "depth": 1.0},
    {"source": "vib_amt", "target": "osc_saw.pitch_mod_semis", "depth": 0.02},
    {"source": "vib_amt", "target": "osc_pls.pitch_mod_semis", "depth": 0.02}
  ],
  "params": [
    {"id": "cutoff",     "label": "Cutoff",  "unit": "Hz", "min": 100, "max": 16000, "default": 3200, "taper": "log", "group": "Filter", "bind": [{"node": "filt", "param": "cutoff"}]},
    {"id": "reso",       "label": "Reso",    "min": 0, "max": 0.9, "default": 0.35, "group": "Filter", "bind": [{"node": "filt", "param": "resonance"}]},
    {"id": "keytrack",   "label": "Track",   "min": 0, "max": 1, "default": 0.4, "group": "Filter", "bind": [{"node": "filt", "param": "keytrack"}]},
    {"id": "detune",     "label": "Detune",  "unit": "cents", "min": 0, "max": 30, "default": 11, "group": "Oscillators", "bind": [{"node": "osc_saw", "param": "detune_cents"}]},
    {"id": "pulse_level","label": "Pulse",   "min": 0, "max": 1, "default": 0.4, "group": "Oscillators", "bind": [{"node": "osc_pls", "param": "level"}]},
    {"id": "vib_rate",   "label": "Vib Rate","unit": "Hz", "min": 0.5, "max": 12, "default": 5.2, "taper": "log", "group": "Motion", "bind": [{"node": "lfo_vib", "param": "rate_hz"}]},
    {"id": "drive_amt",  "label": "Drive",   "min": 1, "max": 16, "default": 3.2, "taper": "log", "group": "Output", "bind": [{"node": "drive", "param": "drive"}]},
    {"id": "delay_mix",  "label": "Delay",   "min": 0, "max": 1, "default": 0.22, "group": "Space", "bind": [{"node": "dly", "param": "mix"}]},
    {"id": "amp_attack", "label": "Attack",  "unit": "ms", "min": 0.5, "max": 1000, "default": 8, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "attack_ms"}]},
    {"id": "amp_release","label": "Release", "unit": "ms", "min": 10, "max": 3000, "default": 260, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "release_ms"}]}
  ],
  "macros": [
    {"id": "push", "label": "Push", "default": 0.3, "routes": [{"param": "drive_amt", "depth": 0.6}, {"param": "cutoff", "depth": 0.25}, {"param": "reso", "depth": 0.2}]}
  ],
  "ui": {
    "theme": {"accent": "#F03E7A", "mood": "aggressive"},
    "sections": [
      {"title": "Oscillators", "params": ["detune", "pulse_level"]},
      {"title": "Filter", "params": ["cutoff", "reso", "keytrack"]},
      {"title": "Amp", "params": ["amp_attack", "amp_release"]},
      {"title": "Motion", "params": ["vib_rate"]},
      {"title": "Output", "params": ["drive_amt", "delay_mix"]}
    ],
    "macro_row": ["push"]
  }
})JSON";

// ---------------------------------------------------------------------------
// 5. Bell - inharmonic FM with a GENERATED multi-stage envelope asset.
//    Demonstrates: the 'envelope' asset recipe driving env.multi.
// ---------------------------------------------------------------------------
const char* kCathedralBell = R"JSON({
  "ir_version": "0.1",
  "name": "Cathedral Bell",
  "description": "Inharmonic FM bell with a long generated decay shape and a large reverb.",
  "voicing": "poly",
  "polyphony": 12,
  "assets": [
    {"id": "bell_decay", "kind": "envelope",
     "spec": {"method": "stages", "stages": [
       {"level": 1.0, "time_ms": 2, "curve": "linear"},
       {"level": 0.42, "time_ms": 260, "curve": "exp"},
       {"level": 0.18, "time_ms": 1400, "curve": "exp"},
       {"level": 0.0, "time_ms": 3200, "curve": "exp"}]}}
  ],
  "nodes": [
    {"id": "osc_fm",  "type": "osc.fm2", "scope": "voice", "settings": {"ratio": 3.5}},
    {"id": "osc_hi",  "type": "osc.fm2", "scope": "voice", "settings": {"ratio": 7.1, "octave": 1}},
    {"id": "amp",     "type": "vca", "scope": "voice"},
    {"id": "env_bell","type": "env.multi", "scope": "voice", "settings": {"shape": "bell_decay"}},
    {"id": "env_idx", "type": "env.adsr", "scope": "voice"},
    {"id": "verb",    "type": "fx.reverb", "scope": "global"},
    {"id": "master",  "type": "out.master", "scope": "global"}
  ],
  "audio": [
    {"from": "osc_fm", "to": "amp",    "gain": 1.0},
    {"from": "osc_hi", "to": "amp",    "gain": 0.35},
    {"from": "amp",    "to": "verb",   "gain": 1.0},
    {"from": "verb",   "to": "master", "gain": 1.0}
  ],
  "mod": [
    {"source": "env_bell", "target": "amp.gain",     "depth": 1.0},
    {"source": "env_idx",  "target": "osc_fm.index", "depth": 0.6, "curve": "exp"},
    {"source": "env_idx",  "target": "osc_hi.index", "depth": 0.8, "curve": "exp"}
  ],
  "params": [
    {"id": "strike",    "label": "Strike",  "min": 0, "max": 12, "default": 6, "taper": "exp", "group": "Tone", "bind": [{"node": "osc_fm", "param": "index"}]},
    {"id": "shimmer",   "label": "Shimmer", "min": 0, "max": 1, "default": 0.35, "group": "Tone", "bind": [{"node": "osc_hi", "param": "level"}]},
    {"id": "strike_fall","label": "Fall",   "unit": "ms", "min": 10, "max": 3000, "default": 300, "taper": "log", "group": "Tone", "bind": [{"node": "env_idx", "param": "decay_ms"}]},
    {"id": "decay",     "label": "Decay",   "unit": "x", "min": 0.2, "max": 4, "default": 1, "taper": "log", "group": "Amp", "bind": [{"node": "env_bell", "param": "time_scale"}]},
    {"id": "reverb_mix","label": "Reverb",  "min": 0, "max": 1, "default": 0.4, "group": "Space", "bind": [{"node": "verb", "param": "mix"}]},
    {"id": "reverb_size","label": "Size",   "min": 0, "max": 1, "default": 0.9, "group": "Space", "bind": [{"node": "verb", "param": "size"}]}
  ],
  "macros": [
    {"id": "distance", "label": "Distance", "default": 0.35,
     "routes": [{"param": "reverb_mix", "depth": 0.5}, {"param": "strike", "depth": -0.3}]}
  ],
  "ui": {
    "theme": {"accent": "#C9A227", "mood": "metallic"},
    "sections": [
      {"title": "Tone", "params": ["strike", "shimmer", "strike_fall"]},
      {"title": "Amp", "params": ["decay"]},
      {"title": "Space", "params": ["reverb_mix", "reverb_size"]}
    ],
    "macro_row": ["distance"]
  }
})JSON";

// ---------------------------------------------------------------------------
// 6. Drone - GENERATED wavetable, comb resonance, phaser and reverb.
//    Demonstrates: the 'wavetable' asset recipe and a global LFO morphing a
//    per-voice wavetable position.
// ---------------------------------------------------------------------------
const char* kTectonicDrone = R"JSON({
  "ir_version": "0.1",
  "name": "Tectonic Drone",
  "description": "Slow evolving drone built from a generated hollow wavetable, tuned comb resonance, phaser and a cavernous reverb.",
  "voicing": "poly",
  "polyphony": 8,
  "assets": [
    {"id": "hollow", "kind": "wavetable",
     "spec": {"method": "harmonics", "frames": 4, "morph": "lowpass_sweep",
              "partials": [{"n": 1, "amp": 1.0}, {"n": 3, "amp": 0.45}, {"n": 5, "amp": 0.24},
                           {"n": 7, "amp": 0.16}, {"n": 11, "amp": 0.09}, {"n": 13, "amp": 0.05}]}}
  ],
  "nodes": [
    {"id": "osc_wt",  "type": "osc.wavetable", "scope": "voice", "settings": {"table": "hollow", "octave": -1, "retrigger": false}},
    {"id": "noise",   "type": "osc.noise", "scope": "voice", "settings": {"color": "pink"}},
    {"id": "comb",    "type": "filter.comb", "scope": "voice"},
    {"id": "filt",    "type": "filter.svf", "scope": "voice", "settings": {"mode": "lp", "slope": "12"}},
    {"id": "amp",     "type": "vca", "scope": "voice"},
    {"id": "env_amp", "type": "env.adsr", "scope": "voice"},
    {"id": "lfo_morph","type": "lfo", "scope": "global", "settings": {"wave": "tri", "bipolar": false}},
    {"id": "phs",     "type": "fx.phaser", "scope": "global"},
    {"id": "verb",    "type": "fx.reverb", "scope": "global"},
    {"id": "master",  "type": "out.master", "scope": "global"}
  ],
  "audio": [
    {"from": "osc_wt", "to": "comb",   "gain": 1.0},
    {"from": "noise",  "to": "comb",   "gain": 0.25},
    {"from": "comb",   "to": "filt",   "gain": 1.0},
    {"from": "filt",   "to": "amp",    "gain": 1.0},
    {"from": "amp",    "to": "phs",    "gain": 1.0},
    {"from": "phs",    "to": "verb",   "gain": 1.0},
    {"from": "verb",   "to": "master", "gain": 1.0}
  ],
  "mod": [
    {"source": "env_amp",   "target": "amp.gain",       "depth": 1.0},
    {"source": "lfo_morph", "target": "osc_wt.position","depth": 0.6}
  ],
  "params": [
    {"id": "position",   "label": "Morph",  "min": 0, "max": 1, "default": 0.2, "group": "Tone", "bind": [{"node": "osc_wt", "param": "position"}]},
    {"id": "morph_rate", "label": "Rate",   "unit": "Hz", "min": 0.01, "max": 1, "default": 0.07, "taper": "log", "group": "Tone", "bind": [{"node": "lfo_morph", "param": "rate_hz"}]},
    {"id": "air",        "label": "Air",    "min": 0, "max": 1, "default": 0.25, "group": "Tone", "bind": [{"node": "noise", "param": "level"}]},
    {"id": "comb_freq",  "label": "Res",    "unit": "Hz", "min": 40, "max": 2000, "default": 130, "taper": "log", "group": "Resonator", "bind": [{"node": "comb", "param": "freq"}]},
    {"id": "comb_fb",    "label": "Ring",   "min": 0, "max": 0.9, "default": 0.55, "group": "Resonator", "bind": [{"node": "comb", "param": "feedback"}]},
    {"id": "cutoff",     "label": "Cutoff", "unit": "Hz", "min": 80, "max": 12000, "default": 1800, "taper": "log", "group": "Filter", "bind": [{"node": "filt", "param": "cutoff"}]},
    {"id": "phaser_mix", "label": "Phaser", "min": 0, "max": 1, "default": 0.35, "group": "Space", "bind": [{"node": "phs", "param": "mix"}]},
    {"id": "reverb_mix", "label": "Reverb", "min": 0, "max": 1, "default": 0.5, "group": "Space", "bind": [{"node": "verb", "param": "mix"}]},
    {"id": "amp_attack", "label": "Attack", "unit": "ms", "min": 10, "max": 8000, "default": 1400, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "attack_ms"}]},
    {"id": "amp_release","label": "Release","unit": "ms", "min": 100, "max": 12000, "default": 3500, "taper": "log", "group": "Amp", "bind": [{"node": "env_amp", "param": "release_ms"}]}
  ],
  "macros": [
    {"id": "depth", "label": "Depth", "default": 0.4,
     "routes": [{"param": "cutoff", "depth": -0.3}, {"param": "reverb_mix", "depth": 0.35}, {"param": "comb_fb", "depth": 0.25}]}
  ],
  "ui": {
    "theme": {"accent": "#8A6FE0", "mood": "dark"},
    "sections": [
      {"title": "Tone", "params": ["position", "morph_rate", "air"]},
      {"title": "Resonator", "params": ["comb_freq", "comb_fb"]},
      {"title": "Filter", "params": ["cutoff"]},
      {"title": "Amp", "params": ["amp_attack", "amp_release"]},
      {"title": "Space", "params": ["phaser_mix", "reverb_mix"]}
    ],
    "macro_row": ["depth"]
  }
})JSON";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

const std::vector<CannedInstrument>& cannedLibrary() {
    static const std::vector<CannedInstrument> lib = {
        {"molten_sub", "Molten Sub",
         {"bass", "sub", "low", "808", "warm", "analog", "deep", "fat"}, true,  kMoltenSub},
        {"glass_drift", "Glass Drift",
         {"pad", "ambient", "wide", "lush", "strings", "atmosphere", "soft", "airy"}, true, kGlassDrift},
        {"porcelain_pluck", "Porcelain Pluck",
         {"pluck", "stab", "short", "percussive", "keys", "mallet", "bright"}, false, kPorcelainPluck},
        {"neon_lead", "Neon Lead",
         {"lead", "solo", "melody", "cutting", "acid", "mono", "aggressive"}, false, kNeonLead},
        {"cathedral_bell", "Cathedral Bell",
         {"bell", "chime", "metallic", "glass", "fm", "inharmonic", "toll"}, false, kCathedralBell},
        {"tectonic_drone", "Tectonic Drone",
         {"drone", "texture", "evolving", "dark", "cinematic", "noise", "sustained"}, false, kTectonicDrone},
    };
    return lib;
}

const CannedInstrument* bestCannedMatch(const std::string& prompt) {
    const auto& lib = cannedLibrary();
    if (lib.empty()) return nullptr;

    const std::string p = lower(prompt);
    const CannedInstrument* best = &lib.front();
    int bestScore = 0;

    for (const auto& inst : lib) {
        int score = 0;
        for (const auto& kw : inst.keywords)
            if (p.find(kw) != std::string::npos)
                score += static_cast<int>(kw.size());   // longer matches are more specific
        if (score > bestScore) { bestScore = score; best = &inst; }
    }
    return best;
}

} // namespace forge::llm
