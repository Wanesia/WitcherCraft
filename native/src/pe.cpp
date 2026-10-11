#include "pe.hpp"

#include <Windows.h>
#include <cstring>

namespace wc::pe
{
bool Module::Init(void* imageBase)
{
    base = static_cast<uint8_t*>(imageBase);
    if (!base)
        return false;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;

    imageSize = nt->OptionalHeader.SizeOfImage;
    timeDateStamp = nt->FileHeader.TimeDateStamp;
    entryRva = nt->OptionalHeader.AddressOfEntryPoint;

    sections.clear();
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        Section s;
        std::memcpy(s.name, sec->Name, 8);
        s.begin = base + sec->VirtualAddress;
        s.size = sec->Misc.VirtualSize;
        s.characteristics = sec->Characteristics;
        sections.push_back(s);
    }
    return true;
}

const Section* Module::Find(std::string_view name) const
{
    for (auto& s : sections)
        if (name == s.name)
            return &s;
    return nullptr;
}

bool Module::In(const void* p, size_t len) const
{
    auto a = reinterpret_cast<uintptr_t>(p);
    auto b = reinterpret_cast<uintptr_t>(base);
    return a >= b && a + len <= b + imageSize && a + len >= a;
}

bool Module::InSection(std::string_view name, const void* p, size_t len) const
{
    const Section* s = Find(name);
    if (!s)
        return false;
    auto a = reinterpret_cast<uintptr_t>(p);
    auto b = reinterpret_cast<uintptr_t>(s->begin);
    return a >= b && a + len <= b + s->size;
}
} // namespace wc::pe
