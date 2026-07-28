# IndieVST — Forge

Industry-grade VSTs and similar plugins have large one-time costs and licenses that create
a gap for indie music producers to make music with the quality of sound that can compete
with large-labels and studio producers. IndieVST bridges that gap by allowing producers to
generate their own plugins, personalised to their specific taste.

**Forge** is the first product: a VST3 instrument where you describe a sound in plain
English and get a playable synthesiser — inside your DAW, in a few seconds, with real
knobs and a signal path designed for that specific request.

```
"warm analog bass with a slow filter sweep and a bit of drive"
        │
        ▼
   LLM designs a graph  ──►  validate ──►  repair ──►  safety ──►  build
        │                                                            │
        │  (worker thread, never the audio thread)                   ▼
        └──────────────────────────────────────────►  playable instrument
```

The $150–$500 price of a flagship synth is not protecting scarce raw material. It is
protecting **sound design labour**: choosing DSP modules, wiring them, and dialling
parameters until the result is musical. Forge automates that labour. The DSP itself stays
hand-written, compiled and trusted.

Full design rationale, roadmap and risk register: **[SPEC.md](SPEC.md)**.

---

## Status

| Layer | State |
|---|---|
| DSP engine, IR, validator, safety, repairer, prompt builder | **Built, run and tested** — 51 cases, ~102k assertions, 0 failures |
| JUCE plugin wrapper, parameter pool, graph publisher, LLM session, UI | **Compiles clean against JUCE 8.0.4** — `-Wall -Wextra -Wshadow`, zero warnings; every symbol resolves |
| Linked `.vst3` binary | **Pending your first MSVC build** |

The core is deliberately **JUCE-free**, so it compiles and runs headlessly anywhere with
just a C++20 compiler. That is why the risky half — the validator, the safety clamps, the
graph engine — is already proven.

All 26 translation units were also checked for link completeness: every `forge::` symbol
referenced anywhere is defined somewhere, so nothing is left dangling for the linker. The
final link itself has to happen on Windows — JUCE's Linux GUI backend needs X11 dev
packages that a sandbox cannot provide, and that is not the target platform anyway.

---

## Build

### Requirements

- **CMake ≥ 3.22**
- **Visual Studio 2022** (Desktop development with C++) on Windows x64
- Network access on the first configure — CMake fetches JUCE 8.0.4

### Windows

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

No Administrator shell required. The build installs to the **per-user** VST3 folder,
`%LOCALAPPDATA%\Programs\Common\VST3`, so you never have to run a compiler elevated.

### Using it in FL Studio

1. **Options → Manage plugins**
2. Under *Plugin search paths*, **Add** `%LOCALAPPDATA%\Programs\Common\VST3`
   (paste the expanded path, e.g. `C:\Users\maxim\AppData\Local\Programs\Common\VST3`)
3. Tick **Verify plugins** and click **Find installed plugins**
4. **IndieVST** appears under *Generators* — it is an instrument, not an effect.
   (If you see a plugin called *Forge*, that is a stale build from before the
   rename. Delete it, or FL will keep loading the old one.)
5. Drop it on a channel in the Channel Rack and play it from the piano roll

If you would rather not add a search path, copy the bundle into the folder FL
already scans, from an **elevated** PowerShell:

```powershell
Copy-Item -Recurse -Force "$env:LOCALAPPDATA\Programs\Common\VST3\IndieVST.vst3" "C:\Program Files\Common Files\VST3\"
```

IndieVST is a normal VST3 instrument: MIDI in, stereo out, no input bus, so the
piano roll, mixer routing, inserts on other plugins and rendering to audio all
behave the way FL expects. It reports an 8 second tail, so reverb and delay
tails are not cut off when you render.

**Automation note.** Because each generated instrument has different controls,
IndieVST exposes a fixed pool of **16 macros + 224 parameter slots** and
re-points them per instrument (SPEC §9.1). The pool is declared once at
construction, before the host scans, so the automation list never changes shape
underneath a saved project. FL reads slot names on demand, so *Browse
parameters* shows the current instrument's names. Selector controls (wave shape,
filter mode) rebuild the graph and are therefore **not** automatable — they are
UI-only by design.

Other hosts: **Ableton** — Preferences → Plug-Ins → VST3 Custom Folder.
**Reaper** — Options → Preferences → Plug-ins → VST → Add.

To install system-wide instead, configure from an elevated shell with
`-DFORGE_VST3_COPY_DIR="C:/Program Files/Common Files/VST3"`.

A **Standalone** executable is produced alongside the VST3 — the fastest way to hear
changes without loading a DAW:

```powershell
cmake --build build --config Release --target IndieVST_Standalone
.\build\IndieVST_artefacts\Release\Standalone\IndieVST.exe
```

### Just the core and its tests (no JUCE, seconds to build)

```bash
cmake -B build-core -DFORGE_BUILD_PLUGIN=OFF
cmake --build build-core
./build-core/forge_tests
```

This is the loop to stay in while working on DSP, the IR or the validator.

---

## Configuration

Copy `.env.example` to `.env` and add an API key, **or** use the Settings panel inside the
plugin (which writes to `%APPDATA%\Forge\config.json` and never into your project file).

