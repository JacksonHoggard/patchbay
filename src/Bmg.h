#pragma once

#include <cstdint>
#include <string>
#include <vector>

// BMG is the message (dialogue text) format of many Nintendo games
// ("MESGbmg1"), for example zel_00.bmg in Twilight Princess HD's
// bmgres.arc. Sections: INF1 (per-message DAT1 offset and attributes),
// DAT1 (text), then MID1/FLW1/FLI1 and others, which are kept whole.
namespace bmg {

struct Message {
    std::string attrs; // INF1 entry bytes after the text offset
    std::string text;  // up to and including the terminator
    bool operator==(const Message &o) const { return attrs == o.attrs && text == o.text; }
    bool operator!=(const Message &o) const { return !(*this == o); }
};

struct File {
    std::string raw;
    std::vector<Message> messages;
    std::vector<uint32_t> textOffsets; // per message, into DAT1's data
    std::string dat;                   // DAT1's data (after its 8-byte header)
    std::vector<std::string> sectionMagic;
    std::vector<std::string> sections; // whole sections other than INF1/DAT1 ("" for those)
};

bool isBmg(const std::string &data);

// Fails on encodings other than single-byte/Shift-JIS, or a malformed file.
bool parse(const std::string &data, File &out);

// True if messages and sections line up (same count, same section order).
bool compatible(const File &a, const File &b);

// Rebuilds `layout` with new messages and sections. The original text data
// is kept as is; messages whose text changed get their new text appended.
std::string rebuild(const File &layout, const std::vector<Message> &messages,
                    const std::vector<std::string> &sections);

} // namespace bmg
