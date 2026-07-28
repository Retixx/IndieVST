// ---------------------------------------------------------------------------
// The instrument dropdown's backing store.
//
// Two tiers, deliberately:
//   * session instruments live in the DAW project and travel with it, so a
//     project opened on another machine still plays correctly;
//   * library instruments live in %APPDATA%\Forge\instruments and follow the
//     user across projects.
//
// On first run the library is seeded from the built-in examples, so the plugin
// is never an empty box.
// ---------------------------------------------------------------------------
#pragma once

#include "core/ir/Ir.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace forge {

struct LibraryEntry {
    juce::String   id;
    juce::String   name;
    juce::String   prompt;
    ir::Instrument instrument;
    bool           onDisk = false;
    juce::int64    created = 0;
};

class InstrumentLibrary {
public:
    /// Creates the instruments folder and writes the built-in examples the
    /// first time Forge runs on this machine.
    void seedIfEmpty();

    /// Loads everything from disk, replacing the disk-backed entries. Session
    /// entries are untouched.
    void loadFromDisk();

    /// Adds an instrument, optionally persisting it. Returns the new entry id.
    juce::String add(const ir::Instrument& instrument,
                     const juce::String& prompt,
                     bool persist);

    bool remove(const juce::String& id);
    bool rename(const juce::String& id, const juce::String& newName);
    /// Writes an edited instrument back over its library entry.
    bool updateInstrument(const juce::String& id, const ir::Instrument&);

    const std::vector<LibraryEntry>& entries() const noexcept { return entries_; }
    const LibraryEntry* find(const juce::String& id) const;

    /// Serialises the full instruments (not just references) into the project
    /// state, so nothing depends on the user's local library.
    juce::var toState() const;
    void      fromState(const juce::var& state);

    /// "Molten Sub" -> "Molten Sub (1)" if that name is already taken.
    /// Generating the same kind of sound twice is normal, and a dropdown with
    /// four identical entries is useless.
    juce::String uniqueName(const juce::String& wanted,
                            const juce::String& ignoreId = {}) const;

private:
    static juce::String makeId(const ir::Instrument&);
    static juce::File   fileFor(const juce::String& id);
    void writeToDisk(const LibraryEntry&) const;

    std::vector<LibraryEntry> entries_;
};

} // namespace forge