```
FORGE_LLM_PROVIDER=anthropic
FORGE_ANTHROPIC_API_KEY=sk-ant-...
```

Providers are abstracted behind one interface: **Anthropic**, **OpenAI**, **Ollama**
(local), or **offline**. If a request fails for any reason — no key, no network, a
malformed response — Forge falls back to a built-in library of six hand-authored
instruments. **The demo cannot be killed by bad wi-fi.** Set `FORGE_FORCE_OFFLINE=1` to
rehearse that path deliberately.

---

## How it works

### The Instrument IR

The contract between the model and the engine. The LLM does not write DSP; it emits a
schema-constrained graph — which modules to instantiate, how to wire audio, how to route
modulation, which knobs to expose, and what wavetables/curves/envelopes to bake.

```jsonc
{
  "nodes":  [{"id": "osc_saw", "type": "osc.analog", "scope": "voice",
              "settings": {"wave": "saw", "unison": 2, "octave": -1}}],
  "audio":  [{"from": "osc_saw", "to": "filt", "gain": 1.0}],
  "mod":    [{"source": "env_filt", "target": "filt.cutoff", "depth": 0.6, "curve": "exp"}],
  "params": [{"id": "cutoff", "label": "Cutoff", "unit": "Hz",
              "min": 40, "max": 12000, "default": 420, "taper": "log",
              "bind": [{"node": "filt", "param": "cutoff"}]}],
  "macros": [{"id": "warmth", "label": "Warmth", "default": 0.35,
              "routes": [{"param": "cutoff", "depth": -0.35}]}]
}
```

### Non-negotiable invariants

| # | Invariant |
|---|---|
| I1 | The LLM never runs on, blocks, or allocates for the real-time audio thread |
| I2 | No network, file I/O, locking or heap allocation in `processBlock` |
| I3 | Every LLM output passes schema validation and safety checks before instantiation |
| I4 | Audio is deterministic given (graph, parameters, MIDI, seed) |
| I5 | A malformed or hostile response degrades gracefully — never a crash, hang, or DC blast |
| I6 | Schema and primitives are versioned; saved instruments never break |

### Safety, concretely

Generated content is treated as **untrusted input from the internet**, because that is
exactly what it is.

- Size, nesting-depth and schema limits before the JSON parser is even reached
- Feedback loops legal **only** through `fx.delay` or `filter.comb`; everything else is a
  cycle error
- Hard clamps on filter resonance, delay/comb feedback and gain staging
- `out.master` — DC blocker → soft clip → brickwall limiter — is mandatory, appended
  automatically, and cannot be removed or bypassed by generated content
- Per-block NaN/Inf scan that mutes and resets rather than propagating
- CPU budget that **reduces** (polyphony, then unison, then duplicate reverbs) before it
  ever rejects
- A deterministic repairer that fixes what it can and reports every change to the user

### Threading

```
message thread          worker thread                    audio thread
──────────────          ─────────────                    ────────────
chat / UI  ──prompt──►  LLM → validate → repair
                        → bake assets → build graph
                        (all allocation happens here)
                              │
                              └── atomic exchange ──────►  process()
                                                            reads one pointer,
                                                            never allocates,
                                                            never locks
retire queue ◄── displaced graph, freed only once the audio thread has
                 provably moved two blocks past it
```

Instrument swaps use a 20 ms equal-power crossfade, so you can change instruments while a
chord is still ringing.

---

## Repository layout

```
src/core/          JUCE-free: DSP primitives, graph engine, IR, validator, prompt builder
src/plugin/        AudioProcessor, parameter pool, graph publisher, library, config
src/llm/           Provider implementations and the async generation session
src/ui/            Prompt screen, generated instrument rack, graph view, look and feel
resources/fonts/   Inter, subset and embedded so the UI is identical everywhere
tests/             Headless test suite, including the IR fuzzer
resources/canned/  Human-readable mirror of the built-in instruments
```

### Extending the module set

This is the highest-leverage work in the project and it is deliberately trivial:

1. Write the module class in `src/core/dsp/Modules.cpp`
2. Add its manifest next to it, and one line in `registerBuiltinModules`

The validator, the LLM prompt and the UI all read from the registry, so they pick it up
automatically. **No prompt edits, no schema edits, no UI edits.** If a new capability
requires hand-editing the prompt, the abstraction is wrong.

---

## Known limitations

- **VST3 dynamic parameter names.** Hosts cache parameter names from scan time. Forge
  uses a fixed pool of slots that are re-pointed per instrument; its own GUI is always
  correct, but Ableton in particular may keep showing `Param 07` in its automation list.
  Automation itself works everywhere. Fixed properly by Phase 3 export (SPEC §14.3).
- **Block-rate modulation** (16 samples / 0.33 ms). Audio-rate FM between arbitrary nodes
  is Phase 1.
- Windows x64 only so far. macOS/AU is Phase 2.
- The reverb is a Schroeder/Freeverb topology, not a true FDN.

---

## Licensing

Developed under the **GPLv3** arms of the JUCE and VST3 SDK dual licences — fine for
development and the pitch. Shipping closed-source requires a commercial JUCE licence and a
signed (free) Steinberg VST3 agreement. See [THIRD_PARTY.md](THIRD_PARTY.md) and SPEC §3.1.
