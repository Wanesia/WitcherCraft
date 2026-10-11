#include "config.hpp"

#include "log.hpp"

#include <cstdlib>
#include <cwchar>

namespace wc
{
Config g_config;

void LoadConfig(HMODULE self)
{
    wchar_t path[MAX_PATH * 2]{};
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2)
        return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    wcscpy_s(slash + 1, MAX_PATH * 2 - (slash + 1 - path), L"witchercraft_native.ini");

    const wchar_t* sec = L"WitcherCraft";
    g_config.registerNatives = GetPrivateProfileIntW(sec, L"RegisterNatives", 1, path) != 0;
    g_config.logChannelFallback = GetPrivateProfileIntW(sec, L"LogChannelFallback", 1, path) != 0;
    g_config.verboseCalls = static_cast<int>(GetPrivateProfileIntW(sec, L"VerboseCalls", 20, path));
    g_config.overlay = GetPrivateProfileIntW(sec, L"Overlay", 1, path) != 0;
    g_config.overlayFallbackDelayMs = static_cast<int>(GetPrivateProfileIntW(sec, L"OverlayFallbackDelayMs", 20000, path));
    g_config.overlayPaperWhite = static_cast<int>(GetPrivateProfileIntW(sec, L"OverlayPaperWhite", 0, path));
    {
        // auto | sdr | scrgb | hdr10 (a string: GetPrivateProfileInt can't return negatives).
        wchar_t cs[32]{};
        GetPrivateProfileStringW(sec, L"OverlayColorSpace", L"auto", cs, 32, path);
        g_config.overlayColorSpace = _wcsicmp(cs, L"sdr") == 0     ? 0
                                     : _wcsicmp(cs, L"scrgb") == 0 ? 1
                                     : _wcsicmp(cs, L"hdr10") == 0 ? 12
                                                                   : -1;
    }
    g_config.overlayFrameMatch = GetPrivateProfileIntW(sec, L"OverlayFrameMatch", 1, path) != 0;
    g_config.overlayHistory = static_cast<int>(GetPrivateProfileIntW(sec, L"OverlayHistory", 6, path));
    g_config.overlayProbe = GetPrivateProfileIntW(sec, L"OverlayProbe", 1, path) != 0;
    {
        wchar_t lat[32]{}; // may be negative: read as a string
        GetPrivateProfileStringW(sec, L"OverlayLatencyFrames", L"-1", lat, 32, path);
        g_config.overlayLatencyFrames = _wtoi(lat);
    }
    wcscpy_s(g_config.iniPath, path);
    ReloadOcclusionConfig();
    WLOG("config: OverlayOcclusion=%d OcclusionBias=%.3f OcclusionSoft=%.3f OcclusionDepthPick=%d OcclusionReverseZ=%d "
         "OcclusionNear=%.3f OcclusionFar=%.1f model %s (A=%.6g B=%.6g) OcclusionDepthState=0x%X OcclusionDebug=%d",
         g_config.overlayOcclusion, g_config.occlusionBias, g_config.occlusionSoft, g_config.occlusionDepthPick,
         g_config.occlusionReverseZ, g_config.occlusionNear, g_config.occlusionFar, g_config.occlusionModelSet ? "set" : "auto",
         g_config.occlusionDepthA, g_config.occlusionDepthB, g_config.occlusionDepthState, g_config.occlusionDebug);
    WLOG("config: OverlayFrameMatch=%d OverlayLatencyFrames=%d OverlayHistory=%d OverlayProbe=%d", g_config.overlayFrameMatch,
         g_config.overlayLatencyFrames, g_config.overlayHistory, g_config.overlayProbe);
    WLOG("config: RegisterNatives=%d LogChannelFallback=%d VerboseCalls=%d (ini: %ls)", g_config.registerNatives,
         g_config.logChannelFallback, g_config.verboseCalls, path);
    WLOG("config: Overlay=%d OverlayFallbackDelayMs=%d OverlayPaperWhite=%d OverlayColorSpace=%d", g_config.overlay,
         g_config.overlayFallbackDelayMs, g_config.overlayPaperWhite, g_config.overlayColorSpace);
}

namespace
{
float IniFloat(const wchar_t* key, float def, bool* present = nullptr)
{
    wchar_t v[64]{};
    GetPrivateProfileStringW(L"WitcherCraft", key, L"", v, 64, g_config.iniPath);
    if (present)
        *present = v[0] != 0;
    return v[0] ? static_cast<float>(_wtof(v)) : def;
}

int IniInt(const wchar_t* key, int def)
{
    wchar_t v[64]{};
    GetPrivateProfileStringW(L"WitcherCraft", key, L"", v, 64, g_config.iniPath);
    if (!v[0])
        return def;
    return static_cast<int>(wcstol(v, nullptr, 0)); // decimal, or 0x... hex; may be negative
}
} // namespace

bool ReloadOcclusionConfig()
{
    if (!g_config.iniPath[0])
        return false;
    const Config before = g_config;
    g_config.overlayOcclusion = IniInt(L"OverlayOcclusion", 1) != 0;
    g_config.occlusionBias = IniFloat(L"OcclusionBias", 0.15f);
    g_config.occlusionSoft = IniFloat(L"OcclusionSoft", 0.10f);
    g_config.occlusionDepthPick = IniInt(L"OcclusionDepthPick", -1);
    g_config.occlusionReverseZ = IniInt(L"OcclusionReverseZ", -1);
    g_config.occlusionNear = IniFloat(L"OcclusionNear", 0.2f);
    g_config.occlusionFar = IniFloat(L"OcclusionFar", 0.0f);
    bool haveA = false, haveB = false;
    g_config.occlusionDepthA = IniFloat(L"OcclusionDepthA", 0.0f, &haveA);
    g_config.occlusionDepthB = IniFloat(L"OcclusionDepthB", 0.0f, &haveB);
    g_config.occlusionModelSet = haveA && haveB && g_config.occlusionDepthB != 0.0f;
    g_config.occlusionDepthState = static_cast<unsigned>(IniInt(L"OcclusionDepthState", 0));
    g_config.occlusionEyeHeight = IniFloat(L"OcclusionEyeHeight", 1.62f);
    g_config.occlusionKeepNear = IniFloat(L"OcclusionKeepNear", 1.0f);
    g_config.occlusionDebug = IniInt(L"OcclusionDebug", 0);
    // Off unless asked for: Ctrl+Shift+D is also sprint + sneak + strafe right in Minecraft mode.
    g_config.occlusionHotkeys = IniInt(L"OcclusionHotkeys", 0) != 0;
    g_config.overlayEdgeAA = IniInt(L"OverlayEdgeAA", 1) != 0;
    return before.overlayOcclusion != g_config.overlayOcclusion || before.occlusionBias != g_config.occlusionBias ||
           before.occlusionSoft != g_config.occlusionSoft || before.occlusionDepthPick != g_config.occlusionDepthPick ||
           before.occlusionReverseZ != g_config.occlusionReverseZ || before.occlusionNear != g_config.occlusionNear ||
           before.occlusionFar != g_config.occlusionFar || before.occlusionModelSet != g_config.occlusionModelSet ||
           before.occlusionDepthA != g_config.occlusionDepthA || before.occlusionDepthB != g_config.occlusionDepthB ||
           before.occlusionDepthState != g_config.occlusionDepthState || before.occlusionDebug != g_config.occlusionDebug || before.occlusionKeepNear != g_config.occlusionKeepNear ||
           before.overlayEdgeAA != g_config.overlayEdgeAA;
}
} // namespace wc
