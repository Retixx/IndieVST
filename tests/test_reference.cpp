// ---------------------------------------------------------------------------
// Reference audio analysis.
//
// Every case here builds a signal whose correct answer is known in advance, so
// a failure means the analyser is wrong rather than that the fixture drifted.
// This matters more than usual: these measurements steer an entire generation,
// and a plausible-looking wrong number is worse than no number at all.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/audio/ReferenceAnalysis.h"
#include "core/llm/PromptBuilder.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace forge;

namespace {

constexpr double kSr = 48000.0;
constexpr float  kPi = 3.14159265358979323846f;

struct Clip {
    std::vector<float> left, right;

    audio::ReferenceFeatures analyse(bool stereo = false) const {
        const float* ptr[2] = {left.data(), right.data()};
        return audio::analyse(ptr, stereo ? 2 : 1,
                              static_cast<int>(left.size()), kSr);
    }
};

Clip tone(float hz, double seconds, int partials = 1, bool odd = false) {
    Clip c;
    const int n = static_cast<int>(kSr * seconds);
    c.left.assign(static_cast<size_t>(n), 0.0f);
    for (int p = 1; p <= partials; ++p) {
        if (odd && p % 2 == 0) continue;
        const float amp = 1.0f / static_cast<float>(p);
        for (int i = 0; i < n; ++i)
            c.left[static_cast<size_t>(i)] +=
                amp * std::sin(2.0f * kPi * hz * p * static_cast<float>(i) / static_cast<float>(kSr));
    }
    for (auto& v : c.left) v *= 0.4f;
    c.right = c.left;
    return c;
}

Clip noise(double seconds) {
    Clip c;
    const int n = static_cast<int>(kSr * seconds);
    c.left.resize(static_cast<size_t>(n));
    uint32_t state = 0x13579BDFu;
    for (int i = 0; i < n; ++i) {
        state = state * 1664525u + 1013904223u;
        c.left[static_cast<size_t>(i)] = (static_cast<float>(state >> 8) / 8388608.0f - 1.0f) * 0.4f;
    }
    c.right = c.left;
    return c;
}

/// Applies a linear attack and an exponential decay to a sustain floor.
void shape(Clip& c, float attackMs, float decayMs, float sustain) {
    const int n = static_cast<int>(c.left.size());
    const int a = static_cast<int>(attackMs * kSr / 1000.0f);
    const float dCoef = decayMs > 0.0f ? std::exp(-1.0f / (decayMs * static_cast<float>(kSr) / 1000.0f))
                                       : 0.0f;
    float level = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (i < a)   level = a > 0 ? static_cast<float>(i) / static_cast<float>(a) : 1.0f;
        else         level = sustain + (level - sustain) * dCoef;
        c.left[static_cast<size_t>(i)]  *= level;
        c.right[static_cast<size_t>(i)] *= level;
    }
}

} // namespace

TEST_CASE(reference_rejects_silence_without_pretending) {
    Clip c;
    c.left.assign(48000, 0.0f);
    c.right = c.left;
    const auto f = c.analyse();
    CHECK(!f.valid);
    CHECK(!f.problem.empty());
    CHECK(f.toPromptText().empty());     // nothing misleading reaches the model
}

TEST_CASE(reference_finds_the_pitch_of_a_known_tone) {
    // D3 = 146.83 Hz, exactly the register of the guitar motif this was built for.
    const auto f = tone(146.83f, 2.0, 8).analyse();
    CHECK(f.valid);
    CHECK(f.fundamentalHz > 144.0f);
    CHECK(f.fundamentalHz < 150.0f);
    CHECK(f.noteName == "D3");
}

TEST_CASE(reference_finds_pitch_across_the_register) {
    struct { float hz; const char* name; } cases[] = {
        {55.0f,   "A1"}, {110.0f, "A2"}, {261.63f, "C4"}, {440.0f, "A4"}, {880.0f, "A5"},
    };
    for (const auto& c : cases) {
        const auto f = tone(c.hz, 1.5, 6).analyse();
        CHECK(f.valid);
        CHECK(f.noteName == std::string(c.name));
    }
}

TEST_CASE(reference_reports_no_pitch_for_noise) {
    const auto f = noise(2.0).analyse();
    CHECK(f.valid);
    CHECK(f.fundamentalHz == 0.0f);        // guessing here would poison the design
    CHECK(f.noisiness > 0.3f);
    CHECK(f.harmonicity < 0.2f);
}

