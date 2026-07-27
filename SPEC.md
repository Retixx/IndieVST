# FORGE — AI Instrument Generator VST3

**Spec version:** 0.1 (MVP)
**Author:** Bombo
**Date:** 2026-07-26
**Pitch date:** 2026-07-29 (T+3 days)
**Status:** Design frozen for MVP. Post-MVP roadmap in §14.

---

## 1. Problem & Thesis

Professional-grade virtual instruments sit behind paywalls that price out independent
producers. A single flagship synth is $150–$500; a usable collection is thousands. The
paywall is not protecting scarce raw material — it's protecting *sound design labour*.
Sound design is the act of choosing DSP modules, wiring them, and dialing parameters
until the result is musical.

**Thesis:** that labour is now automatable. An LLM that understands both musical
language and signal-flow semantics can perform the wiring and dialing. The DSP itself —
the part that must be fast, stable, and correct — stays hand-written, compiled, and
trusted.

**Product:** one VST3 plugin. You load it, describe an instrument in plain English, and
a few seconds later you are playing a purpose-built synthesizer with a purpose-built
control surface. Then you keep talking to it to refine it.

---

## 2. Product Definition

### 2.1 One-sentence description

Forge is a VST3 instrument that turns a natural-language description into a playable,
tweakable synthesizer, generated at runtime from a library of trusted DSP primitives.

### 2.2 Core user loop

1. User inserts **Forge** on a MIDI track in their DAW.
2. Plugin opens on a **chat screen**: a text box and a prompt.
   > *"warm analog bass with a slow filter sweep and a bit of drive"*
3. User submits. A background worker calls the LLM, which returns a **schema-constrained
   Instrument Specification (IR)** — modules, audio routing, modulation routing,
   parameters, macro controls, generated assets, and a UI layout description.
4. The IR is **validated, safety-checked, and repaired** if needed, then **instantiated**
   as a runtime DSP graph from precompiled primitives.
5. The UI transitions to the **generated instrument view**: real knobs, sliders, and
   switches chosen by the spec. The instrument is immediately playable from MIDI.
6. User can:
   - play and tweak controls (all real-time, no LLM involvement);
   - reopen chat to **modify** the current instrument ("make it darker, add chorus");
   - use the **instrument dropdown** to switch between instruments generated in this
     session or previously saved;
   - pick **"+ Create New Instrument"** to return to a blank chat screen.

### 2.3 What this is NOT (MVP)

- Not a preset browser dressed up as AI. The LLM composes topology, not just values.
- Not runtime code generation or JIT compilation. No compiler ships in the plugin.
- Not a separate exported VST3 per instrument. Generated instruments live *inside* Forge
  as validated graphs + saved configuration. (Export is a Phase 3 feature — §14.3.)
- Not a sample-based / neural-audio instrument. Pure synthesis DSP.
- Not multi-user, no accounts, no cloud sync in MVP.

### 2.4 Non-negotiable invariants

These hold from MVP through every future phase. Violating any of them is a bug, not a
tradeoff.

| # | Invariant |
|---|-----------|
| I1 | The LLM never runs on, blocks, or allocates for the real-time audio thread. |
| I2 | No network I/O, file I/O, locking, or heap allocation in `processBlock`. |
| I3 | Every LLM output passes schema validation + safety checks before instantiation. |
| I4 | Audio output is deterministic given (graph, parameters, MIDI, seed). |
| I5 | A malformed, hostile, or nonsensical LLM response degrades gracefully — never a crash, never a hang, never a full-scale DC blast into the user's monitors. |
| I6 | The graph schema and primitive library are versioned and extensible without breaking saved instruments. |

---

## 3. Tech Stack

| Layer | Choice | Rationale |
|---|---|---|
| Language | **C++20** | Required for VST3/JUCE; RT-safe control over allocation. |
| Plugin framework | **JUCE 8** (`juce_audio_processors`, `juce_gui_basics`, `juce_audio_utils`) | Fastest path to a real VST3 on Windows; ships the VST3 SDK. Note: `juce_dsp` is deliberately **not** used — every primitive is hand-written in `forge_core`, which keeps the engine framework-free and headlessly testable. |
| Build | **CMake ≥ 3.22** + `juce_add_plugin`, MSVC 2022 x64 | Reproducible, CI-ready, no Projucer drift. |
| JSON | **nlohmann/json** (single header, vendored) | Ergonomic; `json::parse` with exceptions off for untrusted input. |
| HTTP | **JUCE `URL::createInputStream`** (worker thread) | Zero extra deps, TLS on Windows via WinHTTP. Swap to cpp-httplib only if streaming is needed. |
| UI | **JUCE Components** (native, custom LookAndFeel) | 3-day-safe. `WebBrowserComponent` UI is a Phase 2 option (§14.2). |
| Testing | **Catch2** for IR validator + DSP unit tests; **pluginval** for host-compat | Validator correctness is the single highest-risk area. |
| Persistence | JSON files under `%APPDATA%/Forge/instruments/` + full state in DAW session via `getStateInformation` | Instruments survive both project reload and reinstall. |
| LLM | **Provider-abstracted** (§7) | No vendor lock; canned-response fallback keeps the demo alive offline. |

### 3.1 Third-party licensing note (matters for the pitch)

The VST3 SDK is dual-licensed: **GPLv3** or a **proprietary Steinberg license** (free,
requires signing the agreement and registering the product). JUCE is dual-licensed:
**GPLv3** or a **commercial JUCE license**. For a closed-source commercial Forge you need
both the Steinberg VST3 license agreement and a paid JUCE license (or use AGPL-free
alternatives later). This is a known, solved, budgeted cost — not a blocker — but you
should be able to answer it if an investor asks. For the MVP demo, GPLv3 terms are fine.

---

## 4. System Architecture

