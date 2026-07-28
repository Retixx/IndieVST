#include "core/llm/Compliance.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace forge::llm {
namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n.,;:\"'");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n.,;:\"'");
    return s.substr(b, e - b + 1);
}

bool has(const std::string& text, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (text.find(w) != std::string::npos) return true;
    return false;
}

/// Whole-word (or whole-phrase) match.
///
/// Plain substring search is wrong for instrument names and the failures are
/// silent: "flute" contains "lute", so a breathy flute was classified as a
/// plucked instrument and told to build a Karplus string. "harpsichord"
/// contains "harp" for the same reason. A word has to be bounded by something
/// that is not a letter to count.
bool mentionsWord(const std::string& text, const std::string& word) {
    if (word.empty()) return false;
    size_t at = 0;
    while ((at = text.find(word, at)) != std::string::npos) {
        const bool leftOk  = at == 0
                          || !std::isalpha(static_cast<unsigned char>(text[at - 1]));
        const size_t end = at + word.size();
        const bool rightOk = end >= text.size()
                          || !std::isalpha(static_cast<unsigned char>(text[end]));
        if (leftOk && rightOk) return true;
        at += 1;
    }
    return false;
}

/// True if any modulation route in the instrument starts at `source`.
bool routedFrom(const ir::Instrument& inst, const std::string& source) {
    for (const auto& r : inst.mod)
        if (r.source == source) return true;
    return false;
}

/// True if anything at all modulates a parameter whose id contains `fragment`.
bool modulatesSomethingLike(const ir::Instrument& inst, const std::string& source,
                            std::initializer_list<const char*> fragments) {
    for (const auto& r : inst.mod) {
        if (!source.empty() && r.source != source) continue;
        const auto target = lower(r.targetNode + "." + r.targetParam);
        for (const char* f : fragments)
            if (target.find(f) != std::string::npos) return true;
    }
    return false;
}

/// The value a control was left at, or `fallback` if it does not exist.
float valueOf(const ir::Instrument& inst, const std::string& id, float fallback) {
    for (const auto& p : inst.params)
        if (p.id == id) return p.def;
    return fallback;
}

} // namespace

// ---------------------------------------------------------------------------

std::vector<std::string> requestedMacroNames(const std::string& prompt) {
    const auto text  = prompt;
    const auto lowered = lower(text);

    // Find the phrase that introduces the list: "macros:", "macro controls -",
    // "macro knobs called", and so on.
    size_t at = std::string::npos;
    for (const char* key : {"macro controls", "macro knobs", "macros", "macro"}) {
        at = lowered.find(key);
        if (at != std::string::npos) { at += std::string(key).size(); break; }
    }
    if (at == std::string::npos) return {};

    // Skip the connective words between "macros" and the list itself.
    //
    // An em dash is THREE UTF-8 bytes, so `text[at] == '—'` compared a single
    // char against a multi-character constant and could never be true - it also
    // warns. A brief written with a real dash ("macros — Warmth, Drive") had its
    // separator left in place, the name list started with punctuation, and the
    // named macros were silently not found. Which is bug class 1 again: the
    // capability was there and the prompt could not reach it.
    auto isDash = [&](size_t i) {
        return i + 2 < text.size()
            && static_cast<unsigned char>(text[i])     == 0xE2
            && static_cast<unsigned char>(text[i + 1]) == 0x80
            && (static_cast<unsigned char>(text[i + 2]) == 0x94       // em dash
             || static_cast<unsigned char>(text[i + 2]) == 0x93);     // en dash
    };
    while (at < text.size()) {
        if (isDash(at)) { at += 3; continue; }
        if (std::isspace(static_cast<unsigned char>(text[at]))
            || text[at] == ':' || text[at] == '-') { ++at; continue; }
        break;
    }
    for (const char* filler : {"called ", "named ", "labelled ", "labeled "}) {
        const auto len = std::string(filler).size();
        if (lowered.compare(at, len, filler) == 0) { at += len; break; }
    }

    // The list ends at the first sentence break. Anything past that is a new
    // instruction, not another macro name.
    size_t end = text.find_first_of(".;\n", at);
    if (end == std::string::npos) end = text.size();

    std::vector<std::string> names;
    std::string              current;
    auto flush = [&] {
        auto name = trim(current);
        current.clear();
        if (name.empty() || name.size() > 16) return;
        const auto low = lower(name);
        // "and", "a", "the" are separators, not macro names. Neither are the
        // stray words that show up when the sentence has no list at all.
        if (has(low, {"and", "the", "with", "for", "that", "which"}) && low.size() <= 5) return;
        if (!std::isupper(static_cast<unsigned char>(name[0]))) return;   // named things are capitalised
        // Macro labels are single words. Trailing politeness ("Glue please")
        // and stray clauses get trimmed rather than becoming part of the name.
        if (const auto space = name.find(' '); space != std::string::npos)
            name.resize(space);
        if (name.empty()) return;
        names.push_back(std::move(name));
    };

    for (size_t i = at; i < end && names.size() < 16; ++i) {
        const char c = text[i];
        if (c == ',' || c == '/') { flush(); continue; }
        if (c == ' ' && lower(text.substr(i + 1, 4)) == "and ") { flush(); i += 4; continue; }
        current += c;
    }
    flush();

    // A single result is almost always a false positive - "map the mod wheel to
    // a macro" is not a list of names.
    if (names.size() < 2) return {};
    return names;
}

