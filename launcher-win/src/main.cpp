// Burnout Recompilation -- the Windows launcher.
//
// One window, start to finish, like the PS1 launchers:
//
//   1. First run only: ask for the player's own Burnout ISO. Remembered.
//   2. Build: tools\project\tools\build_from_iso.py turns the disc into
//      burnout_game.dll (the recompiled game plus the xboxrecomp runtime),
//      using the Python and zig shipped in tools\. Once.
//   3. Splash: the speed streaks, Play, Exit.
//   4. Play: the menu lets go of the window and the game DLL draws into it.
//
// The game DLL claims the Xbox address space (burnout_game_prepare) BEFORE
// this program creates its own Direct3D device for the menu: a graphics
// driver reserves address space as it starts, and the memory layout wants the
// low 4 GB windows the Xbox used. So a launch that finds a finished build
// prepares the game first, and the one launch that builds it restarts itself
// when the build is done (the only time the window closes and reopens).
//
// Where things live:
//   <folder with Burnout.exe>\tools\     python\, zig\, toolkit\, project\, VERSION
//   %LOCALAPPDATA%\BurnoutRecomp\        settings.ini, work\, logs\, saves\
//   (BURNOUT_TOOLS and BURNOUT_DATA override them, for testing)
//
// Nothing here is derived from the game; the player's disc is the only input.

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static const char *kVersion = "0.1.0";
static const int kGameApi = 1;   // must match BURNOUT_GAME_API in src/main.c
// Shown on the splash and in About when not empty.
static const char *kKnownIssue = "";

// Ask hybrid-graphics laptops for the discrete GPU. NVIDIA Optimus and AMD
// switchable-graphics drivers read these from the process's .exe. The runtime
// exports the same pair (xboxrecomp d3d8_device.c), but the game is a DLL in
// this program, so the launcher has to carry them itself. A machine with one
// GPU ignores them.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// ── Small helpers ───────────────────────────────────────────────