TEST_CASE(brightness_tracks_harmonic_content) {
    const auto dull   = tone(220.0f, 1.5, 2).analyse();
    const auto bright = tone(220.0f, 1.5, 24).analyse();
    CHECK(dull.valid && bright.valid);
    CHECK(bright.centroidHz > dull.centroidHz);
    CHECK(bright.rolloff85Hz > dull.rolloff85Hz);
}

TEST_CASE(odd_even_ratio_separates_square_like_from_saw_like) {
    const auto hollow = tone(220.0f, 1.5, 16, /*odd*/ true).analyse();
    const auto full   = tone(220.0f, 1.5, 16, /*odd*/ false).analyse();
    CHECK(hollow.oddEvenRatio > full.oddEvenRatio);
    CHECK(hollow.oddEvenRatio > 1.3f);     // reads as "hollow" in the summary
}

TEST_CASE(attack_measurement_matches_the_envelope_applied) {
    Clip fast = tone(220.0f, 1.5, 8);  shape(fast, 5.0f,   300.0f, 0.5f);
    Clip slow = tone(220.0f, 2.5, 8);  shape(slow, 400.0f, 300.0f, 0.5f);

    const auto f = fast.analyse();
    const auto s = slow.analyse();
    CHECK(f.attackMs < 25.0f);
    CHECK(s.attackMs > 200.0f);
    CHECK(s.attackMs > f.attackMs * 4.0f);
}

TEST_CASE(percussive_material_is_flagged_as_percussive) {
    Clip pluck = tone(220.0f, 2.0, 10);
    shape(pluck, 2.0f, 60.0f, 0.0f);              // no sustain at all
    Clip pad = tone(220.0f, 2.0, 10);
    shape(pad, 300.0f, 400.0f, 0.85f);            // held

    CHECK(pluck.analyse().percussive);
    CHECK(!pad.analyse().percussive);
    CHECK(pad.analyse().sustainLevel > 0.5f);
}

TEST_CASE(stereo_width_separates_mono_from_decorrelated) {
    Clip mono = tone(220.0f, 1.5, 8);
    const auto m = mono.analyse(true);
    CHECK(m.stereoWidth < 0.05f);

    Clip wide = tone(220.0f, 1.5, 8);
    for (auto& v : wide.right) v = -v;             // fully anti-correlated
    const auto w = wide.analyse(true);
    CHECK(w.stereoWidth > 0.9f);
}

TEST_CASE(analysis_is_independent_of_input_level) {
    // The same bounce at -18 dBFS and at -3 dBFS must describe one instrument.
    Clip loud = tone(146.83f, 1.5, 10);
    Clip quiet = loud;
    for (auto& v : quiet.left)  v *= 0.05f;
    for (auto& v : quiet.right) v *= 0.05f;

    const auto a = loud.analyse();
    const auto b = quiet.analyse();
    CHECK(a.noteName == b.noteName);
    CHECK(std::abs(a.centroidHz - b.centroidHz) < a.centroidHz * 0.02f);
    CHECK(std::abs(a.attackMs - b.attackMs) < 5.0f);
    CHECK(a.peakDb > b.peakDb);            // level itself is still reported
}

TEST_CASE(prompt_text_is_populated_and_readable) {
    Clip c = tone(146.83f, 2.0, 12);
    shape(c, 10.0f, 250.0f, 0.4f);
    const auto f = c.analyse(true);
    const auto text = f.toPromptText();

    CHECK(!text.empty());
    CHECK(text.find("D3") != std::string::npos);
    CHECK(text.find("REFERENCE RECORDING") != std::string::npos);
    CHECK(text.find("TEXT WINS") != std::string::npos);   // precedence is explicit
    CHECK(f.toSummaryLine().find("D3") != std::string::npos);
}

TEST_CASE(a_very_short_clip_still_analyses_or_declines_cleanly) {
    const auto f = tone(220.0f, 0.05, 6).analyse();      // 50 ms
    // Either it manages, or it says why - it must never claim a bogus pitch.
    if (f.valid && f.fundamentalHz > 0.0f) {
        CHECK(f.fundamentalHz > 200.0f);
        CHECK(f.fundamentalHz < 240.0f);
    }
    CHECK(f.durationSec > 0.0);
}

TEST_CASE(long_input_is_capped_rather_than_analysed_whole) {
    const auto f = tone(220.0f, 30.0, 4).analyse();
    CHECK(f.valid);
    CHECK(f.durationSec <= audio::kMaxReferenceSeconds + 0.01);
}

