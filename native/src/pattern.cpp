#include "pattern.hpp"

#include <cctype>

namespace wc::pattern
{
namespace
{
int HexVal(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
} // namespace

Pattern Parse(std::string_view s)
{
    Pattern p;
    size_t i = 0;
    while (i < s.size())
    {
        if (s[i] == ' ')
        {
            ++i;
            continue;
        }
        if (s[i] == '?')
        {
            p.bytes.push_back(0);
            p.mask.push_back(0);
            ++i;
            if (i < s.size() && s[i] == '?')
                ++i;
            continue;
        }
        if (i + 1 < s.size())
        {
            int hi = HexVal(s[i]), lo = HexVal(s[i + 1]);
            if (hi >= 0 && lo >= 0)
            {
                p.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
                p.mask.push_back(1);
                i += 2;
                continue;
            }
        }
        // malformed token -> make the pattern unmatchable
        p.bytes.clear();
        p.mask.clear();
        return p;
    }
    return p;
}

bool MatchAt(const uint8_t* p, const Pattern& pat)
{
    for (size_t i = 0; i < pat.size(); ++i)
        if (pat.mask[i] && p[i] != pat.bytes[i])
            return false;
    return !pat.bytes.empty();
}

std::vector<uint8_t*> FindAll(uint8_t* begin, size_t len, const Pattern& pat, size_t maxHits)
{
    std::vector<uint8_t*> out;
    if (pat.bytes.empty() || len < pat.size())
        return out;
    const size_t last = len - pat.size();

    if (pat.mask[0])
    {
        const uint8_t first = pat.bytes[0];
        size_t i = 0;
        while (i <= last)
        {
            auto* hit = static_cast<uint8_t*>(std::memchr(begin + i, first, last - i + 1));
            if (!hit)
                break;
            if (MatchAt(hit, pat))
            {
                out.push_back(hit);
                if (out.size() >= maxHits)
                    break;
            }
            i = static_cast<size_t>(hit - begin) + 1;
        }
    }
    else
    {
        for (size_t i = 0; i <= last; ++i)
        {
            if (MatchAt(begin + i, pat))
            {
                out.push_back(begin + i);
                if (out.size() >= maxHits)
                    break;
            }
        }
    }
    return out;
}

uint8_t* FindFirst(uint8_t* begin, size_t len, const Pattern& pat)
{
    auto v = FindAll(begin, len, pat, 1);
    return v.empty() ? nullptr : v[0];
}

std::vector<uint8_t*> FindBytes(uint8_t* begin, size_t len, const void* needle, size_t n, size_t maxHits)
{
    std::vector<uint8_t*> out;
    if (n == 0 || len < n)
        return out;
    const auto* nd = static_cast<const uint8_t*>(needle);
    size_t i = 0;
    const size_t last = len - n;
    while (i <= last)
    {
        auto* hit = static_cast<uint8_t*>(std::memchr(begin + i, nd[0], last - i + 1));
        if (!hit)
            break;
        if (std::memcmp(hit, nd, n) == 0)
        {
            out.push_back(hit);
            if (out.size() >= maxHits)
                break;
        }
        i = static_cast<size_t>(hit - begin) + 1;
    }
    return out;
}
} // namespace wc::pattern