```
┌───────────────────────────────────────────────────────────────────────┐
│  DAW (FL Studio / Ableton / Reaper)                                   │
│                                                                       │
│  ┌─────────────────────── Forge VST3 ────────────────────────────┐   │
│  │                                                                │   │
│  │  MESSAGE THREAD              WORKER POOL         AUDIO THREAD  │   │
│  │  ─────────────               ───────────         ────────────  │   │
│  │  ┌──────────────┐                                              │   │
│  │  │  Chat View   │──prompt──▶ ┌──────────────┐                  │   │
│  │  │  Instr. View │            │ LLM Adapter  │                  │   │
│  │  │  Dropdown    │            │  (provider)  │                  │   │
│  │  └──────────────┘            └──────┬───────┘                  │   │
│  │        ▲                            │ raw JSON                 │   │
│  │        │                     ┌──────▼───────┐                  │   │
│  │        │                     │ IR Validator │                  │   │
│  │        │                     │  + Repairer  │                  │   │
│  │        │                     └──────┬───────┘                  │   │
│  │        │                            │ valid IR                 │   │
│  │        │                     ┌──────▼───────┐                  │   │
│  │        │                     │ Asset Baker  │ (wavetables,     │   │
│  │        │                     └──────┬───────┘  curves, LUTs)   │   │
│  │        │                            │                          │   │
│  │        │                     ┌──────▼───────┐                  │   │
│  │        │                     │ Graph Builder│ allocates all    │   │
│  │        │                     │  (compiles   │ buffers, voices, │   │
│  │        │                     │   IR→graph)  │ mod matrix here  │   │
│  │        │                     └──────┬───────┘                  │   │
│  │        │                            │ GraphInstance*           │   │
│  │        │  UI rebuild ◀──────────────┤                          │   │
│  │        │                     ┌──────▼──────────────────┐       │   │
│  │        │                     │ atomic publish (swap)   │──────▶│  │
│  │        │                     └─────────────────────────┘  active│  │
│  │        │                                                  graph │  │
│  │  ┌─────┴────────┐            ┌──────────────┐          ┌───────▼┐ │
│  │  │ Retire Queue │◀───────────│  Old graph   │          │process │ │
│  │  │ (GC on timer)│            └──────────────┘          │ Block  │ │
│  │  └──────────────┘                                      └────────┘ │
│  └────────────────────────────────────────────────────────────────┘   │
└───────────────────────────────────────────────────────────────────────┘
```

### 4.1 Thread model & the lock-free graph swap

This is the mechanism that makes I1/I2 true.

- The audio thread reads `std::atomic<GraphInstance*> activeGraph` once per
  `processBlock` (acquire) and renders from it. Never allocates, never locks.
- The worker thread builds a **complete, fully-allocated** `GraphInstance` — node
  objects, per-voice state, scratch buffers, baked wavetables, mod-matrix routing table,
  parameter smoothers, all sized and prepared for the current sample rate and block size.
- Publish is a single `activeGraph.exchange(newGraph, std::memory_order_acq_rel)`.
- The displaced pointer goes into a lock-free retire FIFO along with the current value of
  an `std::atomic<uint64_t> blockCounter` (incremented by the audio thread each block).
  A message-thread `Timer` (500 ms) deletes retired graphs whose stamp is at least 2
  blocks behind the live counter — guaranteeing no audio thread still holds the pointer.
- Crossfade: on swap, the audio thread ramps the outgoing graph's tail out over 20 ms
  while ramping the new graph in, so switching instruments mid-playback never clicks. The
  outgoing graph is retired only after the ramp completes.

**Rule of thumb enforced in review:** if a function can be reached from `processBlock`,
it may not call `new`, `delete`, `malloc`, `std::string`, `std::vector::push_back`,
`std::mutex::lock`, or anything in `<filesystem>` / `juce::File` / `juce::URL`.

### 4.2 Voice scope vs global scope

The IR marks each node's scope:

- **`voice`** — instantiated once per voice (oscillators, envelopes, per-voice filters,
  per-voice LFOs, VCAs). Polyphony fixed at **16 voices** for MVP, with round-robin
  oldest-quietest voice stealing and a 5 ms release ramp on steal.
- **`global`** — instantiated once, fed the summed voice output (reverb, delay, chorus,
  master drive, limiter, global LFOs used as macro modulators).

The Graph Builder topologically sorts each scope separately and produces a flat
execution list (`std::vector<Node*>` in evaluation order) so the audio thread just walks
an array — no recursion, no pointer chasing through a tree.

### 4.3 Module rendering contract

```cpp
struct RenderContext {
    double sampleRate;
    int    numSamples;
    float* const* scratch;      // pre-allocated scratch bus pool
    const float* modValues;     // resolved modulation for this block
    uint64_t seed;              // deterministic RNG seed
};

class Module {
public:
    virtual ~Module() = default;
    // Called on worker thread only.
    virtual void prepare (double sampleRate, int maxBlock, const NodeSpec&) = 0;
    virtual void reset() = 0;
    // Called on audio thread only. Must be allocation-free and non-blocking.
    virtual void process (const RenderContext&, const float** in, int numIn,
                          float** out, int numOut) noexcept = 0;
};
```

Modulation is resolved **per block** (control rate = block rate, block size clamped to
≤ 64 samples internally by sub-block splitting) so mod-heavy patches stay smooth without
per-sample matrix cost. All continuous parameters run through
`juce::SmoothedValue<float, ValueSmoothingTypes::Linear>` with a 5–20 ms ramp.

---

## 5. The Instrument IR (Intermediate Representation)

The IR is the contract between the LLM and the DSP engine. It is the most important
artifact in the system. It must be:

- **Expressive enough** that two different prompts produce genuinely different signal
  paths, not the same synth with different numbers.
- **Constrained enough** that a language model hits it reliably and a validator can
  prove it safe.
- **Versioned**, so instruments saved today still load after the schema grows.

### 5.1 Top-level shape

