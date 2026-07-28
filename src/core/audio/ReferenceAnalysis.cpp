#include "core/audio/ReferenceAnalysis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numeric>
#include <sstream>

namespace forge::audio {
namespace {

constexpr float  kPi      = 3.14159265358979323846f;
constexpr int    kFftSize = 4096;
constexpr double kSilence = 1e-6;

// --- a small radix-2 FFT ----------------------------------------------------
// Hand-written for the same reason every other DSP primitive here is: the core
// must not depend on a framework, and this runs once per dropped file, so the
// simplest correct implementation is the right one.
void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    if (n <= 1) return;

    for (size_t i = 1, j = 0; i < n; ++i) {           // bit reversal
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * kPi / static_cast<float>(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k]             = u + v;
                a[i + k + len / 2]   = u - v;
                w *= wl;
            }
        }
    }
}

float dbOf(float linear) {
    return linear <= 1e-9f ? -120.0f : 20.0f * std::log10(linear);
}

std::string noteNameFor(float hz, float& centsOut) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F",
                                  "F#", "G", "G#", "A", "A#", "B"};
    if (hz <= 0.0f) { centsOut = 0.0f; return {}; }
    const double midi   = 69.0 + 12.0 * std::log2(static_cast<double>(hz) / 440.0);
    const long   rounded = std::lround(midi);
    centsOut = static_cast<float>((midi - static_cast<double>(rounded)) * 100.0);
    const long note = ((rounded % 12) + 12) % 12;
    const long oct  = rounded / 12 - 1;
    return std::string(names[note]) + std::to_string(oct);
}

/// Pitch by the YIN difference function.
///
/// This started as plain autocorrelation with a "shortest peak that is nearly
/// as good" rule, and it octave-erred on anything harmonically rich: a sawtooth
/// correlates about 0.90 at its true period and about 1.00 at twice it, so the
/// true peak fell below the bar and the reading came back an octave flat. That
/// is the single most damaging way this can fail, because the whole instrument
/// then gets designed in the wrong register.
///
/// YIN exists precisely for that problem. The cumulative mean normalisation
/// divides each candidate by the average of all shorter candidates, which
/// suppresses the multiples of the true period instead of letting them win, and
/// the absolute threshold means the FIRST good period is taken rather than the
/// best-scoring one.
float detectFundamental(const std::vector<float>& mono, double sampleRate, float& confidence) {
    confidence = 0.0f;
    const int minLag = std::max(2,  static_cast<int>(sampleRate / 1600.0));  // 1600 Hz ceiling
    const int maxLag = std::max(16, static_cast<int>(sampleRate / 40.0));    //   40 Hz floor
    const int frame  = std::max(1024, static_cast<int>(sampleRate * 0.060));

    if (static_cast<int>(mono.size()) < frame + maxLag + 1) return 0.0f;

    // Loudest position that still leaves room for the longest lag.
    const int limit = static_cast<int>(mono.size()) - (frame + maxLag);
    int    bestStart  = 0;
    double bestEnergy = -1.0;
    for (int start = 0; start <= limit; start += std::max(1, frame / 2)) {
        double e = 0.0;
        for (int i = 0; i < frame; ++i) {
            const double v = mono[static_cast<size_t>(start + i)];
            e += v * v;
        }
        if (e > bestEnergy) { bestEnergy = e; bestStart = start; }
    }
    if (bestEnergy < kSilence) return 0.0f;

    // --- difference function ------------------------------------------------
    std::vector<double> d(static_cast<size_t>(maxLag) + 1, 0.0);
    for (int lag = 1; lag <= maxLag; ++lag) {
        double sum = 0.0;
        for (int i = 0; i < frame; ++i) {
            const double delta = mono[static_cast<size_t>(bestStart + i)]
                               - mono[static_cast<size_t>(bestStart + i + lag)];
            sum += delta * delta;
        }
        d[static_cast<size_t>(lag)] = sum;
    }

    // --- cumulative mean normalisation --------------------------------------
    std::vector<double> dn(static_cast<size_t>(maxLag) + 1, 1.0);
    double running = 0.0;
    for (int lag = 1; lag <= maxLag; ++lag) {
        running += d[static_cast<size_t>(lag)];
        dn[static_cast<size_t>(lag)] = running > 1e-12
            ? d[static_cast<size_t>(lag)] * static_cast<double>(lag) / running
            : 1.0;
    }

    // --- first candidate below the absolute threshold -----------------------
    constexpr double kThreshold = 0.15;
    int chosen = 0;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        if (dn[static_cast<size_t>(lag)] >= kThreshold) continue;
        // Walk to the bottom of this dip rather than stopping at its edge.
        while (lag + 1 <= maxLag
               && dn[static_cast<size_t>(lag + 1)] < dn[static_cast<size_t>(lag)])
            ++lag;
        chosen = lag;
        break;
    }
    if (chosen == 0) {
        // Nothing crossed the threshold. Take the global minimum, but only if
        // it is convincing - otherwise this is noise and we say so.
        double lowest = 1e18;
        for (int lag = minLag; lag <= maxLag; ++lag)
            if (dn[static_cast<size_t>(lag)] < lowest) {
                lowest = dn[static_cast<size_t>(lag)];
                chosen = lag;
            }
        if (chosen == 0 || lowest > 0.55) return 0.0f;
    }

    // --- parabolic refinement ------------------------------------------------
    double refined = chosen;
    if (chosen > minLag && chosen < maxLag) {
        const double y0 = d[static_cast<size_t>(chosen - 1)];
        const double y1 = d[static_cast<size_t>(chosen)];
        const double y2 = d[static_cast<size_t>(chosen + 1)];
        const double denom = 2.0 * (2.0 * y1 - y0 - y2);
        if (std::abs(denom) > 1e-12) {
            const double shift = (y2 - y0) / denom;
            if (std::abs(shift) < 1.0) refined = chosen + shift;
        }
    }

    confidence = static_cast<float>(std::clamp(1.0 - dn[static_cast<size_t>(chosen)], 0.0, 1.0));
    return static_cast<float>(sampleRate / refined);
}

