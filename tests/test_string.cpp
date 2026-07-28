// ---------------------------------------------------------------------------
// The plucked string, measured across the keyboard.
//
// Reported symptom: "the sound was fine at higher frequencies like C9 but
// horrible at lower end like C4 - makes no sense for a BASS."
//
// It made perfect sense. The excitation burst was a fixed 4 ms regardless of
// pitch. At C7 that is four full periods of the string, which is a healthy
// pluck. At C2, whose period is 15 ms, it is barely a quarter of one period -
// the fundamental was hardly energised at all, so the low notes came out thin
// and choppy while the high ones sounded fine.
//
// These cases measure what actually comes out, so "sounds like ass in the bass"
// becomes a number that either passes or does not.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/arch/Architecture.h"
#include "core/audio/ReferenceAnalysis.h"
#include "core/dsp/GraphBuilder.h"
#include "core/ir/IrRepair.h"
#include "core/ir/IrSafety.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <vector>

using namespace forge;
using json = nlohmann::json;

namespace {

constexpr double kSr = 48000.0;

struct Note {
    std::vector<float> left, right;
    float peak = 0.0f;
    float rms  = 0.0f;
    bool  finite = true;
};

/// Plays one note on a string-only patch and captures the output.
Note pluck(int midiNote, double seconds = 1.2, const char* extraValues = "{}") {
    auto inst = arch::buildFullArchitecture();
    ir::IrReport r;

    json patch = json::parse(R"({"voicing":"poly","polyphony":8,"values":{
        "str_level":0.9,"str_damp":0.30,"str_bright":5000,"str_decay":0.992,
        "osc_a_level":0.0,"osc_b_level":0.0,"osc_sub_level":0.0,
        "f1_cutoff":18000,"f1_reso":0.0,"body_mix":0.0,
        "env_1_a":0.5,"env_1_d":4000,"env_1_s":0.9,"env_1_r":2000,
        "rv_mix":0.0,"dl_mix":0.0,"dr_drive":1.0}})");
    // Parse ONCE. Comparing iterators from two separate temporaries is
    // undefined behaviour, and nlohmann catches it at runtime.
    const auto extra = json::parse(extraValues);
    for (auto it = extra.begin(); it != extra.end(); ++it)
        patch["values"][it.key()] = it.value();

    arch::applyPatch(inst, patch, r);
    ir::repair(inst, r);
    ir::applySafety(inst, r, 0.35f);

    Note out;
    ir::IrReport build;
    auto graph = GraphBuilder::build(inst, kSr, build);
    if (graph == nullptr) return out;

    const int blocks = static_cast<int>(kSr * seconds / 256.0);
    float l[256] = {}, rr[256] = {};
    float* chans[2] = {l, rr};

    graph->noteOn(midiNote, 0.9f);
    double sumSq = 0.0;
    for (int b = 0; b < blocks; ++b) {
        graph->process(chans, 256);
        for (int i = 0; i < 256; ++i) {
            if (!std::isfinite(l[i])) out.finite = false;
            out.left.push_back(l[i]);
            out.right.push_back(rr[i]);
            out.peak = std::max(out.peak, std::abs(l[i]));
            sumSq += static_cast<double>(l[i]) * l[i];
        }
    }
    out.rms = out.left.empty() ? 0.0f
            : static_cast<float>(std::sqrt(sumSq / static_cast<double>(out.left.size())));
    return out;
}

float midiToHz(int note) {
    return 440.0f * std::pow(2.0f, (static_cast<float>(note) - 69.0f) / 12.0f);
}

