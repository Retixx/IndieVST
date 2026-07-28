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

### 5.8 Model-authored layout

The most common failure of a generated instrument is that it does not look generated.
If every patch renders through the same fixed furniture, a glass bell and a distorted
sub arrive on screen as one picture with different numbers under the knobs — and a
producer reads that, correctly, as a preset browser wearing a costume.

So the model designs the front panel as well as the sound. It emits a `layout` block:

```jsonc
{ "pages": [ { "title": "ENGINE", "panels": ["WAVETABLE","OSC A","SUB","MIX"] },
             { "title": "GRIT",   "panels": ["DRIVE","SHAPER","TAPE","FILTER 1"] },
             { "title": "LOW END","panels": ["EQ","TRANSIENT","GATE","OUTPUT"] } ],
  "featured": ["WAVETABLE","DRIVE"],
  "panel_accent": { "WAVETABLE": "#C9762B", "DRIVE": "#8A3B24" } }
```

- **Pages** are the tabs, in reading order, named for what this instrument does.
- A panel placed on no page is **removed from the instrument outright**, module and
  all. That is how a four-panel plucked string exists alongside a fourteen-panel bass.
- **Panel order within a page** is draw order — the arrangement itself is authored.
- **`featured`** panels are drawn with larger controls over fewer rows, giving the
  page a focal point instead of a uniform tile grid.
- **`panel_accent`** colours individual blocks, so the palette carries the mood.

**The boundary is unchanged: the model chooses arrangement, never capability.** Every
panel it can place is backed by a precompiled module that already passed the isolation
sweeps in §8. Layout is applied *after* section pruning and *before* repair, and is
bounded by four guarantees, each with a regression test in `tests/test_layout.cpp`:

| Guarantee | Enforcement |
|---|---|
| Output stage survives | Nodes of type `out.*` and `vca`, plus whatever drives the amp envelope, are protected and force-placed on the last page if the model forgot them |
| The instrument makes a sound | If no oscillator panel was placed, the first one is restored to page 1 |
| No two-knob synth on stage | A layout naming fewer than 4 known panels is treated as a malformed response and **discarded entirely** — the complete default rack is kept |
| No dangling references | Unknown panel names are ignored; `ui.sections` is rebuilt from surviving params only; page count is capped at 6 |

---

### 5.10 Voicing, and telling the musician what was recognised

Two failures found by testing against the real target — "the distorted guitar
lead from Kanye West's *Gorgeous*":

**Voicing.** The model returned a monophonic legato patch, because "lead" reads
as one-note-at-a-time. But that riff opens on a C# chord, and a guitar is a
polyphonic instrument regardless of what it is playing. A mono instrument played
as a chord sounds broken and the musician cannot fix it from the front panel.
Two defences, because this is silent and unrecoverable:

1. The prompt states the rule explicitly — the test is what the real instrument
   physically does, not whether the word "lead" appeared; when in doubt, poly.
2. `buildPatchPrompt` scans the request for a chordal instrument (guitar, piano,
   Rhodes, organ, strings, bells…) and appends a deterministic one-line note.
   This runs on the edit path too.

**Attribution.** `references` carries what the model actually drew on — one line
per record, artist, synth or era, with what was taken from it — and it appears
on hover over the instrument name, alongside the original prompt and the
voicing. If someone asks for Kanye, they can check that Kanye was recognised
rather than trusting that a generically gritty lead means it understood. An
empty list is shown as *"no specific record or artist was recognised"*, because
inventing a plausible influence is worse than admitting none.

The field lives in `meta` rather than as a new member of `ir::Instrument`:
changing that struct forces a full clean rebuild on every machine, and this is
presentation, not schema.

---

### 5.9 Reference audio (optional input)

Describing a sound in words is hard. Playing one is easy. A producer can drop a
recording onto the generation screen and Forge measures it, then hands the
measurements to the model alongside the text prompt.

**This is analysis, not sampling.** Nothing of the source audio survives into the
instrument: the file is decoded, measured, and discarded. The output is a
synthesiser built from the same precompiled modules as any other generation.
That distinction is legal as much as musical — no copyrighted audio is embedded,
redistributed, or resynthesised.

`forge_core/audio/ReferenceAnalysis` is JUCE-free and runs once on a background
thread. It extracts:

| Group | Measurements |
|---|---|
| Pitch | fundamental (Hz), note name, cents offset — or an explicit "unpitched" |
| Spectrum | centroid, 85% rolloff, tilt in dB/oct, low/mid/high energy split |
| Character | harmonicity, inharmonicity, spectral flatness, odd/even harmonic ratio |
| Shape | attack, decay, sustain level, release, crest factor |
| Stereo | width from inter-channel correlation, peak and RMS |