/// Exact energy at one frequency, by Goertzel. No interpretation, no window
/// choice, no bin to fall between - the one measurement in this project that
/// has never lied.
double energyAtHz(const std::vector<float>& x, double sampleRate, double hz) {
    if (x.empty() || hz <= 0.0 || hz >= sampleRate * 0.5) return 0.0;
    const double w = 2.0 * kPi * hz / sampleRate, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (float v : x) { const double s = v + c * s1 - s2; s2 = s1; s1 = s; }
    return std::sqrt(std::abs(s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(x.size());
}

/// YIN reports the period of the WAVEFORM, and that is not always the period of
/// a note. Two notes a fifth apart repeat at twice the period of either; a
/// major third at four times. So a guitar playing a two-note shape comes back
/// an octave and a half BELOW anything on the instrument - 44.7 Hz on a
/// recording whose actual notes were near 179 and 224 Hz - and a synthesiser
/// designed to that measurement is built in the wrong register entirely.
///
/// Nothing is wrong with the detector: the question was ambiguous. Settle it
/// the way everything else here is settled, by measuring. If the reported
/// fundamental has almost no energy at it and one of its low multiples has
/// plenty, that multiple is the note somebody played.
float resolveFundamental(const std::vector<float>& x, double sampleRate, float hz,
                         bool& chordal) {
    chordal = false;
    if (hz <= 0.0f) return hz;

    double best = 0.0;
    int    bestK = 1;
    for (int k = 1; k <= 6; ++k) {
        const double f = hz * k;
        if (f >= sampleRate * 0.4 || f > 2000.0) break;
        const double e = energyAtHz(x, sampleRate, f);
        if (e > best) { best = e; bestK = k; }
    }
    if (bestK == 1 || best <= 0.0) return hz;

    // A real fundamental need not be the loudest partial - on a bridge-picked
    // string it usually is not - but it is never a twelfth of one.
    if (energyAtHz(x, sampleRate, hz) >= best * 0.25) return hz;

    // Doubling and quadrupling is an octave error. Anything else means the
    // detector locked onto the repeat rate of several notes at once.
    chordal = (bestK != 2 && bestK != 4);
    return static_cast<float>(hz * bestK);
}

/// Energy per pitch class, folded across every octave.
///
/// Unlike a single fundamental, chroma survives a dense mix: the guitar, bass
/// and keys of a record all reinforce the same pitch classes even when no one
/// fundamental dominates. On a full record this is the ONLY pitch information
/// worth reporting.
std::array<float, 12> chromaOf(const std::vector<float>& mag, double binHz, double sampleRate) {
    std::array<float, 12> chroma{};
    chroma.fill(0.0f);
    if (binHz <= 0.0) return chroma;

    for (size_t b = 1; b < mag.size(); ++b) {
        const double hz = b * binHz;
        // Below 55 Hz the bins are too coarse to place, and above ~5 kHz the
        // partials of everything smear together into no useful pitch at all.
        if (hz < 55.0 || hz > std::min(5000.0, sampleRate * 0.45)) continue;
        const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
        const int    pc   = static_cast<int>(((std::lround(midi) % 12) + 12) % 12);
        // Weight by magnitude, not power: power lets one loud low note bury
        // everything else, and we want the harmonic content of the whole span.
        chroma[static_cast<size_t>(pc)] += mag[b];
    }
    float total = 0.0f;
    for (float v : chroma) total += v;
    if (total > 0.0f) for (auto& v : chroma) v /= total;
    return chroma;
}

const char* pitchClassName(int pc) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F",
                                  "F#", "G", "G#", "A", "A#", "B"};
    return names[((pc % 12) + 12) % 12];
}

struct Spectrum {
    std::vector<float> mag;      ///< averaged magnitude, bins 0..kFftSize/2
    double             binHz = 0.0;
    bool               ok = false;
};

Spectrum averageSpectrum(const std::vector<float>& mono, double sampleRate) {
    Spectrum out;
    out.binHz = sampleRate / kFftSize;
    if (static_cast<int>(mono.size()) < kFftSize) return out;

    out.mag.assign(kFftSize / 2 + 1, 0.0f);
    std::vector<float> window(kFftSize);
    for (int i = 0; i < kFftSize; ++i)                       // Hann
        window[static_cast<size_t>(i)] =
            0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) / (kFftSize - 1)));

    const int hop    = kFftSize / 2;
    int       frames = 0;
    std::vector<std::complex<float>> buf(kFftSize);
    for (size_t start = 0; start + kFftSize <= mono.size(); start += static_cast<size_t>(hop)) {
        double energy = 0.0;
        for (int i = 0; i < kFftSize; ++i) {
            const float v = mono[start + static_cast<size_t>(i)];
            energy += static_cast<double>(v) * v;
        }
        if (energy < kSilence) continue;                     // skip silence
        for (int i = 0; i < kFftSize; ++i)
            buf[static_cast<size_t>(i)] = {mono[start + static_cast<size_t>(i)]
                                           * window[static_cast<size_t>(i)], 0.0f};
        fft(buf);
        for (size_t b = 0; b < out.mag.size(); ++b) out.mag[b] += std::abs(buf[b]);
        ++frames;
        if (frames >= 64) break;
    }
    if (frames == 0) return out;
    for (auto& m : out.mag) m /= static_cast<float>(frames);
    out.ok = true;
    return out;
}