/// Exact energy at one frequency (Goertzel).
///
/// Three other methods were tried during the investigation this file came from
/// and all three lied. Autocorrelation octave-errs on harmonic-rich material;
/// zero-crossing counts are thrown off by harmonics AND by any DC offset;
/// low-pass-then-count fails when the filter has to sit near the fundamental.
/// Goertzel just answers the question asked, so it is the only measurement this
/// project should make a pitch claim from.
double energyAt(const std::vector<float>& x, size_t from, double hz, double sr) {
    if (from >= x.size()) return 0.0;
    const double w = 2.0 * 3.14159265358979 * hz / sr, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = from; i < x.size(); ++i) { const double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    return std::sqrt(std::abs(s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(x.size() - from);
}

/// True when the energy really sits at the requested note rather than an
/// octave either side of it.
bool playsTheRightNote(const Note& n, int midiNote) {
    const double f0 = midiToHz(midiNote);
    const size_t from = static_cast<size_t>(kSr * 0.1);
    if (n.left.size() <= from + 1000) return false;

    const double below = energyAt(n.left, from, f0 * 0.5, kSr);
    const double at    = energyAt(n.left, from, f0,       kSr);
    const double floorLevel = energyAt(n.left, from, f0 * 0.737, kSr);   // nothing lives here

    return at > below * 3.0 && at > floorLevel * 3.0;
}

} // namespace

// The octave error this test was written for is FIXED - see below - but one
// note still fails, for a different and much smaller reason.
//
// The original fault was not in the string at all. The stock macro row was
// offsetting parameters at its own default positions, so the "Body" macro sat
// at 0.4 with a +0.5 route to the sub oscillator and every instrument ever
// generated carried a sub-octave sine at level 0.2 - regardless of what the
// patch asked for. On a plucked string, whose own fundamental is weak, that
// buried tone WAS the loudest thing present, which is why the instrument
// sounded an octave low. Measured with Goertzel, the sub-octave went from
// E(f0/2)=0.076 to 0.00002 once macros were made neutral at their defaults.
//
// What remains: A1 (55 Hz) arrives about ten times quieter than either of its
// neighbours - E(f0)=0.000021 against 0.000396 at 41 Hz and 0.000174 at 82 Hz -
// which puts it at the measurement floor. Every other note from E1 to C6 is
// correct and healthy. Something about that particular delay length (872.7
// samples) interacts badly with the excitation; not yet isolated.
//
// Left failing on purpose. It is a real, audible weak spot in the bottom
// octave of a bass instrument.
TEST_CASE(the_string_plays_the_note_it_was_given_across_the_keyboard) {
    for (int note : {28, 33, 40, 45, 52, 60, 69, 76, 84}) {
        const auto n = pluck(note);
        CHECK(n.finite);
        CHECK(playsTheRightNote(n, note));
    }
}

TEST_CASE(the_bass_register_is_not_quieter_than_the_treble) {
    // The heart of the complaint. A four-millisecond burst excites a 15 ms
    // period barely at all, so the low notes arrived weak while the high ones
    // were full. Level must be roughly even across the range.
    const auto low  = pluck(33);   // A1, low string on a bass
    const auto mid  = pluck(52);   // E3
    const auto high = pluck(84);   // C6

    CHECK(low.peak  > 0.01f);
    CHECK(mid.peak  > 0.01f);
    CHECK(high.peak > 0.01f);

    // Peak level within a factor of three across four octaves. Measured on
    // peak rather than RMS over a fixed window, because a real string does
    // decay faster at the top of its range and RMS over a long window mostly
    // measures that, not how loud the note arrives.
    const float loudest  = std::max({low.peak, mid.peak, high.peak});
    const float quietest = std::min({low.peak, mid.peak, high.peak});
    CHECK(loudest / std::max(quietest, 1e-6f) < 3.0f);
}

TEST_CASE(a_low_note_sustains_rather_than_stuttering) {
    // "Choppy" in the bass: the voice was released while the string was still
    // ringing, because holdsVoice only covered the excitation burst.
    const auto n = pluck(33, 2.0);
    CHECK(n.finite);
    CHECK(!n.left.empty());
    if (n.left.empty()) return;

    // Energy in the second half must still be a real fraction of the first.
    const size_t half = n.left.size() / 2;
    double first = 0.0, second = 0.0;
    for (size_t i = 0; i < half; ++i)               first  += std::abs(n.left[i]);
    for (size_t i = half; i < n.left.size(); ++i)   second += std::abs(n.left[i]);
    first  /= static_cast<double>(half);
    second /= static_cast<double>(n.left.size() - half);

    CHECK(second > first * 0.08);       // still ringing, not cut dead

    // And it must not drop to silence and come back, which is what stuttering
    // actually looks like in the samples.
    int gaps = 0;
    const size_t window = 2048;
    bool wasSilent = false;
    for (size_t i = 0; i + window < n.left.size(); i += window) {
        float peak = 0.0f;
        for (size_t k = i; k < i + window; ++k) peak = std::max(peak, std::abs(n.left[k]));
        const bool silent = peak < 0.0005f;
        if (!silent && wasSilent) ++gaps;
        wasSilent = silent;
    }
    CHECK(gaps == 0);
}

TEST_CASE(damping_and_decay_do_what_their_labels_say) {
    const auto ringing = pluck(45, 1.5, R"({"str_damp":0.10,"str_decay":0.995})");
    const auto muted   = pluck(45, 1.5, R"({"str_damp":0.85,"str_decay":0.900})");

    CHECK(ringing.rms > muted.rms);      // a muted string is shorter and quieter
    CHECK(muted.finite && ringing.finite);
}

TEST_CASE(the_string_never_runs_away_however_it_is_set) {
    // A feedback loop with the decay at maximum and the filter wide open is
    // exactly where a physical model blows up.
    for (const char* extra : {R"({"str_decay":0.9995,"str_damp":0.0,"str_bright":16000})",
                              R"({"str_decay":0.9995,"str_damp":0.0,"str_bright":16000,
                                  "f1_reso":0.95,"dr_drive":6.0})"}) {
        const auto n = pluck(28, 3.0, extra);
        CHECK(n.finite);
        CHECK(n.peak <= 1.0f);
    }
}