static std::string Utf8(const std::wstring &w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Wide(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring Env(const wchar_t *name)
{
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    return (n > 0 && n < 4096) ? std::wstring(buf, n) : std::wstring();
}

static bool FileExists(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring Join(const std::wstring &a, const std::wstring &b)
{
    if (a.empty()) return b;
    wchar_t last = a.back();
    return (last == L'\\' || last == L'/') ? a + b : a + L"\\" + b;
}

static std::wstring Trim(const std::wstring &s)
{
    size_t b = s.find_first_not_of(L" \t\r\n\""), e = s.find_last_not_of(L" \t\r\n\"");
    return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

static std::wstring FullPath(const std::wstring &p)
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, buf, nullptr);
    return (n > 0 && n < MAX_PATH * 4) ? std::wstring(buf, n) : p;
}

static std::wstring ReadSmallFile(const std::wstring &p)
{
    FILE *f = _wfopen(p.c_str(), L"rb");
    if (!f) return {};
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return Wide(s);
}

// key=value lines, UTF-8. Used for settings.ini and the build's ready.txt.
static std::map<std::wstring, std::wstring> ReadKeyValues(const std::wstring &p)
{
    std::map<std::wstring, std::wstring> kv;
    std::wstring text = ReadSmallFile(p);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

// ── Paths and settings ──────────────────────────────────────────

struct Paths {
    std::wstring app, tools, data, work, logs, settings, ready, dll, python, script;
};
static Paths g_paths;
static std::wstring g_isoPath;
static FILE *g_log;

static void Log(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void InitPaths()
{
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    std::wstring app = exe;
    app = app.substr(0, app.find_last_of(L"\\/"));
    g_paths.app = app;

    std::wstring tools = Env(L"BURNOUT_TOOLS");
    g_paths.tools = tools.empty() ? Join(app, L"tools") : FullPath(tools);

    std::wstring data = Env(L"BURNOUT_DATA");
    if (data.empty()) data = Join(Env(L"LOCALAPPDATA"), L"BurnoutRecomp");
    g_paths.data = FullPath(data);
    g_paths.work = Join(g_paths.data, L"work");
    g_paths.logs = Join(g_paths.data, L"logs");
    g_paths.settings = Join(g_paths.data, L"settings.ini");
    g_paths.ready = Join(g_paths.work, L"ready.txt");
    g_paths.dll = Join(g_paths.work, L"build\\burnout_game.dll");
    g_paths.python = Join(g_paths.tools, L"python\\python.exe");
    g_paths.script = Join(g_paths.tools, L"project\\tools\\build_from_iso.py");

    CreateDirectoryW(g_paths.data.c_str(), nullptr);
    CreateDirectoryW(g_paths.logs.c_str(), nullptr);
}

// Options. Brightness is a slider from -50 to +50 whose middle, "Original",
// is the game's own picture; RuntimeBrightness turns it into the runtime's
// XBOXRECOMP_BRIGHTNESS. Resolution is the multiple of the Xbox's 640x480 the
// game renders at (XBOXRECOMP_RES_SCALE). "GPU on its own thread" is the
// runtime's async PFIFO, which every tested build of this release used.
//
// An empty save folder means <data>\saves, NOT the runtime's own default of
// %LOCALAPPDATA%\xboxrecomp: the runtime maps the Xbox's T:, U: and Z: drives
// to fixed folders inside the save folder with no per-title level, so a title
// sharing that folder with Breakdown would share its save metadata, and
// Breakdown clears Z: (the cache) when it starts a new game.
static int g_brightness;
static int g_scale = 2;
static bool g_async = true;
static std::wstring g_saveDir;
static bool g_fullscreen;
static HWND g_frame, g_view;          // the windows, further down
static void SetFullscreen(bool on);

static std::wstring DefaultSaveDir() { return Join(g_paths.data, L"saves"); }
static std::wstring SaveDir() { return g_saveDir.empty() ? DefaultSaveDir() : g_saveDir; }

static void LoadSettings()
{
    auto kv = ReadKeyValues(g_paths.settings);
    g_isoPath = kv[L"iso"];
    g_brightness = _wtoi(kv[L"picture"].c_str());
    if (g_brightness < -50) g_brightness = -50;
    if (g_brightness > 50) g_brightness = 50;
    if (kv.count(L"scale")) g_scale = _wtoi(kv[L"scale"].c_str());
    if (g_scale < 1) g_scale = 1;
    if (g_scale > 3) g_scale = 3;
    if (kv.count(L"async")) g_async = kv[L"async"] != L"0";
    g_saveDir = kv[L"saves"];
    g_fullscreen = kv[L"fullscreen"] == L"1";
}

static void SaveSettings()
{
    FILE *f = _wfopen(g_paths.settings.c_str(), L"wb");
    if (!f) { Log("cannot write settings.ini"); return; }
    fprintf(f, "iso=%s\n", Utf8(g_isoPath).c_str());
    fprintf(f, "picture=%d\n", g_brightness);
    fprintf(f, "scale=%d\n", g_scale);
    fprintf(f, "async=%d\n", g_async ? 1 : 0);
    fprintf(f, "saves=%s\n", Utf8(g_saveDir).c_str());
    fprintf(f, "fullscreen=%d\n", g_fullscreen ? 1 : 0);
    fclose(f);
}

// Handed to the game through the CRT environment it shares with this
// program (see PrepareGame); read when it starts running, so Options changed
// after the game was prepared still apply.
// The runtime applies out = in^(1/(1 + b/100)) after the game's own gamma
// ramp. kOriginal is the b that matches the original game on a PC screen.
// Breakdown's was -43, matched side by side by the operator; Burnout's has
// not been calibrated yet, so the middle is the game's own ramp, b = 0. Each
// 50 steps either way halves or doubles the exponent.
static const double kOriginal = 0.0;
static int RuntimeBrightness(int slider)
{
    const double middle = 1.0 / (1.0 + kOriginal / 100.0);
    double exponent = middle * pow(2.0, -slider / 50.0);
    return (int)lround(100.0 * (1.0 / exponent - 1.0));
}

// Leaves a variable alone if it was set before the launcher started, so a
// tester can still override any of these from a shell.
static void PutEnvDefault(const char *name, const char *value)
{
    if (Env(Wide(name).c_str()).empty()) _putenv_s(name, value);
}

static void ApplyOptions()
{
    char b[16];
    snprintf(b, sizeof b, "%d", RuntimeBrightness(g_brightness));
    _putenv_s("XBOXRECOMP_BRIGHTNESS", b);
    snprintf(b, sizeof b, "%d", g_scale);
    _putenv_s("XBOXRECOMP_RES_SCALE", b);
    _putenv_s("XBOXRECOMP_PFIFO_ASYNC", g_async ? "1" : "0");
    // How far the CPU may run ahead of the GPU thread. 1024 is what the
    // tested builds ran with; the runtime's default stalls the CPU sooner.
    PutEnvDefault("XBOXRECOMP_PFIFO_MAX_PENDING_PUTS", "1024");
    _wputenv_s(L"XBOXRECOMP_SAVE_DIR", SaveDir().c_str());
    Log("options: brightness %d (runtime %d), resolution %dx, GPU thread %s, saves %s", g_brightness,
        RuntimeBrightness(g_brightness), g_scale, g_async ? "on" : "off", Utf8(SaveDir()).c_str());
}

static std::wstring BundleVersion()
{
    return Trim(ReadSmallFile(Join(g_paths.tools, L"VERSION")));
}

// ── The disc image ──────────────────────────────────────────────

// The Xbox filesystem's volume descriptor sits at sector 32 of the game
// partition, which starts at one of these offsets (trimmed image, or a full
// XGD1/2/3 dump). Same check as tools/xiso_extract.py.
static std::string ValidateIso(const std::wstring &path)
{
    if (path.empty()) return "Choose a file.";
    if (!FileExists(path)) return "That file does not exist.";
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return "That file cannot be read.";
    static const long long offsets[] = { 0, 0x18300000LL, 0x0FD90000LL, 0x02080000LL };
    static const char magic[] = "MICROSOFT*XBOX*MEDIA";
    bool ok = false;
    for (long long off : offsets) {
        char buf[sizeof magic - 1];
        if (_fseeki64(f, off + 32 * 2048, SEEK_SET) == 0 &&
            fread(buf, 1, sizeof buf, f) == sizeof buf &&
            memcmp(buf, magic, sizeof buf) == 0) { ok = true; break; }
    }
    fclose(f);
    return ok ? std::string() : "That is not an Xbox disc image.";
}

// A finished build for this disc and this release of the launcher.
static bool BuildIsReady()
{
    auto kv = ReadKeyValues(g_paths.ready);
    return !g_isoPath.empty() && kv[L"iso"] == g_isoPath &&
           kv[L"version"] == BundleVersion() && FileExists(g_paths.dll);
}

// ── The game DLL ────────────────────────────────────────────────

typedef int (*GameApiFn)(void);
typedef int (*GamePrepareFn)(const wchar_t *, const wchar_t *);
typedef void (*GameRunFn)(HWND);

static GameRunFn g_gameRun;
static std::string g_gameError;
static std::wstring g_gameLog;

static bool PrepareGame()
{
    // Options reach the game through the CRT environment, which the game DLL
    // shares (both link the UCRT), so its getenv sees them. ApplyOptions runs
    // again on Play, after any change made in the menu.
    ApplyOptions();
    // Its dependencies (d3d11, XAudio2, XInput, d3dcompiler) are system DLLs.
    HMODULE m = LoadLibraryExW(g_paths.dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        g_gameError = "The game could not be loaded (error " + std::to_string(GetLastError()) + ").";
        return false;
    }
    auto api = (GameApiFn)(void *)GetProcAddress(m, "burnout_game_api");
    auto prepare = (GamePrepareFn)(void *)GetProcAddress(m, "burnout_game_prepare");
    g_gameRun = (GameRunFn)(void *)GetProcAddress(m, "burnout_game_run");
    if (!api || !prepare || !g_gameRun || api() != kGameApi) {
        g_gameError = "The built game does not match this launcher. Rebuild it.";
        g_gameRun = nullptr;
        return false;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"game-%04d%02d%02d-%02d%02d%02d.log",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    g_gameLog = Join(g_paths.logs, name);
    // The game resolves game_files\ against its working directory.
    if (prepare(g_paths.work.c_str(), g_gameLog.c_str()) != 0) {
        g_gameError = "The game could not start. Details: " + Utf8(g_gameLog);
        g_gameRun = nullptr;
        return false;
    }
    return true;
}

// ── The build ───────────────────────────────────────────────────

// Runs build_from_iso.py and collects its progress lines: STAGE <key> <text>,
// PROGRESS <pct> <detail>, DONE <path>, ERROR <message> (then indented
// context). The whole output also goes to logs\build-<time>.log.
class Build {
public:
    std::mutex lock;
    std::string stage, stageText = "Starting", detail;
    float progress = 0.f;
    std::vector<std::string> errors;
    std::vector<std::string> seen;
    bool done = false, failed = false;
    std::wstring logPath;

    void Start()
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf(name, 64, L"build-%04d%02d%02d-%02d%02d%02d.log",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        logPath = Join(g_paths.logs, name);

        if (!FileExists(g_paths.python) || !FileExists(g_paths.script)) {
            Fail("The build tools are missing from the tools folder next to Burnout.exe.");
            return;
        }
        SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
        HANDLE rd, wr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) { Fail("Could not start the build (pipe)."); return; }
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

        std::wstring cmd = L"\"" + g_paths.python + L"\" -u \"" + g_paths.script + L"\" --iso \"" +
                           g_isoPath + L"\" --work \"" + g_paths.work + L"\" --bundle \"" +
                           g_paths.tools + L"\"";
        STARTUPINFOW si = {};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
        CloseHandle(wr);
        if (ok) {
            // The build and everything it starts (zig, cmake, ninja) end with
            // this launcher: the job closes with the process.
            static HANDLE job;
            if (!job) {
                job = CreateJobObjectW(nullptr, nullptr);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
                lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
            }
            if (job) AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
        }
        if (!ok) {
            CloseHandle(rd);
            Fail("Could not start the build (error " + std::to_string(GetLastError()) + ").");
            return;
        }
        Log("build started: %s", Utf8(cmd).c_str());
        CloseHandle(pi.hThread);
        std::thread([this, rd, proc = pi.hProcess] { Reader(rd, proc); }).detach();
    }

private:
    void Fail(const std::string &msg)
    {
        std::lock_guard<std::mutex> g(lock);
        failed = true;
        errors.push_back(msg);
        Log("build: %s", msg.c_str());
    }

    void Reader(HANDLE rd, HANDLE proc)
    {
        FILE *log = _wfopen(logPath.c_str(), L"wb");
        std::string pending;
        char buf[4096];
        DWORD n;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) {
            if (log) { fwrite(buf, 1, n, log); fflush(log); }
            pending.append(buf, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                OnLine(line);
            }
        }
        CloseHandle(rd);
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        if (log) fclose(log);
        std::lock_guard<std::mutex> g(lock);
        if (!done && !failed) {
            failed = true;
            errors.push_back("The build stopped unexpectedly (exit " + std::to_string(code) + ").");
        }
        Log("build finished: exit %lu, %s", code, done ? "done" : "failed");
    }

    void OnLine(const std::string &line)
    {
        std::lock_guard<std::mutex> g(lock);
        auto rest = [&](size_t from) { return from < line.size() ? line.substr(from) : std::string(); };
        if (line.rfind("STAGE ", 0) == 0) {
            std::string r = rest(6);
            size_t sp = r.find(' ');
            stage = r.substr(0, sp);
            stageText = sp == std::string::npos ? stage : r.substr(sp + 1);
            seen.push_back(stage);
            progress = 0.f;
            detail.clear();
        } else if (line.rfind("PROGRESS ", 0) == 0) {
            std::string r = rest(9);
            size_t sp = r.find(' ');
            progress = (float)atof(r.substr(0, sp).c_str()) / 100.f;
            detail = sp == std::string::npos ? std::string() : r.substr(sp + 1);
        } else if (line.rfind("DONE ", 0) == 0) {
            done = true;
        } else if (line.rfind("ERROR ", 0) == 0) {
            failed = true;
            errors.push_back(rest(6));
        } else if (failed && line.rfind("  ", 0) == 0) {
            errors.push_back(line.substr(2));
            if (errors.size() > 14) errors.erase(errors.begin() + 1);
        }
    }
};

// ── Look ────────────────────────────────────────────────────────

// Burnout is arcade racing through traffic at night and in the sun: an asphalt
// field with the logo's hot orange as the accent colour, yellow where it burns
// brightest, and a deep red trim. The member names are shared with the other
// Xbox recomp launchers so the screens below read the same; Impact is simply
// "the accent colour" here.
namespace Theme {
const ImVec4 Bg(0.06f, 0.06f, 0.08f, 1.f), Text(0.95f, 0.95f, 0.96f, 1.f),
    Dim(0.64f, 0.65f, 0.70f, 1.f), Steel(0.55f, 0.58f, 0.66f, 1.f), Impact(1.00f, 0.55f, 0.06f, 1.f),
    ImpactHot(1.00f, 0.88f, 0.30f, 1.f), Warn(1.00f, 0.76f, 0.30f, 1.f), Error(1.00f, 0.42f, 0.38f, 1.f),
    TrimRed(0.72f, 0.05f, 0.02f, 1.f),
    // The title: red, with a thin silver trim.
    TitleRed(0.88f, 0.07f, 0.05f, 1.f), Silver(0.80f, 0.82f, 0.86f, 1.f),
    // Buttons as in the PS1 recomp launchers: lighter than the field.
    Btn(0.21f, 0.22f, 0.25f, 1.f), BtnHov(0.33f, 0.34f, 0.38f, 1.f), BtnAct(0.47f, 0.48f, 0.53f, 1.f),
    Border(0.45f, 0.46f, 0.50f, 1.f);
ImFont *Body, *Small, *Title, *Subtitle, *Ui;
float Scale = 1.f;

ImU32 Col(ImVec4 c, float alpha = 1.f)
{
    c.w *= alpha;
    return ImGui::GetColorU32(c);
}

// No font ships with the launcher (fonts are not ours to redistribute); use an
// installed face, ImGui's built-in one as the last resort.
void LoadFonts(float scale)
{
    ImGuiIO &io = ImGui::GetIO();
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    const wchar_t *faces[] = { L"segoeuib.ttf", L"arialbd.ttf", L"tahomabd.ttf", L"tahoma.ttf" };
    for (const wchar_t *face : faces) {
        std::wstring p = Join(Join(win, L"Fonts"), face);
        if (!FileExists(p)) continue;
        std::string u = Utf8(p);
        Body = io.Fonts->AddFontFromFileTTF(u.c_str(), 20.f * scale);
        Small = io.Fonts->AddFontFromFileTTF(u.c_str(), 15.f * scale);
        Subtitle = io.Fonts->AddFontFromFileTTF(u.c_str(), 40.f * scale);
        Ui = io.Fonts->AddFontFromFileTTF(u.c_str(), 26.f * scale);
        // The title face is Arial Black, heavy like the game's own logo; every
        // Windows install has it.
        std::wstring heavy = Join(Join(win, L"Fonts"), L"ariblk.ttf");
        Title = io.Fonts->AddFontFromFileTTF(Utf8(FileExists(heavy) ? heavy : p).c_str(), 96.f * scale);
        if (Body && Small && Subtitle && Ui && Title) return;
    }
    ImFontConfig cfg;
    cfg.SizePixels = 13.f * scale;
    Body = Small = Title = Subtitle = Ui = io.Fonts->AddFontDefault(&cfg);
}

void Push()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Bg);
    ImGui::PushStyleColor(ImGuiCol_Text, Text);
    ImGui::PushStyleColor(ImGuiCol_Button, Btn);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BtnHov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, BtnAct);
    ImGui::PushStyleColor(ImGuiCol_Border, Border);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Btn);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Impact);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, Impact);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * Scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.f * Scale, 9.f * Scale));
}