```jsonc
{
  "ir_version": "0.1",
  "name": "Molten Sub",
  "description": "Warm analog bass with slow filter sweep and drive.",
  "meta": { "prompt": "...", "model": "...", "generated_at": "2026-07-26T12:00:00Z" },

  "assets":  [ /* §5.4 */ ],
  "nodes":   [ /* §5.2 */ ],
  "audio":   [ /* §5.3 audio connections */ ],
  "mod":     [ /* §5.3 modulation routes  */ ],
  "params":  [ /* §5.5 exposed controls   */ ],
  "macros":  [ /* §5.6 macro controls     */ ],
  "ui":      { /* §5.7 layout hint        */ }
}
```

### 5.2 Nodes

```jsonc
{
  "id": "osc_main",
  "type": "osc.analog",
  "scope": "voice",
  "settings": { "wave": "saw", "unison": 3, "detune_cents": 9.0, "octave": -1 }
}
```

- `id` — unique, `^[a-z][a-z0-9_]{0,31}$`.
- `type` — must exist in the primitive registry (§6) at the declared `ir_version`.
- `scope` — `voice` | `global`. Validator enforces legality per type.
- `settings` — static, non-automatable construction-time config. Each key is type-checked
  and range-checked against the primitive's manifest.

Hard cap for MVP: **32 nodes**, **≤ 6 voice-scope oscillators**, **≤ 4 filters**,
**≤ 2 reverbs**, **≤ 3 delays**.

### 5.3 Connections

**Audio** (`audio`): `{ "from": "osc_main:out", "to": "filt_lp:in", "gain": 1.0 }`

**Modulation** (`mod`):
```jsonc
{
  "source": "env_filt:out",
  "target": "filt_lp.cutoff",
  "depth": 0.65,
  "curve": "exp",          // linear | exp | log | sqr | asset:<id>
  "bipolar": false
}
```

- `target` addresses a **modulatable parameter** declared in the primitive manifest.
  Nothing else can be modulated.
- `depth` ∈ [-1, 1], applied in the parameter's normalized domain, summed across routes,
  then clamped to [0, 1] before mapping to engineering units. Summing cannot explode.
- Modulation is inherently acyclic-safe because mod is evaluated once per block from the
  previous block's source values.

### 5.4 Generated assets

The LLM does **not** emit thousands of raw floats — that's slow, token-expensive, and
error-prone. It emits compact *generative descriptions*; the **Asset Baker** expands them
on the worker thread into tables.

```jsonc
// Wavetable from a harmonic series
{ "id": "wt_hollow", "kind": "wavetable",
  "spec": { "method": "harmonics",
            "partials": [ {"n":1,"amp":1.0,"phase":0.0},
                          {"n":3,"amp":0.45},{"n":5,"amp":0.22},{"n":7,"amp":0.11} ],
            "frames": 4, "morph": "lowpass_sweep" } }

// Waveshaper transfer curve from breakpoints
{ "id": "shape_soft", "kind": "curve",
  "spec": { "method": "breakpoints",
            "points": [[-1,-0.85],[-0.5,-0.48],[0,0],[0.5,0.48],[1,0.85]],
            "interp": "catmullrom" } }

// Multi-stage envelope shape
{ "id": "env_pluck", "kind": "envelope",
  "spec": { "method": "stages",
            "stages": [ {"level":1.0,"time_ms":2,"curve":"exp"},
                        {"level":0.35,"time_ms":180,"curve":"exp"},
                        {"level":0.0,"time_ms":600,"curve":"exp"} ] } }
```

Baker guarantees: tables are band-limited (mip-mapped per octave for wavetables),
normalized to peak 1.0, DC-removed, and monotonic where required (curves). A curve the
LLM describes as unbounded gets clamped, not rejected.

### 5.5 Parameters (the instrument's controls)

```jsonc
{
  "id": "cutoff",
  "label": "Cutoff",
  "unit": "Hz",
  "min": 40.0, "max": 18000.0, "default": 900.0,
  "taper": "log",
  "bind": [ { "node": "filt_lp", "param": "cutoff" } ],
  "control": "knob",        // knob | slider | switch | combo
  "group": "Filter"
}
```

One parameter may bind to several node parameters (e.g. one "Drive" knob hitting both a
shaper and an output trim with different scaling — supported via optional `scale` and
`offset` per bind entry).

Cap: **≤ 24 exposed parameters** for MVP.

### 5.6 Macros

Macros are top-level performance controls (max **8**) that modulate multiple parameters
with independent depth and curve — the "one knob makes it evolve" control that sells a
synth on stage.

```jsonc
{ "id": "macro_warmth", "label": "Warmth", "default": 0.35,
  "routes": [ { "param": "cutoff",  "depth": -0.4, "curve": "exp" },
              { "param": "drive",   "depth":  0.6 },
              { "param": "reverb_mix", "depth": 0.2 } ] }
```

### 5.7 UI description

```jsonc
{ "theme": { "accent": "#E4572E", "mood": "warm" },
  "sections": [ { "title": "Oscillators", "params": ["osc_mix","detune","sub_level"] },
                { "title": "Filter",      "params": ["cutoff","reso","env_amount"] },
                { "title": "Space",       "params": ["delay_time","reverb_mix"] } ],
  "macro_row": ["macro_warmth","macro_motion"] }
```

The UI renderer is generic: it lays out sections in a responsive grid, draws each control
per its `control` type and `taper`, and applies the accent colour. It **never** trusts
the theme for anything but colour and label text (no HTML, no scripts, no file paths).

---

## 6. DSP Primitive Library (MVP set)

All hand-written, compiled ahead of time, unit-tested, and free of any framework
dependency so they can be exercised headlessly in CI. Each has a
**manifest** declaring its settings, parameters (with ranges, tapers, modulatable flags),
inputs, outputs, legal scopes, and a CPU cost weight used by the budget check (§8.4).

### 6.1 Sources (voice scope)

