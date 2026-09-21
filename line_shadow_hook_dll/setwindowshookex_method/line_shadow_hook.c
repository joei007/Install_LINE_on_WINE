/*
 * line_shadow_hook.c
 * ===================
 *
 * FINAL, proven fix for the LINE (Wine/Proton) drop-shadow "ghost
 * window" problem. See dwmapi_proxy.c's header comment for the full
 * investigation history (confirmed via direct source inspection that
 * this is neither Wine's nor Qt's doing -- it's LINE's own
 * closed-source shadow/resize-grip implementation, triggered by a
 * condition we can't determine without disassembling the binary).
 *
 * Strategy: hook CreateWindowExW to detect the 8 shadow fragments
 * (thin slivers), then hook UpdateLayeredWindow/
 * UpdateLayeredWindowIndirect to let the real draw call still happen
 * (keeping the window's state valid for resize hit-testing) but
 * force the blend's SourceConstantAlpha to 0, making it permanently
 * transparent. Shadow invisible, native resize intact.
 *
 * Loaded via the classic AppInit_DLLs mechanism (Wine implements this
 * the same way real Windows does), which injects it into every
 * process that loads user32.dll in this Wine prefix.
 *
 * Build (from Linux, cross-compiling with mingw-w64):
 *   x86_64-w64-mingw32-gcc -shared -O2 -o line_shadow_hook.dll \
 *       line_shadow_hook.c minhook/src/buffer.c minhook/src/hook.c \
 *       minhook/src/trampoline.c minhook/src/hde/hde64.c \
 *       -Iminhook/include -Iminhook/src -static-libgcc \
 *       -Wl,--out-implib,line_shadow_hook.lib
 */

#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#include "MinHook.h"

#define LOG_PATH L"C:\\hooks\\line_shadow_hook.log"

