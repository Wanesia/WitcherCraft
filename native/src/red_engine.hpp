#pragma once
// REDengine 3 (The Witcher 3, patch 5.0 / FileVersion 5.0.0.1044392) scripting
// bindings, located at runtime by signature scanning.
//
// Everything here is derived from witcher3.exe 5.0 (PE TimeDateStamp 0x6ABD8695).
// The reference RVAs are in red_engine.cpp (CompareWithReference).
#include "pe.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace wc::red
{
// ---------------------------------------------------------------------------
// Engine ABI (5.0)
// ---------------------------------------------------------------------------

// CScriptStackFrame: only two fields are needed.
//   +0x00  IScriptable* context
//   +0x30  uint8_t*     code      (bytecode cursor; params are opcodes)
struct CScriptStackFrame;
constexpr size_t kFrameContextOffset = 0x00;
constexpr size_t kFrameCodeOffset = 0x30;

// Signature of every global script native AND of every opcode handler
// (they share one function table in 5.0).
using ScriptNative = void (*)(void* context, CScriptStackFrame* stack, void* result);

// Script `string` in 5.0 is an 8-bit TString (was wchar_t up to 4.x!).
struct RedString
{
    char* data;    // engine heap (Realloc/Free, memclass 0xE)
    uint32_t size; // bytes INCLUDING the trailing NUL, 0 == empty
    uint32_t pad;
};

// Script `Vector` (16 bytes, 16-aligned).
struct alignas(16) RedVector
{
    float x, y, z, w;
};

struct AllocResult
{
    void* ptr;
    size_t size;
};

struct Offsets
{
    // --- native registration (all from the "RandDifferent" registration block)
    AllocResult* (*Alloc)(AllocResult* out, size_t size, uint32_t align) = nullptr;
    void* Memset = nullptr;
    void* (*GetNamePool)() = nullptr;
    uint32_t (*AddName)(void* pool, const char* name) = nullptr; // returns CName index
    void* (*CFunctionCtor)(void* mem, const uint32_t* name, ScriptNative fn) = nullptr;
    void* (*GetRTTI)() = nullptr;
    void (*RegisterGlobalFunction)(void* rtti, void* func) = nullptr;
    uint32_t cfunctionSize = 0;      // 0xF8 in 5.0 (was 0xC0)
    uint8_t* hookCallSite = nullptr; // `call RegisterGlobalFunction` for RandDifferent
    uint8_t* regBlock = nullptr;

    // --- global native / opcode table (from the CFunction ctor)
    void** nativeTable = nullptr;
    void** counterHolderSlot = nullptr; // *slot -> object, +counterOffset = next free index
    uint32_t counterOffset = 0;
    uint32_t functionIndexOffset = 0; // CFunction+X holds its table index (0xE0)
    uint32_t tableCapacity = 0;       // 4096 in 5.0 (derived)
    void* tableFromNative = nullptr;  // cross-check (lea r15, table in RandDifferent)

    // --- string allocator (from the StrLen native)
    void* (*Realloc)(void* old, uint32_t size, uint32_t align, uint16_t memclass) = nullptr;
    void (*Free)(void* p) = nullptr;
    uint16_t stringMemClass = 0xE;

    // --- Log() sink (from the Log native) -- used by the fallback channel
    void** logSinkSlot = nullptr;
};

// Pure scanning, no writes. Works on the live module or an image-resource mapping.
bool Resolve(const pe::Module& m, Offsets& o);
void DumpOffsets(const pe::Module& m, const Offsets& o);
// Compares with the RVAs measured for build 0x6ABD8695. Returns number of mismatches (-1 = other build).
int CompareWithReference(const pe::Module& m, const Offsets& o);

// Resolved offsets for the live process.
extern Offsets g;

// ---------------------------------------------------------------------------
// Runtime helpers for natives (valid once g is resolved on the live module)
// ---------------------------------------------------------------------------
inline uint8_t*& Code(CScriptStackFrame* s)
{
    return *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(s) + kFrameCodeOffset);
}
inline void* Context(CScriptStackFrame* s)
{
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(s) + kFrameContextOffset);
}

// Evaluates the next parameter expression into `out` (engine's GET_PARAMETER).
void ReadParam(CScriptStackFrame* s, void* out);
// Skips the end-of-params marker (engine's FINISH_PARAMETERS).
inline void FinishParams(CScriptStackFrame* s)
{
    ++Code(s);
}

std::string ReadStringParam(CScriptStackFrame* s);
void AssignString(void* result, std::string_view v);

int NativeSlotsUsed(); // -1 if unknown
bool RegisterGlobalNative(const char* staticName, ScriptNative fn);
} // namespace wc::red
