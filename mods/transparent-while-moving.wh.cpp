// ==WindhawkMod==
// @id              transparent-while-moving
// @name            Transparent While Moving (with Fade)
// @description     Fade to semi-transparent while dragging/resizing, then fade back on release. Uses thread-pool timers for reliability (Explorer-friendly). Per-app include/exclude supported.
// @version         0.6
// @author          You
// @github          vicomannen
// @include         *
// @exclude         windhawk.exe
// @license         MIT
// @compilerOptions -std=c++17
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
Fades a window to a chosen opacity while it is being moved/resized
(`WM_ENTERSIZEMOVE`) and fades it back to fully opaque when finished
(`WM_EXITSIZEMOVE`). For better reliability in Explorer, animation runs via
**thread-pool timers** (not `WM_TIMER`). The mod also supports per-process
include/exclude lists.

### Tips
- If you notice a brief “flash” in Explorer’s left navigation pane, add
  **explorer.exe** to the exclude list in Settings (or use the “only include”
  mode for specific apps).
- This mod releases `WS_EX_LAYERED` when the window is fully opaque to avoid
  visual side-effects in apps that dislike layered windows at rest.

### How it works
- Hooks `DefWindowProcW` globally.
- Starts an easing-less linear tween on `WM_ENTERSIZEMOVE`.
- Ensures the target HWND’s **root window** is animated (defensive if a child
  window receives the message).
- Finishes the fade on `WM_EXITSIZEMOVE` and a few extra “home” messages
  (`WM_CAPTURECHANGED`, `WM_CANCELMODE`, `WM_NCLBUTTONUP @ HTCAPTION`) because
  Explorer sometimes misses `EXITSIZEMOVE`.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- name: opacity
  type: number
  text: Opacity while moving (0-255)
  default: 180
- name: fadeMs
  type: number
  text: Fade duration (ms) - 0 = instant
  default: 120
- name: scope
  type: dropdown
  text: Where to apply
  options:
    - Apply to all apps except those listed
    - Only apply to the apps listed below
  default: 0
- name: appList
  type: multiline-string
  text: App list (exe names, one per line; commas/semicolons also work)
  description: Examples: explorer.exe, snipaste.exe, chrome.exe
  default: ""
*/
// ==/WindhawkModSettings==

#include <windhawk_api.h>
#include <windows.h>
#include <cwctype>
#include <cwchar>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <string>

// ---------------- Settings ----------------
static int  g_targetOpacity = 180;   // 0..255
static int  g_fadeMs        = 120;   // ms; 0 => instant
static int  g_scope         = 0;     // 0 = allExceptListed, 1 = onlyListed
static bool g_enabled       = true;  // computed per process

// Thread-pool timers (reliable under Explorer move/size loop)
static HANDLE g_timerQueue = nullptr;
static const ULONG kTickMs = 15; // ~60–70 FPS

struct WinAnim {
    bool      active       = false;
    BYTE      startAlpha   = 255;
    BYTE      targetAlpha  = 255;
    ULONGLONG startTick    = 0;
    int       durationMs   = 0;

    // our own truth of last applied alpha
    BYTE      lastApplied  = 255;

    // thread-pool timer
    HANDLE    hTimer       = nullptr;
};

static std::unordered_map<HWND, WinAnim> g_anim;

// ---------------- Helpers ----------------
static void SetLayered(HWND hwnd, bool on) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (on) {
        if (!(ex & WS_EX_LAYERED))
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    } else {
        if (ex & WS_EX_LAYERED)
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }
}

static void ApplyAlphaRemember(HWND hwnd, BYTE a) {
    SetLayered(hwnd, true);
    SetLayeredWindowAttributes(hwnd, 0, a, LWA_ALPHA);
    g_anim[hwnd].lastApplied = a;
}

static BYTE ClampByte(int v) {
    return (BYTE)std::min(255, std::max(0, v));
}

static std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}

static std::wstring CurrentExeNameLower() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    if (!name) name = path; else name++;
    return ToLower(name);
}

static std::unordered_set<std::wstring> ParseAppListToSet(const wchar_t* s) {
    std::unordered_set<std::wstring> out;
    if (!s || !*s) return out;
    std::wstring token;
    for (const wchar_t* p = s;; ++p) {
        wchar_t c = *p;
        bool sep = (c == 0 || c == L',' || c == L';' || c == L'\n' || c == L'\r' || c == L'\t' || c == L' ');
        if (!sep) token.push_back(c);
        if (sep) {
            // trim
            size_t i = 0, j = token.size();
            while (i < j && iswspace(token[i])) ++i;
            while (j > i && iswspace(token[j - 1])) --j;
            if (j > i) out.insert(ToLower(token.substr(i, j - i)));
            token.clear();
            if (c == 0) break;
        }
    }
    return out;
}

// ---------------- Animation (thread-pool) ----------------
static VOID CALLBACK FadeTickCb(PVOID ctx, BOOLEAN /*fired*/) {
    HWND hwnd = (HWND)ctx;
    if (!IsWindow(hwnd)) return;

    auto it = g_anim.find(hwnd);
    if (it == g_anim.end()) return;
    WinAnim& st = it->second;
    if (!st.active) return;

    ULONGLONG now = GetTickCount64();
    double t = (st.durationMs > 0)
             ? std::min(1.0, (double)(now - st.startTick) / (double)st.durationMs)
             : 1.0;

    double v = (1.0 - t) * (double)st.startAlpha + t * (double)st.targetAlpha;
    BYTE a = ClampByte((int)(v + 0.5));

    ApplyAlphaRemember(hwnd, a);

    if (t >= 1.0) {
        HANDLE h = st.hTimer;
        st.hTimer = nullptr;
        st.active = false;
        if (h) DeleteTimerQueueTimer(g_timerQueue, h, INVALID_HANDLE_VALUE);

        if (st.targetAlpha == 255) {
            SetLayered(hwnd, false);   // release layered at rest
            st.lastApplied = 255;
        }
    }
}

