// ---------------------------------------------------------------------------
// Typography.
//
// Inter, embedded in three weights. Embedding rather than relying on system
// fonts matters for a plugin: the UI has to look the same on every machine, and
// "whatever sans-serif the host happened to pick" is how plugins end up looking
// amateur.
//
// One type scale, defined once, used everywhere. The single biggest cause of a
// bloated-looking UI is ad-hoc font sizes scattered through paint() calls.
// ---------------------------------------------------------------------------
#pragma once

#include <juce_graphics/juce_graphics.h>

namespace forge::fonts {

enum class Weight { Regular, Medium, SemiBold };

/// Cached typefaces. Safe to call from any thread after the first call on the
/// message thread.
juce::Font get(float height, Weight weight = Weight::Regular);

// --- the type scale -------------------------------------------------------
// Deliberately small and tight. DAW plugins are dense instruments, not web
// pages; oversized type is what made the first pass look bloated.

inline juce::Font title()       { return get(13.0f, Weight::SemiBold); }  ///< "FORGE"
inline juce::Font sectionHead() { return get(9.0f,  Weight::SemiBold); }  ///< "FILTER"
inline juce::Font controlName() { return get(10.0f, Weight::Medium);   }  ///< "Cutoff"
inline juce::Font controlValue(){ return get(9.5f,  Weight::Regular);  }  ///< "4200 Hz"
inline juce::Font body()        { return get(12.0f, Weight::Regular);  }
inline juce::Font bodyMedium()  { return get(12.0f, Weight::Medium);   }
inline juce::Font caption()     { return get(10.0f, Weight::Regular);  }  ///< status strip
inline juce::Font prompt()      { return get(14.0f, Weight::Regular);  }  ///< the input field

/// Letter-spaced small caps, used for section headings. JUCE has no tracking
/// control, so we draw glyph by glyph.
void drawTracked(juce::Graphics&, const juce::String& text, juce::Rectangle<int> area,
                 juce::Justification, float tracking = 1.2f);

} // namespace forge::fonts
