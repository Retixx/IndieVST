// ---------------------------------------------------------------------------
// Reassembling a streamed response.
//
// The bug this exists for: "I dropped in a recording and after 60 seconds it
// timed out and gave me the wrong instrument." Nothing was slow about the
// network. The request was not streamed, so no bytes moved at all while the
// model wrote a 16k-token patch, and JUCE applies its socket timeout per read -
// so a healthy generation and a dead connection look exactly alike. At the
// deadline the whole thing was thrown away and the offline fallback quietly put
// in its place.
//
// Streaming fixes it because bytes arrive the whole time. These cases pin the
// three wire formats, and - just as importantly - the behaviour when a stream
// stops early, because keeping 95% of a patch and repairing it beats losing it.
// ---------------------------------------------------------------------------
#include "TestUtil.h"

#include "core/llm/StreamAssembler.h"
#include "core/llm/JsonSalvage.h"

#include <string>
#include <vector>

using namespace forge;
using Wire = llm::StreamAssembler::Wire;

namespace {

/// Feeds a whole response in chunks of `size` bytes, so an event that straddles
/// two reads is exercised rather than assumed to work.
llm::StreamAssembler run(Wire wire, const std::string& body, size_t size) {
    llm::StreamAssembler a(wire);
    for (size_t i = 0; i < body.size(); i += size)
        a.feed(body.data() + i, std::min(size, body.size() - i));
    a.finish();
    return a;
}

const char* kAnthropic =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\"}}\n"
    "\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0}\n"
    "\n"
    "event: ping\n"
    "data: {\"type\":\"ping\"}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"{\\\"name\\\":\\\"Retro \"}}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"Guitar\\\",\\\"values\\\":{}}\"}}\n"
    "\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n"
    "\n";

const char* kOpenAi =
    "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"{\\\"name\\\":\\\"Re\"}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"tro\\\"}\"}}]}\n"
    "\n"
    "data: [DONE]\n"
    "\n";

const char* kOllama =
    "{\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"name\\\":\"},\"done\":false}\n"
    "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\"Retro\\\"}\"},\"done\":false}\n"
    "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true}\n";

} // namespace

TEST_CASE(anthropic_deltas_reassemble_in_order) {
    const auto a = run(Wire::AnthropicSse, kAnthropic, 4096);
    CHECK(a.text() == R"({"name":"Retro Guitar","values":{}})");
    CHECK(a.complete());
    CHECK(a.error().empty());
}

TEST_CASE(a_stream_split_anywhere_still_reassembles) {
    // A single SSE event routinely straddles two reads, and one that is dropped
    // silently loses a chunk out of the middle of the patch. Byte by byte is
    // the harshest split there is.
    for (size_t chunk : {1u, 2u, 7u, 63u, 300u}) {
        const auto a = run(Wire::AnthropicSse, kAnthropic, chunk);
        CHECK(a.text() == R"({"name":"Retro Guitar","values":{}})");
        CHECK(a.complete());
    }
}

TEST_CASE(openai_and_ollama_wire_formats_reassemble_too) {
    const auto o = run(Wire::OpenAiSse, kOpenAi, 13);
    CHECK(o.text() == R"({"name":"Retro"})");
    CHECK(o.complete());

    const auto l = run(Wire::OllamaJsonLines, kOllama, 11);
    CHECK(l.text() == R"({"name":"Retro"})");
    CHECK(l.complete());
}

TEST_CASE(a_stream_that_stops_early_keeps_what_arrived) {
    // The whole reason this is worth doing. Before, a design that was 95%
    // delivered when the connection broke was discarded and the musician got a
    // generic instrument; now it survives and the salvage pass finishes it.
    std::string cut(kAnthropic);
    cut = cut.substr(0, cut.find("Guitar"));

    const auto a = run(Wire::AnthropicSse, cut, 32);
    CHECK(!a.complete());                 // and we KNOW it did not finish
    CHECK(!a.text().empty());
    CHECK(a.text().find("Retro") != std::string::npos);
}

TEST_CASE(a_truncated_patch_out_of_a_broken_stream_is_still_repairable) {
    llm::StreamAssembler a(Wire::AnthropicSse);
    const char* head =
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\","
            "\"text\":\"{\\\"name\\\":\\\"Retro Guitar\\\",\\\"values\\\":{\\\"str_level\\\":0.85\"}}\n";
    a.feed(head, std::char_traits<char>::length(head));
    a.finish();

    CHECK(!a.complete());
    const auto salvaged = llm::salvageJsonObject(a.text());
    CHECK(static_cast<bool>(salvaged));
    CHECK(salvaged.json.find("Retro Guitar") != std::string::npos);
}

TEST_CASE(a_tool_call_stream_is_read_as_well_as_plain_text) {
    // Structured output arrives as input_json_delta rather than text_delta.
    const char* body =
        "data: {\"type\":\"content_block_delta\",\"delta\":"
            "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"a\\\":1}\"}}\n"
        "data: {\"type\":\"message_stop\"}\n";
    const auto a = run(Wire::AnthropicSse, body, 9);
    CHECK(a.text() == R"({"a":1})");
    CHECK(a.complete());
}

TEST_CASE(an_error_arriving_mid_stream_is_reported_not_swallowed) {
    // A 200 does not mean it worked: overloaded_error arrives inside the
    // stream, and a patch that just stops there must not look complete.
    const char* body =
        "data: {\"type\":\"content_block_delta\",\"delta\":"
            "{\"type\":\"text_delta\",\"text\":\"{\\\"name\\\"\"}}\n"
        "event: error\n"
        "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
            "\"message\":\"Overloaded\"}}\n";
    const auto a = run(Wire::AnthropicSse, body, 64);
    CHECK(a.error() == "Overloaded");
    CHECK(!a.text().empty());
}

TEST_CASE(keepalives_and_unknown_events_contribute_nothing) {
    // Pings are exactly what stops a slow generation from looking like a dead
    // socket, so they must arrive and must not end up in the patch.
    const char* body =
        "event: ping\n"
        "data: {\"type\":\"ping\"}\n"
        ": this is an SSE comment\n"
        "\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n"
        "data: not json at all\n"
        "\n";
    const auto a = run(Wire::AnthropicSse, body, 5);
    CHECK(a.text().empty());
    CHECK(!a.complete());
    CHECK(a.error().empty());
}