void Pop()
{
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(9);
}

// A full-window, undecorated page to draw a screen into.
bool BeginPage(const char *id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    return ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus);
}

void CentredText(const std::string &text, ImVec4 colour, ImFont *font = nullptr)
{
    if (font) ImGui::PushFont(font);
    float wrap = ImGui::GetWindowWidth() - 48.f * Scale;
    float w = ImGui::CalcTextSize(text.c_str()).x;
    if (w <= wrap) {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) * 0.5f);
        ImGui::TextColored(colour, "%s", text.c_str());
    } else {
        ImGui::SetCursorPosX(24.f * Scale);
        ImGui::PushTextWrapPos(24.f * Scale + wrap);
        ImGui::TextColored(colour, "%s", text.c_str());
        ImGui::PopTextWrapPos();
    }
    if (font) ImGui::PopFont();
}

bool CentredButton(const char *label, float width, float height = 0.f)
{
    width *= Scale;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
    return ImGui::Button(label, ImVec2(width, height * Scale));
}

// Centred text with a coloured trim: the text drawn in the trim colour at
// points around it, then the text itself on top.
void TrimmedText(const char *text, ImVec4 core, ImVec4 trim, ImFont *font, float thickness)
{
    ImGui::PushFont(font);
    ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::SetCursorPosX(fmaxf(0.f, (ImGui::GetWindowWidth() - size.x) * 0.5f));
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 t = Col(trim);
    for (float r = 0.5f; r <= thickness * Scale; r += 0.5f)
        for (int i = 0; i < 16; i++) {
            float a = 6.2831853f * i / 16.f;
            dl->AddText(ImVec2(o.x + cosf(a) * r, o.y + sinf(a) * r), t, text);
        }
    dl->AddText(o, Col(core), text);
    ImGui::Dummy(size);   // the draw list does not move the cursor
    ImGui::PopFont();
}