// ---------------------------------------------------------------------------
// Somebody PLAYING, which is what people actually drop in.
//
// The failure, verbatim: a producer dropped in a sound sample of one electric
// guitar - twelve seconds of a riff, one instrument, nothing else - and it was
// reported to the model as a FULL MIX, with its pitch, its attack and its
// harmonicity all deliberately withheld. It scored 0.82 on the mix test purely
// for being longer than one note.
//
// Nothing was wrong with the measurements. Measuring the whole twelve seconds
// as though it were a single note was wrong: the "attack" came out at 3.4
// seconds, which is where the loudest chord happened to fall.

namespace {

/// Eight plucked notes with gaps between them. One instrument, played.
Clip performance(float hz) {
    Clip c;
    const double noteSec = 0.42;
    const int    per     = static_cast<int>(kSr * noteSec);
    const float  steps[] = {0.0f, 3.0f, 5.0f, 7.0f, 5.0f, 3.0f, 0.0f, -2.0f};

    for (float semis : steps) {
        Clip one = tone(hz * std::pow(2.0f, semis / 12.0f), noteSec, 8);
        shape(one, 4.0f, 90.0f, 0.02f);          // plucked: fast in, fast out
        c.left.insert(c.left.end(), one.left.begin(), one.left.begin() + per);
    }
    c.right = c.left;
    return c;
}

} // namespace

TEST_CASE(a_played_phrase_is_not_a_full_mix) {
    const auto f = performance(146.83f).analyse(true);
    CHECK(f.valid);
    CHECK(!f.looksLikeMix);          // one instrument, however many notes
    CHECK(f.performance);
    CHECK(f.noteCount >= 6);
}

TEST_CASE(a_played_phrase_is_measured_one_note_at_a_time) {
    const auto f = performance(146.83f).analyse(true);
    REQUIRE(f.valid);

    // The measured window is ONE note, not the whole phrase.
    CHECK(f.noteLengthSec > 0.1);
    CHECK(f.noteLengthSec < 1.0);

    // ...so the attack is the attack of a pluck, not the time it took to reach
    // the loudest note of the phrase.
    test::note("attack " + std::to_string(f.attackMs) + " ms over "
               + std::to_string(f.noteLengthSec) + " s");
    CHECK(f.attackMs < 40.0f);
    CHECK(f.percussive);

    // And the pitch is one of the notes that was played, rather than nothing at
    // all - which is what a twelve-second window returns.
    CHECK(f.fundamentalHz > 130.0f);
    CHECK(f.fundamentalHz < 240.0f);
    CHECK(f.pitchConfidence > 0.5f);
}

TEST_CASE(a_played_phrase_tells_the_model_it_is_a_performance) {
    const auto text = performance(146.83f).analyse(true).toPromptText();
    CHECK(text.find("PERFORMANCE") != std::string::npos);
    CHECK(text.find("FULL MIX") == std::string::npos);
    // The measurements it was previously denied are all present.
    CHECK(text.find("attack") != std::string::npos);
    CHECK(text.find("harmonicity") != std::string::npos);
    // And it is warned off building the PART rather than the instrument.
    CHECK(text.find("Build the instrument, not the part") != std::string::npos);
}

TEST_CASE(a_single_sustained_note_is_still_measured_whole) {
    // The performance path must not capture the case it was built beside.
    Clip c = tone(146.83f, 2.0, 10);
    shape(c, 8.0f, 300.0f, 0.45f);
    const auto f = c.analyse(true);
    CHECK(!f.performance);
    CHECK(f.noteCount <= 1);
    CHECK(f.toPromptText().find("PERFORMANCE") == std::string::npos);
}

TEST_CASE(a_fundamental_with_no_energy_at_it_is_not_reported_as_the_note) {
    // Two notes a major third apart repeat at four times the period of either,
    // so the period detector answers 45 Hz on a guitar whose lowest string is
    // 82 Hz - and an instrument designed to that is built two octaves low.
    // Physics, not a detector bug; settled by measuring where the energy is.
    Clip c;
    const int n = static_cast<int>(kSr * 1.5);
    c.left.assign(static_cast<size_t>(n), 0.0f);
    for (float hz : {220.0f, 277.18f})                 // A3 and C#4
        for (int p = 1; p <= 6; ++p)
            for (int i = 0; i < n; ++i)
                c.left[static_cast<size_t>(i)] +=
                    (0.35f / static_cast<float>(p))
                    * std::sin(2.0f * kPi * hz * p * static_cast<float>(i) / (float) kSr);
    c.right = c.left;
    shape(c, 5.0f, 400.0f, 0.3f);

    const auto f = c.analyse(true);
    REQUIRE(f.valid);
    test::note("dyad fundamental " + std::to_string(f.fundamentalHz));
    // One of the notes actually played, not their difference tone.
    CHECK(f.fundamentalHz > 200.0f);
}

