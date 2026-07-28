// ---------------------------------------------------------------------------
// Reassembling a streamed model response.
//
// Streaming is not here for the typewriter effect - nothing displays the text
// as it arrives. It is here because a NON-streamed request sends no bytes at
// all until the model has finished writing, and JUCE applies its socket timeout
// per read. A full patch is up to 16k output tokens; past about a minute of
// that, a perfectly healthy generation is indistinguishable from a dead
// connection, so it was abandoned and quietly replaced with the offline
// fallback. Any prompt long enough to be worth writing made that more likely,
// and attaching a reference recording made it near-certain.
//
// Streamed, bytes arrive continuously - the providers send keep-alive pings
// between tokens - so a read timeout only fires on a connection that has really
// stopped. And whatever arrived before a break is kept: a patch cut off at 95%
// is repaired by the salvage pass, where before it was simply lost.
//
// Deliberately JUCE-free and transport-free: it is fed bytes and asked what it
// has, which is what makes the wire formats testable without a network.
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace forge::llm {

class StreamAssembler {
public:
    enum class Wire {
        AnthropicSse,      ///< server-sent events, content_block_delta
        OpenAiSse,         ///< server-sent events, choices[].delta.content
        OllamaJsonLines,   ///< newline-delimited JSON, message.content
    };

    explicit StreamAssembler(Wire wire) : wire_(wire) {}

    /// Feed bytes as they arrive. Any number of chunks, split anywhere - a
    /// single event routinely straddles two reads.
    void feed(const char* data, size_t numBytes);
    void feed(const std::string& s) { feed(s.data(), s.size()); }

    /// A stream that ends without a trailing newline still has a last line.
    void finish();

    /// The assistant text so far, deltas concatenated in order.
    const std::string& text() const { return text_; }
    /// Set when the provider reported a problem mid-stream, after a 200.
    const std::string& error() const { return error_; }
    /// True once the provider said it was finished, as opposed to the
    /// connection merely stopping - which is the difference between a complete
    /// design and one that needs repairing.
    bool complete() const { return done_; }

private:
    void consume(const std::string& line);

    Wire        wire_;
    std::string pending_, text_, error_;
    bool        done_ = false;
};

} // namespace forge::llm