// ---------------------------------------------------------------------------

NamedInstrument namedInstrument(const std::string& prompt) {
    const auto text = lower(prompt);

    struct Entry { const char* words[10]; Family family; };
    static const Entry kFamilies[] = {
        {{"guitar", "gtr", "bass", "harp", "koto", "banjo", "sitar", "ukulele",
          "mandolin", "lute"},                                            Family::Plucked},
        {{"violin", "viola", "cello", "double bass", "fiddle", "strings",
          "orchestra", "erhu", nullptr, nullptr},                         Family::Bowed},
        {{"piano", "rhodes", "wurlitzer", "wurli", "clav", "harpsichord",
          "celesta", nullptr, nullptr, nullptr},                          Family::Struck},
        {{"glockenspiel", "vibraphone", "vibes", "marimba", "xylophone",
          "kalimba", "bell", "chime", "gamelan", "steel drum"},           Family::Mallet},
        {{"flute", "clarinet", "oboe", "bassoon", "trumpet", "trombone",
          "sax", "brass", "voice", "choir"},                              Family::Wind},
        {{"organ", "hammond", "accordion", "harmonium", "bagpipe", nullptr,
          nullptr, nullptr, nullptr, nullptr},                            Family::Reed},
    };

    // An explicit request for a synth wins. "synth bass" is not a bass guitar.
    if (has(text, {"synth", "synthesiser", "synthesizer", "moog", "juno", "303",
                   "808", "sub bass", "reese", "wobble", "dubstep", "supersaw",
                   "fm bass", "wavetable", "analog", "analogue", "oscillator"}))
        return {};

    for (const auto& entry : kFamilies)
        for (const char* w : entry.words) {
            if (w == nullptr) break;
            if (mentionsWord(text, w)) return {entry.family, w};
        }
    return {};
}