Three implementation notes that are the difference between working and appearing
to work, each pinned by a test in `tests/test_reference.cpp`:

- **Fixed comparison length.** Autocorrelation over `window - lag` inflates the
  score of long lags, biasing every reading about 10 cents flat.
- **Peak picking, not thresholding.** A 55 Hz note correlates at 0.98 with itself
  30 samples later, so "first lag above threshold" reports a bass note as 1600 Hz.
  Candidate periods must be local maxima, and the shortest near-best peak wins —
  otherwise a guitar at D3 comes back as D2 and the instrument is built an octave
  low.
- **Level independence.** The signal is peak-normalised before analysis, so the
  same bounce at −18 dBFS and −3 dBFS describes one instrument.

The measurement block is prepended to the *user* message, never the system
prompt, so provider-side prompt caching still hits. It states explicitly that
where the text and the recording disagree, **the text wins** — the recording is
what the musician has, the text is what they want.

---

### 5.11 One window shape

The rack briefly resized the window per tab, so a sparse page would not swim in
empty panel. It worked and it was still wrong: a plugin window that changes size
under the mouse reads as instability, and in a DAW it is actively unpleasant.

The window is now fixed at 7:3 for both screens, and the **rack fits itself to
the window** instead. `layoutActiveTab` searches row counts 1-6 against control
scales from 2.2 down to 0.34 and takes the largest that genuinely fits, measuring
the real wrapped result rather than estimating it. If nothing fits — a 67-control
page in a small window — it takes the arrangement that overflows least, rather
than a fixed guess.

---

### 5.12 Never losing a design to a formatting accident

`"The model did not return a JSON object"` was the worst failure in the product.
It discarded a complete instrument over a transport hiccup and dropped the
musician onto a hand-authored fallback with a dozen knobs — which on screen
reads as a broken plugin, not a degraded one.

Two independent fixes, because this must not happen on stage.

**Salvage** (`core/llm/JsonSalvage`) tries three things in order and reports
which one worked, so a recovered patch is never presented as a clean one:

| Attempt | Handles |
|---|---|
| Parse as-is | the normal case |
| Largest balanced `{…}` in the text | markdown fences, "Here is your instrument:", trailing chat. *Largest*, not first — a decoy object in the prose used to win by appearing earlier |
| Truncation repair | a read that timed out or a token ceiling hit mid-object: close the open string, drop the partial trailing member, close every open bracket — and require the result to parse before accepting it |

It never returns text that does not parse, and it never invents a value: a key
cut off before its value is dropped, not guessed. `max_tokens` is also now set
explicitly to 16000, since a full patch is ~130 values plus layout, matrix,
wavetable and references.

**The floor** (`arch::heuristicPatch`) replaces the canned library as the last
resort. It reads the prompt for instrument type and mood with no model
involved, and returns a patch for the full architecture — three balanced pages,
a mood-matched palette, envelope and filter settings that follow the words. It
is deterministic, so the same request always degrades to the same instrument.

The point: **when Forge fails, it fails to something that still looks like an
instrument.** A dry bass does not arrive drenched in reverb, a pad still swells,
a guitar is still polyphonic, and no page ever has three knobs floating in it.

---

### 5.13 A record is not a sample

The reference feature shipped with a serious flaw, found the only way it could
be: a producer dropped in a finished Kanye record, asked for the guitar from it,
and got **filtered white noise**.

The analyser was not wrong. A dense mix genuinely is spectrally flat, weakly
pitched, broadband and heavily compressed — which is precisely how noise
measures. The mistake was presenting measurements of a *mix* as though they
described one *instrument*.

**Mix detection.** Four independent signs, none conclusive alone: energy present
across all three bands, no confident fundamental, tonal energy smeared over many
pitch classes, and a small crest factor. Above 0.5 the recording is treated as a
record rather than a sample.

**Chroma.** A single fundamental does not survive mixing; pitch classes do,
because the guitar, bass and keys all reinforce the same ones. On the synthetic
Kanye-shaped fixture this correctly returns **C#** — the note the riff is
actually on, which no single-pitch detector was ever going to find.

**Two different briefs.** An isolated sound gets the full measurement list as
design targets. A mix gets a deliberately shortened one — key centres, register,
tonal balance, tilt, width — with harmonicity, flatness, attack and odd/even
*withheld*, and an explicit account of why:

> Do NOT use a noise oscillator because a mix measured as flat. Do NOT match the
> mix's brightness directly — a record contains cymbals and vocal air your one
> instrument will not have.