/// Amplitude envelope at ~1 ms resolution, used for attack/decay/sustain.
std::vector<float> envelopeOf(const std::vector<float>& mono, double sampleRate, int& stepSamples) {
    stepSamples = std::max(1, static_cast<int>(sampleRate / 1000.0));
    std::vector<float> env;
    env.reserve(mono.size() / static_cast<size_t>(stepSamples) + 1);
    for (size_t i = 0; i < mono.size(); i += static_cast<size_t>(stepSamples)) {
        const size_t end = std::min(mono.size(), i + static_cast<size_t>(stepSamples));
        float peak = 0.0f;
        for (size_t k = i; k < end; ++k) peak = std::max(peak, std::abs(mono[k]));
        env.push_back(peak);
    }
    return env;
}

/// Where separate notes start, as indices into a ~1 ms envelope.
///
/// Deliberately conservative: it is looking for "somebody played another note",
/// not for every ghost note in a strum. A rise of 5 dB or so over the last
/// 60 ms, above a fraction of the loudest thing in the file, no closer than
/// 120 ms to the previous one.
std::vector<size_t> detectOnsets(const std::vector<float>& env, double msPerStep) {
    std::vector<size_t> onsets;
    if (env.size() < 8 || msPerStep <= 0.0) return onsets;

    const auto steps = [&](double ms) {
        return static_cast<size_t>(std::max(1.0, ms / msPerStep));
    };
    const size_t look  = steps(10.0);    // short-term level
    const size_t back  = steps(60.0);    // what it was before
    const size_t space = steps(120.0);   // minimum gap between notes

    float peak = 0.0f;
    for (float v : env) peak = std::max(peak, v);
    if (peak <= 0.0f) return onsets;
    const float floorLevel = peak * 0.16f;

    auto shortTerm = [&](size_t i) {
        float m = 0.0f;
        for (size_t k = i; k < std::min(env.size(), i + look); ++k) m = std::max(m, env[k]);
        return m;
    };

    size_t last = 0;
    bool   have = false;
    for (size_t i = 0; i < env.size(); ++i) {
        const float now = shortTerm(i);
        if (now < floorLevel) continue;
        const float before = i >= back ? shortTerm(i - back) : 0.0f;
        // The very first sound in the file is an onset by definition.
        const bool rises = (i < back) ? (i == 0 || env[i - 1] < floorLevel)
                                      : (now > before * 1.8f);
        if (!rises) continue;
        if (have && i - last < space) continue;
        onsets.push_back(i);
        last = i;
        have = true;
    }
    return onsets;
}

std::string fmt(const char* pattern, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, pattern, v);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------