ComplianceReport check(const std::string& prompt, const ir::Instrument& inst) {
    ComplianceReport report;
    const auto text = lower(prompt);

    auto add = [&](std::string id, std::string asked, bool met, std::string fix) {
        report.requirements.push_back({std::move(id), std::move(asked),
                                       std::move(fix), met});
    };

    // --- named macros ------------------------------------------------------
    const auto wanted = requestedMacroNames(prompt);
    if (!wanted.empty()) {
        std::string joined;
        for (size_t i = 0; i < wanted.size(); ++i) joined += (i ? ", " : "") + wanted[i];

        size_t found = 0;
        for (const auto& name : wanted)
            for (const auto& m : inst.macros)
                if (lower(m.label) == lower(name)) { ++found; break; }

        add("macros.named", "macros named " + joined, found == wanted.size(),
            "Emit exactly " + std::to_string(wanted.size()) + " entries in `macros`, "
            "labelled " + joined + ", in that order. Each needs at least two routes "
            "so it moves the sound rather than duplicating one knob.");
    }

    // --- glide -------------------------------------------------------------
    if (has(text, {"glide", "portamento", "slide between", "legato"})) {
        add("voicing.glide", "glide between notes", inst.glideMs > 0.5f,
            "Set `glide_ms` to something audible (30-120 ms for a bassline). It is "
            "currently zero, which means no glide at all. Glide only applies to "
            "\"mono\" or \"legato\" voicing, so set `voicing` accordingly.");
    }

    // --- mod wheel ---------------------------------------------------------
    if (has(text, {"mod wheel", "modwheel", "modulation wheel"})) {
        add("mod.wheel", "the mod wheel mapped to something", routedFrom(inst, "m_wheel"),
            "Add routes in `mod` with \"source\":\"m_wheel\". Without them the wheel "
            "does nothing, however the description reads.");

        // Only check the specific targets the brief actually named.
        if (has(text, {"distort", "drive", "grit", "dirt"}))
            add("mod.wheel.drive", "mod wheel increasing distortion",
                modulatesSomethingLike(inst, "m_wheel", {"drive", "shp", "drv", "sat", "tape"}),
                "Route m_wheel to a drive or shaper amount.");
        if (has(text, {"bright", "cutoff", "filter open", "opens"}))
            add("mod.wheel.bright", "mod wheel increasing brightness",
                modulatesSomethingLike(inst, "m_wheel", {"cutoff"}),
                "Route m_wheel to a filter cutoff.");
        if (has(text, {"movement", "motion", "wobble"}))
            add("mod.wheel.motion", "mod wheel increasing movement",
                modulatesSomethingLike(inst, "m_wheel", {"lfo", "seq", "amt_vib"}),
                "Route m_wheel to an LFO depth or rate.");
    }

    // --- velocity ----------------------------------------------------------
    if (has(text, {"velocity"}) && has(text, {"filter", "cutoff", "opens", "brighter"})) {
        add("mod.velocity.filter", "a filter that opens with velocity",
            modulatesSomethingLike(inst, "m_vel", {"cutoff"})
                || modulatesSomethingLike(inst, "", {"amt_fenv"}),
            "Route m_vel to a filter cutoff, directly or through amt_fenv.");
    }

    // --- pitch movement at the start of a note -----------------------------
    if (has(text, {"pitch movement", "pitch envelope", "pitch drop", "pitch dive",
                   "pitch bend at the start", "initial pitch"})) {
        add("mod.pitch", "pitch movement at the start of each note",
            modulatesSomethingLike(inst, "", {"pitch", "fine", "detune", "ratio"}),
            "Route a fast envelope (env_3 or env_4) to an oscillator pitch or fine "
            "control with a small depth, so each note starts slightly off and settles.");
    }

    // --- a specific number of macros --------------------------------------
    struct { const char* word; size_t n; } counts[] = {
        {"two macro", 2}, {"three macro", 3}, {"four macro", 4},
        {"five macro", 5}, {"six macro", 6}, {"eight macro", 8},
    };
    for (const auto& c : counts) {
        if (text.find(c.word) == std::string::npos) continue;
        add("macros.count", std::string(c.word) + " controls",
            inst.macros.size() == c.n,
            "Emit exactly " + std::to_string(c.n) + " entries in `macros`.");
        break;
    }

    // --- keep the low end mono --------------------------------------------
    //
    // The rack has no bass-mono crossover, so the honest check is the one the
    // architecture can actually satisfy: narrow output width. A requirement
    // that no patch could ever meet would just train everyone to ignore the
    // report.
    if (has(text, {"low end mono", "mono bass", "mono low", "bass in mono", "keep the low"})) {
        add("mix.monoLow", "a narrow low end",
            valueOf(inst, "wd_width", 1.0f) <= 0.55f && valueOf(inst, "v_width", 1.0f) <= 0.6f,
            "Bring `wd_width` down to 0.5 or below and keep `v_width` modest. A wide "
            "bass is what they asked you to avoid.");
    }

    // --- a named acoustic instrument must not come back a synth -------------
    if (const auto named = namedInstrument(prompt)) {
        const bool string = valueOf(inst, "str_level", 0.0f) > 0.15f;
        const bool fm     = valueOf(inst, "osc_fm_level", 0.0f) > 0.15f;
        const bool body   = valueOf(inst, "body_mix", 0.0f) > 0.05f
                         || valueOf(inst, "vow_mix", 0.0f) > 0.05f;

        bool ok = false;
        std::string fix;
        switch (named.family) {
            case Family::Plucked:
                ok  = string;
                fix = "Raise `str_level` to 0.7-0.9 and build this on the Karplus-Strong "
                      "string, with str_damp and str_bright set for the playing style. A "
                      "saw through a lowpass is a synth, not a plucked instrument.";
                break;
            case Family::Struck:
            case Family::Mallet:
                ok  = fm || string;
                fix = "Use `osc_fm_level` as the main source - FM is how you get struck "
                      "and metallic tone - and set `osc_fm_ratio` to one of the "
                      "non-integer values. That inharmonicity is the only thing that "
                      "makes struck metal sound like metal rather than like an organ.";
                break;
            case Family::Reed:
                ok  = body || fm || valueOf(inst, "osc_b_level", 0.0f) > 0.15f;
                fix = "An organ or reed is built from stacked harmonics: raise a second "
                      "oscillator at an octave or fifth, or use FM, and add a little "
                      "`body_mix` for the cabinet.";
                break;
            default:
                ok  = body || string || fm;
                fix = "Raise `body_mix` (or `vow_mix`) so the sound comes out of a "
                      "resonant body rather than straight out of an oscillator.";
                break;
        }
        add("instrument.physical", "a real " + named.word + ", not a synth", ok, fix);
    }

    // --- an electric instrument needs a speaker ------------------------------
    //
    // This is the check that the earlier one missed. A patch could pass
    // "instrument.physical" with str_level at 0.85 and still sound like a
    // synth, because a driven signal with no cabinet has harmonics running to
    // Nyquist - and that 5-20 kHz fizz, which no real amp can produce, is the
    // most reliable single tell of a modelled guitar.
    if (const auto named = namedInstrument(prompt);
        named && (named.family == Family::Plucked || named.family == Family::Struck)) {
        const bool electric = has(text, {"electric", "amp", "pickup", "overdriv", "distort",
                                         "driven", "fuzz", "crunch", "gritty", "rock",
                                         "strat", "tele", "les paul", "rhodes", "wurli"});
        if (electric) {
            const bool driven = valueOf(inst, "dr_drive", 1.0f) > 1.3f
                             || valueOf(inst, "shp_mix", 0.0f) > 0.15f;
            add("tone.cabinet", "an amp, not a DI",
                valueOf(inst, "cab_mix", 0.0f) > 0.25f || !driven,
                "Raise `cab_mix` to 0.7-1.0 with `cab_top` around 4500 (3500 for a "
                "bass). Drive with no speaker after it is fizz, not an amp, and it "
                "is the clearest reason a modelled guitar sounds synthetic.");
        }
    }

    // --- a plucked string has to decay like one ------------------------------
    //
    // The patch that prompted this had `str_decay` at 0.96 - about a thirteen
    // second ring - which is a pad, not a guitar. Getting the right module and
    // then holding the note forever is one of the surest ways to still sound
    // synthetic: real plucked strings lose most of their energy in a couple of
    // seconds, and the ear knows it.
    //
    // Checking the value rather than "did you touch the control", because the
    // rack's defaults are already sensible and a patch that leaves them alone
    // is not doing anything wrong.
    if (const auto named = namedInstrument(prompt);
        named && named.family == Family::Plucked
              && valueOf(inst, "str_level", 0.0f) > 0.15f) {

        add("tone.pluckDecay", "a string that decays like a plucked one",
            valueOf(inst, "str_decay", 0.90f) <= 0.955f,
            "Bring `str_decay` down to 0.88-0.93. Above about 0.96 the string "
            "rings for ten seconds or more, which is a pad rather than a plucked "
            "instrument - and holding a note that long is one of the surest ways "
            "to sound synthetic however good the model is.");

        // And it has to be picked somewhere a player could actually reach.
        const float pick = valueOf(inst, "str_pick", 0.14f);
        add("tone.pickPosition", "a believable picking position",
            pick <= 0.35f,
            "Set `str_pick` between 0.05 and 0.30. Nobody plucks a guitar at its "
            "exact midpoint, and doing so removes the comb of missing harmonics "
            "that makes a plucked string recognisable.");
    }

    // --- a named artist must actually be referenced --------------------------
    //
    // "It had no mention of Steve Lacy besides that being in the generation,
    // leading me to believe it didn't refer to him." Exactly right, and there
    // was no way to tell either way. If the request points at a person or a
    // record, the patch has to say what it took from them.
    if (has(text, {"like ", "style of", "vibes", "sounds like", "inspired by",
                   "-esque", "esque", "reminiscent", "in the vein"})) {
        // Only when there is a proper noun to have referenced - "like a warm
        // pad" is a simile, not a citation.
        bool properNoun = false;
        for (size_t i = 1; i < prompt.size(); ++i)
            if (std::isupper(static_cast<unsigned char>(prompt[i]))
                && std::isalpha(static_cast<unsigned char>(prompt[i - 1])) == 0
                && i > 2)
                properNoun = true;

        if (properNoun) {
            bool cited = false;
            if (auto it = inst.meta.find("references");
                it != inst.meta.end() && it->is_array() && !it->empty())
                cited = true;

            add("references.cited", "the artist or record it named to be referenced",
                cited,
                "Fill in `references` with what you actually drew on - the player, the "
                "record, the era - and one line on what you took from each. If you did "
                "not recognise the name, say so in `description` instead of implying "
                "that you did.");
        }
    }

    // --- restrained reverb -------------------------------------------------
    if (has(text, {"avoid excessive reverb", "little reverb", "not much reverb",
                   "minimal reverb", "dry", "avoid reverb"})) {
        add("fx.dryness", "restrained reverb", valueOf(inst, "rv_mix", 0.0f) <= 0.22f,
            "Bring `rv_mix` down to 0.15 or below. They asked for a dry sound.");
    }

    return report;
}