// BURNOUT in red with a thin silver trim, and "Recompilation" under it. The face sits high in a tall line; pull the
// second line up so the two read as one logo.
void Logo(ImVec4 subtitle)
{
    TrimmedText("BURNOUT", TitleRed, Silver, Title, 1.5f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 10.f * Scale);
    CentredText("Recompilation", subtitle, Subtitle);
}

void Gap(float h) { ImGui::Dummy(ImVec2(0.f, h * Scale)); }
} // namespace Theme

// The splash backdrop: faint streaks of light rushing past, as from a car at
// speed, and every so often a boost, when they all run faster and burn from
// grey to orange before cooling back down.
class SpeedStreaks {
    struct Streak { float y, x, length, speed, alpha; };
    std::vector<Streak> streaks;
    std::mt19937 rng{ std::random_device{}() };
    float boost = 0.f, nextBoost = 2.5f;

    static constexpr int Count = 70;
    static constexpr float MinSpeed = 0.35f, MaxSpeed = 1.6f;   // screen widths per second
    static constexpr float BoostRise = 6.f, BoostFall = 0.9f, BoostSpeed = 2.2f;
    static constexpr float MinGap = 3.f, MaxGap = 7.f;

    float Rand() { return std::uniform_real_distribution<float>(0.f, 1.f)(rng); }

    Streak Make(bool anywhere)
    {
        // Denser and faster towards the top and bottom edges, as the road and
        // the roadside rush past more quickly than the horizon does.
        float y = Rand();
        float edge = fabsf(y - 0.5f) * 2.f;
        return { y, anywhere ? Rand() : 1.f + Rand() * 0.3f, 0.05f + Rand() * 0.25f * (0.4f + edge),
                 MinSpeed + (MaxSpeed - MinSpeed) * edge * Rand(), 0.05f + 0.20f * edge * Rand() };
    }

public:
    void Draw(ImDrawList *dl, ImVec2 origin, ImVec2 size)
    {
        float dt = ImGui::GetIO().DeltaTime;
        dt = dt < 0.f ? 0.f : dt > 0.1f ? 0.1f : dt;
        if (streaks.empty())
            for (int i = 0; i < Count; i++) streaks.push_back(Make(true));

        nextBoost -= dt;
        if (nextBoost <= 0.f) { boost = 1.f; nextBoost = MinGap + Rand() * (MaxGap - MinGap); }
        // Snaps up, then cools off slowly.
        float heat = boost > 0.99f ? 1.f : boost;
        boost = fmaxf(0.f, boost - dt * BoostFall);
        float speedUp = 1.f + BoostSpeed * heat * heat;

        for (Streak &s : streaks) {
            s.x -= s.speed * speedUp * dt;
            if (s.x + s.length < 0.f) s = Make(false);
            float y = origin.y + s.y * size.y;
            float x0 = origin.x + s.x * size.x, x1 = x0 + s.length * size.x * (1.f + heat);
            ImVec4 c(Theme::Steel.x + (Theme::Impact.x - Theme::Steel.x) * heat,
                     Theme::Steel.y + (Theme::Impact.y - Theme::Steel.y) * heat,
                     Theme::Steel.z + (Theme::Impact.z - Theme::Steel.z) * heat, 1.f);
            float a = fminf(1.f, s.alpha * (1.f + 2.f * heat));
            // Bright head, fading tail.
            dl->AddRectFilledMultiColor(ImVec2(x0, y - 1.f * Theme::Scale), ImVec2(x1, y + 1.f * Theme::Scale),
                                        Theme::Col(c, a), Theme::Col(c, 0.f), Theme::Col(c, 0.f),
                                        Theme::Col(c, a));
        }
    }
};