static void StopFade(HWND hwnd, bool forceToTarget) {
    auto it = g_anim.find(hwnd);
    if (it == g_anim.end()) return;
    WinAnim& st = it->second;

    HANDLE h = st.hTimer;
    st.hTimer = nullptr;
    st.active = false;
    if (h) DeleteTimerQueueTimer(g_timerQueue, h, INVALID_HANDLE_VALUE);

    if (forceToTarget) {
        ApplyAlphaRemember(hwnd, st.targetAlpha);
    }
    if (st.targetAlpha == 255) {
        SetLayered(hwnd, false);
        st.lastApplied = 255;
    }
}

static void StartFade(HWND hwnd, BYTE targetAlpha, int durMs) {
    WinAnim& st = g_anim[hwnd];

    if (st.hTimer) {
        DeleteTimerQueueTimer(g_timerQueue, st.hTimer, INVALID_HANDLE_VALUE);
        st.hTimer = nullptr;
    }

    st.startAlpha  = st.lastApplied;
    st.targetAlpha = targetAlpha;
    st.startTick   = GetTickCount64();
    st.durationMs  = std::max(0, durMs);
    st.active      = true;

    if (st.durationMs == 0 || st.startAlpha == st.targetAlpha) {
        ApplyAlphaRemember(hwnd, st.targetAlpha);
        st.active = false;
        if (st.targetAlpha == 255) {
            SetLayered(hwnd, false);
            st.lastApplied = 255;
        }
        return;
    }

    SetLayered(hwnd, true); // ensure layered while animating
    if (!g_timerQueue) g_timerQueue = CreateTimerQueue();
    CreateTimerQueueTimer(&st.hTimer, g_timerQueue, FadeTickCb,
                          (PVOID)hwnd, 0, kTickMs, WT_EXECUTEDEFAULT);
}

// ---------------- Hook ----------------
using DefWindowProcW_t = LRESULT (WINAPI*)(HWND, UINT, WPARAM, LPARAM);
static DefWindowProcW_t DefWindowProcW_Original;

static LRESULT WINAPI DefWindowProcW_Hook(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    if (!g_enabled)
        return DefWindowProcW_Original(hWnd, Msg, wParam, lParam);

    // Always animate the root window (defensive)
    HWND root = GetAncestor(hWnd, GA_ROOT);
    if (!root) root = hWnd;

    switch (Msg) {
    case WM_ENTERSIZEMOVE:
        StartFade(root, (BYTE)g_targetOpacity, g_fadeMs);
        break;

    case WM_EXITSIZEMOVE:
        StartFade(root, 255, g_fadeMs);
        break;

    // Explorer sometimes misses EXITSIZEMOVE
    case WM_CAPTURECHANGED:
        StartFade(root, 255, g_fadeMs);
        break;

    case WM_CANCELMODE:
        StartFade(root, 255, g_fadeMs);
        break;

    case WM_NCLBUTTONUP:
        if (wParam == HTCAPTION) StartFade(root, 255, g_fadeMs);
        break;

    case WM_DESTROY:
    case WM_NCDESTROY:
        StopFade(root, /*forceToTarget=*/false);
        ApplyAlphaRemember(root, 255);  // fail-safe: do not leave transparent
        SetLayered(root, false);
        g_anim.erase(root);
        break;
    }

    return DefWindowProcW_Original(hWnd, Msg, wParam, lParam);
}

// ---------------- Settings & lifecycle ----------------
static void LoadSettings() {
    g_targetOpacity = std::clamp(Wh_GetIntSetting(L"opacity"), 0, 255);
    g_fadeMs        = Wh_GetIntSetting(L"fadeMs");
    if (g_fadeMs < 0) g_fadeMs = 0;

    g_scope = Wh_GetIntSetting(L"scope");

    PCWSTR listStr = Wh_GetStringSetting(L"appList"); // const
    std::wstring listCopy = listStr ? listStr : L"";
    auto set = ParseAppListToSet(listCopy.c_str());

    std::wstring exe = CurrentExeNameLower();
    bool listed = set.find(exe) != set.end();
    g_enabled = (g_scope == 0) ? (!listed) : listed; // 0 = allExceptListed, 1 = onlyListed
}

BOOL Wh_ModInit() {
    LoadSettings();
    g_timerQueue = CreateTimerQueue();

    if (!Wh_SetFunctionHook((void*)DefWindowProcW,
                            (void*)DefWindowProcW_Hook,
                            (void**)&DefWindowProcW_Original)) {
        Wh_Log(L"[TWM] Failed to hook DefWindowProcW");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModUninit() {
    for (auto& kv : g_anim) {
        if (kv.second.hTimer) DeleteTimerQueueTimer(g_timerQueue, kv.second.hTimer, INVALID_HANDLE_VALUE);
    }
    g_anim.clear();
    if (g_timerQueue) {
        DeleteTimerQueue(g_timerQueue);
        g_timerQueue = nullptr;
    }
}

void Wh_ModSettingsChanged() {
    LoadSettings(); // toggles g_enabled live for this process
}
