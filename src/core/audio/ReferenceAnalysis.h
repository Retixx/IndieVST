// ---------------------------------------------------------------------------
// Reference audio analysis.
//
// A producer can describe a sound in words, but the fastest way to tell you
// what they want is to play it to you. This turns a dropped audio file into a
// short list of measurements the model can design against: what note it is,
// how bright, how hard it starts, how pitched versus noisy, how wide.
//
// It is deliberately JUCE-free and allocation-tolerant - it runs once, on a
// background thread, never on the audio thread - so it can be tested headlessly
// against synthetic signals whose answers are known in advance.
//
// This is NOT sample playback and NOT resynthesis. Forge does not embed the
// reference in the instrument; it reads the reference and then designs a
// synthesiser from scratch. That distinction matters legally as well as
// musically: nothing of the source audio survives into the output.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace forge::audio {

struct ReferenceFeatures {
    bool   valid = false;
    std::string problem;      ///< why analysis failed, in plain words

    double durationSec = 0.0;
    double sampleRate  = 0.0;
    int    channels    = 0;

    // --- what KIND of recording this is ---
    //
    // This decides how much of the rest can be trusted. A full record is
    // spectrally flat, weakly pitched and heavily compressed - which is the
    // exact fingerprint of noise. Reporting a mix's harmonicity as though it
    // described one instrument is how a request for a guitar came back as
    // filtered white noise.
    bool   looksLikeMix   = false;
    float  mixLikelihood  = 0.0f;  ///< 0 isolated single note, 1 dense full mix
    bool   polyphonic     = false; ///< several pitches sounding at once

    // --- one instrument, played ---
    //
    // The case that was missing entirely, and the one people actually drop in.
    // Almost nobody has an isolated single note lying around; they have a
    // recording of someone PLAYING - a riff, a lick, a few bars. Measured
    // whole, that spans a dozen notes, so there is no fundamental, the attack
    // is wherever the loudest note happened to fall, and the "decay" describes
    // the gap between phrases. Every one of those numbers is meaningless, and
    // the whole clip then scored as a full mix and was reported to the model
    // with almost all of its measurements withheld.
    //
    // When separate note onsets are found, one clean note is measured instead
    // and everything below describes THAT note. It is the same instrument.
    bool   performance    = false; ///< several separate notes, one instrument
    int    noteCount      = 0;     ///< onsets found in the analysed span
    float  noteConfidence = 0.0f;  ///< mean pitch confidence across those notes
    double noteStartSec   = 0.0;   ///< where the measured note begins
    double noteLengthSec  = 0.0;   ///< and how much of it was measured

    // --- pitch ---
    float  fundamentalHz = 0.0f;   ///< 0 if nothing pitched was found
    std::string noteName;          ///< "D3", "F#2"...
    float  centsOffset   = 0.0f;   ///< how far from equal temperament
    float  pitchConfidence = 0.0f;

    /// Dominant pitch classes, strongest first: "C#", "G#", "F".
    ///
    /// Chroma survives mixing, where a single fundamental does not. On a dense
    /// record this is the only pitch information worth reporting - and it is
    /// what tells you the riff sits on C# rather than D.
    std::vector<std::string> pitchClasses;

    // --- spectrum ---
    float  centroidHz    = 0.0f;   ///< brightness
    float  rolloff85Hz   = 0.0f;   ///< where 85% of the energy is below
    float  tiltDbPerOct  = 0.0f;   ///< negative = darker towards the top
    float  lowEnergy     = 0.0f;   ///< below 250 Hz,  0..1
    float  midEnergy     = 0.0f;   ///< 250 Hz - 2 kHz
    float  highEnergy    = 0.0f;   ///< above 2 kHz

    // --- character ---
    float  harmonicity   = 0.0f;   ///< 0 noise, 1 purely harmonic
    float  inharmonicity = 0.0f;   ///< partial drift from integer multiples
    float  noisiness     = 0.0f;   ///< spectral flatness, 0 tonal, 1 hiss
    float  oddEvenRatio  = 0.0f;   ///< >1 = hollow/square-like, <1 = full/saw-like

    // --- shape over time ---
    float  attackMs      = 0.0f;   ///< 10% to 90% of peak
    float  decayMs       = 0.0f;   ///< peak down to the sustain level
    float  sustainLevel  = 0.0f;   ///< 0..1 relative to peak
    float  releaseMs     = 0.0f;
    bool   percussive    = false;  ///< no meaningful sustain
    float  crestDb       = 0.0f;   ///< peak over RMS: transient-heavy or squashed

    // --- stereo ---
    float  stereoWidth   = 0.0f;   ///< 0 mono, 1 fully decorrelated
    float  peakDb        = 0.0f;
    float  rmsDb         = 0.0f;

    /// Whether `fundamentalHz` is worth quoting - and with it harmonicity,
    /// inharmonicity and oddEvenRatio, every one of which is measured against
    /// that fundamental and is therefore exactly as good as it is.
    ///
    /// A note lifted out of a busy phrase overlaps the one before it, so the
    /// detected period can be a difference tone rather than a note: a guitar
    /// recording measured 45 Hz, a full octave below its lowest string, and the
    /// same reading then described the instrument as inharmonic and bell-like.
    /// Low harmonicity is the tell - it says the partials do not sit on this
    /// fundamental - and where it is low, register and key centres are reported
    /// instead. They survive what a single fundamental does not.
    bool pitchIsTrustworthy() const {
        return fundamentalHz > 0.0f && (!performance || harmonicity >= 0.30f);
    }

    /// The block handed to the model. Plain measurements with plain units, plus
    /// a short reading of what they imply - the model designs better from
    /// "bright, hard-picked, string-like" than from a column of floats.
    std::string toPromptText() const;

    /// One line for the UI, so the user can see it actually heard the file.
    std::string toSummaryLine() const;
};

/// `samples` is non-interleaved: samples[ch][frame].
ReferenceFeatures analyse(const float* const* samples, int numChannels,
                          int numFrames, double sampleRate);

/// Longest span we analyse. A reference is a sound, not a song; past a few
/// seconds the averages stop describing anything in particular.
inline constexpr double kMaxReferenceSeconds = 12.0;

} // namespace forge::audio