**A deterministic backstop.** If the request names a pitched source *and* a
reference was supplied, `buildPatchPrompt` appends a hard instruction to keep
the noise oscillator near zero. This does not depend on the model reading a long
prompt carefully. Genuinely unpitched requests — waves, cymbals, wind — are left
alone, since noise is the right answer there.

### 5.14 Layout performance

The fill-the-page work made the window unusable to drag. The layout search
evaluated ~900 candidate arrangements per resize, and each one called
`widthUnits()` on every control — which measures a string, glyph by glyph. On a
130-control page that is on the order of half a million text layouts per resize.

Two fixes, both structural rather than micro-optimisation:

- **`widthUnits()` is cached per control.** It depends on the label and the
  option list, neither of which changes after construction.
- **Column widths are computed once per (panel, row count)** before the search
  begins, as unit multipliers, so evaluating a candidate is float arithmetic
  with no allocation.
- **The scale search is coarse-to-fine** — a 0.12 sweep, then a 0.02 refinement
  around the winner — reaching the same answer in roughly a quarter of the
  evaluations.

---

### 5.15 Honouring the whole brief

A producer wrote a precise request — four macros named Weight, Bite, Motion and
Space; glide for overlapping notes; the mod wheel mapped to distortion,
brightness and movement — and got an instrument with none of those, plus a
description confidently claiming all of them. It sounded right and was
unplayable the way they asked.

Three separate causes, all now fixed.

**1. It was not possible.** The patch schema had no `macros` field. The
architecture built five fixed macros and nothing let the model name or rewire
them, so compliance was structurally impossible. `macros` is now authored:
label, default and a list of routes into any exposed control. A macro wired to
nothing is discarded — a knob that moves nothing is worse than a missing knob,
because it lies.

**2. Two namespaces for the same thing.** The model is shown *control* ids
(`f1_cutoff`, `dr_drive`), but modulation addresses *node.param*
(`filt_1.cutoff`, `drive.drive`). It wrote `lfo_1.depth`, the route was silently
dropped as invalid, and the wheel did nothing while the description said
otherwise. `applyPatch` now resolves either form, and still rejects targets that
exist in neither.

**3. Nothing was checking.** This is the structural one. A model that satisfies
most of a list and narrates all of it passes unnoticed forever unless something
verifies. `core/llm/Compliance` extracts checkable requests from the prompt —
named macros and their count, glide, mod-wheel mappings and their specific
targets, velocity-to-filter, initial pitch movement, mono low end, restrained
reverb — checks each against the built instrument, and hands misses back as an
itemised correction for one retry. The retry is kept **only if it satisfies
strictly more** requirements; a second pass that fixes two things and breaks
three is not an improvement.

The result line now states the score plainly: *"Ready. 7 of 9 specific requests
satisfied — missing: glide between notes, mod wheel increasing distortion."*
Extraction is deliberately narrow and would rather miss a request than invent
one; every requirement only fires when its words actually appear.

### 5.16 Name and chrome

The plugin is **IndieVST**. `PRODUCT_NAME`, `getName()` and the wordmark all
say so; the CMake target stays `Forge` because renaming it would churn every
reference in the build for no user-visible gain.

The header was a flat slab the same colour as the panels, which read as an
unfinished window. It is now darker than the work surface with a lit top edge
and an accent hairline fading out along the bottom, so the rack looks inset into
the product. The wordmark sets "Indie" in regular and "VST" in semibold with an
accent underscore beneath the second half only — it reads as a logo rather than
a run of tracked capitals.

---

### 5.17 Any instrument, not only synthesisers

A producer asked for "a bass like Steve Lacy and Thundercat" — both bass
*guitar* players — and got a synthesiser. Twice. The sound was fine; it was the
wrong instrument.

The cause was the macros bug again, exactly: **`osc.karplus` was in the module
library, tested and safe, and simply not in the rack.** A Karplus-Strong
physical model had existed the whole time and no prompt could reach it, so every
request for a guitar, harp, koto or bass had nothing but saws and FM to work
with and dutifully came back sounding like a synth.

Three modules now sit in the voice chain, all at zero mix so nothing else
changes:

| Node | Module | What it is for |
|---|---|---|
| `osc_string` | `osc.karplus` | A plucked string: noise decaying in a tuned delay. Level, damping, pick tone, sustain. |
| `body` | `filter.comb` | The box the sound comes out of — guitar body, soundboard, cello belly. |
| `vowel` | `filter.formant` | Cavity and throat character for winds and voice. |

The prompt now carries construction recipes for plucked and struck strings,
electric bass and guitar, bowed strings, piano and Rhodes, mallets and metal,
winds and voice — each naming the specific controls and the values that matter.
The key insight it teaches: *a plucked string is not an oscillator with a fast
envelope; it is an excitation decaying inside a resonator.*

