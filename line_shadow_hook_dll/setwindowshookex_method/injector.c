/*
 * injector.c
 * ===========
 *
 * Finds the LINE window and installs a WH_CBT hook targeted at its
 * thread, using line_shadow_hook.dll. This makes Windows/Wine's own
 * hook-injection machinery load that DLL into LINE's process for us
 * -- no file placement tricks (dwmapi proxy, AppInit_DLLs, DllOverrides)
 * needed, since none of those worked reliably in this environment.
 *
 * Must be kept running the whole time LINE is open: if this process
 * exits, the hook (and therefore the injected DLL's effect) goes
 * away with it, same tradeoff as the earlier line_shadow_killer.py
 * approach, just implemented as a native hook instead of external
 * polling.
 *
 * Behavior:
 *   - Polls for a window whose title contains "line" (case-insensitive)
 *     and is not a tiny sliver (same heuristic used elsewhere in this
 *     project to distinguish the real LINE window from shadow
 *     fragments).
 *   - Once found, loads line_shadow_hook.dll and installs the hook on
 *     that window's thread.
 *   - Keeps checking that the window is still valid; if LINE closes
 *     and reopens (new thread), unhooks and re-installs automatically.
 *
 * Build (from Linux, cross-compiling with mingw-w64):
 *   x86_64-w64-mingw32-gcc -O2 -o injector.exe injector.c -luser32
 *
 * Run: place line_shadow_hook.dll in the SAME FOLDER as injector.exe
 * (or pass a full path via the HOOK_DLL_PATH define below), then run
 * injector.exe and leave it running. No admin rights needed.
 */

#include <windows.h>
#include <wchar.h>
#include <stdio.h>

#define HOOK_DLL_NAME L"line_shadow_hook.dll"
#define NAME_HINT L"line"
#define POLL_MS 1000
#define MIN_REAL_SIZE 50 /* px; guards against matching thin shadow fragments */

static HHOOK g_hook = NULL;
static HWND g_hooked_window = NULL;

typedef struct {
    HWND result;
} FindCtx;

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lparam)
{
    FindCtx *ctx = (FindCtx *)lparam;

    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t title[256];
    int len = GetWindowTextW(hwnd, title, 256);
    if (len == 0) return TRUE;

    for (wchar_t *p = title; *p; p++) *p = towlower(*p);
    if (wcsstr(title, NAME_HINT) == NULL) return TRUE;

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if ((r.right - r.left) < MIN_REAL_SIZE || (r.bottom - r.top) < MIN_REAL_SIZE)
        return TRUE; /* too small, likely not the real window */

    ctx->result = hwnd;
    return FALSE; /* stop enumeration, found it */
}

static HWND find_line_window(void)
{
    FindCtx ctx = {0};
    EnumWindows(enum_proc, (LPARAM)&ctx);
    return ctx.result;
}

static void unhook_if_needed(void)
{
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        printf("[injector] unhooked (window went away)\n");
        fflush(stdout);
        g_hook = NULL;
        g_hooked_window = NULL;
    }
}

static void install_for_window(HWND hwnd, HMODULE hookDll, HOOKPROC proc)
{
    DWORD tid = GetWindowThreadProcessId(hwnd, NULL);
    g_hook = SetWindowsHookExW(WH_CBT, proc, hookDll, tid);
    if (!g_hook) {
        printf("[injector] SetWindowsHookExW failed, error=%lu\n", GetLastError());
        fflush(stdout);
        return;
    }
    g_hooked_window = hwnd;
    printf("[injector] hook installed on tid=%lu (window title matched \"%ls\")\n",
           tid, NAME_HINT);
    fflush(stdout);

    /* Nudge the target thread so the CBT hook actually fires soon
       (loading our DLL) instead of waiting for it to happen to
       process some other window event on its own. */
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

int main(void)
{
    HMODULE hookDll = LoadLibraryW(HOOK_DLL_NAME);
    if (!hookDll) {
        printf("[injector] could not load %ls (put it in the same folder "
               "as injector.exe), error=%lu\n",
               HOOK_DLL_NAME, GetLastError());
        return 1;
    }

    HOOKPROC proc = (HOOKPROC)GetProcAddress(hookDll, "CBTHookProc");
    if (!proc) {
        printf("[injector] CBTHookProc not exported from %ls\n", HOOK_DLL_NAME);
        return 1;
    }

    printf("[injector] ready, watching for the LINE window... (Ctrl+C to stop)\n");
    fflush(stdout);

    for (;;) {
        if (g_hooked_window && !IsWindow(g_hooked_window)) {
            unhook_if_needed();
        }

        if (!g_hook) {
            HWND hwnd = find_line_window();
            if (hwnd) {
                install_for_window(hwnd, hookDll, proc);
            }
        }

        Sleep(POLL_MS);
    }

    return 0;
}
