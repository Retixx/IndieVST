// ---------------------------------------------------------------------------
// Voice allocation and note handling.
//
// Split out of GraphInstance.cpp because this is policy, not signal processing:
// which voice a note lands on, what happens when they run out, and how mono and
// legato modes differ. Getting this wrong is audible long before any DSP bug is.
// ---------------------------------------------------------------------------
#include "core/dsp/GraphInstance.h"

#include <algorithm>

namespace forge {

int GraphInstance::allocateVoice(int midiNote) noexcept {
    if (voices_.empty()) return -1;

    // 1. The same note retriggering: reuse its voice so repeated notes do not
    //    stack up and get progressively louder.
    for (size_t i = 0; i < voices_.size(); ++i)
        if (voices_[i].active && voices_[i].note == midiNote)
            return static_cast<int>(i);

    // 2. A genuinely free voice.
    for (size_t i = 0; i < voices_.size(); ++i)
        if (!voices_[i].active) return static_cast<int>(i);

    // 3. Steal. Prefer a voice that has already been released (it is on its way
    //    out anyway), otherwise the oldest one.
    int best = -1;
    uint64_t bestOrder = ~0ull;
    for (size_t i = 0; i < voices_.size(); ++i) {
        if (voices_[i].held) continue;
        if (voices_[i].order < bestOrder) { bestOrder = voices_[i].order; best = static_cast<int>(i); }
    }
    if (best >= 0) return best;

    bestOrder = ~0ull;
    for (size_t i = 0; i < voices_.size(); ++i)
        if (voices_[i].order < bestOrder) { bestOrder = voices_[i].order; best = static_cast<int>(i); }
    return best;
}

void GraphInstance::noteOn(int midiNote, float velocity) noexcept {
    if (voices_.empty()) return;
    midiNote = clampT(midiNote, 0, 127);
    velocity = clamp01(velocity);

    const bool mono   = (ir_.voicing != "poly");
    const bool legato = (ir_.voicing == "legato");

    if (mono) {
        Voice& v = voices_[0];
        const bool wasSounding = v.active && v.held;

        v.ctx.baseFreqHz = midiNoteToHz(static_cast<float>(midiNote));
        v.ctx.velocity   = velocity;
        v.ctx.keyTrack01 = static_cast<float>(midiNote) / 127.0f;
        v.ctx.gate       = 1.0f;
        v.note   = midiNote;
        v.held   = true;
        v.active = true;
        v.order  = ++noteCounter_;
        v.quietBlocks = 0;

        // Legato: slide to the new note without restarting the envelopes.
        if (!(legato && wasSounding)) {
            if (!wasSounding) v.ctx.glideFreqHz = v.ctx.baseFreqHz;
            for (auto& m : v.modules) m->noteOn(v.ctx.baseFreqHz, velocity);
        }
        return;
    }

    const int index = allocateVoice(midiNote);
    if (index < 0) return;
    Voice& v = voices_[static_cast<size_t>(index)];

    const bool stealing = v.active;

    v.ctx.baseFreqHz  = midiNoteToHz(static_cast<float>(midiNote));
    v.ctx.glideFreqHz = (ir_.glideMs > 0.0f && v.active) ? v.ctx.glideFreqHz : v.ctx.baseFreqHz;
    v.ctx.velocity    = velocity;
    v.ctx.keyTrack01  = static_cast<float>(midiNote) / 127.0f;
    v.ctx.gate        = 1.0f;
    v.note   = midiNote;
    v.held   = true;
    v.active = true;
    v.order  = ++noteCounter_;
    v.quietBlocks = 0;

    for (auto& m : v.modules) { m->reset(); m->noteOn(v.ctx.baseFreqHz, velocity); }

    // Stealing resets a voice mid-sound, which is a discontinuity. A 2 ms
    // fade-in hides it completely and costs nothing.
    if (stealing) {
        v.fade    = 0.0f;
        const float samples = static_cast<float>(global_.sampleRate) * 0.002f;
        v.fadeInc = 1.0f / std::max(samples, 1.0f);
    } else {
        v.fade    = 1.0f;
        v.fadeInc = 0.0f;
    }
}

void GraphInstance::noteOff(int midiNote) noexcept {
    midiNote = clampT(midiNote, 0, 127);
    for (auto& v : voices_) {
        if (!v.active || v.note != midiNote || !v.held) continue;
        v.held = false;
        if (sustain_) continue;      // pedal down: hold the release back
        v.ctx.gate = 0.0f;
        for (auto& m : v.modules) m->noteOff();
    }
}

void GraphInstance::sustainPedal(bool down) noexcept {
    if (sustain_ == down) return;
    sustain_ = down;
    if (down) return;

    // Pedal released: everything that is no longer physically held now enters
    // its release stage.
    for (auto& v : voices_) {
        if (!v.active || v.held) continue;
        v.ctx.gate = 0.0f;
        for (auto& m : v.modules) m->noteOff();
    }
}

void GraphInstance::allNotesOff(bool immediate) noexcept {
    sustain_ = false;
    for (auto& v : voices_) {
        if (!v.active) continue;
        v.held = false;
        v.ctx.gate = 0.0f;
        for (auto& m : v.modules) m->noteOff();
        if (immediate) {
            for (auto& m : v.modules) m->reset();
            v.active = false;
            v.note = -1;
            v.quietBlocks = 0;
        }
    }
    if (immediate) {
        for (auto& m : globalModules_) m->reset();
        std::fill(voiceBus_.begin(), voiceBus_.end(), 0.0f);
    }
}

} // namespace forge