// ── Screens ─────────────────────────────────────────────────────

enum class Screen { Iso, Build, Splash, Restarting };
static Screen g_screen;
static Build *g_build;
static SpeedStreaks g_streaks;
static char g_isoInput[2048];
static std::string g_isoProblem;
static bool g_playRequested, g_focusPlay = true;

static void BrowseForIso(HWND owner)
{
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Xbox disc image (*.iso)\0*.iso\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrTitle = L"Choose your Burnout disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        snprintf(g_isoInput, sizeof g_isoInput, "%s", Utf8(file).c_str());
        g_isoProblem.clear();
    }
}

static void StartBuild()
{
    delete g_build;   // a finished, failed build only; a running one is never replaced
    g_build = new Build();
    g_build->Start();
    g_screen = Screen::Build;
}

static void DrawIso(HWND hwnd)
{
    using namespace Theme;
    if (BeginPage("##iso")) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.20f);
        Logo(Dim);
        Gap(36);
        CentredText("To begin, choose your Burnout (USA) Xbox disc image (.iso).", Text);
        CentredText("The game is rebuilt from your own disc on this computer. Nothing is downloaded.", Dim, Small);
        Gap(20);

        float fieldW = fminf(640.f * Scale, ImGui::GetWindowWidth() - 80.f * Scale);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - fieldW) * 0.5f);
        ImGui::SetNextItemWidth(fieldW - 130.f * Scale);
        ImGui::InputTextWithHint("##isopath", "C:\\path\\to\\Burnout (USA).iso", g_isoInput, sizeof g_isoInput);
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(120.f * Scale, 0.f))) BrowseForIso(g_frame);

        Gap(14);
        if (CentredButton("Continue", 220.f)) {
            std::wstring p = Trim(Wide(g_isoInput));
            g_isoProblem = ValidateIso(p);
            if (g_isoProblem.empty()) {
                g_isoPath = FullPath(p);
                SaveSettings();
                Log("disc image chosen: %s", Utf8(g_isoPath).c_str());
                StartBuild();
            }
        }
        if (!g_isoProblem.empty()) {
            Gap(8);
            CentredText(g_isoProblem, Error);
        }
    }
    ImGui::End();
}

static void DrawBuild()
{
    using namespace Theme;
    static const std::pair<const char *, const char *> stages[] = {
        { "extract", "Extract the game from your disc" },
        { "stage", "Prepare the recompiler" },
        { "lift", "Recompile the game code" },
        { "fix", "Apply fixes" },
        { "compile", "Compile" },
        { "ready", "Ready" },
    };
    bool again = false, choose = false, done = false;
    if (BeginPage("##build")) {
        std::lock_guard<std::mutex> g(g_build->lock);
        float w = ImGui::GetWindowWidth();
        float col = fminf(620.f * Scale, w - 80.f * Scale), x = (w - col) * 0.5f;
        bool failed = g_build->failed;
        done = g_build->done;
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.12f);
        TrimmedText("BURNOUT", TitleRed, Silver, Title, 1.5f);
        CentredText(failed ? "The build did not finish" : "Building the game from your disc", failed ? Error : Dim);
        CentredText("This happens once. Later launches go straight to the menu.", Dim, Small);
        Gap(26);

        for (auto &s : stages) {
            bool current = g_build->stage == s.first && !failed && !done;
            bool seen = false;
            for (auto &k : g_build->seen) seen |= k == s.first;
            bool finished = (seen && !current) || done;
            ImGui::SetCursorPosX(x);
            ImGui::TextColored(current ? Impact : finished ? Text : Dim, "%s  %s",
                               finished ? "+" : current ? ">" : "-", s.second);
        }
        Gap(18);
        ImGui::SetCursorPosX(x);
        if (!failed) {
            ImGui::ProgressBar(done ? 1.f : g_build->progress, ImVec2(col, 0.f),
                               done ? "Done" : g_build->stageText.c_str());
            ImGui::SetCursorPosX(x);
            ImGui::PushFont(Small);
            std::string d = g_build->detail.size() > 90 ? g_build->detail.substr(0, 90) + "..." : g_build->detail;
            ImGui::TextColored(Dim, "%s", d.c_str());
            ImGui::PopFont();
        } else {
            ImGui::PushFont(Small);
            for (auto &e : g_build->errors) {
                ImGui::SetCursorPosX(x);
                ImGui::PushTextWrapPos(x + col);
                ImGui::TextColored(Error, "%s", e.c_str());
                ImGui::PopTextWrapPos();
            }
            Gap(8);
            ImGui::SetCursorPosX(x);
            ImGui::PushTextWrapPos(x + col);
            ImGui::TextColored(Dim, "Full log: %s", Utf8(g_build->logPath).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            Gap(14);
            again = CentredButton("Try again", 340.f);
            Gap(6);
            choose = CentredButton("Choose a different disc image", 340.f);
        }
    }
    ImGui::End();

    // Acted on outside the lock: StartBuild replaces the build it guards.
    if (done) g_screen = Screen::Restarting;
    else if (again) StartBuild();
    else if (choose) {
        g_isoPath.clear();
        SaveSettings();
        g_isoProblem.clear();
        g_screen = Screen::Iso;
    }
}

enum class SplashPage { Main, Options, About };
static SplashPage g_page = SplashPage::Main;

static void ChooseSaveFolder(HWND owner)
{
    // The new-style folder dialog needs COM on this thread. Initialised here,
    // only when the dialog is used, so nothing else in the process changes.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Choose the folder for your Burnout saved games";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    wchar_t path[MAX_PATH * 4];
    if (SHGetPathFromIDListW(id, path)) {
        g_saveDir = FullPath(path);
        SaveSettings();
        Log("save folder chosen: %s", Utf8(g_saveDir).c_str());
    }
    CoTaskMemFree(id);
}