**And it is checked.** If the request names a real instrument and does not ask
for a synth, `Compliance` verifies the patch actually reaches for the physical
model or FM rather than the default oscillator stack, and sends it back with
specific instructions if not. Bare "bass" counts as bass guitar, because that is
what it means to a musician; "synth bass", "808", "sub bass", "analog", "reese"
and "wavetable" all opt out. The offline floor honours this too — ask it for a
nylon guitar with no network and it builds a string, not a saw.

---

### 5.18 The capability audit

This project produced the same bug four separate times, and each one presented
as the model ignoring a direct instruction:

| Symptom | Actual cause |
|---|---|
| "Four macros named Weight, Bite, Motion, Space" produced the five stock ones | The patch schema had no `macros` field |
| Every guitar and bass came back a synthesiser | `osc.karplus` was in the library, not in the rack |
| Glockenspiels came out harmonic, like an organ | `osc_fm.ratio` had no control, so non-integer ratios were unselectable |
| "Pitch movement at the start of each note" never appeared | `pitch_mod_semis` had no knob to route to |

**A capability that exists in the engine and cannot be reached from a prompt is
invisible, and invisible capability is indistinguishable from a disobedient
model.** Every hour spent tuning prompt wording against one of these was wasted.

`tests/test_capability.cpp` now fails the build when anything is added to the
engine and not wired up. Every module must be in the rack, every node parameter
must have a knob, every setting must have a selector — or appear on an exemption
list **with a written reason**, which forces the decision to be deliberate rather
than forgotten. It also asserts that every control and modulation source in the
rack actually appears in the system prompt, because a knob the model is never
shown may as well not exist.

Bugs the audit found immediately on first run:

- **`gt_hld` had a logarithmic taper with a zero minimum**, so the full
  architecture *did not validate*. Any generation that kept the GATE panel
  failed and fell back silently. It went unnoticed for weeks because the layout
  pass usually pruned that panel. There is now a test that the bare rack
  validates and builds.
- **`SwitchControl` had no branch for float settings.** They fell through to the
  integer case and were enumerated as whole numbers — which is why no
  inharmonic FM ratio could be chosen. Float settings now render as a curated
  list, with the inharmonic ratios labelled as metallic.
- **Instrument names were matched as substrings**, so "flute" matched "lute" and
  a breathy flute was told to build a plucked string. "harpsichord" matched
  "harp" the same way. Matching is now word-bounded.
- Seven further modules and thirty controls were unreachable; `fx.chorus`,
  `fx.pitch`, FM ratio/fine/pitch-mod, per-oscillator pitch modulation and the
  semitone transposers are all now exposed.

### 5.19 The whole orchestra

`tests/test_orchestra.cpp` builds sixteen instruments across every family —
nylon and electric guitar, bass, cello, violin, viola, glockenspiel,
vibraphone, marimba, piano, Rhodes, church organ, flute, brass, harp, kalimba —
from the recipes the prompt teaches. Each must validate, build a graph, render
audio that is finite, audible and inside the limiter, and be recognised by the
compliance checker as the instrument it claims to be.

That last condition is the important one: **if a recipe in the prompt cannot
satisfy the checker, the prompt is lying to the model.** Forty-five instrument
names are classified into six families — plucked, bowed, struck, mallet, wind
and reed — each with its own construction requirement.

---

### 5.20 Requests versus guarantees

Three mechanisms already existed to stop "electric guitar" returning a
sawtooth synth: the prompt teaches the recipe, the checker catches the miss,
the retry sends it back with instructions. **All three are requests.** It came
back a synth anyway.

`llm::enforceInstrumentFamily` is the guarantee. If the musician named a
plucked, struck, bowed or reed instrument and the patch still is not built that
way after the retry, the *source section* is rebuilt deterministically — string
level, damping and pick tone for plucked; FM with a matching modulator ratio
for struck and mallet; body and a bowed attack for strings and winds. Mood
words in the prompt still steer it, so a "warm muted lofi guitar" gets more
damping and a darker pick tone than a "bright crisp steel guitar".

Everything else the model chose is left alone — filter, effects, layout,
macros. Overwriting those would make every corrected instrument identical, and
there is a test asserting they survive untouched. The correction is recorded in
the repair report, so the musician is told it happened.

This is defensible because it is not a matter of taste: a plucked string is a
physical model, not a sawtooth, and that knowledge is ours, deterministic and
testable.

**Attribution** is checked the same way. If the request points at a person or a
record — "like Steve Lacy", "inspired by", "-esque" — and contains a proper
noun, `references` must be non-empty. Previously there was no way to tell
whether a named artist had been used or silently ignored.