| Type | Notes |
|---|---|
| `osc.analog` | PolyBLEP saw/square/tri/sine/pulse, PWM, unison ≤ 7, detune, octave/semi/fine. |
| `osc.wavetable` | Reads a baked `wavetable` asset; position modulatable; mip-mapped, band-limited. |
| `osc.noise` | White / pink / filtered; seeded, deterministic. |
| `osc.fm2` | 2-operator FM pair with ratio + index (cheap route to bells, basses, e-pianos). |

### 6.2 Modulators

| Type | Notes |
|---|---|
| `env.adsr` | Exponential/linear-selectable segments, velocity sensitivity. |
| `env.multi` | Follows a baked `envelope` asset; up to 8 stages; optional loop. |
| `lfo` | sine/tri/saw/sqr/S&H/random-smooth; free or tempo-synced to host BPM; per-voice or global. |
| `mod.velocity`, `mod.keytrack`, `mod.aftertouch`, `mod.modwheel` | MIDI-derived sources. |

### 6.3 Processors

| Type | Notes |
|---|---|
| `filter.svf` | TPT state-variable: LP/HP/BP/notch/peak, 12/24 dB, resonance clamped ≤ self-osc blow-up. |
| `filter.ladder` | Saturating 4-pole ladder, drive-in-the-loop character. |
| `filter.comb` | Short delay + feedback (feedback hard-clamped < 0.98). |
| `shaper.wave` | Baked `curve` asset or built-ins (tanh, soft, fold, bitcrush). |
| `dyn.compressor` | Simple feed-forward comp for glue/pump. |
| `vca`, `mixer`, `pan`, `util.math`, `util.const`, `util.smooth` | Routing/arithmetic utilities. |

### 6.4 Effects (global scope)

| Type | Notes |
|---|---|
| `fx.delay` | Stereo, ping-pong option, tempo-sync, feedback clamp ≤ 0.92, damping. |
| `fx.reverb` | FDN reverb (JUCE `dsp::Reverb` for MVP, custom FDN in Phase 2). |
| `fx.chorus` / `fx.phaser` | Modulated-delay family, stereo. |
| `fx.drive` | Oversampled (2×) saturation. |
| `out.master` | **Always present, always last.** DC blocker → soft-clip → brickwall limiter → output trim. Cannot be removed or bypassed by the LLM. |

### 6.5 Registry & versioning

The registry is a compile-time table: `type string → {factory, manifest, min_ir_version}`.
The same table is serialized to JSON and **injected into the LLM prompt** so the model
always sees the exact, current capability surface. Adding a primitive = add the class,
add the manifest entry, done — the prompt, validator, and UI all pick it up automatically.
This is the extensibility hinge of the entire product.

---

## 7. LLM Layer

### 7.1 Provider abstraction

```cpp
struct GenerationRequest {
    juce::String prompt;
    std::optional<juce::String> currentIrJson;  // set = edit mode
    juce::String capabilityManifestJson;
    int   maxTokens;
    float temperature;
};

struct GenerationResult {
    bool ok;
    juce::String irJson;
    juce::String errorMessage;
    int  attemptCount;
    double latencyMs;
};

class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    virtual juce::String name() const = 0;
    virtual GenerationResult generate (const GenerationRequest&,
                                       std::function<bool()> shouldCancel) = 0;
};
```

Concrete providers, all selectable in Settings:

- `AnthropicProvider` — Messages API, forced tool-use with the IR JSON Schema as the tool
  input schema (strongest structured-output guarantee).
- `OpenAiProvider` — Chat Completions with `response_format: json_schema`, `strict: true`.
- `LocalOllamaProvider` — `/api/chat` with `format: json`; weaker adherence, repair pass
  does more work.
- `CannedProvider` — **demo safety net.** Ships 6 pre-generated, hand-QA'd IRs keyed by
  fuzzy prompt match (bass / pad / pluck / lead / bell / drone). Used automatically if the
  network call fails or exceeds the timeout. **The pitch demo cannot be killed by hotel
  Wi-Fi.**

API keys live in `%APPDATA%/Forge/config.json` (user-entered in Settings), never in the
binary, never in the DAW project state.

### 7.2 Prompt construction

System prompt contains, in order:

1. Role: "You design synthesizers by emitting a validated instrument graph."
2. The **capability manifest** (auto-generated from the registry, §6.5): every module
   type, its settings, its parameters with ranges and units, its modulatable targets,
   legal scopes.
3. The IR JSON Schema.
4. Hard constraints (node caps, param caps, `out.master` is implicit and mandatory,
   no cycles except via explicit delay/comb nodes).
5. **Sound-design heuristics** — the domain knowledge that makes output musical rather
   than merely valid. Examples: "bass → low-passed saw + sub sine, short filter env,
   mono-ish, minimal reverb"; "pad → detuned unison, slow attack, chorus + long reverb,
   slow LFO on cutoff"; "pluck → fast decay env on both amp and filter, short delay".
6. 3 **few-shot examples** of prompt → complete valid IR, covering distinct topologies.

User message = the raw prompt. In edit mode, the current IR is included and the model is
instructed to return a **full replacement IR** (not a patch) — simpler to validate, and
diffing at 32 nodes is not worth the failure modes.

### 7.3 Retry & repair ladder

1. Attempt 1 → validate. Pass? Done.
2. Fail → send the **structured validator error list** back to the model, ask for a
   corrected full IR. Attempt 2 → validate.
3. Fail again → run the **deterministic repairer** (§8.5). If the repaired graph is
   valid, use it and flag "auto-repaired" in the UI.
4. Still invalid → fall back to `CannedProvider` nearest match, and surface a clear,
   non-alarming message: *"I had trouble with that one — here's a starting point you can
   edit."*

Total wall-clock budget: **60 s** to first sound, typically 20-40 s. Hard cancel
button on the chat screen.

