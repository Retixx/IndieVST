#include "core/llm/PromptBuilder.h"

#include "core/dsp/Registry.h"
#include "core/ir/Ir.h"
#include "core/llm/CannedLibrary.h"

#include <sstream>

namespace forge::llm {
namespace {

const char* kRole = R"(You are the instrument designer inside Forge, an AI synthesiser plugin.

You do NOT write DSP code. You design instruments by emitting a JSON "instrument
graph": which pre-built DSP modules to instantiate, how to wire their audio,
how to route modulation between them, which knobs to expose to the musician,
and what generated assets (wavetables, transfer curves, envelope shapes) to
bake.

Your output is validated, safety-checked and instantiated on a real-time audio
thread. Anything that does not validate is rejected. Your job is to be BOTH
musically imaginative AND structurally exact.

The most important thing you can do is choose a signal path that actually suits
what the user described. Two different requests should produce two structurally
different graphs, not the same synthesiser with different numbers.)";

const char* kSchema = R"(## Specification format

Return ONE JSON object. No prose, no markdown fences, no explanation.

{
  "ir_version": "0.1",
  "name": "Short Instrument Name",
  "description": "One sentence describing the sound.",
  "voicing": "poly" | "mono" | "legato",
  "glide_ms": 0,                       // portamento; only meaningful for mono/legato
  "polyphony": 1..32,                  // must be 1 for mono/legato
  "assets":  [ ... ],
  "nodes":   [ ... ],
  "audio":   [ ... ],
  "mod":     [ ... ],
  "params":  [ ... ],
  "macros":  [ ... ],
  "ui":      { ... }
}

### nodes
{"id": "osc_main", "type": "osc.analog", "scope": "voice",
 "settings": {"wave": "saw", "unison": 3}}

- `id` must match ^[a-z][a-z0-9_]{0,31}$ and be unique.
- `type` must be one of the module types listed below. Nothing else exists.
- `scope`: "voice" means one instance per played note (oscillators, envelopes,
  per-note filters). "global" means one instance total, fed by the sum of all
  voices (reverb, delay, chorus, the output stage). Each module lists the
  scopes it allows.
- `settings` are construction-time choices. They cannot be automated or
  modulated. Anything not listed as a setting for that type is invalid.

### audio
{"from": "osc_main", "to": "filt", "gain": 1.0}

- Both ends are node ids. Multiple connections into one node SUM automatically,
  so you rarely need an explicit mixer.
- Signal flows voice -> voice, voice -> global, global -> global. A global node
  can NEVER feed a voice node.
- A feedback loop is legal ONLY if it passes through fx.delay or filter.comb.
- Do not declare out.master; it is added automatically as the final stage.

### mod
{"source": "env_filt", "target": "filt.cutoff", "depth": 0.6,
 "curve": "linear"|"exp"|"log", "bipolar": false}

- `source` is the id of a node whose entry says mod_source: true.
- `target` is "nodeId.paramId" and the parameter must exist on that module.
- `depth` is -1..1 applied in the target's NORMALISED domain, so 1.0 sweeps the
  parameter across its whole range. Depths sum and are then clamped.
- Modulation is resolved every 16 samples using the previous block's source
  values, so modulation loops are safe.
- A voice-scope modulator cannot drive a global-scope parameter (there are many
  voices and only one global node). A global modulator CAN drive voice-scope
  parameters - that is how you make one LFO sweep every note together.
- To make a modulation DEPTH user-adjustable, route the modulator into a
  util.math node's "a" input, expose its "b" input as a parameter, and route
  the util.math output at the target. The bass example below does exactly this.

### params  (the knobs the musician actually gets)
{"id": "cutoff", "label": "Cutoff", "unit": "Hz",
 "min": 40, "max": 18000, "default": 900, "taper": "log",
 "control": "knob"|"slider"|"switch"|"combo", "group": "Filter",
 "bind": [{"node": "filt", "param": "cutoff", "scale": 1.0, "offset": 0.0}]}

- min/max/default are in ENGINEERING units, and must sit inside the range the
  bound module parameter allows.
- taper "log" requires min > 0 and is right for frequencies and times.
  "exp" is right for amounts that should feel fine near zero.
- One parameter may bind to several module parameters:
  moduleValue = paramValue * scale + offset.
- Every parameter must bind to at least one module parameter.

### macros
{"id": "warmth", "label": "Warmth", "default": 0.35,
 "routes": [{"param": "cutoff", "depth": -0.4}, {"param": "drive_amt", "depth": 0.6}]}

- A macro is 0..1 and offsets several exposed parameters at once, in their
  normalised domain. This is the "one knob that makes it evolve" control.

### assets  (generated tables - emit recipes, never raw samples)
Wavetable:
{"id": "hollow", "kind": "wavetable",
 "spec": {"method": "harmonics", "frames": 4,
          "morph": "lowpass_sweep"|"harmonic_fade"|"odd_to_even"|"none",
          "partials": [{"n": 1, "amp": 1.0, "phase": 0.0}, {"n": 3, "amp": 0.5}]}}
  (or {"method": "shape", "shape": "saw"|"square"|"triangle"|"sine"|"odd"|"even",
       "max_harmonic": 64})
Transfer curve:
{"id": "soft_fold", "kind": "curve",
 "spec": {"method": "breakpoints", "interp": "linear"|"catmullrom",
          "points": [[-1,-0.85],[-0.4,-0.4],[0,0],[0.4,0.4],[1,0.85]]}}
  (or {"method": "polynomial", "coeffs": [0, 1, 0, -0.3]})
Envelope shape:
{"id": "bell_decay", "kind": "envelope",
 "spec": {"method": "stages", "sustain_stage": -1, "loop": false,
          "stages": [{"level": 1.0, "time_ms": 2, "curve": "linear"},
                     {"level": 0.3, "time_ms": 600, "curve": "exp"},
                     {"level": 0.0, "time_ms": 2000, "curve": "exp"}]}}

### ui
{"theme": {"accent": "#E4572E", "mood": "warm"},
 "sections": [{"title": "Filter", "params": ["cutoff", "reso"]}],
 "macro_row": ["warmth"]})";

