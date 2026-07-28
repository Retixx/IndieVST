#include "core/llm/InstrumentCorrection.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace forge::llm {
namespace {

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool mentions(const std::string& text, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (text.find(w) != std::string::npos) return true;
    return false;
}

float* valuePtr(ir::Instrument& inst, const char* id) {
    for (auto& p : inst.params)
        if (p.id == id) return &p.def;
    return nullptr;
}

float valueOr(const ir::Instrument& inst, const char* id, float fallback) {
    for (const auto& p : inst.params)
        if (p.id == id) return p.def;
    return fallback;
}

/// Sets a control, clamped to its own range. Silently ignores controls the
/// layout pass removed - a correction must never resurrect a panel the model
/// deliberately left out.
void set(ir::Instrument& inst, const char* id, float value) {
    for (auto& p : inst.params)
        if (p.id == id) { p.def = std::clamp(value, p.min, p.max); return; }
}

void setSetting(ir::Instrument& inst, const char* node, const char* key, nlohmann::json v) {
    if (auto* n = inst.findNode(node)) {
        if (!n->settings.is_object()) n->settings = nlohmann::json::object();
        n->settings[key] = std::move(v);
    }
}

} // namespace

bool enforceInstrumentFamily(ir::Instrument& inst, const std::string& prompt,
                             ir::IrReport& report) {
    const auto named = namedInstrument(prompt);
    if (!named) return false;

    const auto text = lowered(prompt);

    const float string = valueOr(inst, "str_level", 0.0f);
    const float fm     = valueOr(inst, "osc_fm_level", 0.0f);
    const float body   = valueOr(inst, "body_mix", 0.0f);

    // Mood, read off the same words the rest of the system uses, so a corrected
    // instrument still sounds like what was asked for rather than like a preset.
    const bool dark   = mentions(text, {"dark", "warm", "mellow", "muted", "dusty",
                                        "round", "soft", "lofi", "lo-fi", "woody"});
    const bool bright = mentions(text, {"bright", "crisp", "glassy", "sharp", "twang",
                                        "steel", "shimmer", "cutting"});
    const bool gritty = mentions(text, {"gritty", "driven", "distort", "dirty", "fuzz",
                                        "overdriv", "crunch"});
    const bool low    = mentions(text, {"bass", "sub", "low", "deep"});

    switch (named.family) {
        case Family::Plucked: {
            if (valuePtr(inst, "str_level") == nullptr) return false;   // panel removed

            const bool electric = mentions(text, {"electric", "amp", "pickup", "strat",
                                                  "tele", "les paul", "jazzmaster", "rock"})
                                || gritty || low;

            // The string may already be the voice and the instrument STILL sound
            // synthetic, which is exactly what happened: str_level 0.85, a body,
            // sensible filtering - and a thirteen-second ring straight into the
            // output with no speaker. Getting the engine right is necessary and
            // nowhere near sufficient, so tone is corrected on its own.
            if (string > 0.15f) {
                bool touched = false;

                if (valueOr(inst, "str_decay", 0.90f) > 0.955f) {
                    set(inst, "str_decay", dark ? 0.885f : 0.915f);
                    touched = true;
                }
                if (electric && valueOr(inst, "cab_mix", 0.0f) < 0.25f) {
                    set(inst, "cab_mix", 0.85f);
                    set(inst, "cab_top", low ? 3500.0f : 4800.0f);
                    touched = true;
                }
                if (valueOr(inst, "str_pick", 0.14f) > 0.35f) {
                    set(inst, "str_pick", 0.14f);
                    touched = true;
                }
                // A body up in the register the instrument plays in reinforces
                // one fixed frequency and its multiples under every note. On a
                // low E a 200 Hz comb lands on the fourth harmonic and the ear
                // hears the wrong octave - which is most of what "it sounds
                // like a synth" turned out to mean.
                if (valueOr(inst, "body_mix", 0.0f) > 0.05f
                    && valueOr(inst, "body_freq", 220.0f) > 150.0f) {
                    if (electric) set(inst, "body_mix", 0.0f);
                    else          set(inst, "body_freq", low ? 62.0f : 100.0f);
                    touched = true;
                }
                // And the amplifier must not be shutting the string off. A
                // plucked patch whose VCA closes at 0.1 after 400 ms is a synth
                // pluck wearing a physical model.
                if (valueOr(inst, "env_1_s", 0.8f) < 0.30f
                    && valueOr(inst, "env_1_d", 400.0f) < 900.0f) {
                    set(inst, "env_1_s", 0.60f);
                    set(inst, "env_1_d", 2500.0f);
                    touched = true;
                }

                if (touched)
                    report.fixed("instrument",
                                 "The " + named.word + " was built on the string model but "
                                 "voiced like a pad - the ring was shortened and it was put "
                                 "through a speaker.");
                return touched;
            }

            // The physical model becomes the voice. The oscillators are pulled
            // back rather than silenced: a little saw under a string is how a
            // pickup and an amp actually sound, and the model may have chosen
            // those levels deliberately.
            set(inst, "str_level",  0.85f);
            // The loop damping is applied once per period, so it bites far
            // harder than the numbers suggest, and the pickup rolls the top off
            // again downstream. 0.32/4600 measured a centroid of 797 Hz against
            // 1424 for a real recording of the same instrument - correct in
            // every structural way and still obviously not a guitar.
            set(inst, "str_damp",   dark ? 0.38f : (bright ? 0.14f : 0.22f));
            set(inst, "str_bright", low ? 2600.0f : (bright ? 8000.0f : 6500.0f));
            set(inst, "str_decay",  dark ? 0.885f : 0.915f);
            // The three that separate a string MODEL from an instrument. A
            // correction that only swaps the oscillator for a Karplus loop
            // still hands back something that sounds like a synth.
            set(inst, "str_pick",  low ? 0.16f : 0.12f);
            set(inst, "str_stiff", 0.30f);

            for (const char* id : {"osc_a_level", "osc_b_level", "osc_wt_level"})
                set(inst, id, std::min(valueOr(inst, id, 0.0f), 0.12f));
            set(inst, "osc_noise_level", std::min(valueOr(inst, "osc_noise_level", 0.0f), 0.06f));
            // A box, for the instruments that have one. A solid-body electric
            // does not - its resonator is the speaker below - and 200 Hz is a
            // poor choice of one in any case: the comb reinforces 200, 400 and
            // 600 Hz whatever note is playing, which on a low E puts an
            // unrequested peak straight onto a harmonic.
            if (!electric && body < 0.05f) {
                set(inst, "body_mix",  0.24f);
                set(inst, "body_freq", low ? 62.0f : 100.0f);
            }

            // A string decays on its own, so the amplifier only has to get out
            // of the way: no slow swell at the front, no pad-length sustain
            // holding it up, and no fast shutdown cutting the ring off either.
            set(inst, "env_1_a", 1.0f);
            if (valueOr(inst, "env_1_s", 0.0f) > 0.80f) set(inst, "env_1_s", 0.60f);
            if (valueOr(inst, "env_1_s", 0.0f) < 0.30f) set(inst, "env_1_s", 0.60f);
            if (valueOr(inst, "env_1_d", 0.0f) < 900.0f) set(inst, "env_1_d", 2500.0f);
            if (gritty) set(inst, "dr_drive", std::max(valueOr(inst, "dr_drive", 1.0f), 2.2f));

            // An electric instrument needs a speaker. Distortion runs to
            // Nyquist and no amp can do that, so without a cabinet the result
            // is fizz - the most reliable single tell of a modelled guitar.
            if (electric && valueOr(inst, "cab_mix", 0.0f) < 0.2f) {
                set(inst, "cab_mix", 0.85f);
                set(inst, "cab_top", low ? 3500.0f : 4800.0f);
            }

            report.fixed("instrument", "The request named a " + named.word
                                       + ", so the sound source was rebuilt on the "
                                         "physical string model instead of oscillators.");
            return true;
        }

        case Family::Mallet:
        case Family::Struck: {
            if (fm > 0.15f || string > 0.15f) return false;
            if (valuePtr(inst, "osc_fm_level") == nullptr) return false;

            set(inst, "osc_fm_level", 0.82f);
            set(inst, "osc_fm_index", named.family == Family::Mallet ? 3.4f : 2.6f);
            for (const char* id : {"osc_a_level", "osc_b_level", "osc_wt_level"})
                set(inst, id, std::min(valueOr(inst, id, 0.0f), 0.12f));

            // Inharmonic for metal, harmonic for wood and for strings under
            // hammers. This one setting is the whole difference.
            const bool wooden = mentions(text, {"marimba", "wood", "xylophone", "kalimba"});
            const bool piano  = named.family == Family::Struck;
            setSetting(inst, "osc_fm", "ratio", piano ? 1.0 : (wooden ? 4.0 : 3.5));

            set(inst, "env_1_a", 0.3f);
            set(inst, "env_1_s", piano ? 0.15f : 0.0f);
            if (valueOr(inst, "env_1_d", 0.0f) < 300.0f)
                set(inst, "env_1_d", wooden ? 450.0f : 1800.0f);

            report.fixed("instrument", "The request named a " + named.word
                                       + ", so the sound source was rebuilt on FM with "
                                         "a matching modulator ratio.");
            return true;
        }

        case Family::Bowed:
        case Family::Wind: {
            // Oscillators are correct for these; what they were missing is a
            // body, and an attack that is not instant.
            if (body > 0.05f || valueOr(inst, "vow_mix", 0.0f) > 0.05f) return false;
            if (valuePtr(inst, "body_mix") == nullptr) return false;

            set(inst, "body_mix",  0.24f);
            set(inst, "body_freq", low ? 110.0f : 320.0f);
            if (named.family == Family::Wind) set(inst, "vow_mix", 0.20f);
            if (valueOr(inst, "env_1_a", 0.0f) < 25.0f)
                set(inst, "env_1_a", named.family == Family::Bowed ? 120.0f : 70.0f);
            if (valueOr(inst, "env_1_s", 0.0f) < 0.5f) set(inst, "env_1_s", 0.80f);

            report.fixed("instrument", "The request named a " + named.word
                                       + ", so a resonant body and a bowed attack were "
                                         "added; it was arriving as a bare oscillator.");
            return true;
        }

        case Family::Reed: {
            if (body > 0.05f || fm > 0.15f
                || valueOr(inst, "osc_b_level", 0.0f) > 0.15f) return false;
            if (valuePtr(inst, "osc_b_level") == nullptr) return false;

            set(inst, "osc_b_level", 0.45f);
            setSetting(inst, "osc_b", "octave", 1);
            set(inst, "body_mix", 0.14f);
            report.fixed("instrument", "The request named a " + named.word
                                       + ", so a second harmonic rank and a cabinet were "
                                         "added.");
            return true;
        }

        case Family::None:
        default:
            return false;
    }
}

} // namespace forge::llm
