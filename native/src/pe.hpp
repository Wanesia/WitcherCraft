#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wc::pe
{
struct Section
{
    char name[9]{};
    uint8_t* begin{};
    size_t size{};
    uint32_t characteristics{};
};

// A view over a mapped PE image (either the live witcher3.exe module or an
// image-resource mapping of the file on disk). Read-only helper.
struct Module
{
    uint8_t* base{};
    size_t imageSize{};
    uint32_t timeDateStamp{};
    uint32_t entryRva{};
    std::vector<Section> sections;

    bool Init(void* imageBase);
    const Section* Find(std::string_view name) const;
    bool In(const void* p, size_t len = 1) const;
    bool InSection(std::string_view name, const void* p, size_t len = 1) const;
    uintptr_t Rva(const void* p) const
    {
        return p ? reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(base) : 0;
    }
};
} // namespace wc::pe