TEST_CASE(reference_measurements_reach_the_prompt) {
    Clip c = tone(146.83f, 2.0, 12);
    shape(c, 10.0f, 250.0f, 0.4f);
    const auto f = c.analyse(true);

    const auto without = llm::buildPatchPrompt("gritty psych-rock lead", {}, {});
    const auto with    = llm::buildPatchPrompt("gritty psych-rock lead", {}, f.toPromptText());

    // The system prompt is identical, so provider-side prompt caching still hits.
    CHECK(without.system == with.system);
    CHECK(with.user.size() > without.user.size());
    CHECK(with.user.find("D3") != std::string::npos);
    CHECK(with.user.find("gritty psych-rock lead") != std::string::npos);
    // Evidence first, request second.
    CHECK(with.user.find("REFERENCE RECORDING") < with.user.find("gritty psych-rock lead"));
}

TEST_CASE(no_reference_leaves_the_prompt_untouched) {
    const auto plain = llm::buildPatchPrompt("a warm pad");
    CHECK(plain.user.find("REFERENCE") == std::string::npos);
    CHECK(plain.user.find("a warm pad") != std::string::npos);
}

TEST_CASE(chordal_instruments_are_flagged_as_polyphonic_in_the_prompt) {
    // The bug this guards: "guitar LEAD" reads as monophonic to a language
    // model, so a riff that opens on a C# chord came back as a mono legato
    // patch and the chord collapsed to one note.
    for (const char* p : {"distorted electric guitar lead, gorgeous vibes",
                          "a Rhodes piano hook", "warm string pad",
                          "church organ melody", "bell motif"}) {
        const auto spec = llm::buildPatchPrompt(p);
        CHECK(spec.user.find("POLYPHONIC") != std::string::npos);
    }
}

TEST_CASE(genuinely_monophonic_requests_are_not_flagged) {
    for (const char* p : {"a fat 303 acid bassline", "solo flute melody",
                          "a theremin wail", "screaming monosynth lead"}) {
        const auto spec = llm::buildPatchPrompt(p);
        CHECK(spec.user.find("POLYPHONIC") == std::string::npos);
    }
}

TEST_CASE(the_voicing_note_survives_the_edit_path_too) {
    const auto spec = llm::buildPatchPrompt("make the guitar brighter", "{\"name\":\"x\"}");
    CHECK(spec.user.find("POLYPHONIC") != std::string::npos);
}

// --- a record is not a sample ------------------------------------------------
//
// The failure these guard against, verbatim: a producer dropped in a finished
// Kanye record, asked for the guitar from it, and got filtered white noise. The
// analyser was not wrong - a dense mix genuinely is spectrally flat, weakly
// pitched and heavily compressed, which is exactly how noise measures. The
// mistake was presenting those numbers as a description of one instrument.

namespace {

/// Something with the statistics of a mastered record: a chord, a bass note, a
/// noisy percussion bed, and heavy compression flattening the crest factor.
Clip fullMix(double seconds) {
    Clip c;
    const int n = static_cast<int>(kSr * seconds);
    c.left.assign(static_cast<size_t>(n), 0.0f);

    // A C# minor-ish chord: C#3, E3, G#3, plus a C#2 bass.
    for (float hz : {138.59f, 164.81f, 207.65f, 69.30f})
        for (int p = 1; p <= 8; ++p) {
            const float amp = 0.16f / static_cast<float>(p);
            for (int i = 0; i < n; ++i)
                c.left[static_cast<size_t>(i)] +=
                    amp * std::sin(2.0f * kPi * hz * p * static_cast<float>(i) / (float) kSr);
        }

    // Drums and cymbals: broadband noise with regular transients.
    uint32_t state = 0x2468ACE1u;
    for (int i = 0; i < n; ++i) {
        state = state * 1664525u + 1013904223u;
        const float white = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
        const int   pos   = i % static_cast<int>(kSr * 0.5);
        const float hit   = std::exp(-static_cast<float>(pos) / (kSr * 0.03f));
        c.left[static_cast<size_t>(i)] += white * (0.10f + 0.55f * hit);
    }

    // Mastering: soft clip everything into a small crest factor.
    for (auto& v : c.left) v = std::tanh(v * 2.6f) * 0.85f;
    c.right = c.left;
    for (size_t i = 0; i < c.right.size(); i += 3) c.right[i] *= 0.93f;   // slight width
    return c;
}

} // namespace

