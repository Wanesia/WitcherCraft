// Visual sync bookkeeping: which game frame (and so which Minecraft frame) each Present shows.
// See camsync.hpp.
#include "camsync.hpp"

#include "log.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace wc::camsync
{
namespace
{
constexpr int kGameRing = 1024;    // game frames remembered (~7 s at 140 fps)
constexpr int kPresentRing = 1024; // presents remembered
constexpr int kDWindow = 64;       // presents in the d = (script frames - presents) mode window
constexpr int kMaxLatency = 8;     // k candidates 0..kMaxLatency
constexpr int kEstimateWindow = 720;
constexpr int kDefaultLatency = 1; // before anything was measured (W3: the script runs a tick ahead)

struct GameRec
{
    uint64_t g = 0;
    bool driving = false;
    CamPose pose;
};

struct PresentRec
{
    uint64_t p = 0;
    int64_t dMode = 0;
    bool shiftValid = false;
    float shift = 0;
    int64_t dInst = 0; // this Present's own (script frames - presents), racy
};

SRWLOCK g_lock = SRWLOCK_INIT;
GameRec g_game[kGameRing];
PresentRec g_pres[kPresentRing];
std::atomic<uint64_t> g_gameFrames{0};
std::atomic<uint64_t> g_mcFrames{0};

// Present-thread state (under g_lock).
uint64_t g_presents = 0;
int64_t g_dWin[kDWindow]{};
int g_dCount = 0;
int64_t g_dMode = 0;
bool g_dModeValid = false;
uint64_t g_dSamples = 0, g_dOff = 0; // presents, and those whose instantaneous d differed from the mode
uint64_t g_probeNew = 0, g_probeTotal = 0;

std::atomic<int> g_latencySetting{-1};
std::atomic<int> g_latencyAuto{-1}; // -1 = not measured yet
std::atomic<bool> g_frameMatch{true};
ProbeResult g_lastProbe;
int g_pendingK = -1;
// "Off-mode" check: do the Presents whose instantaneous d differs from the mode show
// the camera of P + d_mode - k (a harmless sampling race) or of P + d_inst - k (a real pipeline
// change, then the mode mapping draws the wrong Minecraft frame for them)?
std::atomic<bool> g_useInstD{false};
float g_offCorrMode = 0, g_offCorrInst = 0;
uint64_t g_offSamples = 0;
int g_offVotes = 0;

// Rates (1 s windows).
uint64_t g_rateT0 = 0, g_rateP0 = 0, g_rateG0 = 0, g_rateM0 = 0;
float g_presentFps = 0, g_gameFps = 0, g_mcFps = 0;
int g_fgWindows = 0;
bool g_frameGen = false;
int g_slowWindows = 0;
uint64_t g_lastSlowWarnMs = 0;
uint64_t g_lastLogMs = 0;
bool g_drivingRecently = false;

// Picks.
std::atomic<uint64_t> g_pickExact{0}, g_pickNearest{0}, g_pickNewest{0};
std::atomic<uint64_t> g_nearestErrMilli{0}; // sum of degrees * 1000

GameSettings g_settings;
std::string g_warning;
bool g_warnedFg = false, g_warnedBlur = false;

float WrapDeg(float d)
{
    d = std::fmod(d, 360.0f);
    if (d > 180.0f)
        d -= 360.0f;
    else if (d < -180.0f)
        d += 360.0f;
    return d;
}

void Warn(const std::string& text)
{
    WLOG("sync: WARNING %s", text.c_str());
    if (g_warning.empty())
        g_warning = text;
    else if (g_warning.find(text) == std::string::npos)
        g_warning += "<br>" + text;
}

// Sticky mode of d over the window: a new value takes over when it holds 40 of the last 64, or
// the last 16 in a row (after a pause / menu the offset changes for good).
void UpdateDMode(int64_t d)
{
    g_dWin[g_dCount % kDWindow] = d;
    ++g_dCount;
    const int n = std::min(g_dCount, kDWindow);
    if (!g_dModeValid)
    {
        g_dMode = d;
        g_dModeValid = true;
        return;
    }
    if (d == g_dMode)
        return;
    bool run = g_dCount >= 16;
    for (int i = 1; i <= 16 && run; ++i)
        run = g_dWin[(g_dCount - i) % kDWindow] == d;
    int count = 0;
    for (int i = 0; i < n; ++i)
        count += g_dWin[i] == d;
    if (run || count >= 40)
        g_dMode = d;
}

const GameRec* FindGame(uint64_t g)
{
    if (g == 0)
        return nullptr;
    const GameRec& r = g_game[g % kGameRing];
    return r.g == g ? &r : nullptr;
}

// -yaw change of game frame g (deg, + = the picture should move right), or NAN.
float CommandedShift(uint64_t g)
{
    const GameRec* a = FindGame(g);
    const GameRec* b = g > 1 ? FindGame(g - 1) : nullptr;
    if (!a || !b || !a->driving || !b->driving)
        return NAN;
    return -WrapDeg(a->pose.yaw - b->pose.yaw);
}

// Correlates the probe's image shifts with the commanded yaw changes at every candidate lag.
// High-passed (first differences) so a steady turn doesn't make all lags look alike.
void Estimate()
{
    const uint64_t P = g_presents;
    float corr[kMaxLatency + 1];
    uint64_t used = ~0ull, moving = ~0ull;
    for (int k = 0; k <= kMaxLatency; ++k)
    {
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        uint64_t n = 0, mv = 0;
        for (uint64_t p = P > kEstimateWindow ? P - kEstimateWindow : 2; p < P; ++p)
        {
            const PresentRec& r = g_pres[p % kPresentRing];
            const PresentRec& q = g_pres[(p - 1) % kPresentRing];
            if (r.p != p || q.p != p - 1 || !r.shiftValid || !q.shiftValid)
                continue;
            const int64_t g1 = static_cast<int64_t>(p) + r.dMode - k;
            const int64_t g0 = static_cast<int64_t>(p - 1) + q.dMode - k;
            if (g1 < 2 || g0 < 2)
                continue;
            const float x1 = CommandedShift(static_cast<uint64_t>(g1)), x0 = CommandedShift(static_cast<uint64_t>(g0));
            if (std::isnan(x1) || std::isnan(x0))
                continue;
            // Camera cuts / teleports (huge steps) would dominate a correlation: leave them out.
            if (std::fabs(x1) > 8.0f || std::fabs(x0) > 8.0f)
                continue;
            const double x = x1 - x0, y = r.shift - q.shift;
            sx += x, sy += y, sxx += x * x, syy += y * y, sxy += x * y;
            ++n;
            mv += std::fabs(x) > 0.03;
        }
        const double cx = n ? sxx - sx * sx / n : 0, cy = n ? syy - sy * sy / n : 0, cxy = n ? sxy - sx * sy / n : 0;
        corr[k] = cx > 1e-9 && cy > 1e-9 ? static_cast<float>(cxy / std::sqrt(cx * cy)) : 0.0f;
        used = std::min(used, n);       // every candidate must have the data, or none is judged
        moving = std::min(moving, mv);
    }
    int best = 0;
    for (int k = 1; k <= kMaxLatency; ++k)
        if (corr[k] > corr[best])
            best = k;
    float second = -1;
    for (int k = 0; k <= kMaxLatency; ++k)
        if (k != best)
            second = std::max(second, corr[k]);
    // Need real turning (many frames whose yaw step changed by > 0.03 deg) and a clear winner.
    if (used < 240 || moving < 80)
        return;
    g_lastProbe = {best, corr[best], second, used};
    char line[256];
    int o = std::snprintf(line, sizeof(line), "sync: probe lag correlation over %llu presents:", static_cast<unsigned long long>(used));
    for (int k = 0; k <= kMaxLatency && o < static_cast<int>(sizeof(line)) - 12; ++k)
        o += std::snprintf(line + o, sizeof(line) - o, " k%d=%.2f", k, corr[k]);
    static uint64_t s_logs = 0;
    const bool confident = corr[best] >= 0.5f && corr[best] - second >= 0.15f;
    const int prevAuto = g_latencyAuto.load();
    if (s_logs++ < 20 || (confident && best != prevAuto))
        WLOG("%s -> best k=%d%s", line, best, confident ? "" : " (not confident)");
    if (!confident)
        return;
    // Two agreeing estimates in a row before switching (one if nothing was measured yet).
    if (prevAuto < 0 || best == g_pendingK)
    {
        if (best != prevAuto)
            WLOG("sync: measured latency k=%d (Present P shows the camera of game frame P + d - k; corr %.2f, next best %.2f)", best,
                 corr[best], second);
        g_latencyAuto = best;
    }
    g_pendingK = best;
}

// Correlation of the probe's image shifts with the commanded yaw steps, mapping off-mode Presents
// with d_mode (inst = false) or with their own d (inst = true). Same high-pass as Estimate().
float OffModeCorr(int k, bool inst, uint64_t& offUsed)
{
    const uint64_t P = g_presents;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    uint64_t n = 0;
    offUsed = 0;
    for (uint64_t p = P > kEstimateWindow ? P - kEstimateWindow : 2; p < P; ++p)
    {
        const PresentRec& r = g_pres[p % kPresentRing];
        const PresentRec& q = g_pres[(p - 1) % kPresentRing];
        if (r.p != p || q.p != p - 1 || !r.shiftValid || !q.shiftValid)
            continue;
        const bool off1 = r.dInst != r.dMode, off0 = q.dInst != q.dMode;
        const int64_t g1 = static_cast<int64_t>(p) + (inst && off1 ? r.dInst : r.dMode) - k;
        const int64_t g0 = static_cast<int64_t>(p - 1) + (inst && off0 ? q.dInst : q.dMode) - k;
        if (g1 < 2 || g0 < 2)
            continue;
        const float x1 = CommandedShift(static_cast<uint64_t>(g1)), x0 = CommandedShift(static_cast<uint64_t>(g0));
        if (std::isnan(x1) || std::isnan(x0) || std::fabs(x1) > 8.0f || std::fabs(x0) > 8.0f)
            continue;
        const double x = x1 - x0, y = r.shift - q.shift;
        sx += x, sy += y, sxx += x * x, syy += y * y, sxy += x * y;
        ++n;
        offUsed += off1 || off0;
    }
    const double cx = n ? sxx - sx * sx / n : 0, cy = n ? syy - sy * sy / n : 0, cxy = n ? sxy - sx * sy / n : 0;
    return cx > 1e-9 && cy > 1e-9 ? static_cast<float>(cxy / std::sqrt(cx * cy)) : 0.0f;
}

void CompareOffMode()
{
    const int k = LatencyInUse();
    uint64_t usedM = 0, usedI = 0;
    const float cm = OffModeCorr(k, false, usedM), ci = OffModeCorr(k, true, usedI);
    if (usedM < 60)
        return; // too few off-mode Presents while turning to judge
    g_offCorrMode = cm, g_offCorrInst = ci, g_offSamples = usedM;
    const bool instBetter = ci > cm + 0.05f && ci > 0.5f, modeBetter = cm > ci + 0.05f;
    g_offVotes = instBetter ? std::min(g_offVotes + 1, 3) : modeBetter ? std::max(g_offVotes - 1, -3) : g_offVotes;
    const bool want = g_offVotes >= 2;
    if (want != g_useInstD.load())
    {
        g_useInstD = want;
        WLOG("sync: off-mode Presents follow the %s d (corr %.2f with their own d vs %.2f with the mode, %llu off-mode samples): %s",
             want ? "INSTANTANEOUS" : "mode", ci, cm, static_cast<unsigned long long>(usedM),
             want ? "they are real pipeline changes, now mapped with their own d" : "a harmless sampling race, mapped with the mode");
    }
}

void Rates(uint64_t nowMs)
{
    if (g_rateT0 == 0)
    {
        g_rateT0 = nowMs, g_rateP0 = g_presents, g_rateG0 = g_gameFrames.load(), g_rateM0 = g_mcFrames.load();
        return;
    }
    const uint64_t dt = nowMs - g_rateT0;
    if (dt < 1000)
        return;
    const uint64_t G = g_gameFrames.load(), M = g_mcFrames.load();
    const float s = dt / 1000.0f;
    g_presentFps = (g_presents - g_rateP0) / s;
    g_gameFps = (G - g_rateG0) / s;
    g_mcFps = (M - g_rateM0) / s;
    g_rateT0 = nowMs, g_rateP0 = g_presents, g_rateG0 = G, g_rateM0 = M;

    // Frame generation: about two Presents per game frame while the game runs.
    const bool fgNow = g_gameFps > 20.0f && g_presentFps > 1.7f * g_gameFps;
    g_fgWindows = fgNow ? g_fgWindows + 1 : 0;
    if (g_fgWindows >= 3 && !g_frameGen)
    {
        g_frameGen = true;
        WLOG("sync: FRAME GENERATION suspected: %.0f presents/s for %.0f game frames/s", g_presentFps, g_gameFps);
    }
    else if (g_fgWindows == 0 && g_frameGen && g_gameFps > 20.0f)
    {
        g_frameGen = false;
        WLOG("sync: presents and game frames are 1:1 again (%.0f / %.0f per s)", g_presentFps, g_gameFps);
    }
    if (g_frameGen && !g_warnedFg)
    {
        g_warnedFg = true;
        Warn("Frame Generation looks ON. Minecraft's blocks cannot follow generated frames (they judder and smear): turn it OFF in "
             "Options > Video > Graphics.");
    }
    // Minecraft can't keep up with the Witcher's frame rate: the world moves in steps. Not while
    // Minecraft draws nothing at all (a loading screen or fast travel: it waits, presents go on).
    const bool slow = g_drivingRecently && g_gameFps > 30.0f && g_mcFps >= 1.0f && g_mcFps < 0.85f * g_gameFps;
    g_slowWindows = slow ? g_slowWindows + 1 : 0;
    if (g_slowWindows == 10 && nowMs - g_lastSlowWarnMs > 300000)
    {
        g_lastSlowWarnMs = nowMs;
        char buf[256];
        const int cap = g_mcFps >= 115 ? 120 : g_mcFps >= 85 ? 90 : g_mcFps >= 55 ? 60 : 30;
        std::snprintf(buf, sizeof(buf),
                      "Minecraft draws %.0f fps but The Witcher runs %.0f fps, so the view moves in uneven steps. Lower the frame rate "
                      "limit (Options > Video > Graphics) to %d.",
                      g_mcFps, g_gameFps, cap);
        Warn(buf);
    }
}
} // namespace

void OnGameFrame(bool driving, const CamPose& pose)
{
    AcquireSRWLockExclusive(&g_lock);
    const uint64_t g = g_gameFrames.load(std::memory_order_relaxed) + 1;
    GameRec& r = g_game[g % kGameRing];
    r.g = g;
    r.driving = driving;
    r.pose = pose;
    g_gameFrames.store(g, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lock);
}

uint64_t GameFrames()
{
    return g_gameFrames.load(std::memory_order_acquire);
}

PresentInfo OnPresent()
{
    PresentInfo info;
    const uint64_t nowMs = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    const uint64_t P = ++g_presents;
    const uint64_t S = g_gameFrames.load(std::memory_order_acquire);
    const int64_t dInst = static_cast<int64_t>(S) - static_cast<int64_t>(P);
    UpdateDMode(dInst);
    ++g_dSamples;
    if (dInst != g_dMode)
        ++g_dOff; // the script thread was not where it usually is at this Present (timing race)
    PresentRec& pr = g_pres[P % kPresentRing];
    pr = {P, g_dMode, false, 0.0f, dInst};
    const int setting = g_latencySetting.load();
    const int k = setting >= 0 ? setting : g_latencyAuto.load() >= 0 ? g_latencyAuto.load() : kDefaultLatency;
    // Off by more than one frame = a pause/hitch, never a race: those keep the mode.
    const bool useInst = g_useInstD.load() && dInst != g_dMode && std::llabs(dInst - g_dMode) == 1;
    int64_t g = static_cast<int64_t>(P) + (useInst ? dInst : g_dMode) - k;
    if (g > static_cast<int64_t>(S))
        g = static_cast<int64_t>(S); // can't show a frame that wasn't scripted yet
    info.present = P;
    info.latency = k;
    if (g >= 1)
    {
        info.gameFrame = static_cast<uint64_t>(g);
        if (const GameRec* r = FindGame(info.gameFrame))
        {
            info.driving = r->driving;
            info.havePose = r->driving;
            info.pose = r->pose;
        }
    }
    if (const GameRec* last = FindGame(S))
        g_drivingRecently = last->driving;
    Rates(nowMs);
    info.frameGen = g_frameGen;
    if (P % 120 == 0 && g_probeNew >= 60 && setting < 0)
    {
        g_probeNew = 0;
        Estimate();
        CompareOffMode();
    }
    const bool logNow = nowMs - g_lastLogMs >= 10000;
    if (logNow)
        g_lastLogMs = nowMs;
    ReleaseSRWLockExclusive(&g_lock);
    if (logNow)
        WLOG("%s", StatusLine().c_str());
    return info;
}

void OnProbeShift(uint64_t present, bool valid, float shiftPx)
{
    AcquireSRWLockExclusive(&g_lock);
    PresentRec& r = g_pres[present % kPresentRing];
    if (r.p == present)
    {
        r.shiftValid = valid;
        r.shift = shiftPx;
        if (valid)
            ++g_probeNew, ++g_probeTotal;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void OnMcFrame()
{
    g_mcFrames.fetch_add(1, std::memory_order_relaxed);
}

void OnPicked(Pick how, float poseErrDeg)
{
    switch (how)
    {
    case Pick::Exact:
        ++g_pickExact;
        break;
    case Pick::Nearest:
        ++g_pickNearest;
        g_nearestErrMilli += static_cast<uint64_t>(std::min(poseErrDeg, 1000.0f) * 1000.0f);
        break;
    default:
        ++g_pickNewest;
        break;
    }
}

void SetLatency(int k)
{
    k = std::clamp(k, -1, kMaxLatency);
    g_latencySetting = k;
    WLOG("sync: OverlayLatencyFrames=%d (%s)", k, k < 0 ? "measured by the image probe" : "fixed");
}

int LatencySetting()
{
    return g_latencySetting.load();
}

int LatencyInUse()
{
    const int s = g_latencySetting.load();
    return s >= 0 ? s : g_latencyAuto.load() >= 0 ? g_latencyAuto.load() : kDefaultLatency;
}

void SetFrameMatch(bool on)
{
    g_frameMatch = on;
    WLOG("sync: OverlayFrameMatch=%d (%s)", on ? 1 : 0,
         on ? "draw the Minecraft frame W3 rendered with" : "always draw the newest Minecraft frame (old behaviour)");
}

bool FrameMatch()
{
    return g_frameMatch.load();
}

void SetGameSettings(const GameSettings& s)
{
    AcquireSRWLockExclusive(&g_lock);
    g_settings = s;
    WLOG("sync: W3 graphics settings: frame generation %s, motion blur %s, blur %s, frame rate limit %d, vsync %s", s.frameGen ? "ON" : "off",
         s.motionBlur ? "ON" : "off", s.blur ? "ON" : "off", s.limitFps, s.vsync ? "on" : "off");
    if (s.frameGen && !g_warnedFg)
    {
        g_warnedFg = true;
        Warn("Frame Generation is ON. Minecraft's blocks cannot follow generated frames (they judder and smear): turn it OFF in "
             "Options > Video > Graphics.");
    }
    if ((s.motionBlur || s.blur) && !g_warnedBlur)
    {
        g_warnedBlur = true;
        Warn("Motion Blur / Blur is ON. The Minecraft layer is never blurred, so the mix looks smeared while you turn: turn both OFF "
             "in Options > Video > Graphics.");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

std::string StatusLine()
{
    AcquireSRWLockShared(&g_lock);
    const uint64_t ex = g_pickExact.load(), ne = g_pickNearest.load(), nw = g_pickNewest.load();
    char buf[700];
    std::snprintf(buf, sizeof(buf),
                  "sync: presents %.0f/s, game frames %.0f/s, MC frames %.0f/s | frame gen %s | d=%lld (off-mode %.1f%%) k=%d (%s; probe "
                  "k=%d corr %.2f vs %.2f, %llu shifts) | off-mode presents follow %s (corr own d %.2f vs mode %.2f, %llu) | picks exact %llu "
                  "nearest %llu (mean %.2f deg) newest %llu | match %s",
                  g_presentFps, g_gameFps, g_mcFps, g_frameGen ? "SUSPECTED" : "no", static_cast<long long>(g_dMode),
                  g_dSamples ? 100.0 * g_dOff / g_dSamples : 0.0, LatencyInUse(),
                  g_latencySetting.load() >= 0 ? "fixed" : g_latencyAuto.load() >= 0 ? "measured" : "default", g_lastProbe.k,
                  g_lastProbe.corr, g_lastProbe.runnerUp, static_cast<unsigned long long>(g_probeTotal),
                  g_offSamples ? (g_useInstD ? "their own d" : "the mode") : "? (turn to measure)", g_offCorrInst, g_offCorrMode,
                  static_cast<unsigned long long>(g_offSamples), static_cast<unsigned long long>(ex), static_cast<unsigned long long>(ne),
                  ne ? g_nearestErrMilli.load() / 1000.0 / ne : 0.0, static_cast<unsigned long long>(nw), g_frameMatch ? "on" : "OFF");
    ReleaseSRWLockShared(&g_lock);
    return buf;
}

std::string ConsumeWarning()
{
    AcquireSRWLockExclusive(&g_lock);
    std::string w;
    w.swap(g_warning);
    ReleaseSRWLockExclusive(&g_lock);
    return w;
}

ProbeResult LastProbe()
{
    AcquireSRWLockShared(&g_lock);
    const ProbeResult r = g_lastProbe;
    ReleaseSRWLockShared(&g_lock);
    return r;
}
} // namespace wc::camsync
