#pragma once
#include <cstdint>

namespace wc::hook
{
// Emits "jmp qword ptr [rip+0]; dq target" into an executable page allocated
// within +-2 GB of `nearAddr`, so a rel32 call/jmp at nearAddr can reach it.
void* MakeNearJump(void* nearAddr, void* target);

// Re-targets an existing `E8 rel32` call instruction to `newTarget` (through a
// near jump stub). Returns the previous call target in *oldTarget.
bool RedirectCall(uint8_t* callInsn, void* newTarget, void** oldTarget);
} // namespace wc::hook