ReferenceFeatures analyse(const float* const* samples, int numChannels,
                          int numFrames, double sampleRate) {
    ReferenceFeatures f;
    f.sampleRate = sampleRate;
    f.channels   = numChannels;

    if (samples == nullptr || numChannels <= 0 || numFrames <= 0 || sampleRate <= 0.0) {
        f.problem = "The file contained no audio.";
        return f;
    }

    const int frames = static_cast<int>(std::min<double>(numFrames, sampleRate * kMaxReferenceSeconds));
    f.durationSec = frames / sampleRate;

    // --- stereo width ------------------------------------------------------
    // Measured first, from the channels themselves. An anti-correlated stereo
    // file downmixes to digital silence, and computing width after the downmix
    // meant the widest possible input was reported as an empty one.
    if (numChannels >= 2) {
        double sl = 0.0, sr = 0.0, slr = 0.0;
        for (int i = 0; i < frames; ++i) {
            const double l = samples[0][i], r = samples[1][i];
            sl += l * l; sr += r * r; slr += l * r;
        }
        const double denom = std::sqrt(sl * sr);
        const double correlation = denom > kSilence ? slr / denom : 1.0;
        f.stereoWidth = static_cast<float>(std::clamp(1.0 - correlation, 0.0, 1.0));
    }

    // --- downmix + levels --------------------------------------------------
    std::vector<float> mono(static_cast<size_t>(frames), 0.0f);
    double sumSq = 0.0, peak = 0.0;
    for (int i = 0; i < frames; ++i) {
        float acc = 0.0f;
        for (int c = 0; c < numChannels; ++c) acc += samples[c][i];
        acc /= static_cast<float>(numChannels);
        mono[static_cast<size_t>(i)] = acc;
        sumSq += static_cast<double>(acc) * acc;
        peak = std::max(peak, static_cast<double>(std::abs(acc)));
    }
    const double rms = std::sqrt(sumSq / frames);
    f.peakDb = dbOf(static_cast<float>(peak));
    f.rmsDb  = dbOf(static_cast<float>(rms));
    f.crestDb = f.peakDb - f.rmsDb;

    if (peak < 1e-4 && numChannels >= 2) {
        // The sum cancelled but the channels are not empty - an out-of-phase
        // pair. Analyse one side rather than declaring the file silent.
        double sidePeak = 0.0;
        for (int i = 0; i < frames; ++i) sidePeak = std::max(sidePeak, std::abs((double) samples[0][i]));
        if (sidePeak > 1e-4) {
            sumSq = 0.0;
            for (int i = 0; i < frames; ++i) {
                mono[static_cast<size_t>(i)] = samples[0][i];
                sumSq += static_cast<double>(samples[0][i]) * samples[0][i];
            }
            peak = sidePeak;
            f.peakDb  = dbOf(static_cast<float>(peak));
            f.rmsDb   = dbOf(static_cast<float>(std::sqrt(sumSq / frames)));
            f.crestDb = f.peakDb - f.rmsDb;
        }
    }
    if (peak < 1e-4) {
        f.problem = "The file is silent, or far too quiet to analyse.";
        return f;
    }

    // Normalise so every measurement below is level-independent. Someone's
    // bounce at -18 dBFS must describe the same instrument as the same bounce
    // at -3 dBFS.
    const float gain = static_cast<float>(1.0 / peak);
    for (auto& v : mono) v *= gain;

    // --- is this one note, or somebody playing? ----------------------------
    //
    // Everything below - pitch, brightness, harmonicity, attack, decay - only
    // means anything if it is measuring ONE note. Run over a twelve-second riff
    // it describes no note at all: the "attack" is however long it took to
    // reach the loudest chord, the "decay" is the gap between phrases, and the
    // pitch detector finds nothing because there are a dozen pitches in there.
    //
    // So find the notes, pick a clean one, and measure that. The instrument is
    // the same either way, and a real recording of somebody playing is what
    // people actually have to hand.
    int stepSamples = 1;
    const auto fullEnv  = envelopeOf(mono, sampleRate, stepSamples);
    const double msPerStep = stepSamples * 1000.0 / sampleRate;
    const auto onsets   = detectOnsets(fullEnv, msPerStep);
    f.noteCount = static_cast<int>(onsets.size());

    size_t noteFromEnv = 0, noteToEnv = fullEnv.size();
    if (onsets.size() >= 2) {
        const size_t minLen = static_cast<size_t>(std::max(1.0, 220.0 / msPerStep));
        const size_t maxLen = static_cast<size_t>(std::max(1.0, 2500.0 / msPerStep));

        // Candidates, loudest first, so the ones actually worth the pitch
        // detector's time get looked at when there are thirty of them.
        struct Candidate { size_t start, end; float amp; double isolation; };
        std::vector<Candidate> candidates;
        for (size_t k = 0; k < onsets.size(); ++k) {
            const size_t start = onsets[k];
            const size_t next  = (k + 1 < onsets.size()) ? onsets[k + 1] : fullEnv.size();
            if (next - start < minLen) continue;
            Candidate c;
            c.start = start;
            c.end   = std::min(next, start + maxLen);
            c.amp   = 0.0f;
            for (size_t i = c.start; i < c.end; ++i) c.amp = std::max(c.amp, fullEnv[i]);
            c.isolation = std::min(1.0, static_cast<double>(next - start)
                                          / static_cast<double>(maxLen));
            candidates.push_back(c);
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.amp > b.amp; });

        // Twelve is plenty to find a clean note, and the pitch detector is
        // O(frame x maxLag) per call - this runs once, on a dropped file, but
        // it still runs while somebody is waiting to see the result.
        const size_t examine = std::min<size_t>(candidates.size(), 12);
        double bestScore = -1.0, confSum = 0.0;
        int    confCount = 0;
        for (size_t k = 0; k < examine; ++k) {
            const auto& c = candidates[k];
            const size_t a = c.start * static_cast<size_t>(stepSamples);
            const size_t b = std::min(mono.size(), c.end * static_cast<size_t>(stepSamples));
            if (b <= a) continue;
            std::vector<float> seg(mono.begin() + static_cast<long>(a),
                                   mono.begin() + static_cast<long>(b));
            float conf = 0.0f;
            const float raw = detectFundamental(seg, sampleRate, conf);
            confSum += conf;
            ++confCount;
            bool chordal = false;
            resolveFundamental(seg, sampleRate, raw, chordal);

            // Loud, given room to ring, actually pitched, and preferably ONE
            // note. Picking purely by level chose the biggest transient in the
            // clip, which on a guitar recording was a scrape with no note in it
            // at all, and the model was told the reference was "unpitched".
            const double score = static_cast<double>(c.amp)
                               * (0.5 + 0.5 * c.isolation)
                               * (0.30 + 0.70 * static_cast<double>(conf))
                               * (chordal ? 0.55 : 1.0);
            if (score > bestScore) { bestScore = score; noteFromEnv = c.start; noteToEnv = c.end; }
        }
        // Averaged over the notes, this is the honest answer to "is one
        // instrument playing here?" - far better than asking the whole clip,
        // where the answer is always no, and better than the spectral flatness
        // that made a mix and a distorted guitar look identical.
        if (confCount > 0) f.noteConfidence = static_cast<float>(confSum / confCount);
        f.performance = bestScore > 0.0;
        if (!f.performance) { noteFromEnv = 0; noteToEnv = fullEnv.size(); }
    }

    const size_t noteFrom = std::min(mono.size(), noteFromEnv * static_cast<size_t>(stepSamples));
    const size_t noteTo   = std::min(mono.size(), noteToEnv   * static_cast<size_t>(stepSamples));
    const std::vector<float> note(mono.begin() + static_cast<long>(noteFrom),
                                  mono.begin() + static_cast<long>(noteTo));
    f.noteStartSec  = static_cast<double>(noteFrom) / sampleRate;
    f.noteLengthSec = static_cast<double>(note.size()) / sampleRate;

    // --- pitch -------------------------------------------------------------
    float pitchConfidence = 0.0f;
    f.fundamentalHz = detectFundamental(note, sampleRate, pitchConfidence);
    bool chordalNote = false;
    f.fundamentalHz = resolveFundamental(note, sampleRate, f.fundamentalHz, chordalNote);
    f.pitchConfidence = pitchConfidence;
    if (f.fundamentalHz > 0.0f) f.noteName = noteNameFor(f.fundamentalHz, f.centsOffset);
    if (!f.performance) f.noteConfidence = pitchConfidence;

    // --- spectrum ----------------------------------------------------------
    const auto spec = averageSpectrum(note, sampleRate);
    if (spec.ok) {
        double total = 0.0, weighted = 0.0;
        double low = 0.0, mid = 0.0, high = 0.0;
        double logSum = 0.0;
        int    counted = 0;
        for (size_t b = 1; b < spec.mag.size(); ++b) {
            const double hz  = b * spec.binHz;
            if (hz > sampleRate * 0.48) break;
            const double m = spec.mag[b];
            total    += m;
            weighted += m * hz;
            if      (hz < 250.0)  low  += m;
            else if (hz < 2000.0) mid  += m;
            else                  high += m;
            logSum += std::log(static_cast<double>(m) + 1e-12);
            ++counted;
        }
        if (total > kSilence) {
            f.centroidHz = static_cast<float>(weighted / total);
            f.lowEnergy  = static_cast<float>(low  / total);
            f.midEnergy  = static_cast<float>(mid  / total);
            f.highEnergy = static_cast<float>(high / total);

            double running = 0.0;
            for (size_t b = 1; b < spec.mag.size(); ++b) {
                running += spec.mag[b];
                if (running >= total * 0.85) { f.rolloff85Hz = static_cast<float>(b * spec.binHz); break; }
            }
            // Spectral flatness: geometric mean over arithmetic mean. Hiss
            // approaches 1, a pure tone approaches 0.
            const double geo  = std::exp(logSum / std::max(1, counted));
            const double arith = total / std::max(1, counted);
            f.noisiness = static_cast<float>(std::clamp(geo / (arith + 1e-12), 0.0, 1.0));

            // Tilt: least squares of dB against log2(Hz), which is what a
            // producer actually hears as "dark" or "bright".
            double n = 0.0, sx = 0.0, sy = 0.0, sxy = 0.0, sxx = 0.0;
            for (size_t b = 1; b < spec.mag.size(); ++b) {
                const double hz = b * spec.binHz;
                if (hz < 50.0 || hz > std::min(16000.0, sampleRate * 0.45)) continue;
                const double xv = std::log2(hz);
                const double yv = 20.0 * std::log10(static_cast<double>(spec.mag[b]) + 1e-9);
                n += 1.0; sx += xv; sy += yv; sxy += xv * yv; sxx += xv * xv;
            }
            if (n > 2.0) {
                const double denom = n * sxx - sx * sx;
                if (std::abs(denom) > 1e-9)
                    f.tiltDbPerOct = static_cast<float>((n * sxy - sx * sy) / denom);
            }
        }

        // --- harmonic structure -------------------------------------------
        if (f.fundamentalHz > 20.0f && total > kSilence) {
            double harmonic = 0.0, odd = 0.0, even = 0.0, drift = 0.0;
            int    found = 0;
            for (int h = 1; h <= 16; ++h) {
                const double target = f.fundamentalHz * h;
                if (target > sampleRate * 0.45) break;
                // Search a window around the ideal partial and take the peak;
                // the offset of that peak IS the inharmonicity.
                const int centre = static_cast<int>(std::lround(target / spec.binHz));
                const int span   = std::max(2, static_cast<int>(centre * 0.03));
                int   bestBin = centre;
                float bestMag = 0.0f;
                for (int b = centre - span; b <= centre + span; ++b) {
                    if (b < 1 || b >= static_cast<int>(spec.mag.size())) continue;
                    if (spec.mag[static_cast<size_t>(b)] > bestMag) {
                        bestMag = spec.mag[static_cast<size_t>(b)];
                        bestBin = b;
                    }
                }
                harmonic += bestMag;
                if (h % 2 == 1) odd += bestMag; else even += bestMag;
                if (bestMag > 0.0f && target > 0.0) {
                    drift += std::abs(bestBin * spec.binHz - target) / target;
                    ++found;
                }
            }
            f.harmonicity   = static_cast<float>(std::clamp(harmonic / total, 0.0, 1.0))
                            * std::clamp(pitchConfidence, 0.0f, 1.0f);
            f.inharmonicity = found > 0 ? static_cast<float>(drift / found) : 0.0f;
            f.oddEvenRatio  = even > kSilence ? static_cast<float>(odd / even) : 0.0f;
        }
    }

    // --- pitch classes and what kind of recording this is -------------------
    //
    // Chroma is the one measurement that wants the WHOLE recording: a riff
    // spells out its key over several bars, and the single note measured above
    // only ever names itself.
    const Spectrum wide = f.performance ? averageSpectrum(mono, sampleRate) : spec;
    if (wide.ok) {
        const auto chroma = chromaOf(wide.mag, wide.binHz, sampleRate);

        std::array<int, 12> order{};
        for (int i = 0; i < 12; ++i) order[static_cast<size_t>(i)] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return chroma[static_cast<size_t>(a)] > chroma[static_cast<size_t>(b)];
        });
        // Anything meaningfully above a flat 1/12 distribution counts as
        // present. A perfectly flat chroma means no tonal centre at all.
        for (int i = 0; i < 4; ++i) {
            const int pc = order[static_cast<size_t>(i)];
            if (chroma[static_cast<size_t>(pc)] > 0.115f)
                f.pitchClasses.push_back(pitchClassName(pc));
        }
        // How concentrated is the tonal energy? One note concentrates in one or
        // two classes; a chord in three or four; a full mix spreads everywhere.
        float top3 = 0.0f;
        for (int i = 0; i < 3; ++i) top3 += chroma[static_cast<size_t>(order[static_cast<size_t>(i)])];
        f.polyphonic = chordalNote
                    || (!f.performance && f.pitchClasses.size() >= 3 && f.pitchConfidence < 0.85f);

        // --- is this an isolated instrument, or a record? -------------------
        //
        // The previous test asked whether the WHOLE CLIP had one confident
        // fundamental and sat in one part of the spectrum, and a person playing
        // a guitar for twelve seconds fails both of those as badly as a Kanye
        // record does. A real dropped sample of one electric guitar scored 0.82
        // and was handed to the model as a full mix with its attack, its
        // harmonicity and its pitch all withheld.
        //
        // What actually separates them is per-NOTE behaviour. Ask each note
        // whether it has a pitch. One instrument answers yes even while the
        // clip as a whole answers no; a mix answers no at every scale, because
        // there is a drum kit and three other parts inside every window.
        float score = 0.0f;
        if (f.noteConfidence < 0.55f)   score += 0.40f;
        else if (f.noteConfidence < 0.68f) score += 0.15f;
        // Broadband noise everywhere is drums and cymbals, and it is the one
        // spectral measure a solo instrument does not produce.
        if (f.noisiness > 0.30f)        score += 0.25f;
        // Tonal energy smeared across every pitch class.
        if (top3 < 0.42f)               score += 0.20f;
        // Mastered and squashed: no single instrument recording measures this
        // flat.
        if (f.crestDb < 9.0f)           score += 0.25f;
        f.mixLikelihood = std::clamp(score, 0.0f, 1.0f);
        f.looksLikeMix  = f.mixLikelihood >= 0.5f;
        // A recording that resolves into separate notes with pitches is a
        // performance, not a record, whatever else it measures like.
        if (f.performance && f.noteConfidence >= 0.62f && f.noisiness < 0.30f)
            f.looksLikeMix = false;
    }

    // --- envelope ----------------------------------------------------------
    //
    // Of the measured NOTE. Attack and decay are properties of how the
    // instrument is played, and taken across a whole performance they measure
    // the arrangement instead: a riff came back with a 3.4 second "attack",
    // which is simply where its loudest chord happened to land.
    const std::vector<float> env(fullEnv.begin() + static_cast<long>(noteFromEnv),
                                 fullEnv.begin() + static_cast<long>(noteToEnv));
    if (env.size() > 4) {
        const auto   peakIt    = std::max_element(env.begin(), env.end());
        const size_t peakIdx   = static_cast<size_t>(std::distance(env.begin(), peakIt));
        const float  peakVal   = *peakIt;

        size_t tenIdx = 0, ninetyIdx = peakIdx;
        for (size_t i = 0; i <= peakIdx; ++i) if (env[i] >= peakVal * 0.1f) { tenIdx = i; break; }
        for (size_t i = tenIdx; i <= peakIdx; ++i) if (env[i] >= peakVal * 0.9f) { ninetyIdx = i; break; }
        f.attackMs = static_cast<float>((ninetyIdx - tenIdx) * msPerStep);

        // Sustain: the median of the middle of the tail, which ignores both the
        // transient and the fade-out.
        if (peakIdx + 4 < env.size()) {
            std::vector<float> tail(env.begin() + static_cast<long>(peakIdx) + 1, env.end());
            const size_t a = tail.size() / 4, b = std::max(a + 1, tail.size() * 3 / 4);
            std::vector<float> middle(tail.begin() + static_cast<long>(a),
                                      tail.begin() + static_cast<long>(std::min(b, tail.size())));
            if (!middle.empty()) {
                std::nth_element(middle.begin(), middle.begin() + static_cast<long>(middle.size() / 2),
                                 middle.end());
                f.sustainLevel = peakVal > 0.0f ? middle[middle.size() / 2] / peakVal : 0.0f;
            }
            // Decay: peak down to within 10% of that sustain level.
            const float threshold = peakVal * std::max(f.sustainLevel, 0.02f) * 1.1f;
            for (size_t i = peakIdx; i < env.size(); ++i)
                if (env[i] <= threshold) {
                    f.decayMs = static_cast<float>((i - peakIdx) * msPerStep);
                    break;
                }
            // Release: from the last point above 10% of peak to the end.
            size_t lastLoud = peakIdx;
            for (size_t i = env.size(); i-- > peakIdx;)
                if (env[i] >= peakVal * 0.1f) { lastLoud = i; break; }
            f.releaseMs = static_cast<float>((env.size() - lastLoud) * msPerStep);
        }
        f.percussive = f.sustainLevel < 0.15f;
    }

    f.valid = true;
    return f;
}