### 5.21 More unreachable-capability bugs

The audit's own pattern caught two more:

- **The offline fallback's entire effects section did nothing.** It wrote to
  `dly_mix`, `drv_amount`, `rev_mix`, `tape_amount` and `wid_amount` — none of
  which are real control ids. `applyPatch` warns and moves on, so the safety-net
  instrument applied cleanly and changed nothing. There is now a test that every
  control id referenced anywhere in the codebase resolves, and that two very
  different prompts produce measurably different fallback instruments.
- **Renaming `PRODUCT_NAME` without renaming the CMake target** left
  `Forge.exe`/`Forge.vst3` sitting beside the new `IndieVST` artefacts. Both
  load fine, so it was easy to keep launching the old one and conclude that
  none of the changes had landed — which is exactly what happened. The target is
  renamed too, and the build now emits a warning if a stale `Forge.vst3` is
  found in the VST3 folder.

---

### 5.22 The plucked string, and how to measure a pitch bug

Reported as "fine at C9 but horrible at C4 - makes no sense for a BASS". Four
separate defects were behind it, three now fixed.

**1. Decay was applied per sample.** The coefficient multiplied the feedback
loop every sample, so the gain per PERIOD was `decay^period` - and the period is
a hundred times longer in the bass than the treble. At 0.992 a top-octave note
lost 4% per period and rang for seconds; a C2 lost 99.8% and was gone in three
milliseconds. The knob now sets a ring time and the per-sample coefficient is
derived from it.

**2. Damping was a fixed cutoff in Hz.** The loop filter is applied once per
trip, so its attenuation also compounds per period: a fixed 3.6 kHz cutoff damps
a 440 Hz note 440 times a second and an 82 Hz note only 82. Brightness is now
held relative to the fundamental, never below four harmonics above the note.
These first two imbalances ran in opposite directions and partly cancelled,
which is why the instrument sounded plausible mid-keyboard and wrong at both
ends.

**3. No DC blocker in the loop.** The damping filter has unity gain at DC and
the loop gain approaches 0.99999, so whatever DC the excitation burst contained
was amplified towards `1/(1-g)`. Measured offsets reached **-0.046 against a
signal RMS of 0.05** - the string spent most of its life pinned off-centre,
eating headroom and pumping the limiter. A `DcBlocker` in the feedback path
takes it to +/-0.0001.

**4. The excitation burst was a fixed 4 ms** regardless of pitch - four periods
at C7, a quarter of one at C2. It now scales with the period, and `holdsVoice`
covers the whole ring rather than just the pluck, so voices are no longer stolen
mid-note.

**Still open: the string plays an octave low inside the graph.** Isolated but
not root-caused. See the note on the failing test in `tests/test_string.cpp`.

**On measurement.** Three methods disagreed with each other during this
investigation and two of them sent me down blind alleys - at one point I
concluded the entire engine was an octave low, which was false. Autocorrelation
octave-errs on harmonic-rich material; zero-crossing counts are corrupted by
harmonics *and* by DC offset; a low-pass-then-count approach fails when the
filter sits close to the fundamental. **Goertzel settles it** - exact energy at
a chosen frequency, no interpretation - and is what any future pitch claim in
this project should be backed by.

---

### 5.23a Where a DC blocker belongs

Removing the DC that a Karplus loop accumulates is necessary, but putting the
blocker INSIDE the feedback path was wrong, and wrong in a way that only the
bottom octave revealed. Everything in that loop is applied once per trip, so the
blocker's low-frequency attenuation compounds **per period**: at 55 Hz that is
55 applications a second, and A1 came out roughly ten times quieter than either
of its neighbours.

Filtering the excitation burst on its way in achieves exactly the same thing -
no DC enters, so none can accumulate - and leaves the loop's own response
untouched. Moving it made the bottom octave **22x louder** (E(f0) at 41 Hz went
from 0.0004 to 0.0086) and brought every note from E1 to C6 to the right pitch
at a healthy level.

The general rule: anything placed inside a resonator's feedback path has its
effect multiplied by the number of trips per second, which is the pitch. A
filter that is gentle at one end of the keyboard is savage at the other.

### 5.23 A macro at rest must do nothing

The single worst bug found in this project, and it had been silently rewriting
every instrument ever generated.

Macros offset exposed parameters. The offset was computed straight from the
macro's current position, so a macro sitting at its authored default was
**already contributing**. The stock row shipped with `Body` at 0.4 routing +0.5
to the sub oscillator, `Space` at 0.3 routing +0.5 to reverb, `Drive` at 0.2
routing +0.6 to drive:

| The model asked for | What was actually built |
|---|---|
| `osc_sub_level: 0.0` | sub oscillator at 0.2 |
| `rv_mix: 0.05` (deliberately dry) | reverb at 0.20 |
| a clean tone | drive raised by 0.12 |

So **every generated instrument carried an unrequested sine an octave below the
note**. On a plucked string, whose own fundamental is weak, that buried tone was
the loudest thing in the output - which is exactly why a bass guitar sounded an
octave low and "like ass". Measured with Goertzel: `E(f0/2)` fell from 0.076 to
0.00002 when this was fixed, and the string's fundamental went from being
outnumbered 50:1 to leading by 150:1.

It also explains why the macros felt dead. Half their travel was spent undoing
an offset that should never have been applied, and the parameters they moved
were already sitting part-way up their range.

The fix is one subtraction: the offset is measured **from the macro's default
position**, so an untouched macro contributes exactly zero and its full travel
is available in both directions. That is also what a macro means musically - a
performance control that starts where the sound designer left it.

**The lesson is the same one this project keeps learning.** Nobody was checking
that `level: 0` produced silence. A parameter that cannot reach its own stated
minimum is invisible until someone hears it, and by then it looks like the model
disobeying rather than the engine lying.

---

### 5.24 The gate is the source of truth

`porcelain_pluck` held a voice forever after noteOff. The envelope's release
depended entirely on a one-shot `noteOff()` callback arriving, and if it did not
- for any reason - the envelope sat in sustain, held its voice, and the note
never stopped. A held voice is never handed back, so polyphony leaked until the
instrument stopped responding.

`env.adsr` now reads `VoiceContext::gate` at the top of every block: gate low
and still sounding means release, unconditionally. The voice manager already
maintained that state, so the envelope is self-correcting - whatever happens to
the callback, an envelope whose gate is low WILL release.

The general principle, and the third time this project has learned it: **prefer
state that is re-evaluated every block over an event that must arrive exactly
once.** An event you can miss is a bug waiting for the right timing; state you
re-read is self-healing.

### 5.25 Not everything is a knob

Two instruments with identical geometry in different palettes read as the same
product. Colour was never going to be enough, so the model now chooses the
control SHAPES and the visual LANGUAGE as well.

**Control shapes** (`controls` in the patch, per exposed control id):

| Shape | For |
|---|---|
| `vslider` | things that move up and down - pitch, transpose, a sweep. A vertical pitch fader is standard on real instruments |
| `hslider` | blends, mixes, morphs - a travel between two things |
| `toggle` | genuinely two-state: sync, bypass, mono/stereo |
| `knob` | everything else, which is still most things |

An unknown shape leaves the control alone rather than reaching the UI as
something it cannot render.

**Visual language** (`layout.style`) changes the geometry, not just the palette:

| Style | Rendering |
|---|---|
| `sharp` | angular pointers, tick marks, tight rings - clinical, utilitarian |
| `soft` | thick rounded arcs, no ticks, larger caps - warm and hand-made |
| `vintage` | cream faces, chunky caps, dark lettering |
| `minimal` | hairline indicators, thin rings, almost no chrome |

The offline floor honours both. "A warm dusty reggae groove bass" comes back
`vintage` in amber with warm per-panel accents; "a bright clinical techno stab"
comes back `sharp` in teal. Same engine, different objects.

**One trap worth naming.** The first version styled `osc_a_pmod` on every
instrument - including plucked ones, which have no OSC A panel at all, so the
setting silently did nothing. That is the same dead-reference failure this
codebase has produced five times now. `tests/test_capability.cpp` now asserts
that every control the offline patch shapes actually survives into the
instrument it builds.

---

### 5.26 Why a physically-modelled guitar still sounded like a synth

The routing was right. `str_level` 0.85, oscillators pulled back to 0.15, body
engaged, sensible filtering - and it still did not sound like a guitar. Getting
the right MODULE turned out to be necessary and nowhere near sufficient.

**A bare Karplus-Strong string is very close to a filtered sawtooth**: every
harmonic present at full strength, all of them perfectly in tune with each
other, no excitation character, and full bandwidth to Nyquist. Four things
separate that from an instrument, and each is now measured rather than asserted.

**1. Pick position** (`str_pick`). A pluck a fraction B along a string cannot
excite any harmonic with a node there, so every 1/B-th harmonic is missing.
That comb of notches is the most recognisable signature of a plucked
instrument - it is why a guitar picked at the bridge is nasal and the same
string picked over the neck is round. Tested by picking at exactly 1/4 and
confirming the 4th harmonic drops below its neighbour.

