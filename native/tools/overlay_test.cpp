// Offline overlay test: stands in for The Witcher 3's renderer AND its scripts.
//
//   overlay_test.exe [seconds=420] [sdr|scrgb|hdr10] [out.bmp] [width=1600] [height=900] [early|late] [world]
//
// "world" (M3b world layer, needs MC started with -Dwitchercraft.worldLayer=true): the player looks
// due south (MC yaw 0) and 25 degrees down; once MC's player has arrived, the host types /fill,
// /setblock and /summon commands that build a little scene in front of it (grass floor, stone
// brick wall, log, glass, crafting table, campfire smoke, a pig, a dropped diamond) and puts a
// pickaxe in the hand. The capture is taken ~10 s after the last command.
//
// - Creates its own window, D3D12 device, a DIRECT "present" queue, a second DIRECT "decoy" queue
//   (submits right before every Present, the worst case for a timing-based queue guess) and a
//   flip-model swapchain in the requested format/colour space.
// - Installs the overlay exactly like the DLL: "early" = factory hooks first, then the swapchain
//   is created through them (the in-game path); "late" = swapchain first, then the dummy-device
//   fallback (the path used if the game's swapchain already existed).
// - Starts the SkyCraft host (same code as dinput8.dll) and feeds a standing player + flat
//   ground like fake_host, so the hidden Minecraft links and joins its world.
// - Draws a checkerboard "game frame" each frame and presents; the overlay hook composites MC's
//   HUD on top. Once HUD frames are flowing it captures the composited back buffer to a BMP.
#include "buildmode.hpp"
#include "camsync.hpp"
#include "combat.hpp"
#include "config.hpp"
#include "control.hpp"
#include "log.hpp"
#include "overlay_dx12.hpp"
#include "skycraft_host.hpp"
#include "skycraft_protocol.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
volatile bool g_quit = false;
BOOL WINAPI OnCtrl(DWORD)
{
    g_quit = true;
    return TRUE;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE)
    {
        g_quit = true;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int g_mode = 0; // 0 sdr, 1 scrgb, 2 hdr10
const float kPaperWhite = 200.0f;

float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float Pq(float nits)
{
    const float y = std::pow(std::fmin(std::fmax(nits / 10000.0f, 0.0f), 1.0f), 0.1593017578125f);
    return std::pow((0.8359375f + 18.8515625f * y) / (1.0f + 18.6875f * y), 78.84375f);
}
// An sRGB colour as this back buffer wants it (so the test pattern looks the same in every mode).
void Encode(const float srgb[3], float out[4])
{
    out[3] = 1.0f;
    if (g_mode == 0)
    {
        out[0] = srgb[0], out[1] = srgb[1], out[2] = srgb[2];
        return;
    }
    const float l[3] = {SrgbToLinear(srgb[0]), SrgbToLinear(srgb[1]), SrgbToLinear(srgb[2])};
    if (g_mode == 1)
    {
        for (int i = 0; i < 3; ++i)
            out[i] = l[i] * kPaperWhite / 80.0f;
        return;
    }
    const float r = 0.6274040f * l[0] + 0.3292820f * l[1] + 0.0433136f * l[2];
    const float g = 0.0690970f * l[0] + 0.9195400f * l[1] + 0.0113612f * l[2];
    const float b = 0.0163916f * l[0] + 0.0880132f * l[1] + 0.8955950f * l[2];
    out[0] = Pq(r * kPaperWhite), out[1] = Pq(g * kPaperWhite), out[2] = Pq(b * kPaperWhite);
}

// ---- sync mode: a W3-like pipeline and a world-anchored reference ----------------
// A camera in Minecraft's convention (what MC renders with; JOML rotationYXZ(180-yaw, -pitch, roll)).
struct McCam
{
    double eye[3] = {10.0, 65.62, 5.0};
    float yaw = 0, pitch = 25, roll = 0, fov = 70;
};

McCam FromFrame(const wc::control::Frame& f)
{
    McCam c;
    c.eye[0] = f.eye[0], c.eye[1] = f.eye[2], c.eye[2] = -f.eye[1];
    c.yaw = 180.0f - f.headingDeg;
    c.pitch = -f.pitchUpDeg;
    c.roll = f.rollDeg;
    c.fov = f.mcFovDeg >= 10.0f ? f.mcFovDeg : 70.0f;
    return c;
}

McCam FromPose(const wc::overlay::Pose& p)
{
    McCam c;
    c.eye[0] = p.eye[0], c.eye[1] = p.eye[1], c.eye[2] = p.eye[2];
    c.yaw = p.yaw, c.pitch = p.pitch, c.roll = p.rollDeg, c.fov = p.fovDeg;
    return c;
}

// World point -> back-buffer pixel with MC's camera and projection. False if behind the camera.
bool Project(const McCam& c, double W, double H, const double p[3], double& sx, double& sy, double* depth = nullptr)
{
    const double d2r = 3.14159265358979 / 180.0;
    const double a = (180.0 - c.yaw) * d2r, b = -c.pitch * d2r, r = c.roll * d2r;
    const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cr = std::cos(r), sr = std::sin(r);
    // M = Ry(a) * Rx(b) * Rz(r): camera -> world. local = M^T (p - eye).
    const double Ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
    const double Rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
    const double Rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    double T[3][3]{}, M[3][3]{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                T[i][j] += Ry[i][k] * Rx[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                M[i][j] += T[i][k] * Rz[k][j];
    const double v[3] = {p[0] - c.eye[0], p[1] - c.eye[1], p[2] - c.eye[2]};
    double l[3];
    for (int i = 0; i < 3; ++i)
        l[i] = M[0][i] * v[0] + M[1][i] * v[1] + M[2][i] * v[2];
    if (-l[2] < 0.05)
        return false;
    const double tv = std::tan(c.fov * 0.5 * d2r), th = tv * W / H;
    sx = (l[0] / -l[2] / th * 0.5 + 0.5) * W;
    sy = (0.5 - l[1] / -l[2] / tv * 0.5) * H;
    if (depth)
        *depth = -l[2];
    return true;
}

void PushRect(std::vector<D3D12_RECT>& out, double x0, double y0, double x1, double y1, UINT W, UINT H)
{
    const LONG l = static_cast<LONG>(std::fmax(0.0, std::floor(x0))), t = static_cast<LONG>(std::fmax(0.0, std::floor(y0)));
    const LONG r = static_cast<LONG>(std::fmin(double(W), std::ceil(x1))), b = static_cast<LONG>(std::fmin(double(H), std::ceil(y1)));
    if (r > l && b > t)
        out.push_back({l, t, r, b});
}

// A world-space segment drawn as a dotted chain of squares (ClearRenderTargetView rects).
void Line(std::vector<D3D12_RECT>& out, const McCam& c, UINT W, UINT H, const double a[3], const double b[3], double thick)
{
    double ax, ay, bx, by;
    if (!Project(c, W, H, a, ax, ay) || !Project(c, W, H, b, bx, by))
        return;
    const int n = static_cast<int>(std::fmax(std::fabs(bx - ax), std::fabs(by - ay)) / 1.5) + 1;
    for (int i = 0; i <= n && n < 8000; ++i)
    {
        const double x = ax + (bx - ax) * i / n, y = ay + (by - ay) * i / n;
        PushRect(out, x - thick * 0.5, y - thick * 0.5, x + thick * 0.5, y + thick * 0.5, W, H);
    }
}

uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// The scene's stone-brick wall front face (fill 6 64 12 14 66 12): x 6..15, y 64..67, z = 12.
const double kWall[4][3] = {{6, 64, 12}, {15, 64, 12}, {15, 67, 12}, {6, 67, 12}};

// Pixel distance between where MC drew the wall (pose `drawn`) and where it belongs in this W3
// image (pose `truth`): the max over the face's corners and centre.
double BlockErrorPx(const McCam& truth, const McCam& drawn, UINT W, UINT H)
{
    const double pts[5][3] = {{6, 64, 12}, {15, 64, 12}, {15, 67, 12}, {6, 67, 12}, {10.5, 65.5, 12}};
    double worst = -1;
    for (const auto& p : pts)
    {
        double ax, ay, bx, by;
        if (!Project(truth, W, H, p, ax, ay) || !Project(drawn, W, H, p, bx, by))
            continue;
        worst = std::fmax(worst, std::hypot(ax - bx, ay - by));
    }
    return worst;
}

bool PoseFromOverlay(wc::control::OverlayPose& out)
{
    wc::overlay::Pose p;
    if (!wc::overlay::LastDrawnPose(p))
        return false;
    out.frameId = p.frameId;
    out.takenMs = p.takenMs;
    out.eye[0] = p.eye[0], out.eye[1] = p.eye[1], out.eye[2] = p.eye[2];
    out.yaw = p.yaw, out.pitch = p.pitch, out.fovDeg = p.fovDeg, out.rollDeg = p.rollDeg;
    return true;
}

struct PhaseStats
{
    const char* name = "";
    std::vector<double> err;
    uint64_t exact = 0, nearest = 0, newest = 0, presents = 0, mc0 = 0, mc1 = 0;
    double t0 = 0, t1 = 0;
};

void Report(const PhaseStats& s, double& mean, double& p95, double& mx)
{
    std::vector<double> e = s.err;
    std::sort(e.begin(), e.end());
    mean = 0;
    for (double v : e)
        mean += v;
    mean = e.empty() ? 0 : mean / e.size();
    p95 = e.empty() ? 0 : e[std::min(e.size() - 1, static_cast<size_t>(e.size() * 0.95))];
    mx = e.empty() ? 0 : e.back();
    const double secs = s.t1 - s.t0;
    WLOG("overlay_test: SYNC phase '%s': %llu presents (%.0f/s), MC frames %.0f/s | block error px: mean %.2f p95 %.2f max %.2f over %zu "
         "measured | picks exact %llu nearest %llu newest %llu",
         s.name, static_cast<unsigned long long>(s.presents), secs > 0 ? s.presents / secs : 0.0,
         secs > 0 ? (s.mc1 - s.mc0) / secs : 0.0, mean, p95, mx, e.size(), static_cast<unsigned long long>(s.exact),
         static_cast<unsigned long long>(s.nearest), static_cast<unsigned long long>(s.newest));
}

// ---- occlusion mode: a W3-like scene + depth buffer ------------------------
// W3 geometry in MC coordinates: flat ground at y = 64 (exactly MC's grass top: tests the bias),
// pillar A in FRONT of MC's wall (must hide it), pillar B BEHIND the wall (must not).
enum OccKind
{
    kOccSky = 0,
    kOccGround,
    kOccPillarA,
    kOccPillarB
};

struct OccHit
{
    int kind = kOccSky;
    double z = 0; // view-axis distance (m)
};

OccHit TraceW3(const McCam& c, double aspect, double u, double v)
{
    const double d2r = 3.14159265358979 / 180.0;
    const double a = (180.0 - c.yaw) * d2r, b = -c.pitch * d2r, r = c.roll * d2r;
    const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cr = std::cos(r), sr = std::sin(r);
    const double Ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
    const double Rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
    const double Rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
    double T[3][3]{}, M[3][3]{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                T[i][j] += Ry[i][k] * Rx[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                M[i][j] += T[i][k] * Rz[k][j];
    const double tv = std::tan(c.fov * 0.5 * d2r), th = tv * aspect;
    const double l[3] = {u * th, v * tv, -1.0};
    double dir[3];
    for (int i = 0; i < 3; ++i)
        dir[i] = M[i][0] * l[0] + M[i][1] * l[1] + M[i][2] * l[2];
    OccHit h;
    double best = 1e30;
    if (dir[1] < -1e-9)
    {
        const double t = (64.0 - c.eye[1]) / dir[1];
        if (t > 0 && t < best)
            best = t, h.kind = kOccGround;
    }
    struct Pillar
    {
        double z, x0, x1, y0, y1;
        int kind;
    };
    const Pillar pillars[2] = {{7.0, 10.5, 11.5, 64.0, 69.0, kOccPillarA}, {13.5, 5.0, 7.5, 64.0, 70.0, kOccPillarB}};
    for (const Pillar& p : pillars)
        if (dir[2] > 1e-9)
        {
            const double t = (p.z - c.eye[2]) / dir[2];
            const double px = c.eye[0] + t * dir[0], py = c.eye[1] + t * dir[1];
            if (t > 0 && t < best && px >= p.x0 && px <= p.x1 && py >= p.y0 && py <= p.y1)
                best = t, h.kind = p.kind;
        }
    if (h.kind != kOccSky)
        h.z = best;
    return h;
}

struct OccRun
{
    D3D12_RECT rect;
    int kind;
    double z;
};

// Runs of equal kind per row of a W x H image of the W3 scene.
std::vector<OccRun> TraceImage(const McCam& c, UINT W, UINT H)
{
    std::vector<OccRun> runs;
    const double aspect = double(W) / H;
    for (UINT y = 0; y < H; ++y)
    {
        const double v = 1.0 - 2.0 * (y + 0.5) / H;
        int cur = -1;
        UINT x0 = 0;
        double z = 0;
        for (UINT x = 0; x <= W; ++x)
        {
            OccHit h;
            if (x < W)
                h = TraceW3(c, aspect, 2.0 * (x + 0.5) / W - 1.0, v);
            if (x == W || h.kind != cur)
            {
                if (cur >= 0)
                    runs.push_back({{LONG(x0), LONG(y), LONG(x), LONG(y + 1)}, cur, z});
                cur = h.kind;
                x0 = x;
                z = h.z;
            }
        }
    }
    return runs;
}

// Pixel of a 32-bit top-down BMP written by the overlay capture.
bool BmpPixel(const std::wstring& path, int x, int y, uint8_t rgb[3])
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f)
        return false;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fread(&fh, sizeof(fh), 1, f);
    fread(&ih, sizeof(ih), 1, f);
    const int w = ih.biWidth, h = ih.biHeight < 0 ? -ih.biHeight : ih.biHeight;
    bool ok = false;
    if (x >= 0 && y >= 0 && x < w && y < h)
    {
        const int row = ih.biHeight < 0 ? y : h - 1 - y;
        fseek(f, long(fh.bfOffBits + (size_t(row) * w + x) * 4), SEEK_SET);
        uint8_t px[4]{};
        ok = fread(px, 4, 1, f) == 1;
        rgb[0] = px[2], rgb[1] = px[1], rgb[2] = px[0];
    }
    fclose(f);
    return ok;
}

bool NearColour(const uint8_t rgb[3], const float srgb[3], int tol)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(int(rgb[i]) - int(srgb[i] * 255.0f + 0.5f)) > tol)
            return false;
    return true;
}

#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        const HRESULT hr_ = (x);                                                                                       \
        if (FAILED(hr_))                                                                                               \
        {                                                                                                              \
            WLOG("overlay_test: %s failed 0x%08lX", #x, static_cast<unsigned long>(hr_));                              \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
} // namespace

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 420;
    const std::string modeArg = argc > 2 ? argv[2] : "sdr";
    std::wstring out = L"overlay_test.bmp";
    if (argc > 3)
    {
        wchar_t w[MAX_PATH]{};
        MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, w, MAX_PATH);
        out = w;
    }
    const UINT width = argc > 4 ? static_cast<UINT>(std::atoi(argv[4])) : 1600;
    const UINT height = argc > 5 ? static_cast<UINT>(std::atoi(argv[5])) : 900;
    const bool late = argc > 6 && std::string(argv[6]) == "late";
    // sync [lag=2] [fps=120]: W3-like pipeline. The "Witcher" image of each Present
    // is drawn with the camera the script placed `lag` game frames earlier; the test runs at `fps`
    // and turns the camera continuously; a world-anchored outline marks where MC's wall belongs.
    const bool sync = argc > 8 && std::string(argv[8]) == "sync";
    // occl [rev|std] [scale]: W3-like depth buffer (reversed or standard Z, at scale x the back
    // buffer = an upscaler's render resolution) with pillars in front of / behind MC's wall.
    const bool occl = argc > 8 && std::string(argv[8]) == "occl";
    // "tip" (with "world"): press M as Steve ~2 s before the capture. With the WitcherCraft option
    // "Panel keys" off in that Minecraft, its toast "M: the Witcher's map is turned off" is in the picture.
    const bool tip = argc > 8 && std::string(argv[8]) == "tip";
    // "names" (with "world"): two fake Witcher actors, as the script publishes them: "Bandit" 4.5
    // blocks in front of Steve and "Far Bandit" 21 blocks away. With the WitcherCraft option
    // "Minecraft name tags" on, only "Bandit" must carry a name tag in the picture (range 16).
    const bool names = argc > 8 && std::string(argv[8]) == "names";
    // dyn: W3 5.0-like DYNAMIC RESOLUTION: a pool of D32S8 depth buffers (one per scale), the one in
    // use changing every 0.4 s, a full-resolution decoy depth used a little each frame, and the whole
    // pool destroyed and recreated mid-run (pointers get reused).
    const bool occlDyn = argc > 9 && std::string(argv[9]) == "dyn";
    const bool occlRev = !(argc > 9 && std::string(argv[9]) == "std");
    const double occlScale = argc > 10 ? std::atof(argv[10]) : 1.0;
    const bool world = sync || occl || names || (argc > 7 && std::string(argv[7]) == "world");
    const int kSim = argc > 9 && !occl ? std::atoi(argv[9]) : 2;
    const int testFps = argc > 10 ? std::atoi(argv[10]) : 120;
    std::wstring outBase = out;
    if (outBase.size() > 4 && outBase.substr(outBase.size() - 4) == L".bmp")
        outBase.resize(outBase.size() - 4);
    g_mode = modeArg == "scrgb" ? 1 : modeArg == "hdr10" ? 2 : 0;
    const DXGI_FORMAT fmt = g_mode == 1   ? DXGI_FORMAT_R16G16B16A16_FLOAT
                            : g_mode == 2 ? DXGI_FORMAT_R10G10B10A2_UNORM
                                          : DXGI_FORMAT_R8G8B8A8_UNORM;

    SetConsoleCtrlHandler(OnCtrl, TRUE);
    wc::log::InitConsole();
    WLOG("overlay_test: %d s, %s, %ux%u, %s install, capture -> %ls", seconds, modeArg.c_str(), width, height,
         late ? "late (dummy device)" : "early (factory hook)", out.c_str());
    wc::g_config.overlay = true;
    wc::g_config.overlayPaperWhite = static_cast<int>(kPaperWhite);
    if (const char* oc = std::getenv("WC_OCCLUSION")) // regression runs: 0 = no depth hooks at all
        wc::g_config.overlayOcclusion = std::atoi(oc) != 0;
    if (const char* aa = std::getenv("WC_EDGEAA"))
        wc::g_config.overlayEdgeAA = std::atoi(aa) != 0; // before/after captures
    const double kOccNear = 0.1, kOccFar = 1000.0;
    if (occl)
    {
        wc::g_config.overlayOcclusion = true;
        wc::g_config.occlusionNear = static_cast<float>(kOccNear);
        wc::g_config.occlusionFar = occlRev ? 0.0f : static_cast<float>(kOccFar);
        wc::g_config.occlusionReverseZ = -1; // auto-detected from the clear value
        wc::g_config.occlusionHotkeys = false;
        WLOG("overlay_test: OCCLUSION mode: %s Z, W3 depth at %.3f x the back buffer, near %.2f far %s", occlRev ? "reversed" : "standard",
             occlScale, kOccNear, occlRev ? "infinite" : "1000");
    }

    // Window.
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WitcherCraftOverlayTest";
    RegisterClassExW(&wc);
    RECT r{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"WitcherCraft overlay test", WS_OVERLAPPEDWINDOW, 40, 40, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    if (!late && !wc::overlay::InstallNow(false))
    {
        WLOG("overlay_test: factory hook install failed");
        return 1;
    }

    // Device + queues.
    IDXGIFactory6* factory = nullptr;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    IDXGIAdapter1* adapter = nullptr;
    factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    ID3D12Device* device = nullptr;
    CHECK(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* decoy = nullptr;
    ID3D12CommandQueue* copyQueue = nullptr;
    CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&decoy)));
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    CHECK(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&copyQueue)));
    WLOG("overlay_test: device %p, present queue %p, decoy DIRECT queue %p, copy queue %p", static_cast<void*>(device),
         static_cast<void*>(queue), static_cast<void*>(decoy), static_cast<void*>(copyQueue));

    // Swapchain.
    constexpr UINT kBuffers = 3;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = fmt;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    CHECK(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &sc1));
    IDXGISwapChain3* sc = nullptr;
    CHECK(sc1->QueryInterface(IID_PPV_ARGS(&sc)));
    sc1->Release();
    WLOG("overlay_test: swapchain %p (%s)", static_cast<void*>(sc), modeArg.c_str());

    if (late && !wc::overlay::InstallNow(true))
    {
        WLOG("overlay_test: dummy-device hook install failed");
        return 1;
    }

    if (g_mode != 0)
    {
        const DXGI_COLOR_SPACE_TYPE cs =
            g_mode == 1 ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        UINT support = 0;
        sc->CheckColorSpaceSupport(cs, &support);
        const HRESULT hr = (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ? sc->SetColorSpace1(cs) : E_FAIL;
        WLOG("overlay_test: colour space %d support 0x%X, SetColorSpace1 -> 0x%08lX", cs, support, static_cast<unsigned long>(hr));
        if (FAILED(hr))
        {
            // SDR display: the swapchain can't be put in that colour space, so tell the overlay
            // explicitly (ini override path) - the capture decodes it the same way.
            wc::g_config.overlayColorSpace = cs;
            WLOG("overlay_test: forcing OverlayColorSpace=%d (ini override path)", cs);
        }
    }

    // Our own "game" rendering resources.
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBuffers;
    CHECK(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
    const UINT rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12CommandAllocator* alloc[kBuffers]{};
    ID3D12GraphicsCommandList* list[kBuffers]{};
    UINT64 frameFence[kBuffers]{};
    for (UINT i = 0; i < kBuffers; ++i)
    {
        CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i])));
        CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[i], nullptr, IID_PPV_ARGS(&list[i])));
        CHECK(list[i]->Close());
    }
    ID3D12CommandAllocator* decoyAlloc = nullptr;
    ID3D12GraphicsCommandList* decoyList = nullptr;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&decoyAlloc)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, decoyAlloc, nullptr, IID_PPV_ARGS(&decoyList)));
    CHECK(decoyList->Close());
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Occlusion mode: the "W3" depth buffer(s), created after the swapchain like the game does.
    struct W3Depth
    {
        ID3D12Resource* res = nullptr;
        UINT w = 0, h = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        std::vector<OccRun> runs;
    };
    std::vector<W3Depth> pool;
    ID3D12Resource* shadow = nullptr; // decoy: a square shadow map (never a candidate)
    ID3D12Resource* uiDepth = nullptr; // dyn decoy: full-res depth, used a little each frame, always far
    D3D12_CPU_DESCRIPTOR_HANDLE uiDsv{};
    ID3D12DescriptorHeap* dsvHeap = nullptr;
    UINT dsvInc = 0;
    const double dynScales[] = {0.5, 0.625, 0.75, 0.875, 1.0};
    const D3D12_RESOURCE_STATES poolRest = occlDyn ? static_cast<D3D12_RESOURCE_STATES>(0xE0) : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    int poolGen = 0;
    auto createPool = [&]() -> bool {
        const int n = occlDyn ? 5 : 1;
        for (int i = 0; i < n; ++i)
        {
            const double sc = occlDyn ? dynScales[i] : occlScale;
            W3Depth e;
            e.w = std::max(1u, static_cast<UINT>(width * sc + 0.5));
            e.h = std::max(1u, static_cast<UINT>(height * sc + 0.5));
            D3D12_RESOURCE_DESC dd{};
            dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            dd.Width = e.w;
            dd.Height = e.h;
            dd.DepthOrArraySize = 1;
            dd.MipLevels = 1;
            dd.Format = occlDyn ? DXGI_FORMAT_R32G8X24_TYPELESS : occlRev ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
            dd.SampleDesc.Count = 1;
            dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_HEAP_PROPERTIES dhp{};
            dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = occlDyn ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
            cv.DepthStencil.Depth = occlRev ? 0.0f : 1.0f;
            if (FAILED(device->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &dd, poolRest, &cv, IID_PPV_ARGS(&e.res))))
                return false;
            e.dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
            e.dsv.ptr += SIZE_T(i) * dsvInc;
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{};
            dsvd.Format = cv.Format;
            dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(e.res, &dsvd, e.dsv);
            pool.push_back(std::move(e));
        }
        ++poolGen;
        WLOG("overlay_test: W3 depth pool generation %d: %zu buffer(s) %s", poolGen, pool.size(),
             occlDyn ? "R32G8X24_TYPELESS (D32S8), 0.5x..1.0x" : occlRev ? "R32_TYPELESS" : "D32_FLOAT");
        return true;
    };
    if (occl)
    {
        D3D12_DESCRIPTOR_HEAP_DESC dhd{};
        dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dhd.NumDescriptors = 8;
        CHECK(device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dsvHeap)));
        dsvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        if (!createPool())
        {
            WLOG("overlay_test: W3 depth pool creation failed");
            return 1;
        }
        D3D12_RESOURCE_DESC dd{};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = 2048;
        dd.Height = 2048;
        dd.DepthOrArraySize = 1;
        dd.MipLevels = 1;
        dd.Format = DXGI_FORMAT_R32_TYPELESS;
        dd.SampleDesc.Count = 1;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES dhp{};
        dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
        CHECK(device->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                              IID_PPV_ARGS(&shadow)));
        if (occlDyn)
        {
            dd.Width = width;
            dd.Height = height;
            dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
            CHECK(device->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                  IID_PPV_ARGS(&uiDepth)));
            uiDsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
            uiDsv.ptr += SIZE_T(7) * dsvInc;
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{};
            dsvd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(uiDepth, &dsvd, uiDsv);
        }
    }
    int poolActive = 0, poolLast = -1, poolSwitches = 0;
    std::vector<OccRun> occColor, occDepth;
    McCam occCam;
    bool occCamSet = false;
    const float skySrgb[3] = {0.30f, 0.38f, 0.52f}, groundSrgb[3] = {0.20f, 0.24f, 0.60f}, pillarASrgb[3] = {0.90f, 0.40f, 0.10f},
                pillarBSrgb[3] = {0.75f, 0.70f, 0.20f};
    float occE[4][4];
    Encode(skySrgb, occE[0]);
    Encode(groundSrgb, occE[1]);
    Encode(pillarASrgb, occE[2]);
    Encode(pillarBSrgb, occE[3]);
    int occPhase = -1, occCaptured = -1;
    ULONGLONG occPhaseT = 0;
    bool occCapBusy = false;
    const wchar_t* const occFiles[] = {L"_off.bmp", L"_on.bmp", L"_dbg.bmp", L"_depth.bmp", L"", L"_cal.bmp"};

    // Checkerboard of 64 px cells + colour bars along the top.
    std::vector<D3D12_RECT> darkCells;
    for (UINT y = 0; y < height; y += 64)
        for (UINT x = 0; x < width; x += 64)
            if (((x / 64) + (y / 64)) & 1)
                darkCells.push_back({LONG(x), LONG(y), LONG(std::min(x + 64, width)), LONG(std::min(y + 64, height))});
    const float light[3] = {0.55f, 0.62f, 0.50f}, dark[3] = {0.20f, 0.26f, 0.22f};
    const float bars[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {0.5f, 0.5f, 0.5f}, {0, 0, 0}};
    float lightE[4], darkE[4], barsE[6][4];
    Encode(light, lightE);
    Encode(dark, darkE);
    for (int i = 0; i < 6; ++i)
        Encode(bars[i], barsE[i]);

    // Host + fake scripts.
    if (!wc::host::Start())
    {
        WLOG("overlay_test: host failed to start");
        return 1;
    }
    wc::host::SetWorld("fake\\overlaytest.w2w");
    const float px = 10.0f, py = -5.0f, pz = 64.0f;

    // World scene: MC coords (x, y, z) = W3 (x, z, -y); the player's feet are at MC (10, 64, 5),
    // looking +Z (south) and down, so everything below sits 1-7 blocks in front of the camera.
    const char* const kScene[] = {
        "item replace entity @s weapon.mainhand with minecraft:diamond_pickaxe",
        "fill 6 63 6 14 63 13 minecraft:grass_block",
        "fill 6 64 12 14 66 12 minecraft:stone_bricks",
        "fill -14 63 6 5 63 60 minecraft:grass_block", // a long field (grazing angles: texture filtering)
        "setblock 7 64 8 minecraft:oak_log",
        "setblock 7 65 8 minecraft:oak_log",
        "setblock 13 64 8 minecraft:glass",
        "setblock 12 64 10 minecraft:crafting_table",
        "setblock 8 64 10 minecraft:campfire",
        "summon minecraft:pig 11.5 64 9 {NoAI:1b,Rotation:[150f,0f]}",
        "summon minecraft:item 9.5 64.3 7.5 {Item:{id:\"minecraft:diamond\",count:1},PickupDelay:32767s,Age:-32768s}",
    };
    const float headingDeg = world ? 180.0f : 30.0f; // W3 heading 180 -> MC yaw 0 (+Z)
    const float pitchDeg = world ? -25.0f : -5.0f;   // W3 pitch down -> MC pitch +25
    bool sceneQueued = false;
    ULONGLONG sceneDone = 0;
    int tipStage = 0; // "tip": 0 not yet, 1 M down, 2 released
    ULONGLONG tipAt = 0;

    // Sync mode state.
    if (sync)
    {
        wc::control::SetPoseProvider(&PoseFromOverlay);
        wc::control::SetMcPace(0, 0);
        wc::g_config.overlayLatencyFrames = -1;
        wc::g_config.overlayFrameMatch = false;
        WLOG("overlay_test: SYNC mode: W3 image drawn with the camera of %d game frames earlier, %d fps", kSim, testFps);
    }
    constexpr int kCamHist = 64;
    McCam camHist[kCamHist];
    uint64_t gameFrame = 0;
    int phase = -1; // -1 warm-up, 0 legacy, 1 locked (MC paced per frame), 2 locked (MC at ~60 Hz), 3 done
    double phaseStart = 0, rotStart = -1;
    bool phaseCaptured = false, captureBusy = false;
    PhaseStats stats[3];
    stats[0].name = "legacy (newest MC frame, MC ~60 Hz)";
    stats[1].name = "frame-matched, MC paced per game frame";
    stats[2].name = "frame-matched, MC ~60 Hz";
    const double phaseLen[3] = {14.0, 22.0, 14.0}, measureFrom[3] = {4.0, 6.0, 4.0}, captureAt[3] = {8.0, 12.8, 99.0};
    const wchar_t* const phaseFile[3] = {L"_legacy.bmp", L"_locked.bmp", L""};
    LARGE_INTEGER qf, qStart;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&qStart);
    uint64_t loops = 0;
    const float bgSrgb[3] = {0.16f, 0.18f, 0.24f}, postSrgb[3][3] = {{0.45f, 0.42f, 0.36f}, {0.62f, 0.60f, 0.52f}, {0.30f, 0.34f, 0.30f}};
    const float refSrgb[3] = {1.0f, 0.0f, 1.0f}, tickSrgb[3] = {1.0f, 0.9f, 0.0f};
    float bgE[4], postE[3][4], refE[4], tickE[4];
    Encode(bgSrgb, bgE);
    for (int i = 0; i < 3; ++i)
        Encode(postSrgb[i], postE[i]);
    Encode(refSrgb, refE);
    Encode(tickSrgb, tickE);

    const ULONGLONG t0 = GetTickCount64();
    ULONGLONG firstDraw = 0;
    int scanIndex = 0, lastPrint = -1, result = 2;
    bool captureRequested = false;
    while (!g_quit)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const double t = (GetTickCount64() - t0) / 1000.0;
        if (t > seconds)
            break;

        // Scripts: ground samples around the player and a tick (standing still, in game).
        std::string batch;
        for (int n = 0; n < 160; ++n)
        {
            const int idx = scanIndex++ % (33 * 33);
            char item[64];
            std::snprintf(item, sizeof(item), "%d,%d,%.3f;", 10 + idx % 33 - 16, -5 + idx / 33 - 16, pz);
            batch += item;
        }
        wc::host::PushHeights(batch);
        wc::host::Tick(px, py, pz, headingDeg, pitchDeg, wc::host::kTickInGame);
        if (world)
        {
            const auto hs0 = wc::host::GetStatus();
            if (!sceneQueued && hs0.mcAlive && hs0.haveMcState && (hs0.mcFlags & skycraft::proto::kMcInWorld) &&
                hs0.teleportAck == hs0.teleportSeq && std::fabs(hs0.mcY - pz) < 0.5)
            {
                sceneQueued = true;
                for (const char* c : kScene)
                    wc::buildmode::QueueCommand(c);
                WLOG("overlay_test: MC player arrived (t=%.1f s); typing %zu scene commands", t, std::size(kScene));
            }
            if (sceneQueued && !sceneDone && wc::buildmode::CommandsPending() == 0)
            {
                sceneDone = GetTickCount64();
                WLOG("overlay_test: scene commands done (t=%.1f s)", t);
            }
            if (names && sceneQueued)
            {
                // The script's 10 Hz actor push (witchercraft_combat.ws PushActors); W3 = (MC x, -MC z, MC y).
                static ULONGLONG lastActors = 0;
                if (GetTickCount64() - lastActors >= 100)
                {
                    lastActors = GetTickCount64();
                    char rec[256];
                    std::snprintf(rec, sizeof(rec), "-7001,10.50,-9.50,%.2f,0.0,0.60,1.80,1.000,9,12,Bandit;-7002,12.50,-26.00,%.2f,0.0,0.60,1.80,1.000,9,12,Far Bandit;", pz, pz);
                    wc::combat::SetActors(rec);
                }
            }
        }

        // Sync mode: the script's per-frame camera (control::OnFrame), phases, and the mouse.
        McCam renderCam;
        double tq = 0;
        if (sync)
        {
            LARGE_INTEGER qn;
            QueryPerformanceCounter(&qn);
            tq = static_cast<double>(qn.QuadPart - qStart.QuadPart) / qf.QuadPart;
            const wc::control::Frame lf = wc::control::LastFrame();
            if (phase == -1 && sceneDone && GetTickCount64() - sceneDone > 6000 && lf.engaged && lf.fromPose)
            {
                phase = 0;
                phaseStart = rotStart = tq;
                phaseCaptured = false;
                wc::camsync::SetFrameMatch(false);
                wc::control::SetMcPace(0, 0);
                stats[0].t0 = tq;
                stats[0].mc0 = wc::overlay::GetStats().acquired;
                WLOG("overlay_test: SYNC phase 0: %s", stats[0].name);
            }
            else if (phase >= 0 && phase < 3 && tq - phaseStart > phaseLen[phase] && !captureBusy)
            {
                stats[phase].t1 = tq;
                stats[phase].mc1 = wc::overlay::GetStats().acquired;
                ++phase;
                phaseStart = tq;
                phaseCaptured = false;
                if (phase < 3)
                {
                    wc::camsync::SetFrameMatch(true);
                    wc::control::SetMcPace(phase == 1 ? 1 : 0, 0);
                    stats[phase].t0 = tq;
                    stats[phase].mc0 = wc::overlay::GetStats().acquired;
                    WLOG("overlay_test: SYNC phase %d: %s", phase, stats[phase].name);
                }
            }
            if (phase == 3)
                break;
            // Turn: +-30 deg at up to ~150 deg/s (two sines), pitch 25 +- 6 deg; closed loop on the look.
            if (rotStart >= 0)
            {
                const double tr = tq - rotStart, pi2 = 6.283185307;
                const double wantYaw = 30.0 * std::sin(pi2 * tr / 1.6) + 2.5 * std::sin(pi2 * tr / 0.37);
                const double wantPitch = 17.0 + 5.0 * std::sin(pi2 * tr / 2.3);
                float yaw = 0, pitch = 0;
                wc::control::GetLook(yaw, pitch);
                double dyaw = std::fmod(wantYaw - yaw + 540.0, 360.0) - 180.0;
                wc::control::InjectMouse(static_cast<int>(std::lround(dyaw / 0.15)), static_cast<int>(std::lround((wantPitch - pitch) / 0.15)));
            }
            const wc::control::Frame f = wc::control::OnFrame(0, headingDeg, pitchDeg);
            camHist[gameFrame % kCamHist] = f.engaged && f.eyeValid ? FromFrame(f) : McCam{};
            ++gameFrame;
            // The image presented now was rendered with the camera of kSim game frames ago.
            if (gameFrame > static_cast<uint64_t>(kSim))
                renderCam = camHist[(gameFrame - 1 - kSim) % kCamHist];
        }

        // "Game" frame.
        const UINT bi = sc->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < frameFence[bi])
        {
            fence->SetEventOnCompletion(frameFence[bi], fenceEvent);
            WaitForSingleObject(fenceEvent, 2000);
        }
        ID3D12Resource* bb = nullptr;
        CHECK(sc->GetBuffer(bi, IID_PPV_ARGS(&bb)));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(bi) * rtvInc;
        device->CreateRenderTargetView(bb, nullptr, rtv);
        CHECK(alloc[bi]->Reset());
        CHECK(list[bi]->Reset(alloc[bi], nullptr));
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list[bi]->ResourceBarrier(1, &b);
        if (sync)
        {
            // A world-anchored "Witcher" picture for renderCam: posts all around (texture for the
            // image probe) and a magenta outline exactly on the edges of MC's stone-brick wall, with
            // yellow ticks above each block seam. Locked = a thin even magenta rim around the wall.
            list[bi]->ClearRenderTargetView(rtv, bgE, 0, nullptr);
            std::vector<D3D12_RECT> posts[3], ref, ticks;
            for (uint32_t i = 0; i < 90; ++i)
            {
                const uint32_t h = Hash(i + 17);
                const double az = (i * 4.0 + (h % 100) * 0.02) * 3.14159265358979 / 180.0;
                const double dist = 20.0 + (h >> 8) % 12, w = 0.35 + ((h >> 16) % 10) * 0.12;
                const double cx = 10.0 - std::sin(az) * dist, cz = 5.0 + std::cos(az) * dist;
                const double top[3] = {cx, 82.0, cz}, bot[3] = {cx, 38.0, cz}, mid[3] = {cx, 60.0, cz};
                double tx, ty, bx2, by2, mx, my, depth;
                if (!Project(renderCam, width, height, top, tx, ty) || !Project(renderCam, width, height, bot, bx2, by2) ||
                    !Project(renderCam, width, height, mid, mx, my, &depth))
                    continue;
                const double focal = height * 0.5 / std::tan(renderCam.fov * 0.5 * 3.14159265358979 / 180.0);
                const double wpx = w * focal / depth;
                PushRect(posts[h % 3], mx - wpx * 0.5, std::fmin(ty, by2), mx + wpx * 0.5, std::fmax(ty, by2), width, height);
            }
            for (int e = 0; e < 4; ++e)
                Line(ref, renderCam, width, height, kWall[e], kWall[(e + 1) % 4], 4.0);
            for (int x = 6; x <= 15; ++x)
            {
                const double a[3] = {double(x), 67.15, 12.0}, c[3] = {double(x), 67.6, 12.0};
                Line(ticks, renderCam, width, height, a, c, 3.0);
            }
            for (int i = 0; i < 3; ++i)
                if (!posts[i].empty())
                    list[bi]->ClearRenderTargetView(rtv, postE[i], UINT(posts[i].size()), posts[i].data());
            if (!ref.empty())
                list[bi]->ClearRenderTargetView(rtv, refE, UINT(ref.size()), ref.data());
            if (!ticks.empty())
                list[bi]->ClearRenderTargetView(rtv, tickE, UINT(ticks.size()), ticks.data());
        }
        else if (occl)
        {
            // The W3 frame for the camera of the Minecraft frame on screen (static scene).
            const wc::overlay::Composited comp = wc::overlay::LastComposited();
            McCam cam = comp.valid && comp.pose.fovDeg >= 10.0f ? FromPose(comp.pose) : McCam{};
            if (!occCamSet || std::fabs(cam.yaw - occCam.yaw) > 1e-3f || std::fabs(cam.pitch - occCam.pitch) > 1e-3f ||
                std::fabs(cam.fov - occCam.fov) > 1e-3f || std::fabs(cam.eye[1] - occCam.eye[1]) > 1e-4 ||
                std::fabs(cam.eye[0] - occCam.eye[0]) > 1e-4 || std::fabs(cam.eye[2] - occCam.eye[2]) > 1e-4)
            {
                occCam = cam;
                occCamSet = true;
                occColor = TraceImage(cam, width, height);
                for (auto& e : pool)
                    e.runs.clear();
                WLOG("overlay_test: W3 scene traced for camera (%.3f, %.3f, %.3f) yaw %.2f pitch %.2f vfov %.2f: %zu colour runs",
                     cam.eye[0], cam.eye[1], cam.eye[2], cam.yaw, cam.pitch, cam.fov, occColor.size());
            }
            const bool occCamDirty = false;
            std::vector<D3D12_RECT> byKind[4];
            for (const OccRun& run : occColor)
                byKind[run.kind].push_back(run.rect);
            for (int k = 0; k < 4; ++k)
                if (!byKind[k].empty())
                    list[bi]->ClearRenderTargetView(rtv, occE[k], UINT(byKind[k].size()), byKind[k].data());
            // Depth: like a game's opaque pass (barrier to DEPTH_WRITE, bind several times, clear, draw),
            // then read in post. dyn: the buffer of the current "resolution" changes every 0.4 s.
            poolActive = occlDyn ? static_cast<int>((GetTickCount64() - t0) / 400) % static_cast<int>(pool.size()) : 0;
            if (poolActive != poolLast)
                ++poolSwitches, poolLast = poolActive;
            W3Depth& wd = pool[poolActive];
            if (wd.runs.empty() || occCamDirty)
                wd.runs = TraceImage(occCam, wd.w, wd.h);
            D3D12_RESOURCE_BARRIER db[2]{};
            for (int i = 0; i < 2; ++i)
            {
                db[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                db[i].Transition.pResource = i ? shadow : wd.res;
                db[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                db[i].Transition.StateBefore = i ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : poolRest;
                db[i].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }
            list[bi]->ResourceBarrier(2, db);
            const D3D12_VIEWPORT vp{0, 0, float(wd.w), float(wd.h), 0, 1};
            for (int k = 0; k < (occlDyn ? 8 : 1); ++k) // many draws with the scene depth bound
            {
                list[bi]->OMSetRenderTargets(1, &rtv, FALSE, &wd.dsv);
                list[bi]->RSSetViewports(1, &vp);
            }
            list[bi]->ClearDepthStencilView(wd.dsv, static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH | (occlDyn ? D3D12_CLEAR_FLAG_STENCIL : 0)),
                                            occlRev ? 0.0f : 1.0f, 0, 0, nullptr);
            for (const OccRun& run : wd.runs)
            {
                if (run.kind == kOccSky)
                    continue;
                const double d = occlRev ? kOccNear / run.z : kOccFar / (kOccFar - kOccNear) * (1.0 - kOccNear / run.z);
                list[bi]->ClearDepthStencilView(wd.dsv, D3D12_CLEAR_FLAG_DEPTH, static_cast<float>(d), 0, 1, &run.rect);
            }
            for (int i = 0; i < 2; ++i)
                std::swap(db[i].Transition.StateBefore, db[i].Transition.StateAfter);
            list[bi]->ResourceBarrier(2, db);
            if (uiDepth)
            {
                // The decoy: a full-res depth used once per frame for something else (always far).
                D3D12_RESOURCE_BARRIER ub = db[1];
                ub.Transition.pResource = uiDepth;
                ub.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                ub.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                list[bi]->ResourceBarrier(1, &ub);
                list[bi]->OMSetRenderTargets(1, &rtv, FALSE, &uiDsv);
                list[bi]->ClearDepthStencilView(uiDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                std::swap(ub.Transition.StateBefore, ub.Transition.StateAfter);
                list[bi]->ResourceBarrier(1, &ub);
            }
        }
        else
        {
            list[bi]->ClearRenderTargetView(rtv, lightE, 0, nullptr);
            list[bi]->ClearRenderTargetView(rtv, darkE, UINT(darkCells.size()), darkCells.data());
            for (int i = 0; i < 6; ++i)
            {
                const D3D12_RECT bar{LONG(40 + i * 90), 40, LONG(40 + i * 90 + 80), 120};
                list[bi]->ClearRenderTargetView(rtv, barsE[i], 1, &bar);
            }
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list[bi]->ResourceBarrier(1, &b);
        CHECK(list[bi]->Close());
        ID3D12CommandList* lists[] = {list[bi]};
        queue->ExecuteCommandLists(1, lists);
        bb->Release();
        // Decoy: another DIRECT queue submits last, right before Present, on this same thread.
        CHECK(decoyAlloc->Reset());
        CHECK(decoyList->Reset(decoyAlloc, nullptr));
        CHECK(decoyList->Close());
        ID3D12CommandList* dl[] = {decoyList};
        decoy->ExecuteCommandLists(1, dl);
        decoy->Signal(fence, ++fenceValue); // decoy work done before its allocator is reset again
        const UINT64 decoyDone = fenceValue;

        const HRESULT phr = sc->Present(sync ? 0 : 1, 0);
        if (FAILED(phr))
        {
            WLOG("overlay_test: Present failed 0x%08lX", static_cast<unsigned long>(phr));
            break;
        }
        if (sync)
        {
            // What did the compositor draw for this image, and how far is MC's wall from its outline?
            const wc::overlay::Composited comp = wc::overlay::LastComposited();
            static uint64_t s_lastPresent = 0;
            if (phase >= 0 && phase < 3 && comp.valid && comp.present != s_lastPresent)
            {
                s_lastPresent = comp.present;
                PhaseStats& ps = stats[phase];
                ++ps.presents;
                if (tq - phaseStart >= measureFrom[phase])
                {
                    const double e = BlockErrorPx(renderCam, FromPose(comp.pose), width, height);
                    if (e >= 0)
                        ps.err.push_back(e);
                    (comp.pick == 0 ? ps.exact : comp.pick == 1 ? ps.nearest : ps.newest)++;
                }
                // Proof image mid-turn (yaw speed is highest at multiples of 0.8 s).
                const double trc = tq - rotStart, yawSpeed = 30.0 * 6.283185307 / 1.6 * std::cos(6.283185307 * trc / 1.6);
                if (!phaseCaptured && phaseFile[phase][0] && tq - phaseStart >= captureAt[phase] && std::fabs(yawSpeed) > 100.0)
                {
                    phaseCaptured = captureBusy = true;
                    wc::overlay::RequestCapture((outBase + phaseFile[phase]).c_str());
                    float yaw = 0, pitch = 0;
                    wc::control::GetLook(yaw, pitch);
                    WLOG("overlay_test: capture requested (%ls) at yaw %.1f pitch %.1f; this image's block error %.2f px",
                         (outBase + phaseFile[phase]).c_str(), yaw, pitch, BlockErrorPx(renderCam, FromPose(comp.pose), width, height));
                }
            }
            if (captureBusy && wc::overlay::CaptureDone())
                captureBusy = false;
            // Throttle to testFps (a W3 running faster than MC's 60 Hz).
            ++loops;
            const double due = static_cast<double>(loops) / testFps;
            for (;;)
            {
                LARGE_INTEGER qn;
                QueryPerformanceCounter(&qn);
                const double now = static_cast<double>(qn.QuadPart - qStart.QuadPart) / qf.QuadPart;
                if (now >= due || due - now > 0.5)
                    break;
                if (due - now > 0.002)
                    Sleep(1);
            }
        }
        CHECK(queue->Signal(fence, ++fenceValue));
        frameFence[bi] = fenceValue;
        if (fence->GetCompletedValue() < decoyDone)
        {
            fence->SetEventOnCompletion(decoyDone, fenceEvent);
            WaitForSingleObject(fenceEvent, 2000);
        }

        const auto st = wc::overlay::GetStats();
        const auto hs = wc::host::GetStatus();
        if (st.draws && !firstDraw)
            firstDraw = GetTickCount64();
        if (static_cast<int>(t) != lastPrint)
        {
            lastPrint = static_cast<int>(t);
            WLOG("t=%3ds overlay: presents=%llu draws=%llu uploads=%llu acquired=%llu busy=%llu tex=%ux%u bb=%ux%u mode=%d "
                 "visible=%d disabled=%d | %s",
                 lastPrint, static_cast<unsigned long long>(st.presents), static_cast<unsigned long long>(st.draws),
                 static_cast<unsigned long long>(st.uploads), static_cast<unsigned long long>(st.acquired),
                 static_cast<unsigned long long>(st.skippedBusy), st.texW, st.texH, st.bbW, st.bbH, st.colorMode, st.visible,
                 st.disabled, wc::host::StatusLine().c_str());
        }
        if (st.disabled)
        {
            WLOG("overlay_test: overlay disabled itself - FAIL");
            result = 3;
            break;
        }
        // Capture once HUD frames have been flowing for a while at the back buffer size.
        if (occl)
        {
            const ULONGLONG nowMs = GetTickCount64();
            if (occCapBusy && wc::overlay::CaptureDone())
                occCapBusy = false;
            if (occPhase == -1 && sceneDone && nowMs - sceneDone > 10000 && st.texW == width && st.texH == height)
            {
                occPhase = 0;
                occPhaseT = nowMs;
                wc::overlay::OcclusionCommand(1, 0.0f);
                WLOG("overlay_test: OCCL phase 0: occlusion OFF (before)");
            }
            else if (occPhase >= 0 && occPhase < 6 && !occCapBusy && nowMs - occPhaseT > 2500)
            {
                // Each phase: settle 2.5 s, capture (if it has a file), then the next phase's settings.
                if (occFiles[occPhase][0] && occCaptured != occPhase)
                {
                    occCaptured = occPhase;
                    occCapBusy = true;
                    wc::overlay::RequestCapture((outBase + occFiles[occPhase]).c_str());
                    WLOG("overlay_test: OCCL capture %ls", (outBase + occFiles[occPhase]).c_str());
                }
                else
                {
                    ++occPhase;
                    occPhaseT = nowMs;
                    switch (occPhase)
                    {
                    case 1: wc::overlay::OcclusionCommand(1, 1.0f); break;
                    case 2:
                        wc::overlay::OcclusionCommand(9, 1.0f);
                        if (occlDyn)
                        {
                            // Destroy and recreate the whole pool (like a W3 resolution / DRS change).
                            queue->Signal(fence, ++fenceValue);
                            fence->SetEventOnCompletion(fenceValue, fenceEvent);
                            WaitForSingleObject(fenceEvent, 2000);
                            for (auto& e : pool)
                                e.res->Release();
                            pool.clear();
                            if (!createPool())
                            {
                                WLOG("overlay_test: pool recreation failed");
                                result = 6;
                            }
                        }
                        break;
                    case 3: wc::overlay::OcclusionCommand(9, 2.0f); break;
                    case 4:
                        wc::overlay::OcclusionCommand(9, 0.0f);
                        wc::overlay::OcclusionCommand(4, 0.35f); // a WRONG near plane: calibration must fix it
                        wc::overlay::OcclusionCommand(7, 0.0f);
                        break;
                    default: break;
                    }
                    WLOG("overlay_test: OCCL phase %d", occPhase);
                }
            }
            if (occPhase == 6)
            {
                // Verdict from the captures: project test points with the MC camera.
                auto pt = [&](const double p[3], int& x, int& y) {
                    double sx, sy;
                    if (!Project(occCam, width, height, p, sx, sy))
                        return false;
                    x = static_cast<int>(sx), y = static_cast<int>(sy);
                    return true;
                };
                const double pA[3] = {11.0, 65.5, 7.0}; // pillar A centre: MC's wall is behind it
                const double pW[3] = {8.0, 65.5, 12.0}; // MC wall, nothing of W3 in front
                const double pG[3] = {13.5, 64.0, 7.5}; // MC grass exactly on the W3 ground (bias)
                const double pB[3] = {5.05, 65.0, 13.5}; // pillar B left of the wall (W3 only)
                int ax = 0, ay = 0, wx = 0, wy = 0, gx = 0, gy = 0, bx2 = 0, by2 = 0;
                uint8_t c[3]{};
                bool ok = pt(pA, ax, ay) && pt(pW, wx, wy) && pt(pG, gx, gy) && pt(pB, bx2, by2);
                auto chk = [&](const wchar_t* file, int x, int y, const float* col, bool want, const char* what) {
                    const bool got = BmpPixel(outBase + file, x, y, c) && NearColour(c, col, 14);
                    WLOG("overlay_test: OCCL check %-40s %ls (%4d,%4d) rgb %3u %3u %3u -> %s", what, file, x, y, c[0], c[1], c[2],
                         got == want ? "ok" : "WRONG");
                    return got == want;
                };
                if (ok)
                {
                    ok &= chk(L"_off.bmp", ax, ay, pillarASrgb, false, "OFF: MC wall drawn over pillar A");
                    ok &= chk(L"_on.bmp", ax, ay, pillarASrgb, true, "ON: pillar A hides the MC wall");
                    ok &= chk(L"_on.bmp", wx, wy, skySrgb, false, "ON: MC wall visible (not W3 sky)");
                    ok &= chk(L"_on.bmp", wx, wy, pillarBSrgb, false, "ON: MC wall in front of pillar B");
                    ok &= chk(L"_on.bmp", gx, gy, groundSrgb, false, "ON: MC grass on W3 ground (bias)");
                    // Pillar B stands behind the wall: only a strip of it shows past the wall's top-right corner.
                    int found = 0;
                    for (int y = 0; y < static_cast<int>(height) / 8; y += 2)
                        for (int x = static_cast<int>(width) * 2 / 3; x < static_cast<int>(width) * 7 / 8; x += 2)
                            found += BmpPixel(outBase + L"_on.bmp", x, y, c) && NearColour(c, pillarBSrgb, 14);
                    WLOG("overlay_test: OCCL check %-40s _on.bmp: %d pillar-B pixels (top right) -> %s", "ON: pillar B visible past the wall",
                         found, found >= 20 ? "ok" : "WRONG");
                    ok &= found >= 20;
                    (void)bx2, (void)by2;
                    ok &= chk(L"_cal.bmp", ax, ay, pillarASrgb, true, "CAL: pillar A hides the MC wall");
                    ok &= chk(L"_cal.bmp", gx, gy, groundSrgb, false, "CAL: MC grass on W3 ground");
                }
                else
                {
                    WLOG("overlay_test: OCCL test points not on screen");
                }
                float A = 0, B = 0;
                bool cal = false, occluded = false;
                wc::overlay::OcclusionModel(A, B, cal, occluded);
                const double wantA = occlRev ? 0.0 : kOccFar / (kOccFar - kOccNear);
                const double wantB = occlRev ? kOccNear : -kOccFar * kOccNear / (kOccFar - kOccNear);
                // Compare the fitted model by the distances it gives for true 1 m and 20 m.
                const double z1 = B / ((wantA + wantB / 1.0) - A), z20 = B / ((wantA + wantB / 20.0) - A);
                const bool calOk = cal && std::fabs(z1 - 1.0) < 0.03 && std::fabs(z20 / 20.0 - 1.0) < 0.05;
                WLOG("overlay_test: OCCL calibration: %s A=%.6g B=%.6g (truth A=%.6g B=%.6g): 1 m -> %.3f m, 20 m -> %.2f m -> %s",
                     cal ? "calibrated" : "NOT calibrated", A, B, wantA, wantB, z1, z20, calOk ? "ok" : "WRONG");
                WLOG("overlay_test: %s", wc::overlay::OcclusionCommand(0, 0).c_str());
                uint64_t sw = 0, fw = 0, fwo = 0;
                bool failed = false;
                wc::overlay::OcclusionStats(sw, fw, fwo, failed);
                const bool dynOk = !occlDyn || (sw >= 5 && !failed && fw > 20 * fwo && poolGen == 2);
                WLOG("overlay_test: OCCL depth switches %llu (test pool switched %d times, generation %d), frames with/without W3 depth %llu/%llu, "
                     "occlusion %s -> %s",
                     static_cast<unsigned long long>(sw), poolSwitches, poolGen, static_cast<unsigned long long>(fw),
                     static_cast<unsigned long long>(fwo), failed ? "FAILED" : "alive", dynOk ? "ok" : "WRONG");
                result = ok && calOk && dynOk && !st.disabled ? 0 : 5;
                WLOG("overlay_test: OCCLUSION %s", result == 0 ? "PASS" : "FAIL");
                break;
            }
        }
        if (tip && sceneDone && GetTickCount64() - sceneDone > 8000 && tipStage < 2)
        {
            const ULONGLONG nowTip = GetTickCount64();
            if (tipStage == 0)
            {
                wc::buildmode::QueueEvent({skycraft::proto::kInKey, 16, 1, 0, 0}); // SDL scancode M
                tipAt = nowTip;
                tipStage = 1;
                WLOG("overlay_test: TIP pressed M");
            }
            else if (nowTip - tipAt > 100)
            {
                wc::buildmode::QueueEvent({skycraft::proto::kInKey, 16, 0, 0, 0});
                tipStage = 2;
            }
        }
        if (!sync && !occl && !captureRequested && firstDraw && GetTickCount64() - firstDraw > 12000 && st.uploads > 60 && st.texW == width &&
            st.texH == height && (hs.mcFlags & skycraft::proto::kMcInWorld) &&
            (!world || (sceneDone && GetTickCount64() - sceneDone > 10000 && !(hs.mcFlags & skycraft::proto::kMcScreenOpen))))
        {
            captureRequested = true;
            wc::overlay::RequestCapture(out.c_str());
            WLOG("overlay_test: capture requested");
        }
        if (captureRequested && wc::overlay::CaptureDone())
        {
            WLOG("overlay_test: PASS - capture written to %ls", out.c_str());
            result = 0;
            break;
        }
    }
    if (sync)
    {
        double mean[3]{}, p95[3]{}, mx[3]{};
        for (int i = 0; i < 3; ++i)
            Report(stats[i], mean[i], p95[i], mx[i]);
        const wc::camsync::ProbeResult pr = wc::camsync::LastProbe();
        WLOG("overlay_test: SYNC probe: measured latency k=%d (simulated %d), corr %.2f (next best %.2f), %llu samples; in use k=%d",
             pr.k, kSim, pr.corr, pr.runnerUp, static_cast<unsigned long long>(pr.samples), wc::camsync::LatencyInUse());
        WLOG("overlay_test: %s", wc::camsync::StatusLine().c_str());
        const bool lockedOk = phase == 3 && stats[1].err.size() > 200 && stats[2].err.size() > 200 && mean[1] < 0.5 && p95[1] < 1.5 &&
                              mean[2] < 0.5 && p95[2] < 1.5;
        const bool probeOk = pr.k == kSim && wc::camsync::LatencyInUse() == kSim;
        const bool legacyBad = mean[0] > 3.0;
        result = lockedOk && probeOk ? 0 : 4;
        WLOG("overlay_test: SYNC %s (locked phases %s, probe %s, legacy phase shows the old swim: %s)", result == 0 ? "PASS" : "FAIL",
             lockedOk ? "OK" : "BAD", probeOk ? "OK" : "BAD", legacyBad ? "yes" : "no");
    }
    else if (result == 2)
        WLOG("overlay_test: timed out without a capture (MC not linked / no HUD frames?)");

    // Drain the GPU before tearing down.
    queue->Signal(fence, ++fenceValue);
    fence->SetEventOnCompletion(fenceValue, fenceEvent);
    WaitForSingleObject(fenceEvent, 2000);
    wc::host::Stop();
    WLOG("overlay_test: done (%d); %s", result, wc::host::StatusLine().c_str());
    ExitProcess(static_cast<UINT>(result)); // skip COM teardown order issues with hooks still installed
}