> **Revised from 12 s after live testing.** Claude Sonnet 5 has adaptive thinking
> enabled by default, so a ~13k-token capability manifest plus a full instrument
> graph regularly takes 20-40 s. The original 12 s ceiling did not merely rush the
> model — it aborted the connection before any response arrived, which surfaced as
> "could not reach the API" and was indistinguishable from the network being down.
> Latency is now a pacing problem to be covered by honest progress states, not a
> limit to be enforced.

### 7.4 Threading

One `juce::ThreadPool` with a single job at a time. The editor may be closed, the plugin
may be deleted, or the user may cancel mid-flight — the job holds a `std::weak_ptr` to
the processor's generation session and checks `shouldCancel()` between phases. No job
ever touches the audio thread or the `activeGraph` pointer directly; it hands the finished
`GraphInstance` to the publisher (§4.1).

---

## 8. Validation, Safety & Repair

The validator is the trust boundary. **Treat every LLM response as untrusted input from
the internet, because that is exactly what it is.**

### 8.1 Structural validation

- Well-formed JSON; parse with exceptions disabled; size cap 256 KB.
- `ir_version` known and ≤ current.
- Schema conformance: required fields, types, string patterns, array bounds.
- All node `type`s exist in the registry; all `settings` keys known and in range.
- All connection endpoints reference existing node IDs and existing port names.
- All `mod` targets reference parameters flagged `modulatable: true`.
- All asset references resolve; all asset specs within size limits.
- Node/param/macro counts within MVP caps (§5).

### 8.2 Graph legality

- Audio graph must be a **DAG** except through nodes that introduce ≥ 1 block of latency
  (`fx.delay`, `filter.comb`). Cycle detection via DFS colouring; any cycle not passing
  through a latency node is rejected.
- Every voice-scope chain must terminate at the voice sum bus; the voice sum must reach
  `out.master`. Orphan nodes are pruned (not an error).
- Scope legality: a `global` node may not feed a `voice` node's audio input.
- Exactly one `out.master`; it is inserted by the builder if absent and moved to the end
  if misplaced.

### 8.3 Audio safety

| Risk | Mitigation |
|---|---|
| Runaway feedback | Feedback gains hard-clamped: delay ≤ 0.92, comb ≤ 0.98, any user-visible feedback param range-limited at manifest level. |
| Excessive gain staging | Sum of static gains into any node clamped; per-node output soft-limited; **`out.master` limiter is non-removable** and set to −0.3 dBFS ceiling. |
| Self-oscillating filters at full resonance | Resonance ranges capped per filter type; ladder drive compensated. |
| DC offset | DC blocker in `out.master`, and after every `shaper.wave`. |
| NaN / Inf propagation | Per-block NaN scan on the master bus (cheap: check first + last sample + running sum); on detection, mute and reset the graph, log it. FTZ/DAZ enabled on the audio thread. |
| Denormals | `juce::ScopedNoDenormals` in `processBlock`. |
| Click on graph swap | 20 ms equal-power crossfade (§4.1). |

### 8.4 CPU budget

Each manifest entry has a `costWeight` (measured, not guessed — a benchmark harness
fills these in). Builder sums `costWeight × instanceCount` (voice nodes × polyphony) and
rejects graphs above a configurable budget (default: est. 35 % of one core at 48 kHz).
Over-budget graphs are first **auto-reduced** (drop polyphony 16 → 8 → 4, drop unison,
drop the second reverb) before being rejected.

### 8.5 Deterministic repairer

Applied before rejection, in order:

1. Remove connections with unknown endpoints.
2. Prune orphan and unreachable nodes.
3. Insert missing `out.master`; route the dangling terminal chain into it.
4. Clamp out-of-range settings, depths, gains to nearest legal value.
5. Break illegal cycles by removing the last-added edge in the cycle.
6. Drop parameters/macros beyond caps (lowest-priority group first).
7. If any exposed parameter binds to a pruned node, drop that parameter.

The repairer's every action is recorded and shown in a collapsible "what I fixed" note —
useful in the demo as evidence the safety layer is real, not decorative.

### 8.6 Fuzz testing

A Catch2 fuzz target feeds the validator: (a) random bytes, (b) valid-IR-with-one-field-
mutated, (c) adversarial cases (deeply nested JSON, 10⁶-element arrays, `NaN` literals,
self-referential IDs, unicode IDs, 100 % feedback, `-inf` gains). **Zero crashes, zero
hangs, zero unclamped audio** is the acceptance bar.

---

## 9. Plugin Integration Details

### 9.1 The VST3 dynamic-parameter problem — and the solution

VST3 hosts enumerate a plugin's parameters at scan/instantiation time and generally do
**not** tolerate parameters appearing or disappearing at runtime. But Forge's whole point
is that each generated instrument has a different control set. This is the single
sharpest technical constraint in the project.

**Solution — static parameter pool with dynamic remapping:**

- At construction, Forge declares a **fixed pool**: 8 `Macro 1..8` + 32 `Param 01..32`,
  all `AudioParameterFloat` normalized 0…1, plus a small fixed set of always-present
  globals (Master Volume, Glide, Polyphony, Bend Range).
- When an instrument loads, each IR parameter is **bound to a pool slot** in declaration
  order. The slot's display name, unit, and value-to-text conversion are updated, and the
  host is notified via
  `AudioProcessorListener::audioProcessorChanged(this, ChangeDetails{}.withParameterInfoChanged(true))`.
- Host behaviour varies: Reaper and FL update names promptly; Ableton caches aggressively
  and may keep showing `Param 07`. **This does not affect the demo**, because Forge's own
  GUI always shows correct, live, correctly-named controls. Host-side automation still
  works — it just may display a generic name in some hosts.
- Unbound slots report a neutral name and are inert.

This is the standard approach used by shipping modular/generative plugins, and it's the
right answer for the pitch: it's honest, it works, and the Phase 3 export path (§14.3)
removes the limitation entirely for users who want a fixed plugin.