// ---------------------------------------------------------------------------

std::string ReferenceFeatures::toPromptText() const {
    if (!valid) return {};

    std::ostringstream os;

    // --- a whole record, not one instrument --------------------------------
    //
    // This branch exists because of a real failure: someone dropped in a
    // finished record and asked for the guitar from it. A dense mix is
    // spectrally flat, weakly pitched and heavily compressed - which is
    // precisely how NOISE measures - so the model read the numbers correctly
    // and built filtered white noise.
    //
    // The measurements were right. Presenting them as a description of one
    // instrument was wrong.
    if (looksLikeMix) {
        os << "The musician supplied a REFERENCE RECORDING, and it is a FULL MIX -\n"
              "a finished record with several instruments playing at once, not an\n"
              "isolated sample. Most spectral measurements of a mix describe the mix,\n"
              "not any instrument in it, so only the reliable ones are given here.\n\n";

        if (!pitchClasses.empty()) {
            os << "  key centres     ";
            for (size_t i = 0; i < pitchClasses.size(); ++i)
                os << (i ? ", " : "") << pitchClasses[i];
            os << "   (strongest first - the tonal centre of the part)\n";
        }
        os << "  register        " << (lowEnergy > 0.45f ? "bass-weighted"
                                     : highEnergy > 0.35f ? "treble-weighted"
                                                          : "mid-weighted") << "\n";
        os << "  tonal balance   low " << fmt("%.0f", lowEnergy * 100.0)
           << "%  mid " << fmt("%.0f", midEnergy * 100.0)
           << "%  high " << fmt("%.0f", highEnergy * 100.0) << "%\n";
        os << "  brightness      centroid " << fmt("%.0f", centroidHz) << " Hz, "
           << "85% below " << fmt("%.0f", rolloff85Hz) << " Hz\n";
        os << "  spectral tilt   " << fmt("%+.1f", tiltDbPerOct) << " dB/oct\n";
        os << "  stereo width    " << fmt("%.2f", stereoWidth) << "\n";
        os << "  loudness        crest " << fmt("%.1f", crestDb)
           << " dB   (a mastered record, so this says nothing about the part)\n";
        os << "  length          " << fmt("%.2f", durationSec) << " s\n";

        os << "\nWHAT NOT TO CONCLUDE FROM THIS - read this carefully:\n\n"
              "- Harmonicity, spectral flatness and attack are DELIBERATELY OMITTED.\n"
              "  On a full mix they measure the drums and the mastering, not the\n"
              "  instrument, and every mix looks noisy and unpitched by those numbers.\n"
              "- Do NOT use a noise oscillator because a mix measured as flat. If the\n"
              "  musician named a pitched instrument, build a pitched instrument.\n"
              "- Do NOT match the mix's brightness directly. A full record contains\n"
              "  cymbals and vocal air that your one instrument will not have; a part\n"
              "  that matches the whole mix's spectrum will be far too bright.\n\n"
              "USE the key centres to set the register, and use the tonal balance and\n"
              "tilt as a gentle steer toward dark or bright. For everything else -\n"
              "which instrument, how it is played, what it went through - rely on the\n"
              "TEXT PROMPT and on what you know about the record it names.\n";
        return os.str();
    }

    // --- somebody playing, or one isolated sound ---------------------------
    //
    // The performance case was missing entirely, and it is the one people
    // actually have: not a lone sample but a few bars of somebody playing.
    // Measured whole that is meaningless, and it scored as a full mix, so a
    // dropped recording of one electric guitar reached the model with its
    // pitch, its attack and its harmonicity all withheld. One clean note out of
    // the performance describes the instrument perfectly well.
    const bool trustPitch = pitchIsTrustworthy();

    if (performance) {
        os << "The musician supplied a REFERENCE RECORDING, and it is a PERFORMANCE -\n"
              "somebody PLAYING one instrument, not an isolated sample. "
           << noteCount << " separate note\nattacks were found in "
           << fmt("%.2f", durationSec) << " s. Measuring the whole clip would describe the\n"
              "riff rather than the instrument, so ONE clean note was measured and every\n"
              "number below except the key centres describes THAT note. It was analysed -\n"
              "not sampled. You are designing a synthesiser that behaves like this, from\n"
              "scratch, using the modules available to you.\n\n";
        os << "  the recording   " << fmt("%.2f", durationSec) << " s"
           << (channels >= 2 ? "  (stereo)" : "  (mono)") << ", " << noteCount
           << " note attacks\n";
        os << "  note measured   " << fmt("%.2f", noteLengthSec) << " s from "
           << fmt("%.2f", noteStartSec) << " s in\n";
    } else {
        os << "The musician also supplied a REFERENCE RECORDING of what looks like a\n"
              "single isolated sound. It was analysed - not sampled. You are designing a\n"
              "synthesiser that behaves like this, from scratch, using the modules\n"
              "available to you. Measurements:\n\n";
        os << "  length          " << fmt("%.2f", durationSec) << " s"
           << (channels >= 2 ? "  (stereo)" : "  (mono)") << "\n";
    }

    if (trustPitch) {
        os << "  pitch           " << fmt("%.1f", fundamentalHz) << " Hz  (" << noteName;
        if (std::abs(centsOffset) >= 8.0f) os << ", " << fmt("%+.0f", centsOffset) << " cents";
        os << ")\n";
    } else if (fundamentalHz > 0.0f) {
        // Say so rather than quoting it. A note picked out of a busy phrase
        // often overlaps the one before it, and a fundamental read off that is
        // as likely to be a difference tone as a note - which is how a guitar
        // came back measured at 45 Hz, well below the instrument's lowest
        // string. Register and key centres survive that; a specific Hz figure
        // does not, and it is the one number that would put the whole
        // instrument in the wrong octave.
        os << "  pitch           around " << fmt("%.0f", fundamentalHz) << " Hz ("
           << noteName << "), NOT reliable - notes overlap here.\n"
              "                  Use the key centres and the register instead.\n";
    } else {
        os << "  pitch           none detected - unpitched or noise-led\n";
    }
    if (!pitchClasses.empty()) {
        os << "  key centres     ";
        for (size_t i = 0; i < pitchClasses.size(); ++i)
            os << (i ? ", " : "") << pitchClasses[i];
        os << (performance ? "   (from the whole performance)\n" : "\n");
    }
    if (performance)
        os << "  register        " << (lowEnergy > 0.45f ? "bass-weighted"
                                     : highEnergy > 0.35f ? "treble-weighted"
                                                          : "mid-weighted") << "\n";
    if (polyphonic)
        os << "  polyphony       several pitches at once - a chord, not a single note\n";

    os << "  brightness      centroid " << fmt("%.0f", centroidHz) << " Hz, "
       << "85% below " << fmt("%.0f", rolloff85Hz) << " Hz\n";
    os << "  spectral tilt   " << fmt("%+.1f", tiltDbPerOct) << " dB/oct\n";
    os << "  energy split    low " << fmt("%.0f", lowEnergy * 100.0)
       << "%  mid " << fmt("%.0f", midEnergy * 100.0)
       << "%  high " << fmt("%.0f", highEnergy * 100.0) << "%\n";
    // Harmonicity, inharmonicity and odd/even are all measured against ONE
    // harmonic series - the one starting at `fundamentalHz`. Where that
    // fundamental is not trustworthy neither are they, and a perfectly ordinary
    // guitar came back described as "inharmonic, bell or metal-like" on the
    // strength of a fundamental that was not a note anybody played. Withhold
    // them rather than quote a number that means something else, the same way
    // the full-mix branch does.
    if (trustPitch) {
        os << "  harmonicity     " << fmt("%.2f", harmonicity)
           << "   (1.0 = purely harmonic, 0.0 = noise)\n";
        os << "  inharmonicity   " << fmt("%.3f", inharmonicity)
           << "   (partial drift from integer multiples)\n";
        os << "  odd/even        " << fmt("%.2f", oddEvenRatio)
           << "   (>1.3 hollow like a square, <0.8 full like a saw)\n";
    } else {
        os << "  harmonicity     omitted, with inharmonicity and odd/even. All three are\n"
              "                  measured against the fundamental above, and it is not\n"
              "                  solid enough to hang them on - they would describe the\n"
              "                  interval between overlapping notes, not the instrument.\n";
    }
    os << "  noisiness       " << fmt("%.2f", noisiness) << "   (spectral flatness)\n";
    os << "  attack          " << fmt("%.0f", attackMs) << " ms\n";
    os << "  decay           " << fmt("%.0f", decayMs) << " ms to a sustain of "
       << fmt("%.2f", sustainLevel) << "\n";
    os << "  release         " << fmt("%.0f", releaseMs) << " ms\n";
    os << "  crest factor    " << fmt("%.1f", crestDb)
       << " dB   (>14 transient-heavy, <8 compressed or sustained)\n";
    os << "  stereo width    " << fmt("%.2f", stereoWidth) << "\n";

    os << "\nWhat those numbers mean here: " << toSummaryLine() << "\n\n";

    if (performance)
        os << "This is one instrument being PLAYED. Build the instrument, not the part:\n"
              "the tempo, the rhythm and the notes are the musician's, and none of them\n"
              "belong in the patch. Match how a note STARTS and how it DIES away, and\n"
              "the balance of low, mid and high energy. Do not add a sequencer, an\n"
              "arpeggiator or a delay to imitate the playing.\n\n";

    os << "Design to these measurements, and let the text prompt decide everything\n"
          "they do not cover - the emotion, the arrangement, the front panel. Where\n"
          "the text and the recording disagree, the TEXT WINS: the recording is what\n"
          "the musician has, the text is what they want. Match the pitch register,\n"
          "the attack, and the balance of low/mid/high energy closely; treat the\n"
          "rest as strong guidance.\n\n"
          "The noise oscillator is for sounds that are genuinely unpitched - breath,\n"
          "wind, a cymbal, the scrape at the front of a bowed note. A low harmonicity\n"
          "reading alone is NOT a reason to reach for it.\n";
    return os.str();
}