const char* kConstraints = R"(## Hard constraints

1. EVERY instrument needs an amplitude path that ends: something must drive a
   `vca` gain from an envelope (env.adsr or env.multi) in voice scope. Without
   it a note starts and never stops.
2. Do not declare an `out.master` node. It is appended automatically.
3. Never reference a module type, setting, or parameter that is not in the
   capability manifest. Inventing one fails validation.
4. A `log` taper requires min > 0.
5. Voice-scope source modules: at most 6. Filters: at most 4. Reverbs: at most
   2. Delays: at most 3.
6. Prefer FEWER, well-chosen modules over many. A great bass is five nodes.
7. Set `polyphony` to 1 and `voicing` to "mono" or "legato" for basses and
   leads. Use "poly" with 8-16 voices for pads, keys and bells.
8. Give the musician 6-14 parameters. Every one should change the sound in a
   way they can hear. Name and group them the way a synth would.
9. Include at least one macro. Macros are what make an instrument feel designed
   rather than generated.
10. Reuse an idiom only when it fits. If the request is unusual, build an
    unusual graph - the module set supports comb resonators, FM, wavetable
    morphing, waveshaping and phase modulation, not just saw-into-filter.)";

const char* kHeuristics = R"(## Sound design heuristics

These are starting points, not templates. Depart from them when the request
calls for it.

- BASS: mono, glide 20-60 ms, saw or square an octave down plus a sine sub two
  octaves down, filter.ladder low-pass with cutoff 200-600 Hz, a filter
  envelope with a 200-900 ms decay, modest resonance, fx.drive for weight,
  little or no reverb. Keep it centred.
- PAD: poly 12-16, two detuned analog oscillators with unison 3-7 (retrigger
  false), filter.svf lp 24 dB opened to 1.5-4 kHz, slow attack 0.5-2 s, long
  release, a slow global LFO drifting the cutoff by 0.1-0.25 depth, chorus then
  a large reverb.
- PLUCK / KEYS: poly, fast attack (1-5 ms), decay 150-600 ms, sustain low. Put
  an envelope on the timbre as well as the amplitude - FM index or filter
  cutoff falling fast is what makes a pluck read as a pluck. A synced delay
  fills the space between notes.
- LEAD: mono or legato with 40-80 ms glide, detuned saws, ladder filter with
  keytrack 0.3-0.6, vibrato from a voice LFO scaled by mod.modwheel, drive,
  a dotted-eighth delay.
- BELL / METALLIC: osc.fm2 with a non-integer ratio (3.5, 7.1, 5.7), a fast
  envelope on FM index, a long multi-stage amplitude decay, generous reverb.
  Inharmonic ratios are what make metal sound like metal.
- PERCUSSIVE / NOISE: osc.noise into a band-pass filter.svf with a very short
  envelope; add filter.comb for tuned resonance.
- PLUCKED STRING: osc.noise burst into filter.comb with feedback 0.85-0.95 and
  the comb frequency tracking the note.
- DARK / LOFI: lower the cutoff, add shaper.wave with type "bitcrush" or a
  generated "curve", roll off the delay damping.

Loudness discipline: oscillator levels around 0.6-0.8, and if two oscillators
both run hot, use connection gains below 1.0. The output stage limits, but
arriving at it already clipped sounds worse than arriving with headroom.)";

std::string exampleBlock() {
    std::ostringstream os;
    os << "## Worked examples\n\n"
          "Study the structure, not the specific numbers.\n";
    for (const auto& inst : cannedLibrary()) {
        if (!inst.useAsExample) continue;
        os << "\nUser: \"" << (std::string(inst.id) == "molten_sub"
                                ? "warm analog bass with a slow filter sweep and a bit of drive"
                                : "wide evolving pad that drifts, glassy and cold")
           << "\"\nYou:\n" << inst.json << "\n";
    }
    return os.str();
}

} // namespace