### 9.2 State persistence

`getStateInformation` writes a single JSON blob:

```jsonc
{ "forge_state_version": 1,
  "instruments": [ { "id": "...", "ir": { ... }, "paramValues": { ... } } ],
  "activeInstrumentId": "...",
  "chatHistory": [ ... ] }
```

Full IRs are embedded, so a project opened on another machine works without the user's
instrument library. Separately, every generated instrument is auto-saved to
`%APPDATA%/Forge/instruments/<slug>-<shorthash>.forge.json` so it appears in the dropdown
across all future projects.

### 9.3 MIDI & musical behaviour

- Note on/off, velocity, pitch bend (range param), mod wheel, sustain pedal, aftertouch
  (channel). MPE is Phase 2.
- Mono/legato/poly mode is an IR-level setting (`"voicing": "poly" | "mono" | "legato"`)
  with portamento time; the LLM sets it (bass → mono+glide, pad → poly).
- Tempo-synced modules read `AudioPlayHead::PositionInfo`; if the host provides no tempo,
  fall back to 120 BPM.

### 9.4 Host/format matrix (MVP)

| | Status |
|---|---|
| Windows x64 VST3 | **Required.** Primary target. |
| Standalone (JUCE) | Free with `juce_add_plugin` — useful for fast iteration without a DAW. |
| macOS VST3/AU | Phase 2. |
| Reaper / FL Studio / Ableton Live | Test all three; Reaper is the dev host. |
| pluginval | Must pass **strictness level 8** before the pitch. |

---

## 10. User Interface

Three screens, one window (min 900 × 600, resizable, DPI-aware).

### 10.1 Chat screen

