#pragma once
#include <string>
#include <vector>

namespace aoi {

// Build the interpretation-history prefix injected into user prompts.
// Returns "" when there is no recent translation backlog; otherwise a capped
// snippet. Mirrors agent-utils.ts buildContextPrefix().
std::string buildContextPrefix(const std::vector<std::string>& history);

// Clean model output before TTS: strip think blocks, code fences, emoji and
// markdown punctuation, collapsing whitespace. Mirrors agent-utils.ts.
std::string sanitizeForTts(const std::string& text);

// True when a TTS candidate is pure punctuation/spacing. Mirrors isTtsJunk().
bool isTtsJunk(const std::string& text);

// Split text on sentence-ending punctuation (ASCII .!? and CJK 。！？), keeping
// the delimiter at the end. Mirrors the JS SENTENCE_DELIMITERS lookbehind.
std::vector<std::string> splitSentences(const std::string& s);

// Truncate to at most maxBytes WITHOUT splitting a multi-byte UTF-8
// character: a mid-sequence cut produces invalid UTF-8, and any later JSON
// serialization of the string (nlohmann dump) throws type_error.316
// ("invalid UTF-8 byte at index ...").
void utf8SafeTruncate(std::string& s, size_t maxBytes);

// Length of the longest prefix of `s` that ends on a complete UTF-8 character
// (a trailing partial sequence is excluded). Used for byte-paged reads so a
// page never ends mid-character.
size_t utf8CompleteLength(const std::string& s);

// Replace invalid UTF-8 bytes with U+FFFD so JSON serialization never throws
// (nlohmann's dump validates strings - invalid input aborts callers that
// forgot to catch). Keeps valid characters intact.
std::string sanitizeUtf8(const std::string& s);

} // namespace aoi
