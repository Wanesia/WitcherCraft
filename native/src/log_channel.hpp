#pragma once
#include <string_view>

namespace wc::logchannel
{
// FALLBACK script->C++ channel that needs no new natives and no code patching:
// the Log()/LogChannel() natives forward every message to a function pointer
// stored in .data (a no-op `ret` in release builds). We chain our own sink in
// and react to messages that start with "WCRAFT:".
bool Start();
// Shared command handler (also used by the WCraft_Rpc native).
std::string_view Prefix();
} // namespace wc::logchannel
