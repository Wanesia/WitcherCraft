#include "log_channel.hpp"

#include "log.hpp"
#include "natives.hpp"
#include "red_engine.hpp"

#include <Windows.h>
#include <atomic>
#include <cstring>
#include <string>

namespace wc::logchannel
{
namespace
{
using Sink = void (*)(const uint32_t* channel, const red::RedString* text);

std::atomic<Sink> g_prev{nullptr};
std::atomic<long> g_hits{0};
constexpr char kPrefix[] = "WCRAFT:";

void OurSink(const uint32_t* channel, const red::RedString* text)
{
    if (text && text->data && text->size > sizeof(kPrefix) - 1 &&
        std::strncmp(text->data, kPrefix, sizeof(kPrefix) - 1) == 0)
    {
        std::string msg(text->data + sizeof(kPrefix) - 1, strnlen(text->data, text->size) - (sizeof(kPrefix) - 1));
        long n = ++g_hits;
        std::string reply = natives::HandleRpc(msg);
        if (n <= 50)
            WLOG("log-channel: \"%s\" -> \"%s\"", msg.c_str(), reply.c_str());
    }
    Sink prev = g_prev.load();
    if (prev && prev != &OurSink)
        prev(channel, text);
}

DWORD WINAPI Watchdog(LPVOID)
{
    // The engine (re)installs its own sink during init (e.g. RVA 0x227F7D5 / 0x24D4E92),
    // so keep re-chaining ourselves in front of whatever is there.
    void** slot = red::g.logSinkSlot;
    for (;;)
    {
        void* cur = *reinterpret_cast<void* volatile*>(slot);
        if (cur != reinterpret_cast<void*>(&OurSink))
        {
            g_prev.store(reinterpret_cast<Sink>(cur));
            if (InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(&OurSink), cur) == cur)
                WLOG("log-channel: sink installed (chained previous %p)", cur);
        }
        Sleep(250);
    }
}
} // namespace

std::string_view Prefix()
{
    return kPrefix;
}

bool Start()
{
    if (!red::g.logSinkSlot)
        return false;
    HANDLE t = CreateThread(nullptr, 0, &Watchdog, nullptr, 0, nullptr);
    if (!t)
        return false;
    CloseHandle(t);
    return true;
}
} // namespace wc::logchannel