static void OpenFolder(const std::wstring &dir)
{
    CreateDirectoryW(dir.c_str(), nullptr);   // the default exists only after a first game
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void BackButton()
{
    using namespace Theme;
    Gap(18);
    ImGui::PushFont(Ui);
    if (CentredButton("Back", 200.f, 46.f)) { g_page = SplashPage::Main; g_focusPlay = true; }
    ImGui::PopFont();
}

static void DrawOptions(HWND hwnd)
{
    using namespace Theme;
    float w = fminf(640.f * Scale, ImGui::GetWindowWidth() - 80.f * Scale);
    float x = (ImGui::GetWindowWidth() - w) * 0.5f;

    CentredText("Options", Impact);
    Gap(18);

    ImGui::SetCursorPosX(x);
    ImGui::TextColored(Text, "Brightness");
    ImGui::SetCursorPosX(x);
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderInt("##brightness", &g_brightness, -50, 50, g_brightness == 0 ? "Original" : "%+d"))
        SaveSettings();
    ImGui::SetCursorPosX(x);
    ImGui::PushTextWrapPos(x + w);
    ImGui::TextColored(Dim, "Original, in the middle, is the game's own picture. Move left for a "
                            "darker picture, right to brighten the dark parts.");
    ImGui::PopTextWrapPos();

    Gap(26);
    ImGui::SetCursorPosX(x);
    ImGui::TextColored(Text, "Resolution");
    ImGui::SetCursorPosX(x);
    {
        static const char *names[] = { "1x  (640 x 480, the Xbox's own)", "2x  (1280 x 960)",
                                       "3x  (1920 x 1440)" };
        ImGui::SetNextItemWidth(w);
        int i = g_scale - 1;
        if (ImGui::Combo("##scale", &i, names, 3)) { g_scale = i + 1; SaveSettings(); }
    }
    ImGui::SetCursorPosX(x);
    ImGui::PushTextWrapPos(x + w);
    ImGui::TextColored(Dim, "Higher is sharper and needs a faster graphics card. 2x is the default.");
    ImGui::PopTextWrapPos();

    Gap(26);
    ImGui::SetCursorPosX(x);
    if (ImGui::Checkbox("Run the graphics chip on its own thread", &g_async)) SaveSettings();
    ImGui::SetCursorPosX(x);
    ImGui::PushTextWrapPos(x + w);
    ImGui::TextColored(Dim, "On by default, and much faster. Turn it off only if the game hangs or "
                            "draws wrongly, and say so in a bug report.");
    ImGui::PopTextWrapPos();

    Gap(26);
    ImGui::SetCursorPosX(x);
    if (ImGui::Checkbox("Full screen", &g_fullscreen)) {
        SetFullscreen(g_fullscreen);
        SaveSettings();
    }
    ImGui::SetCursorPosX(x);
    ImGui::TextColored(Dim, "F11 or Alt+Enter switches at any time, in the game as well.");

    Gap(26);
    ImGui::SetCursorPosX(x);
    ImGui::TextColored(Text, "Saved games folder");
    ImGui::SetCursorPosX(x);
    ImGui::PushTextWrapPos(x + w);
    ImGui::TextColored(g_saveDir.empty() ? Dim : Text, "%s%s", Utf8(SaveDir()).c_str(),
                       g_saveDir.empty() ? "  (default)" : "");
    ImGui::PopTextWrapPos();
    Gap(6);
    ImGui::SetCursorPosX(x);
    if (ImGui::Button("Choose folder...")) ChooseSaveFolder(g_frame);
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) OpenFolder(SaveDir());
    if (!g_saveDir.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Use default")) { g_saveDir.clear(); SaveSettings(); }
    }
    ImGui::SetCursorPosX(x);
    ImGui::PushTextWrapPos(x + w);
    ImGui::TextColored(Dim, "Saves already made are not moved. To keep them, copy the contents of the "
                            "old folder into the new one.");
    ImGui::PopTextWrapPos();

    BackButton();
}

static void AboutItem(const char *name, const char *text)
{
    using namespace Theme;
    ImGui::TextColored(Text, "%s", name);
    ImGui::Indent(18.f * Scale);
    ImGui::TextColored(Dim, "%s", text);
    ImGui::Unindent(18.f * Scale);
    ImGui::Dummy(ImVec2(0.f, 4.f * Scale));
}

static void AboutSection(const char *title)
{
    using namespace Theme;
    ImGui::Dummy(ImVec2(0.f, 10.f * Scale));
    ImGui::TextColored(Impact, "%s", title);
    ImGui::Separator();
}

