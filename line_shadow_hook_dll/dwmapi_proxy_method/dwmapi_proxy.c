/*
 * dwmapi_proxy.c
 * ===============
 *
 * A "proxy DLL": pretends to be dwmapi.dll (installed as System32's
 * dwmapi.dll in this Wine prefix, with the genuine file renamed aside
 * to dwmapi_real.dll -- see resolve_real_dwmapi() below for the
 * install layout this expects). Every real dwmapi.dll export is
 * forwarded unchanged to the genuine DLL via a blind assembly
 * trampoline (so signatures/calling convention never need to be
 * gotten exactly right). LINE keeps 100% normal DWM functionality.
 *
 * FINAL, proven fix for the LINE (Wine/Proton) drop-shadow
 * "ghost window" problem:
 *   Root cause -- confirmed NOT to be Wine's or Qt's doing (checked
 *   both source trees directly, zero matches for the
 *   "shadow_side_*"/"shadow_corner_*" class names) -- is LINE's OWN
 *   closed-source code: it creates 8 auxiliary top-level windows
 *   (4 edges + 4 corners) that serve as BOTH the visual shadow AND
 *   the resize-grab hit-test area for its custom-chrome main window.
 *   Many experiments (stripping WS_POPUP, stripping WS_THICKFRAME,
 *   stripping CS_DROPSHADOW, spoofing DwmIsCompositionEnabled /
 *   IsThemeActive / IsCompositionActive) either didn't stop it or
 *   also broke native resize -- LINE's internal trigger condition
 *   can't be determined without disassembling the binary, which is
 *   out of scope.
 *
 *   So instead of preventing creation, this hooks CreateWindowExW to
 *   detect the 8 shadow fragments (thin slivers, <=12px in one
 *   dimension) and hooks UpdateLayeredWindow/UpdateLayeredWindowIndirect
 *   to let the REAL draw call still happen (so the window keeps valid
 *   state for resize hit-testing) but forces the blend's
 *   SourceConstantAlpha to 0, making it permanently, fully
 *   transparent. Net effect: shadow invisible, native resize intact.
 *
 * Logs to C:\hooks\line_shadow_hook.log only on error/warning
 * conditions (missing real dwmapi.dll, hook install failure) -- no
 * routine/diagnostic logging in normal operation.
 *
 * Build (from Linux, cross-compiling with mingw-w64):
 *   x86_64-w64-mingw32-gcc -shared -O2 -o dwmapi.dll \
 *       dwmapi_proxy.c dwmapi_proxy.def \
 *       ../minhook/src/buffer.c ../minhook/src/hook.c \
 *       ../minhook/src/trampoline.c ../minhook/src/hde/hde64.c \
 *       -I../minhook/include -I../minhook/src \
 *       -static-libgcc -Wl,--out-implib,dwmapi.lib -luser32 -lkernel32
 *
 * Install: in the Wine prefix's system32, rename the genuine
 * dwmapi.dll to dwmapi_real.dll, then place this build as dwmapi.dll
 * in its place. Also needs:
 *   wine reg add "HKEY_CURRENT_USER\Software\Wine\DllOverrides" /v dwmapi /d native /f
 */

#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#include "MinHook.h"

/* ============================ logging ============================ */

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

/* ================= forward every real dwmapi export ================= */

#define DWM_EXPORT_LIST(X) \
    X(DwmAttachMilContent)              \
    X(DwmDefWindowProc)                 \
    X(DwmDetachMilContent)              \
    X(DwmEnableBlurBehindWindow)        \
    X(DwmEnableComposition)             \
    X(DwmEnableMMCSS)                   \
    X(DwmExtendFrameIntoClientArea)     \
    X(DwmFlush)                         \
    X(DwmGetColorizationColor)          \
    X(DwmGetCompositionTimingInfo)      \
    X(DwmGetGraphicsStreamClient)       \
    X(DwmGetGraphicsStreamTransformHint)\
    X(DwmGetTransportAttributes)        \
    X(DwmGetWindowAttribute)            \
    X(DwmInvalidateIconicBitmaps)       \
    X(DwmIsCompositionEnabled)          \
    X(DwmModifyPreviousDxFrameDuration) \
    X(DwmQueryThumbnailSourceSize)      \
    X(DwmRegisterThumbnail)             \
    X(DwmRenderGesture)                 \
    X(DwmSetDxFrameDuration)            \
    X(DwmSetIconicLivePreviewBitmap)    \
    X(DwmSetIconicThumbnail)            \
    X(DwmSetPresentParameters)          \
    X(DwmSetWindowAttribute)            \
    X(DwmShowContact)                   \
    X(DwmTetherContact)                 \
    X(DwmTransitionOwnedWindow)         \
    X(DwmUnregisterThumbnail)           \
    X(DwmUpdateThumbnailProperties)     \
    X(DllCanUnloadNow)                  \
    X(DllGetClassObject)

/* one real-function-pointer slot per forwarded export.
   volatile: prevents -O2 from treating these as dead stores, since
   the compiler can't see the inline-asm string references them. */
#define DECLARE_SLOT(name) static void *volatile g_real_##name = NULL;
DWM_EXPORT_LIST(DECLARE_SLOT)
#undef DECLARE_SLOT

