#pragma once

#include <cstdint>
#include <string>

// Yaz0 is Nintendo's LZ77 variant, used for .szs and many compressed files
// on the Wii U. Header (16 bytes, big-endian): "Yaz0", u32 decompressed size,
// u32 alignment hint (0 on older titles), u32 reserved.
namespace yaz0 {

bool isYaz0(const std::string &data);

// Decompresses `in` into `out`. Fails on a truncated or corrupt stream.
bool decompress(const std::string &in, std::string &out);

// The alignment hint stored in a Yaz0 header (0 if `data` is not Yaz0).
uint32_t alignmentHint(const std::string &data);

// Compresses `in`. `alignment` is written to the header unchanged; copy it
// from the file being replaced.
std::string compress(const std::string &in, uint32_t alignment = 0);

} // namespace yaz0
