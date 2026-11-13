#pragma once

#include <set>
#include <string>

namespace titles {

// Upper-case 16-digit hex IDs of every title installed on the console
// (game, update and DLC partitions). Empty if MCP is unavailable.
std::set<std::string> installed();

} // namespace titles