// ---------------------------------------------------------------------------

int ComplianceReport::unmetCount() const {
    int n = 0;
    for (const auto& r : requirements) if (!r.met) ++n;
    return n;
}

std::string ComplianceReport::toModelFeedback() const {
    if (allMet()) return {};

    std::ostringstream os;
    os << "The instrument you returned is close, but it does not do everything the "
          "musician explicitly asked for. These are not suggestions - each one was "
          "stated in the request:\n\n";
    int n = 0;
    for (const auto& r : requirements) {
        if (r.met) continue;
        os << ++n << ". They asked for " << r.asked << ", and the patch does not "
           << "deliver it.\n   " << r.howToFix << "\n";
    }
    os << "\nReturn the COMPLETE corrected patch with everything else unchanged. "
          "Do not describe a feature in `description` unless the patch actually "
          "implements it.\n";
    return os.str();
}

std::string ComplianceReport::toUserSummary() const {
    if (requirements.empty()) return {};
    const int total = static_cast<int>(requirements.size());
    const int met   = total - unmetCount();
    if (met == total)
        return std::to_string(total) + (total == 1 ? " specific request satisfied"
                                                   : " specific requests satisfied");

    std::ostringstream os;
    os << met << " of " << total << " specific requests satisfied - missing: ";
    int n = 0;
    for (const auto& r : requirements) {
        if (r.met) continue;
        if (n++) os << ", ";
        os << r.asked;
        if (n >= 3) { os << "..."; break; }
    }
    return os.str();
}

} // namespace forge::llm