TEST_CASE(a_full_mix_is_recognised_as_a_full_mix) {
    const auto mix = fullMix(4.0).analyse(true);
    CHECK(mix.valid);
    CHECK(mix.looksLikeMix);
    CHECK(mix.mixLikelihood >= 0.5f);

    // ...and an isolated note is not.
    Clip note = tone(146.83f, 2.0, 10);
    shape(note, 8.0f, 300.0f, 0.45f);
    CHECK(!note.analyse().looksLikeMix);
}

TEST_CASE(a_mix_never_tells_the_model_it_sounds_like_noise) {
    // This is the whole bug in one assertion.
    const auto text = fullMix(4.0).analyse(true).toPromptText();

    CHECK(!text.empty());
    CHECK(text.find("FULL MIX") != std::string::npos);

    // The measurements that make a mix look like noise are never QUOTED - the
    // measurement lines themselves must be absent. The prose may still name
    // them, because explaining why they were withheld is the point.
    CHECK(text.find("  harmonicity  ") == std::string::npos);
    CHECK(text.find("  noisiness    ") == std::string::npos);
    CHECK(text.find("  attack       ") == std::string::npos);
    CHECK(text.find("  odd/even     ") == std::string::npos);
    CHECK(text.find("DELIBERATELY OMITTED") != std::string::npos);

    // And the trap is called out by name.
    CHECK(text.find("noise oscillator") != std::string::npos);
}

TEST_CASE(an_isolated_sound_still_gets_the_full_measurements) {
    Clip note = tone(146.83f, 2.0, 12);
    shape(note, 10.0f, 250.0f, 0.4f);
    const auto text = note.analyse(true).toPromptText();

    CHECK(text.find("harmonicity") != std::string::npos);
    CHECK(text.find("attack") != std::string::npos);
    CHECK(text.find("D3") != std::string::npos);
    CHECK(text.find("FULL MIX") == std::string::npos);
}

TEST_CASE(chroma_finds_the_key_centres_a_single_pitch_detector_misses) {
    // No single fundamental dominates a chord over a drum bed, but the pitch
    // classes survive - and C# is exactly what the musician said the riff was.
    const auto mix = fullMix(4.0).analyse(true);
    CHECK(!mix.pitchClasses.empty());

    bool foundCSharp = false;
    for (const auto& pc : mix.pitchClasses) if (pc == "C#") foundCSharp = true;
    CHECK(foundCSharp);
}

TEST_CASE(chroma_is_right_for_a_plain_chord) {
    Clip chord;
    const int n = static_cast<int>(kSr * 2.0);
    chord.left.assign(static_cast<size_t>(n), 0.0f);
    for (float hz : {261.63f, 329.63f, 392.00f})            // C major
        for (int p = 1; p <= 6; ++p)
            for (int i = 0; i < n; ++i)
                chord.left[static_cast<size_t>(i)] +=
                    (0.2f / p) * std::sin(2.0f * kPi * hz * p * static_cast<float>(i) / (float) kSr);
    chord.right = chord.left;

    const auto f = chord.analyse();
    CHECK(!f.pitchClasses.empty());
    CHECK(f.pitchClasses[0] == "C" || f.pitchClasses[0] == "G" || f.pitchClasses[0] == "E");
}

TEST_CASE(a_pitched_request_with_a_reference_is_told_not_to_use_noise) {
    const auto mixText = fullMix(4.0).analyse(true).toPromptText();

    for (const char* p : {"the distorted guitar from Kanye's Gorgeous",
                          "a warm rhodes chord", "cello melody", "deep sub bass"}) {
        const auto spec = llm::buildPatchPrompt(p, {}, mixText);
        CHECK(spec.user.find("PITCHED source") != std::string::npos);
        CHECK(spec.user.find("noise oscillator at or near zero") != std::string::npos);
    }

    // Genuinely unpitched requests are left alone - noise is the right answer.
    for (const char* p : {"ocean waves", "a cymbal wash", "wind through a canyon"}) {
        const auto spec = llm::buildPatchPrompt(p, {}, mixText);
        CHECK(spec.user.find("PITCHED source") == std::string::npos);
    }
}

TEST_CASE(the_guard_only_applies_when_a_reference_was_supplied) {
    const auto spec = llm::buildPatchPrompt("distorted guitar lead", {}, {});
    CHECK(spec.user.find("PITCHED source") == std::string::npos);
}