static void DrawAbout()
{
    using namespace Theme;
    CentredText("About Recompilation", Impact);
    CentredText(std::string("v") + kVersion, Dim, Small);
    Gap(10);

    float w = fminf(760.f * Scale, ImGui::GetWindowWidth() - 60.f * Scale);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - w) * 0.5f);
    float h = ImGui::GetContentRegionAvail().y - 90.f * Scale;
    if (ImGui::BeginChild("##about", ImVec2(w, h))) {
        ImGui::PushTextWrapPos(w - 24.f * Scale);

        // First, not last: a warning nobody scrolls to is a warning nobody reads.
        AboutSection("Work in progress");
        ImGui::TextColored(Warn, "This is an early recompilation and it has not been played through. "
                                 "Expect problems, including ones that can block progress. Save often, "
                                 "and keep more than one save.");
        if (*kKnownIssue) ImGui::TextColored(Warn, "Known issue: %s", kKnownIssue);

        AboutSection("Recompilation");
        AboutItem("xboxrecomp", "by sp00nznet -- the Xbox static recompiler and runtime this is built on "
                                "(MIT). This project uses a fork of it. Not affiliated with or endorsed "
                                "by them.");
        AboutItem("xemu", "the open-source Xbox emulator (GPL-2.0-or-later). The runtime's graphics and "
                          "audio hardware code is derived from it, and it is the reference this port is "
                          "checked against.");

        AboutSection("Original game");
        AboutItem("Burnout", "developed by Criterion Games and published by Acclaim (2002). All game "
                             "code, assets and trademarks belong to their respective owners. This "
                             "launcher distributes none of them -- it rebuilds the game from the disc "
                             "you already own, on your own computer.");

        AboutSection("Libraries and tools");
        AboutItem("Dear ImGui", "by Omar Cornut and contributors (MIT) -- this launcher's interface");
        AboutItem("Capstone", "disassembly framework (BSD) -- reads the game's code during the build");
        AboutItem("Python", "by the Python Software Foundation (PSF License) -- runs the build");
        AboutItem("Zig", "by the Zig Software Foundation (MIT) -- the C compiler for the build");
        AboutItem("CMake and Ninja", "by Kitware (BSD-3-Clause) and the Ninja authors (Apache-2.0)");

        Gap(8);
        ImGui::TextColored(Dim, "Licenses differ between components. See each project's own license for "
                                "the terms that apply to it.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    BackButton();
}

static void DrawSplash(HWND hwnd)
{
    using namespace Theme;
    if (BeginPage("##splash")) {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        g_streaks.Draw(dl, vp->WorkPos, vp->WorkSize);
        float h = ImGui::GetWindowHeight();

        if (g_page != SplashPage::Main) {
            // A dark plate over the whole backdrop: these pages are for reading.
            dl->AddRectFilled(vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                                                  vp->WorkPos.y + vp->WorkSize.y), Col(Bg, 0.90f));
            ImGui::SetCursorPosY(40.f * Scale);
            if (g_page == SplashPage::Options) DrawOptions(hwnd);
            else DrawAbout();
            // B on the pad, or Escape, goes back -- unless a slider or text
            // field is being edited, where the same key means "stop editing".
            if (!ImGui::IsAnyItemActive() && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                              ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
                g_page = SplashPage::Main;
                g_focusPlay = true;
            }
            ImGui::End();
            return;
        }

        ImGui::SetCursorPosY(h * 0.14f);
        // A dark plate behind the title so the streaks never fight the text.
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(ImVec2(vp->WorkPos.x, p.y - 16.f * Scale),
                          ImVec2(vp->WorkPos.x + vp->WorkSize.x, p.y + 170.f * Scale), Col(Bg, 0.82f));
        Logo(Impact);
        Gap(30);

        // Sized and spaced as in the PS1 recomp launchers.
        const float bw = 320.f, bh = 58.f;
        ImGui::PushFont(Ui);
        if (g_gameRun) {
            if (CentredButton("Play Game", bw, bh)) g_playRequested = true;
        } else {
            ImGui::PushFont(Small);
            CentredText(g_gameError, Error);
            ImGui::PopFont();
            Gap(10);
            // The failed DLL may still be loaded, and Windows will not let the
            // build replace a loaded DLL: rebuild from a fresh process instead.
            if (CentredButton("Rebuild the game", bw, bh)) {
                DeleteFileW(g_paths.ready.c_str());
                DeleteFileW(Join(g_paths.work, L"stamps\\build.json").c_str());
                g_screen = Screen::Restarting;
            }
        }
        // Something is selected on arrival, so a pad user need not press a
        // direction first to find out the menu is there.
        if (g_focusPlay) { ImGui::SetItemDefaultFocus(); ImGui::SetKeyboardFocusHere(-1); g_focusPlay = false; }
        Gap(8);
        if (CentredButton("Options", bw, bh)) g_page = SplashPage::Options;
        Gap(8);
        if (CentredButton("About", bw, bh)) g_page = SplashPage::About;
        Gap(8);
        if (CentredButton("Exit", bw, bh)) PostQuitMessage(0);
        ImGui::PopFont();

        ImGui::SetCursorPosY(h - 70.f * Scale);
        if (*kKnownIssue)
            CentredText(std::string("Known issue in ") + kVersion + ": " + kKnownIssue, Warn, Small);
        else
            Gap(18);
        CentredText(std::string("v") + kVersion + "  -  rebuilt from your own disc; no game data is included.",
                    Dim, Small);
    }
    ImGui::End();
}

// ── Window and Direct3D ─────────────────────────────────────────

static ID3D11Device *g_device;
static ID3D11DeviceContext *g_context;
static IDXGISwapChain *g_swap;
static ID3D11RenderTargetView *g_rtv;
static bool g_menuLive;   // the menu owns the window's swap chain
static bool g_gameLive;   // the game DLL owns the window