/* blind trampoline: jumps to the resolved real function with every
   register/stack argument exactly as the caller set them up -- works
   for any real signature since we never touch the calling convention.
   Excludes DllCanUnloadNow/DllGetClassObject, which windows.h already
   declares with a fixed COM prototype -- those get explicit thunks
   below instead so their declarations don't conflict. */
#define DWM_THUNK_LIST(X)                   \
    X(DwmAttachMilContent)                  \
    X(DwmDefWindowProc)                     \
    X(DwmDetachMilContent)                  \
    X(DwmEnableBlurBehindWindow)            \
    X(DwmEnableComposition)                 \
    X(DwmEnableMMCSS)                       \
    X(DwmExtendFrameIntoClientArea)         \
    X(DwmFlush)                             \
    X(DwmGetColorizationColor)              \
    X(DwmGetCompositionTimingInfo)          \
    X(DwmGetGraphicsStreamClient)           \
    X(DwmGetGraphicsStreamTransformHint)    \
    X(DwmGetTransportAttributes)            \
    X(DwmGetWindowAttribute)                \
    X(DwmInvalidateIconicBitmaps)           \
    X(DwmIsCompositionEnabled)              \
    X(DwmModifyPreviousDxFrameDuration)     \
    X(DwmQueryThumbnailSourceSize)          \
    X(DwmRegisterThumbnail)                 \
    X(DwmRenderGesture)                     \
    X(DwmSetDxFrameDuration)                \
    X(DwmSetIconicLivePreviewBitmap)        \
    X(DwmSetIconicThumbnail)                \
    X(DwmSetPresentParameters)              \
    X(DwmSetWindowAttribute)                \
    X(DwmShowContact)                       \
    X(DwmTetherContact)                     \
    X(DwmTransitionOwnedWindow)             \
    X(DwmUnregisterThumbnail)               \
    X(DwmUpdateThumbnailProperties)

#define DECLARE_THUNK(name)                                    \
    __attribute__((naked)) void name(void)                     \
    {                                                            \
        __asm__ volatile(                                      \
            "movq g_real_" #name "(%%rip), %%rax\n\t"           \
            "jmp *%%rax\n\t"                                     \
            :                                                    \
            :                                                    \
            : "rax");                                            \
    }
DWM_THUNK_LIST(DECLARE_THUNK)
#undef DECLARE_THUNK

/* COM entry points: windows.h already declares these with a fixed
   prototype (via combaseapi.h), so redeclare them to match instead
   of the generic void(void) shape. The naked body is identical --
   we still just blind-jump to the resolved real function. */
__attribute__((naked)) HRESULT STDAPICALLTYPE DllCanUnloadNow(void)
{
    __asm__ volatile(
        "movq g_real_DllCanUnloadNow(%rip), %rax\n\t"
        "jmp *%rax\n\t");
}

__attribute__((naked)) HRESULT STDAPICALLTYPE
DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    __asm__ volatile(
        "movq g_real_DllGetClassObject(%rip), %rax\n\t"
        "jmp *%rax\n\t");
}

/* Tries, in order:
     1) <system32>\dwmapi_real.dll  -- the genuine file, renamed aside,
        used when THIS proxy itself has been installed AS System32's
        dwmapi.dll.
     2) <system32>\dwmapi.dll       -- the genuine file, untouched,
        used when this proxy was instead installed next to the
        target EXE (System32's copy is still the real one). */
static HMODULE load_real_dwmapi(void)
{
    WCHAR sysdir[MAX_PATH];
    WCHAR path[MAX_PATH];
    HMODULE h;

    if (!GetSystemDirectoryW(sysdir, MAX_PATH)) {
        log_line("[error] GetSystemDirectoryW failed");
        return NULL;
    }

    _snwprintf(path, MAX_PATH, L"%s\\dwmapi_real.dll", sysdir);
    h = LoadLibraryW(path);
    if (h) return h;

    _snwprintf(path, MAX_PATH, L"%s\\dwmapi.dll", sysdir);
    h = LoadLibraryW(path);
    if (h) return h;

    log_line("[error] could not load real dwmapi via either "
             "dwmapi_real.dll or dwmapi.dll in %ls", sysdir);
    return NULL;
}

static BOOL resolve_real_dwmapi(void)
{
    HMODULE real = load_real_dwmapi();
    if (!real) return FALSE;

    int missing = 0;
#define RESOLVE(name)                                            \
    g_real_##name = (void *)GetProcAddress(real, #name);         \
    if (!g_real_##name) missing++;
    DWM_EXPORT_LIST(RESOLVE)
#undef RESOLVE

    if (missing) {
        log_line("[warn] %d dwmapi export(s) not found in real DLL "
                  "(fine unless LINE actually calls one of them)",
                  missing);
    }
    return TRUE;
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

/* ============================== DllMain ============================== */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved)
{
    (void)lpReserved;
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hinstDLL);
        InitializeCriticalSection(&g_lock);
        g_lock_ready = TRUE;

        if (!resolve_real_dwmapi()) {
            log_line("[fatal] real dwmapi.dll functions unresolved -- "
                      "DWM calls from the app may crash it. Aborting hook "
                      "install but keeping the proxy loaded.");
        }
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