std::string buildSystemPrompt() {
    std::ostringstream os;
    os << kRole << "\n\n";

    os << "## Capability manifest\n\n"
          "This is the complete set of DSP modules that exist. Every type, setting,\n"
          "parameter and range below is authoritative; nothing outside it is real.\n\n```json\n"
       << Registry::instance().capabilityJson().dump(1, ' ', false,
                                                     nlohmann::json::error_handler_t::replace)
       << "\n```\n\n";

    os << kSchema     << "\n\n";
    os << kConstraints << "\n\n";
    os << kHeuristics  << "\n\n";
    os << exampleBlock();

    os << "\n\nReturn only the JSON object for the instrument the user asks for.\n";
    return os.str();
}

PromptSpec buildGenerationPrompt(const std::string& userPrompt, const std::string& currentIrJson) {
    PromptSpec spec;
    spec.system = buildSystemPrompt();

    std::ostringstream os;
    if (!currentIrJson.empty()) {
        os << "Here is the instrument currently loaded:\n\n```json\n"
           << currentIrJson << "\n```\n\n"
           << "The musician wants this change:\n\n\"" << userPrompt << "\"\n\n"
           << "Apply it and return the COMPLETE updated specification as one JSON object. "
              "Keep everything they did not ask you to change - same node ids, same "
              "parameter ids, same layout - so their knob positions survive the edit.";
    } else {
        os << "Design an instrument for this request:\n\n\"" << userPrompt << "\"\n\n"
           << "Return one JSON object.";
    }
    spec.user = os.str();
    return spec;
}

int estimateTokens(const std::string& text) {
    return static_cast<int>(text.size() / 4);   // good enough for a build-time warning
}

std::string irJsonSchema() {
    using nlohmann::json;

    auto strType = [] { return json{{"type", "string"}}; };
    auto numType = [] { return json{{"type", "number"}}; };

    json nodeSchema = {
        {"type", "object"},
        {"required", json::array({"id", "type", "scope"})},
        {"properties", {
            {"id", strType()},
            {"type", strType()},
            {"scope", {{"type", "string"}, {"enum", json::array({"voice", "global"})}}},
            {"settings", {{"type", "object"}}}
        }}
    };

    json audioSchema = {
        {"type", "object"},
        {"required", json::array({"from", "to"})},
        {"properties", {{"from", strType()}, {"to", strType()}, {"gain", numType()}}}
    };

    json modSchema = {
        {"type", "object"},
        {"required", json::array({"source", "target", "depth"})},
        {"properties", {
            {"source", strType()}, {"target", strType()}, {"depth", numType()},
            {"curve", {{"type", "string"}, {"enum", json::array({"linear", "exp", "log"})}}},
            {"bipolar", {{"type", "boolean"}}}
        }}
    };

    json bindSchema = {
        {"type", "object"},
        {"required", json::array({"node", "param"})},
        {"properties", {{"node", strType()}, {"param", strType()},
                        {"scale", numType()}, {"offset", numType()}}}
    };

    json paramSchema = {
        {"type", "object"},
        {"required", json::array({"id", "label", "min", "max", "default", "bind"})},
        {"properties", {
            {"id", strType()}, {"label", strType()}, {"unit", strType()},
            {"min", numType()}, {"max", numType()}, {"default", numType()},
            {"taper", {{"type", "string"}, {"enum", json::array({"linear", "log", "exp"})}}},
            {"control", {{"type", "string"}, {"enum", json::array({"knob", "slider", "switch", "combo"})}}},
            {"group", strType()},
            {"bind", {{"type", "array"}, {"items", bindSchema}}}
        }}
    };

    json macroSchema = {
        {"type", "object"},
        {"required", json::array({"id", "label", "routes"})},
        {"properties", {
            {"id", strType()}, {"label", strType()}, {"default", numType()},
            {"routes", {{"type", "array"}, {"items", {
                {"type", "object"},
                {"required", json::array({"param", "depth"})},
                {"properties", {{"param", strType()}, {"depth", numType()},
                                {"curve", strType()}}}
            }}}}
        }}
    };

    json assetSchema = {
        {"type", "object"},
        {"required", json::array({"id", "kind", "spec"})},
        {"properties", {
            {"id", strType()},
            {"kind", {{"type", "string"}, {"enum", json::array({"wavetable", "curve", "envelope"})}}},
            {"spec", {{"type", "object"}}}
        }}
    };

    json root = {
        {"type", "object"},
        {"required", json::array({"name", "nodes", "audio", "params"})},
        {"properties", {
            {"ir_version", strType()},
            {"name", strType()},
            {"description", strType()},
            {"voicing", {{"type", "string"}, {"enum", json::array({"poly", "mono", "legato"})}}},
            {"glide_ms", numType()},
            {"polyphony", {{"type", "integer"}}},
            {"assets", {{"type", "array"}, {"items", assetSchema}}},
            {"nodes",  {{"type", "array"}, {"items", nodeSchema}}},
            {"audio",  {{"type", "array"}, {"items", audioSchema}}},
            {"mod",    {{"type", "array"}, {"items", modSchema}}},
            {"params", {{"type", "array"}, {"items", paramSchema}}},
            {"macros", {{"type", "array"}, {"items", macroSchema}}},
            {"ui",     {{"type", "object"}}}
        }}
    };
    return root.dump();
}

} // namespace forge::llm