**2. String stiffness** (`str_stiff`). Real strings are stiff, so upper partials
travel faster and sit progressively SHARP of the harmonic series. Three
first-order allpasses in the loop; measured, the 8th partial runs +3.5 cents and
the 12th +6.9 cents while the fundamental stays exactly in tune. Two sign and
compensation bugs were found here: a negative coefficient made partials flat
instead of sharp, and the DC approximation to the allpass delay left the whole
string 21 cents flat, so the phase delay is now evaluated exactly at f0.

**3. Excitation character.** A pick is a band-limited scrape whose brightness
tracks how hard it is played, not flat white noise. Verified: hard playing has
more 8th-harmonic energy relative to the fundamental, not merely more level.

**4. A SPEAKER** (`fx.cabinet`) - the single biggest one, and the reason
anything driven read as synthetic. A guitar cabinet has no deep bass, a
presence peak around 2-4 kHz and an extremely steep rolloff above 5 kHz.
Distortion produces harmonics all the way to Nyquist, and it is exactly that
5-20 kHz fizz - which no real speaker can produce - that the ear hears as
artificial. Measured: +2.3 dB at 3 kHz, -10 dB at 6 kHz, -28 dB at 10 kHz,
-49 dB at 16 kHz.

**And a default that was quietly ruining every string.** `str_decay` defaulted
to 0.985, which through the T60 mapping is a **twelve second ring** - a pad, not
a pluck. Now 0.90, about four seconds.

**The correction is in two independent parts**, because the reported patch
exposed the gap between them: the ENGINE corrector rebuilds the source when the
model reached for oscillators, and a separate TONE corrector fixes voicing when
the engine is already right but the instrument is voiced like a pad. The
reported patch triggered only the second, and the first would have returned
early and declared success.

### 5.27 Why it STILL sounded like a synth: a string with no fundamental

Everything in 5.26 was true and none of it was enough. The report came back
again - "guitars still sound like pianos and synths, not organic guitars, I want
that plucky sound" - and this time the measurement was unambiguous. Rendering
the offline electric guitar at E3 (164.8 Hz) and reading the partials with
Goertzel:

| partial | level relative to h1 |
| --- | --- |
| h2 | +2.1 dB |
| h3 | +3.0 dB |
| **h4 (659 Hz)** | **+11.8 dB** |

The loudest thing in a low E was its fourth harmonic. The ear resolves that as
a different note two octaves up, and "hollow, wrong octave, no bottom" is
exactly what a synthesiser sounds like. Three separate causes, each measured.

**1. The excitation had no fundamental in it.** Karplus-Strong is usually taught
as a burst of white noise in a delay line, and white noise starts every harmonic
at equal strength - which is the spectrum of a filtered sawtooth, the very thing
5.26 set out to stop sounding like. A real pluck starts from a **triangular
displacement** with its apex at the pick point, whose harmonics fall away as
`sin(k·pi·beta) / k²`: the comb of missing harmonics AND, crucially, the 1/k²
that makes a plucked note fundamental-led. Measured on a bare string, h2 went
from +12.9 dB above the fundamental to below it.

**2. The pickup reads velocity, not position.** 1/k² alone is correct and
unusably dull - centroid 418 Hz against 1424 Hz for a real recording of the same
instrument, with 85% of the energy below 668 Hz. A magnetic pickup, and a
soundboard, respond to how fast the string is *moving*; velocity is the
derivative of displacement, which lifts the spectrum by one power of k to about
1/k. Normalised at the fundamental so brightness stays relative to pitch, for
the same reason the loop damping is. Measured tilt afterwards: -9.0 dB/oct
against -8.8 for the reference recording.

**3. The output comb was set to 0.86** and, with the pluck shape now carrying
the pick comb itself, that was a second full-depth comb on top of the first. It
is the PICKUP position, it is shallower than that in reality, and at 0.42 it
colours the tone without taking the fundamental out with it.

### 5.28 A parameter that cannot reach its own minimum, again

Bug class 2 from 5.23, in the modulation matrix rather than in the macros, and
much worse: **modulation is summed, so `m_vel -> amp.gain` at depth 0.35 put a
permanent 0.35 floor under the amplifier that the amplitude envelope had no way
to remove.** The note never ended. Measured on the offline guitar with the
envelope shut completely - sustain 0, release 1 ms - the level sat at -17 dB
forever, and was still there eight seconds later.

Every offline instrument did this, and so did every model-authored one that took
the system prompt's advice, because the prompt said in as many words: "route
m_vel to amp.gain". A guitar that never stops ringing is an organ. This was the
single largest contributor to the reported sound, larger than anything in 5.26.