static void log_line(const char *fmt, ...)
{
    CreateDirectoryW(L"C:\\hooks", NULL); /* ignore failure: already exists is fine */
    FILE *f = _wfopen(LOG_PATH, L"a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

/* ==================== shadow-window neutralizing hooks ==================== */

#define MAX_TRACKED 64

static HWND g_tracked[MAX_TRACKED];
static int g_tracked_count = 0;
static CRITICAL_SECTION g_lock;
static BOOL g_lock_ready = FALSE;

typedef HWND(WINAPI *CreateWindowExW_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                         int, int, int, HWND, HMENU,
                                         HINSTANCE, LPVOID);
typedef BOOL(WINAPI *UpdateLayeredWindow_t)(HWND, HDC, POINT *, SIZE *, HDC,
                                             POINT *, COLORREF,
                                             BLENDFUNCTION *, DWORD);
typedef BOOL(WINAPI *UpdateLayeredWindowIndirect_t)(HWND, const VOID *);

static CreateWindowExW_t Real_CreateWindowExW = NULL;
static UpdateLayeredWindow_t Real_UpdateLayeredWindow = NULL;
static UpdateLayeredWindowIndirect_t Real_UpdateLayeredWindowIndirect = NULL;

static void track_add(HWND hwnd)
{
    if (!g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    if (g_tracked_count < MAX_TRACKED) g_tracked[g_tracked_count++] = hwnd;
    LeaveCriticalSection(&g_lock);
}

static BOOL track_contains(HWND hwnd)
{
    if (!g_lock_ready) return FALSE;
    BOOL found = FALSE;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_tracked_count; i++) {
        if (g_tracked[i] == hwnd) { found = TRUE; break; }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

static HWND WINAPI Hook_CreateWindowExW(
    DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle,
    int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
    HINSTANCE hInstance, LPVOID lpParam)
{
    HWND hwnd = Real_CreateWindowExW(dwExStyle, lpClassName, lpWindowName,
                                      dwStyle, X, Y, nWidth, nHeight,
                                      hWndParent, hMenu, hInstance, lpParam);
    if (!hwnd) return hwnd;

    BOOL is_shadow_named = FALSE;
    if (lpClassName && ((ULONG_PTR)lpClassName > 0xFFFF)) {
        is_shadow_named = (wcsncmp(lpClassName, L"shadow_side_", 12) == 0) ||
                           (wcsncmp(lpClassName, L"shadow_corner_", 14) == 0);
    }

    /* Note: the shadow_side_* / shadow_corner_* fragments are always
       created at w=0,h=0 (real size set later via a separate call),
       so a size check here would never actually match them -- it
       would only risk false-positiving on some unrelated tiny
       window. Class-name match only; should_suppress() (paint time)
       re-confirms the same class-name match once they're resized. */
    if (is_shadow_named) {
        track_add(hwnd);
    }
    return hwnd;
}

/* Confirms via the window's actual class name that this is really
   one of the shadow fragments -- "thin" alone turned out to also
   match transient Qt popup menus mid-animation, permanently marking
   them transparent and making them unclickable. Checking for
   "SaveBits" in the class name was ALSO too broad: Qt uses that
   naming for the backing-store optimization on every popup (menus,
   tooltips, dropdowns...), not just the shadow-related ones -- so it
   caught real menus too. Only the "shadow_side_"/"shadow_corner_"
   prefixes are unique to LINE's actual shadow fragments. */
static BOOL is_shadow_class(HWND hwnd)
{
    wchar_t cls[128];
    if (!GetClassNameW(hwnd, cls, 128)) return FALSE;
    return (wcsncmp(cls, L"shadow_side_", 12) == 0) ||
           (wcsncmp(cls, L"shadow_corner_", 14) == 0);
}

/* Checks the tracked set first (cheap), then falls back to a live
   class-name check -- catches shadow fragments even if their
   CreateWindowExW call was missed (e.g. created with w=0,h=0 then
   resized afterward, which happens for some of these). Class name
   alone (no size/thinness gate): a DPI-based thickness threshold
   would be fragile -- on a HiDPI display the fragments could render
   wider than any fixed pixel cutoff we guess, silently escaping
   detection. GetClassNameW is cheap enough to call on every paint. */
static BOOL should_suppress(HWND hwnd)
{
    if (track_contains(hwnd)) return TRUE;
    if (is_shadow_class(hwnd)) {
        track_add(hwnd);
        return TRUE;
    }
    return FALSE;
}

static BOOL WINAPI Hook_UpdateLayeredWindow(
    HWND hWnd, HDC hdcDst, POINT *pptDst, SIZE *psize, HDC hdcSrc,
    POINT *pptSrc, COLORREF crKey, BLENDFUNCTION *pblend, DWORD dwFlags)
{
    if (should_suppress(hWnd)) {
        /* Let the REAL call still happen (window keeps valid state
           for resize hit-testing), just force full transparency. */
        BLENDFUNCTION blend = pblend ? *pblend
                                      : (BLENDFUNCTION){AC_SRC_OVER, 0, 255,
                                                         AC_SRC_ALPHA};
        blend.SourceConstantAlpha = 0;
        return Real_UpdateLayeredWindow(hWnd, hdcDst, pptDst, psize, hdcSrc,
                                         pptSrc, crKey, &blend,
                                         dwFlags | ULW_ALPHA);
    }
    return Real_UpdateLayeredWindow(hWnd, hdcDst, pptDst, psize, hdcSrc,
                                     pptSrc, crKey, pblend, dwFlags);
}

static BOOL WINAPI Hook_UpdateLayeredWindowIndirect(HWND hWnd,
                                                      const VOID *pULWInfo)
{
    if (should_suppress(hWnd) && pULWInfo) {
        UPDATELAYEREDWINDOWINFO info = *(const UPDATELAYEREDWINDOWINFO *)pULWInfo;
        BLENDFUNCTION blend = info.pblend ? *info.pblend
                                           : (BLENDFUNCTION){AC_SRC_OVER, 0,
                                                              255,
                                                              AC_SRC_ALPHA};
        blend.SourceConstantAlpha = 0;
        info.pblend = &blend;
        info.dwFlags |= ULW_ALPHA;
        return Real_UpdateLayeredWindowIndirect(hWnd, &info);
    }
    return Real_UpdateLayeredWindowIndirect(hWnd, pULWInfo);
}

static void install_hooks(void)
{
    MH_STATUS st;

    if (MH_Initialize() != MH_OK) {
        log_line("[error] MH_Initialize failed");
        return;
    }

    st = MH_CreateHookApi(L"user32", "CreateWindowExW",
                           (LPVOID)&Hook_CreateWindowExW,
                           (LPVOID *)&Real_CreateWindowExW);
    if (st != MH_OK) {
        log_line("[error] hooking CreateWindowExW failed: %s",
                  MH_StatusToString(st));
    }

    st = MH_CreateHookApi(L"user32", "UpdateLayeredWindow",
                           (LPVOID)&Hook_UpdateLayeredWindow,
                           (LPVOID *)&Real_UpdateLayeredWindow);
    if (st != MH_OK) {
        log_line("[error] hooking UpdateLayeredWindow failed: %s",
                  MH_StatusToString(st));
    }

    st = MH_CreateHookApi(L"user32", "UpdateLayeredWindowIndirect",
                           (LPVOID)&Hook_UpdateLayeredWindowIndirect,
                           (LPVOID *)&Real_UpdateLayeredWindowIndirect);
    if (st != MH_OK) {
        log_line("[error] hooking UpdateLayeredWindowIndirect failed: %s",
                  MH_StatusToString(st));
    }

    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        log_line("[error] MH_EnableHook(ALL) failed: %s",
                  MH_StatusToString(st));
    }
}

/* Exported WH_CBT hook procedure -- only needed by the injector.exe /
   SetWindowsHookEx alternative loading path. Its only job is to give
   SetWindowsHookEx a reason to map this DLL into the target process;
   the actual work happens in DllMain. Harmless to keep exported even
   when loading via AppInit_DLLs instead. */
__declspec(dllexport) LRESULT CALLBACK CBTHookProc(int nCode, WPARAM wParam,
                                                     LPARAM lParam)
{
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved)
{
    (void)lpReserved;
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hinstDLL);
        InitializeCriticalSection(&g_lock);
        g_lock_ready = TRUE;

        install_hooks();
        break;
    }
    case DLL_PROCESS_DETACH:
        MH_Uninitialize();
        if (g_lock_ready) {
            DeleteCriticalSection(&g_lock);
            g_lock_ready = FALSE;
        }
        break;
    }
    return TRUE;
}