- Centered prompt field with a rotating placeholder ("a glassy bell that decays into
  noise…").
- Send button + Enter. Cancel button while generating.
- Progress states: *Designing… → Validating… → Building… → Ready*. These are honest
  status labels, and they also make the 6–10 s wait feel like work rather than a hang.
- Conversation transcript so edits read as a dialogue.
- Quick-start chips for the demo: "warm analog bass", "glassy pad", "metallic pluck".

### 10.2 Generated instrument screen

- Header: instrument name, **instrument dropdown**, "+ Create New Instrument", "Edit with
  chat" button, save/rename.
- Generic renderer builds sections from `ui.sections`; each control drawn per its type and
  taper, with value readout, double-click-to-default, fine-drag on Shift.
- Macro row across the bottom, visually distinct.
- Small oscilloscope + output meter (sells "it's really making sound" from the back of a
  room).
- A collapsible **"How it's built"** panel showing the node graph as a simple flow
  diagram. This is a pitch weapon: it proves the AI composed a real topology rather than
  turning knobs on a fixed synth.

### 10.3 Instrument dropdown

Lists session-generated instruments (in-memory) and library instruments (disk), grouped,
with the original prompt as a tooltip. Selecting one triggers the standard graph swap —
including the 20 ms crossfade, so you can switch instruments while a chord is ringing.

---

## 11. Repository Layout

```
forge/
├─ CMakeLists.txt
├─ SPEC.md
├─ cmake/                        # JUCE fetch, MSVC flags, packaging
├─ src/
│  ├─ PluginProcessor.{h,cpp}    # AudioProcessor, param pool, state
│  ├─ PluginEditor.{h,cpp}       # screen router
│  ├─ ir/
│  │  ├─ IrSchema.h              # C++ IR structs + JSON (de)serialization
│  │  ├─ IrValidator.{h,cpp}     # §8.1–8.2
│  │  ├─ IrSafety.{h,cpp}        # §8.3–8.4
│  │  └─ IrRepair.{h,cpp}        # §8.5
│  ├─ dsp/
│  │  ├─ Module.h                # base contract
│  │  ├─ Registry.{h,cpp}        # type table + manifest emission
│  │  ├─ modules/                # one file per primitive
│  │  ├─ GraphBuilder.{h,cpp}    # IR → GraphInstance
│  │  ├─ GraphInstance.{h,cpp}   # exec lists, voices, buffers
│  │  ├─ VoiceManager.{h,cpp}
│  │  └─ AssetBaker.{h,cpp}      # wavetables, curves, envelopes
│  ├─ llm/
│  │  ├─ LlmProvider.h
│  │  ├─ AnthropicProvider.cpp
│  │  ├─ OpenAiProvider.cpp
│  │  ├─ OllamaProvider.cpp
│  │  ├─ CannedProvider.cpp
│  │  ├─ PromptBuilder.{h,cpp}
│  │  └─ GenerationSession.{h,cpp}
│  ├─ ui/
│  │  ├─ ChatView.{h,cpp}
│  │  ├─ InstrumentView.{h,cpp}
│  │  ├─ ControlFactory.{h,cpp}
│  │  ├─ GraphView.{h,cpp}
│  │  └─ ForgeLookAndFeel.{h,cpp}
│  └─ util/                      # RetireQueue, LockFreeFifo, Logging, Config
├─ resources/
│  └─ canned/                    # 6 fallback IRs for the demo
└─ tests/
   ├─ validator_tests.cpp
   ├─ builder_tests.cpp
   ├─ dsp_tests.cpp              # NaN, DC, gain, determinism
   └─ fuzz_ir.cpp
```

---

## 12. Three-Day Build Plan

Ordered so that **there is a demoable artifact at the end of every day**. If a day
overruns, you cut scope from the *next* day, never from the safety layer.

### Day 1 — Skeleton, sound, and the swap (target: it makes noise in Reaper)

| Block | Work |
|---|---|
| 1 | CMake + JUCE 8 + `juce_add_plugin` VST3 + Standalone. Confirm it scans and loads in Reaper. |
| 2 | `Module` contract, `Registry`, `GraphInstance`, `VoiceManager` (16 voices). |
| 3 | First 8 primitives: `osc.analog`, `osc.noise`, `env.adsr`, `lfo`, `filter.svf`, `vca`, `mixer`, `out.master`. |
| 4 | Hardcode one IR in C++, build it, play it from MIDI. **Milestone: sound.** |
| 5 | Lock-free swap + retire queue + crossfade. Test by hot-swapping two hardcoded graphs from a button. **Milestone: no clicks, no crashes.** |

### Day 2 — IR, validator, LLM (target: a prompt produces a new instrument)

| Block | Work |
|---|---|
| 1 | IR structs + JSON round-trip. `GraphBuilder`: IR → GraphInstance. |
| 2 | `IrValidator` + `IrSafety` + `IrRepair`. Unit tests. **Do not skip — this is the load-bearing wall.** |
| 3 | Manifest emission from the registry; `PromptBuilder` with heuristics + 3 few-shot examples. |
| 4 | `LlmProvider` interface + one real provider + `CannedProvider`. Async session with cancel and a 60 s timeout. |
| 5 | Wire prompt → IR → validate → build → swap. **Milestone: type text, hear a new instrument.** |
| 6 | Remaining primitives: `osc.wavetable`, `osc.fm2`, `filter.ladder`, `shaper.wave`, `fx.delay`, `fx.reverb`, `fx.chorus`, `fx.drive`, `env.multi`, keytrack/velocity. `AssetBaker`. |

### Day 3 — UI, persistence, hardening (target: pitch-ready)

| Block | Work |
|---|---|
| 1 | `ChatView` + `InstrumentView` + generic `ControlFactory` from `ui.sections`. |
| 2 | Static parameter pool + dynamic remapping (§9.1). Macros. |
| 3 | Instrument dropdown, disk library, `get/setStateInformation`, save/reload test in all three hosts. |
| 4 | LookAndFeel pass, meter + scope, "How it's built" graph panel. |
| 5 | **Hardening:** pluginval level 8, fuzz run, NaN/gain tests, 30-prompt generation sweep — every result must be safe, and ≥ 80 % must be musically sensible. Tune the prompt until it is. |
| 6 | **Demo rehearsal:** run the exact pitch script 5× end-to-end, once with the network disabled to confirm the canned fallback. Pre-generate and save the demo instruments to the library as a backup path. |

### Cut list (drop in this order if time runs short)

1. "How it's built" graph panel
2. Oscilloscope / meter
3. `osc.fm2`, `filter.ladder`, `fx.chorus`
4. Disk library (session-only instruments still work)
5. Edit-via-chat (generate-new still works)

**Never cut:** validator, safety clamps, `out.master` limiter, canned fallback, crossfade.

---

## 13. Acceptance Criteria & Demo Script

### 13.1 Definition of done for the MVP

- [ ] VST3 loads in Reaper, FL Studio, and Ableton Live on Windows x64.
- [ ] Passes pluginval strictness 8.
- [ ] Prompt → playable instrument in ≤ 60 s, ≥ 90 % of the time, over 30 varied prompts.
- [ ] All 30 outputs are *safe*: no clipping above −0.3 dBFS, no NaN, no runaway feedback,
      no CPU spike above budget.
- [ ] ≥ 3 structurally distinct topologies observed across the 30 (proving it isn't one
      fixed architecture).
- [ ] All generated knobs affect the sound audibly and in real time.
- [ ] Instrument dropdown switches instruments without a click or dropout.
- [ ] Project save/close/reopen restores the active instrument and all parameter values.
- [ ] Network disabled → canned fallback produces a playable instrument with a clear
      message.
- [ ] 10-minute continuous play, no leaks (retire queue drains), no drift.

### 13.2 Pitch demo script (≈ 4 minutes)

1. Show an empty Reaper project. Insert Forge. Chat screen appears. *(10 s)*
2. Type **"warm analog bass with a slow filter sweep and a bit of drive"**. Play the
   generated instrument. Sweep the Cutoff knob and the Warmth macro. *(70 s)*
3. Open "How it's built" — show the actual node graph the AI composed. *(20 s)*
4. Reopen chat: **"make it darker and add a short delay"**. Play again, A/B it against
   the previous version via the dropdown. *(60 s)*
5. New instrument: **"a glassy bell pad that decays into noise"** — deliberately a
   completely different topology. Play a chord. *(50 s)*
6. Close the argument: "Every instrument you just heard is a runtime graph built from
   audited DSP primitives. The AI never touches the audio thread. This is the sound design
   labour that costs $300 a plugin, generated on demand." *(30 s)*

**Backup plan:** the same instruments are pre-saved in the dropdown. If generation fails
live, load from the dropdown, say "here's one I generated earlier", and keep moving. Do
not debug on stage.

---

## 14. Beyond the MVP — Extension Roadmap

The MVP intentionally restricts the schema and primitive set. **Nothing in the MVP
architecture forecloses any of the following**, and the order below is chosen so each
phase is independently shippable.

### 14.1 Phase 1 — Deepen the creative space (weeks 1–4 post-pitch)

*Goal: outputs stop feeling like variations and start feeling like distinct instruments.*

- **Expand the primitive library** (this is the highest-leverage work, and it's purely
  additive — new class + manifest entry, and the LLM immediately knows how to use it):
  granular source, physical-modelling resonator (Karplus-Strong, modal bank), formant
  filter, vocoder, sample-and-hold quantizer, ring modulator, phase distortion,
  wavefolder, spectral freeze, multiband processing, convolution reverb, transient
  shaper, arpeggiator/sequencer nodes.
- **Raise the schema caps**: 32 → 128 nodes, 24 → 64 parameters, nested subgraphs
  ("macro-modules") so the LLM can build and reuse composite structures.
- **Per-sample modulation option** for audio-rate FM/AM between arbitrary nodes (currently
  block-rate only). Requires a second, sample-accurate execution mode in `GraphInstance`.
- **Richer generated assets**: multi-frame morphing wavetables, impulse responses
  synthesized from a description, noise textures, custom filter kernels.
- **Measure real `costWeight`s** with a benchmark harness rather than estimates, enabling
  a much more accurate CPU budget and higher polyphony.

### 14.2 Phase 2 — Quality, control, and reach

- **Two-stage generation**: a "design brief" pass (the model writes a short prose sound
  design plan) followed by an IR pass conditioned on that plan. Measurably improves
  coherence over single-shot on complex prompts.
- **Retrieval over a curated IR corpus**: embed a library of hand-built reference
  instruments; retrieve the 2–3 nearest and include them as dynamic few-shots. This is how
  quality scales without a bigger model.
- **Audio-feedback loop (closing the perceptual gap)**: render a short audition of the
  generated instrument, extract features (spectral centroid, RMS envelope, harmonicity,
  stereo width), compare against features implied by the prompt, and let the model do one
  corrective pass. This is the difference between "valid" and "actually sounds like what
  I asked for."
- **Fine-tuning / preference data**: log (prompt, IR, user kept-or-regenerated, final
  edited IR). That dataset is the real moat — it teaches a model *your* mapping from
  language to signal flow, which no competitor can copy.
- **macOS build** (VST3 + AU), universal binary, notarization, and hardened runtime.
- **MPE**, per-note modulation, microtuning (Scala/MTS-ESP).
- **Undo/redo history** across generations; A/B compare slots; instrument versioning.
- **WebView UI** (JUCE 8 `WebBrowserComponent`) if the generic native renderer becomes the
  bottleneck for AI-designed layouts — the IR already carries a UI description, so this is
  a frontend swap, not an architecture change.

### 14.3 Phase 3 — Export as standalone VST3

Users will want to ship a generated instrument to a collaborator who doesn't have Forge.
The architecture already supports this cleanly:

- The exporter writes the frozen IR + baked assets into a **prebuilt, code-signed "player"
  VST3 shell** — the same DSP engine binary, no LLM, no chat UI, with the IR embedded as a
  binary resource and the parameter pool statically named at export time (which removes
  the §9.1 dynamic-naming limitation entirely).
- **No compiler on the user's machine, no DAW rescan pain beyond the new plugin, no
  runtime codegen.** Export is a file-writing operation, not a build.
- Optional cloud export service for signed/notarized binaries per platform.
- This also unlocks a marketplace: generated instruments become shareable, sellable
  artifacts — which is the business model, not just a feature.

### 14.4 Phase 4 — Platform

- **Instrument sharing / marketplace**: IRs are small JSON documents; sharing is trivial
  and viral. Ratings, remix lineage, creator payouts.
- **Sound-alike from audio reference**: user drops in an audio clip; feature extraction +
  parameter search (differentiable proxy or CMA-ES over the IR parameter space) fits a
  generated graph to it. The graph structure comes from the LLM; the fine values come from
  optimization.
- **DAW-context awareness**: read the project tempo, key, and neighbouring tracks to
  generate instruments that sit in the existing mix.
- **Effects-plugin variant**: the same graph engine with an audio input node ships as a
  VST3 *effect*, generated from prompts the same way. Almost entirely code reuse.
- **Cmajor / Faust JIT as an opt-in "expert" tier**: for users who accept the risk, allow
  genuinely novel DSP to be generated and JIT-compiled in a sandboxed process, with the
  audited-primitive path remaining the default. This is the only path to *truly*
  unbounded originality, and it is deliberately last — it is where all the stability risk
  lives.

### 14.5 Architectural rules for all future work

1. **Never** let generated content run on the audio thread without passing the validator.
2. **Always** bump `ir_version` for schema changes and keep a migration function for every
   prior version. Saved instruments must never break.
3. **New capability = new primitive + manifest entry.** If a feature requires special-casing
   the LLM prompt by hand, the abstraction is wrong.
4. Every new primitive ships with unit tests for NaN-safety, gain bounds, determinism, and
   a measured `costWeight`.
5. The capability manifest is generated from the registry, never hand-maintained.

---

## 15. Risk Register

| Risk | Severity | Mitigation |
|---|---|---|
| LLM output is valid but musically bad | **High** | Sound-design heuristics + few-shot examples in the prompt; 30-prompt sweep on Day 3 with prompt tuning until ≥ 80 % land; canned fallbacks for demo prompts. |
| Generation latency kills demo pacing | High | 60 s timeout, honest progress states, pre-generated backups in the dropdown, network-off rehearsal. Talk over the wait — the graph panel gives you something to narrate. |
| VST3 dynamic parameter naming misbehaves in Ableton | Medium | Static pool + own GUI always correct (§9.1); demo primarily in Reaper. |
| 3 days is not enough | **High** | Day-by-day demoable milestones; explicit cut list (§12); safety layer is never cut. |
| Real-time safety bug → crash in the DAW on stage | **Critical** | Strict `processBlock` rules, lock-free swap, retire queue, pluginval 8, 10-minute soak test. |
| API key / network unavailable at venue | Medium | `CannedProvider` auto-fallback; rehearse with Wi-Fi off. |
| Licensing (JUCE + VST3 SDK) questioned in pitch | Low | Known costs, dual-license path documented (§3.1). |
| "This is just presets with extra steps" objection | Medium | The "How it's built" graph panel is the direct rebuttal — show two prompts producing structurally different topologies, live. |

---

## 16. Open Questions

1. Which LLM provider is the demo default, and is a key available at the venue?
2. Preferred demo host — Reaper (most forgiving) or the DAW you actually produce in?
3. Should the MVP include instrument *renaming* and library management, or is
   session-only acceptable for the pitch?
4. Target polyphony — is 16 voices enough for the pad demo, or should it be 24?
5. Is a visible "auto-repaired this graph" note an asset (shows rigour) or a liability
   (shows the AI erred) in front of your specific audience?