std::string ReferenceFeatures::toSummaryLine() const {
    if (!valid) return problem.empty() ? "not analysed" : problem;

    std::vector<std::string> notes;

    if (looksLikeMix) {
        notes.push_back("a full mix, not an isolated sound");
        if (!pitchClasses.empty()) {
            std::string keys = "key centres ";
            for (size_t i = 0; i < pitchClasses.size(); ++i)
                keys += (i ? "/" : "") + pitchClasses[i];
            notes.push_back(keys);
        }
        notes.push_back(lowEnergy > 0.45f ? "bass-weighted"
                        : highEnergy > 0.35f ? "treble-weighted" : "mid-weighted");
        if (stereoWidth > 0.45f) notes.push_back("wide stereo");
        std::string out;
        for (size_t i = 0; i < notes.size(); ++i) out += (i ? ", " : "") + notes[i];
        return out;
    }

    if (performance)
        notes.push_back("one instrument being played, " + std::to_string(noteCount)
                        + " notes - measured on one of them");

    if (pitchIsTrustworthy()) {
        notes.push_back("pitched around " + noteName);
        if (fundamentalHz < 110.0f)      notes.push_back("sub/bass register");
        else if (fundamentalHz > 700.0f) notes.push_back("high register");
    } else if (fundamentalHz > 0.0f) {
        notes.push_back(lowEnergy > 0.45f ? "bass-weighted"
                        : highEnergy > 0.35f ? "treble-weighted" : "mid-weighted");
    } else {
        notes.push_back("unpitched");
    }

    if      (centroidHz < 600.0f)  notes.push_back("dark");
    else if (centroidHz < 1800.0f) notes.push_back("mid-forward");
    else if (centroidHz < 4500.0f) notes.push_back("bright");
    else                           notes.push_back("very bright, airy");

    if      (attackMs < 8.0f)   notes.push_back("hard immediate attack");
    else if (attackMs < 40.0f)  notes.push_back("plucked attack");
    else if (attackMs < 200.0f) notes.push_back("soft attack");
    else                        notes.push_back("slow swell");

    if (percussive)             notes.push_back("percussive, little sustain");
    else if (sustainLevel > 0.5f) notes.push_back("strongly sustained");

    if      (noisiness > 0.45f)   notes.push_back("noise-heavy");
    else if (harmonicity > 0.55f) notes.push_back("strongly harmonic");

    // Only where they mean what they say: both are read off `fundamentalHz`,
    // so a fundamental that is not a note makes a guitar look like a bell.
    if (pitchIsTrustworthy()) {
        if (inharmonicity > 0.02f)  notes.push_back("inharmonic, bell or metal-like");
        if (oddEvenRatio > 1.4f)    notes.push_back("hollow, odd-harmonic");
    }
    if (crestDb > 14.0f)        notes.push_back("transient-heavy");
    else if (crestDb < 8.0f)    notes.push_back("compressed or continuous");
    if (stereoWidth > 0.45f)    notes.push_back("wide stereo");
    else if (stereoWidth < 0.08f && channels >= 2) notes.push_back("essentially mono");

    std::string out;
    for (size_t i = 0; i < notes.size(); ++i) out += (i ? ", " : "") + notes[i];
    return out;
}

} // namespace forge::audio