Fixed by giving `ParamDesc` a `multiplicativeMod` flag, set on `vca.gain`. Where
it is set, each route contributes a factor of `1 + depth·(source - 1)` for a
unipolar source and `1 + depth·source` for a bipolar one, and the factors
multiply. An envelope at depth 1.0 is then exactly the envelope, velocity
becomes a scaling, tremolo works properly, and zero stays reachable whatever
else is routed in. After: the same note decays to -101 dB by three seconds, and
velocity still changes the level.

The lesson from 5.23 was "the offset must be measured from the default". The
lesson here is the more general one: **anything that gates has to multiply.**

### 5.29 A body tuned into the register it supports

`body` is a comb resonator, so it reinforces its frequency and every multiple of
it *under whatever note is playing*. The rack default was 220 Hz and the offline
fallback wrote 220 Hz, which is A3 - so on a low E the comb sat squarely on the
fourth harmonic and produced most of the +11.8 dB above. Real boxes resonate
BELOW the register they support: a guitar's air resonance is near 100 Hz, a big
bass body nearer 60. Both the default and the fallback now use those, the prompt
explains why, and a solid-body electric gets no comb at all - its resonator is
the speaker.

### 5.30 The offline fallback could not reach the speaker

`heuristicPatch` set `cab_mix` faithfully and it did nothing, because a panel
that appears on no page is removed from the instrument entirely - node and all -
and the acoustic layout had no CABINET panel. Every offline electric guitar was
a DI into a distortion box, which 5.26 identifies as the single biggest tell of
a modelled guitar. It also still wrote `str_decay` 0.984 - an eleven second ring
- straight over the top of the default that 5.26 had just fixed, and never set
`str_pick` or `str_stiff` at all.

This is bug class 1 for the fourth time, and the pattern is now unmistakable:
**a value that is set but unreachable is indistinguishable from a value that was
never set, and only a test that renders audio can tell them apart.**

### 5.31 A recording of somebody PLAYING is not a full mix

A producer dropped in a sound sample of one electric guitar - twelve seconds,
one instrument, nothing else - and it was reported to the model as a FULL MIX
with its pitch, its attack and its harmonicity all deliberately withheld
(see 5.x on mix detection). It scored 0.82 on the mix test purely for being
longer than one note: no single confident fundamental across twelve seconds, and
energy in all three bands, which is true of a distorted guitar and of a Kanye
record alike.

The measurements were not wrong. Measuring twelve seconds as though it were one
note was wrong - the "attack" came out at 3.4 seconds, which is simply where the
loudest chord happened to fall.

Almost nobody has an isolated single note to hand; they have a recording of
someone playing. So the analyser now detects note onsets, and where it finds
several it measures ONE clean note and reports that. The note is chosen by
level, by how much room it has to ring, by whether the pitch detector is
confident about it, and against being a chord - picking purely by level chose
the loudest transient in the clip, which was a scrape with no note in it, and
the model was told the reference was "unpitched".

Mix detection was rebuilt on the same idea: ask each NOTE whether it has a
pitch. One instrument answers yes even where the whole clip answers no; a mix
answers no at every scale, because there is a drum kit inside every window. The
guitar sample now scores 0.20 and is described as a performance.

**And a pitch that is not a note.** YIN reports the period of the waveform, and
that is not always the period of a note: two notes a fifth apart repeat at twice
the period of either, a major third at four times. The guitar recording measured
**44.7 Hz** - well below its lowest string - and everything derived from that
fundamental then described the instrument as inharmonic and bell-like. Settled
by measuring: if the reported fundamental has almost no energy at it and a low
multiple has plenty, that multiple is the note. Where even that leaves the
reading weak, the pitch is reported as unreliable and the register and key
centres are given instead, and the three measurements that are read off the
fundamental - harmonicity, inharmonicity, odd/even - are withheld rather than
quoted as something they are not.

### 5.32 The generation that "timed out after 60 seconds"

Not a slow network. The request was not streamed, so **no bytes moved at all
while the model wrote a 16k-token patch**, and JUCE applies its socket timeout
per read - which makes a healthy generation and a dead connection look
identical. At the deadline the whole thing was discarded and the offline
fallback quietly put in its place, which is what "it timed out and gave me the
wrong instrument" actually was. Section 10 of the handoff had already recorded
the per-read behaviour as a reason to disable thinking; the same fact is a
reason to stream.

All three providers now stream, and the reassembler is JUCE-free so the three
wire formats are tested headlessly rather than against a network. Bytes arrive
continuously - the providers send keep-alive pings between tokens - so a read
timeout only fires on a connection that has genuinely stopped. Whatever arrived
before a break is kept and repaired by the salvage pass, and the musician is
told that is what happened. The whole-generation budget went from 60 s to 150 s,
because it is now the point at which we stop waiting rather than the point at
which waiting gets mistaken for failure.

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
