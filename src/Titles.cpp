#include "Titles.h"

#include <coreinit/mcp.h>
#include <cstdio>
#include <vector>

namespace titles {

std::set<std::string> installed() {
    std::set<std::string> out;
    int32_t handle = MCP_Open();
    if (handle < 0) return out;

    int32_t count = MCP_TitleCount(handle);
    if (count > 0) {
        std::vector<MCPTitleListType> list((size_t) count);
        uint32_t got = 0;
        if (MCP_TitleList(handle, &got, list.data(), (uint32_t) (list.size() * sizeof(MCPTitleListType))) >= 0) {
            for (uint32_t i = 0; i < got && i < list.size(); ++i) {
                char buf[17];
                std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long) list[i].titleId);
                out.insert(buf);
            }
        }
    }
    MCP_Close(handle);
    return out;
}

} // namespace titles