static void CreateRtv()
{
    ID3D11Texture2D *back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static bool CreateDevice(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
    if (FAILED(hr)) { Log("D3D11 device creation failed: 0x%08lX", (unsigned long)hr); return false; }
    // Alt+Enter is ours (borderless full screen); DXGI's own would switch the
    // display mode.
    IDXGIFactory *factory = nullptr;
    if (SUCCEEDED(g_swap->GetParent(__uuidof(IDXGIFactory), (void **)&factory))) {
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
    }
    CreateRtv();
    return true;
}

// Two windows. The frame is the top-level window the user sees, moves and
// makes full screen; it is black. The view is a child of it, kept at the
// game's 4:3 and centred, and is what the menu and then the game draw into.
// The game's swap chain is a fixed 640x480 that DXGI stretches over its
// window, so drawing into the frame itself would squash the picture on any
// screen that is not 4:3; the bars come from the frame showing round it.
static WINDOWPLACEMENT g_windowed = { sizeof(WINDOWPLACEMENT) };

static void LayoutView()
{
    RECT c;
    GetClientRect(g_frame, &c);
    int fw = c.right, fh = c.bottom;
    if (!g_view || fw <= 0 || fh <= 0) return;
    int w = fw, h = fw * 3 / 4;
    if (h > fh) { h = fh; w = fh * 4 / 3; }
    SetWindowPos(g_view, nullptr, (fw - w) / 2, (fh - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Borderless full screen on whichever monitor the window is on: no display
// mode change, so switching is instant and other windows stay where they are.
static void SetFullscreen(bool on)
{
    LONG_PTR style = GetWindowLongPtrW(g_frame, GWL_STYLE);
    bool is = !(style & WS_OVERLAPPEDWINDOW);
    if (on == is) return;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(g_frame, &g_windowed);
        GetMonitorInfoW(MonitorFromWindow(g_frame, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(g_frame, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g_frame, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(g_frame, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_frame, &g_windowed);
        SetWindowPos(g_frame, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                                  SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    LayoutView();
    SetFocus(g_view);
}

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        LayoutView();
        return 0;
    case WM_SETFOCUS:
        if (g_view) SetFocus(g_view);   // keys go to the view, menu and game alike
        return 0;
    case WM_ERASEBKGND: {
        RECT c;
        GetClientRect(hwnd, &c);
        FillRect((HDC)wp, &c, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // Alt alone must not open a menu
        break;
    case WM_CLOSE:
        // While the game runs, its own handler on the view flushes its logs
        // and ends the process.
        if (g_view) SendMessageW(g_view, WM_CLOSE, 0, 0);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Hand the window to the game: everything the menu drew with goes, so the
// game's own device can create its swap chain on the same window.
static void ReleaseMenu()
{
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_menuLive = false;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_context) { g_context->ClearState(); g_context->Flush(); }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// The view's procedure. In the game the game's own procedure sits in front
// of this one and passes on what it does not handle, so F11 and Alt+Enter
// work there too.
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    bool altEnter = msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29));
    if ((msg == WM_KEYDOWN && wp == VK_F11) || altEnter) {
        if (!(lp & (1 << 30))) {   // not a key repeat
            g_fullscreen = !g_fullscreen;
            SetFullscreen(g_fullscreen);
            SaveSettings();
        }
        return 0;
    }
    if (msg == WM_SYSCHAR && wp == '\r') return 0;   // no beep for Alt+Enter
    if (g_menuLive && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        // g_swap, not g_menuLive: a saved full screen is applied at start-up
        // after the device exists but before ImGui is up, and skipping the
        // resize then left the swap chain at the windowed size, so the menu
        // was laid out for the screen and drawn off to the right (HeroGame,
        // 2026-10-13). g_swap is released before the game takes the window.
        if (g_swap && wp != SIZE_MINIMIZED) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            CreateRtv();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // Alt alone must not open a menu
        break;
    case WM_MOUSEACTIVATE:
        SetFocus(hwnd);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI GameThread(LPVOID hwnd)
{
    Log("game started");
    g_gameRun((HWND)hwnd);   // never returns; closing the window ends the process
    return 0;
}

static void Restart()
{
    // The fresh process prepares the game before it touches Direct3D (see the
    // top of this file).
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Log("restart failed: %lu", GetLastError());
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    // Per-monitor DPI awareness, looked up so the exe still starts on systems
    // without it.
    typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
    if (auto set = (SetCtxFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                    "SetProcessDpiAwarenessContext"))
        set((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);

    InitPaths();
    g_log = _wfopen(Join(g_paths.logs, L"launcher.log").c_str(), L"ab");
    Log("Burnout launcher %s; tools %s; data %s", kVersion, Utf8(g_paths.tools).c_str(),
        Utf8(g_paths.data).c_str());
    LoadSettings();

    // Order matters: prepare the game, if it is built, before any Direct3D.
    bool haveIso = !g_isoPath.empty() && ValidateIso(g_isoPath).empty();
    if (haveIso && BuildIsReady()) {
        if (PrepareGame()) Log("game prepared; log %s", Utf8(g_gameLog).c_str());
        else Log("prepare failed: %s", g_gameError.c_str());
        g_screen = Screen::Splash;
    } else {
        if (!haveIso) g_isoPath.clear();
        g_screen = haveIso ? Screen::Build : Screen::Iso;
    }

    WNDCLASSEXW fc = { sizeof fc };
    fc.lpfnWndProc = FrameProc;
    fc.hInstance = inst;
    fc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    fc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    fc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    fc.lpszClassName = L"BurnoutFrame";
    RegisterClassExW(&fc);

    WNDCLASSEXW wc = { sizeof wc };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"BurnoutLauncher";
    RegisterClassExW(&wc);

    // 4:3, the game's own shape, so there are no bars in a window. Sized to
    // the primary monitor's DPI and kept within its work area.
    HDC screen = GetDC(nullptr);
    float scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.f;
    ReleaseDC(nullptr, screen);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int ch = (int)(720 * scale), cw = ch * 4 / 3;
    int maxH = (work.bottom - work.top) * 9 / 10;
    if (ch > maxH) { ch = maxH; cw = ch * 4 / 3; }
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_frame = CreateWindowExW(0, fc.lpszClassName, L"Burnout Recompilation", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr,
                              nullptr, inst, nullptr);
    HWND hwnd = g_frame ? CreateWindowExW(0, wc.lpszClassName, L"Burnout Recompilation", WS_CHILD | WS_VISIBLE, 0, 0,
                                          cw, ch, g_frame, nullptr, inst, nullptr)
                        : nullptr;
    g_view = hwnd;
    LayoutView();
    if (!hwnd || !CreateDevice(hwnd)) {
        MessageBoxW(nullptr, L"Burnout could not open its window (Direct3D 11 is required).",
                    L"Burnout", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_frame, SW_SHOWDEFAULT);
    if (g_fullscreen) SetFullscreen(true);
    UpdateWindow(g_frame);
    SetFocus(g_view);

    typedef UINT(WINAPI * DpiForWindowFn)(HWND);
    if (auto dpiFor = (DpiForWindowFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))
        scale = dpiFor(g_frame) / 96.f;
    Theme::Scale = scale;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nothing to remember between runs
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    Theme::LoadFonts(scale);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    g_menuLive = true;

    if (g_screen == Screen::Build) StartBuild();
    // Testing aid: BURNOUT_AUTOPLAY=1 presses Play as soon as the splash is up.
    if (g_screen == Screen::Splash && g_gameRun && Env(L"BURNOUT_AUTOPLAY") == L"1")
        g_playRequested = true;

    MSG msg;
    for (;;) {
        if (g_gameLive) {
            // The game draws on its own threads; this thread only delivers the
            // window's messages (the game's handler sits in front of ours).
            BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (IsIconic(hwnd)) { Sleep(50); continue; }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Theme::Push();
        ImGui::PushFont(Theme::Body);
        switch (g_screen) {
        case Screen::Iso: DrawIso(hwnd); break;
        case Screen::Build: DrawBuild(); break;
        case Screen::Splash: DrawSplash(hwnd); break;
        case Screen::Restarting:
            if (Theme::BeginPage("##restart")) {
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.45f);
                Theme::CentredText("Starting Burnout...", Theme::Text);
            }
            ImGui::End();
            break;
        }
        ImGui::PopFont();
        Theme::Pop();
        ImGui::Render();

        const float bg[4] = { Theme::Bg.x, Theme::Bg.y, Theme::Bg.z, 1.f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, bg);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);

        if (g_screen == Screen::Restarting) {
            Log("restarting");
            Restart();
            break;
        }
        if (g_playRequested) {
            g_playRequested = false;
            ApplyOptions();
            ReleaseMenu();
            g_gameLive = true;
            HANDLE t = CreateThread(nullptr, 16u << 20, GameThread, hwnd, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                    nullptr);
            if (t) CloseHandle(t);
            else { Log("could not start the game thread: %lu", GetLastError()); break; }
        }
    }

    if (g_menuLive) ReleaseMenu();
    if (IsWindow(g_frame)) DestroyWindow(g_frame);
    Log("launcher exiting");
    // The game, once prepared, has threads of its own; end them with us.
    ExitProcess(0);
}
