/* Shows the RSX display buffer: a plain Win32 window blitting the A8R8G8B8 framebuffer from guest
 * RSX local memory with GDI. MNR_HEADLESS=1 disables the window. MNR_SHOT=<n>[,<n>...] saves the
 * displayed buffer at those flip numbers as shot_<n>.bmp in the working directory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "rsx.h"
#include "rsx_d3d.h"

static HWND wnd;
static uint32_t *pixels;            /* host BGRA copy of the last presented frame */
static uint32_t pw, ph;
static SRWLOCK px_lock = SRWLOCK_INIT;
HWND rsx_window;                    /* read by the pad code for focus */

static void toggle_fullscreen(HWND h);
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        if (rsx_use_d3d == 1) { ValidateRect(h, NULL); return 0; }
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        AcquireSRWLockShared(&px_lock);
        if (pixels) {
            BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), (LONG)pw, -(LONG)ph, 1, 32, BI_RGB}};
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, pw, ph, pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        ReleaseSRWLockShared(&px_lock);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_F9 && !(lp & (1 << 30))) { extern void mnr_request_dump(void); mnr_request_dump(); }   /* thread dump to logs/ */
        if (wp == VK_F11 && !(lp & (1 << 30))) { toggle_fullscreen(h); return 0; }
        { extern int osk_key(unsigned msg, unsigned wp); if (osk_key(WM_KEYDOWN, (unsigned)wp)) return 0; }   /* the text entry (host.c) */
        {   /* - and = (or the numpad's - and +): the game's overall volume (audio.c) */
            extern void audio_volume_step(int dir);
            if (wp == VK_OEM_MINUS || wp == VK_SUBTRACT) { audio_volume_step(-1); return 0; }
            if (wp == VK_OEM_PLUS || wp == VK_ADD) { audio_volume_step(1); return 0; }
            if (wp == VK_F8 && !(lp & (1 << 30))) { extern void voice_mic_cycle(void); voice_mic_cycle(); return 0; }   /* voice.c: next microphone */
        }
        break;
    case WM_CHAR: {   /* typed text for the text entry; an ANSI window: characters above 127 come in the system code page */
        extern int osk_key(unsigned msg, unsigned wp);
        unsigned ch = (unsigned)wp;
        if (ch >= 128 && ch < 256) { char b = (char)ch; WCHAR w; if (MultiByteToWideChar(CP_ACP, 0, &b, 1, &w, 1) == 1) ch = w; }
        if (osk_key(WM_CHAR, ch)) return 0;
        break;
    }
    case WM_SYSKEYDOWN:   /* Alt+Enter: the same borderless fullscreen (DXGI's own exclusive mode is turned off in rsx_d3d.c) */
        if (wp == VK_RETURN && (lp & (1 << 29))) { if (!(lp & (1 << 30))) toggle_fullscreen(h); return 0; }
        break;
    case WM_SYSCHAR: if (wp == '\r') return 0; break;   /* no "ding" for Alt+Enter */
    case WM_CLOSE:
#ifdef MNR_PGO_GEN   /* build.py --pgo-gen: the profile goes to re\mnr.profraw (ExitProcess skips the runtime's own write) */
        { extern void __llvm_profile_set_filename(const char *); extern int __llvm_profile_write_file(void);
          __llvm_profile_set_filename("mnr.profraw"); __llvm_profile_write_file(); }
#endif
        ExitProcess(0);
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* F11 / Alt+Enter: borderless fullscreen on the window's monitor and back. The 1280x720 picture is stretched to the
 * window by the swap chain, so nothing else changes size. MNR_FULLSCREEN=1 starts in it. */
static void toggle_fullscreen(HWND h) {
    static WINDOWPLACEMENT saved = {sizeof saved}; static int full;
    if (!full) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(h, &saved) || !GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
        SetWindowLongPtrA(h, GWL_STYLE, (GetWindowLongPtrA(h, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 1;
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, (GetWindowLongPtrA(h, GWL_STYLE) & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &saved);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        full = 0;
    }
}

static DWORD WINAPI window_thread(void *p) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "MNRWindow"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, 1280, 720};
    {   /* MNR_RESOLUTION: open at that size when it fits the screen (fullscreen uses the screen's own) */
        const char *e = getenv("MNR_RESOLUTION"); int h = e ? atoi(e) : 0;
        if (h > 720 && h * 16 / 9 <= GetSystemMetrics(SM_CXSCREEN) && h <= GetSystemMetrics(SM_CYSCREEN) - 80) { r.right = h * 16 / 9; r.bottom = h; }
    }
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wnd = CreateWindowA("MNRWindow", "ModNation Racers (recompiled)", WS_OVERLAPPEDWINDOW | (p ? 0 : WS_VISIBLE),
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    rsx_window = wnd;
    if (!p && getenv("MNR_FULLSCREEN") && atoi(getenv("MNR_FULLSCREEN"))) toggle_fullscreen(wnd);
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageA(&m); }
    return 0;
}

static void save_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t sz = pw * ph * 4;
    BITMAPFILEHEADER fh = {0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + sz, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER ih = {sizeof ih, (LONG)pw, -(LONG)ph, 1, 32, BI_RGB};
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(pixels, 1, sz, f);
    fclose(f);
}

static int want_shot(long n) {
    const char *e = getenv("MNR_SHOT");
    for (; e && *e; ) {
        char *end; long v = strtol(e, &end, 10);
        if (end == e) break;
        if (v == n) return 1;
        e = *end == ',' ? end + 1 : end;
    }
    return 0;
}

void rsx_present(uint32_t buffer) {
    static int headless = -1;
    if (rsx_use_d3d < 0) { const char *e = getenv("MNR_RSX"); rsx_use_d3d = !(e && !strcmp(e, "sw")); }
    if (headless < 0) {
        const char *e = getenv("MNR_HEADLESS");
        headless = e && *e == '1';
        if (!headless || rsx_use_d3d == 1) CreateThread(NULL, 1 << 20, window_thread, (void *)(intptr_t)headless, 0, NULL);
        if (rsx_use_d3d == 1) {   /* the D3D device presents into the window (hidden when headless) */
            for (int i = 0; i < 500 && !rsx_window; i++) Sleep(10);
            if (rsx_window) rsx_d3d_init(rsx_window);
        }
    }
    long n = rsx_flips;
    {   /* FPS counter in the window title, refreshed twice a second (MNR_NO_FPS=1 turns it off) */
        static int off = -1; static LARGE_INTEGER t0, f; static long n0;
        if (off < 0) { off = getenv("MNR_NO_FPS") != NULL; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); n0 = n; }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - t0.QuadPart) / f.QuadPart;
        if (!off && rsx_window && dt >= 0.5) {
            char title[96]; double fps = (n - n0) / dt;
            snprintf(title, sizeof title, "ModNation Racers (recompiled) - %.1f fps (%.1f ms)", fps, fps > 0 ? 1000.0 / fps : 0.0);
            DWORD_PTR r; SendMessageTimeoutA(rsx_window, WM_SETTEXT, 0, (LPARAM)title, SMTO_ABORTIFHUNG, 50, &r);   /* never stall the frame on a busy window */
            t0 = now; n0 = n;
        }
    }
    int shot = want_shot(n);
    {   extern volatile int pad_script_shot; static int k;   /* MNR_PAD_SCRIPT "shot" steps */
        if (pad_script_shot) { pad_script_shot = 0; shot = 2 + k++; } }
    {   /* MNR_SHOT_EVERY=<k>: every k-th frame once drawing is on (with MNR_RSX_DRAW_HEAVY) */
        extern volatile int rsx_heavy_started; extern long rsx_heavy_flip;
        const char *e = getenv("MNR_SHOT_EVERY");
        if (e && rsx_heavy_started == 1 && rsx_heavy_flip && n > rsx_heavy_flip + 1 && (n - rsx_heavy_flip) % atol(e) == 0) shot = 1;
    }
    {   extern void perf_frame(void); perf_frame(); }   /* [perf] lines (host.c) */
    if (rsx_use_d3d == 1) {
        rsx_d3d_present(buffer);
        if (!shot) return;
        rsx_d3d_read_display(buffer, NULL, 0, 0);   /* write the display surface back for the screenshot */
    }
    if (headless && !shot) return;
    rsx_display_t d = rsx_display[buffer & 7];
    if (!d.width || !d.height || d.width > 4096 || d.height > 4096) return;
    AcquireSRWLockExclusive(&px_lock);
    if (pw != d.width || ph != d.height) { free(pixels); pw = d.width; ph = d.height; pixels = malloc(pw * ph * 4); }
    for (uint32_t y = 0; y < ph; y++) {
        const uint8_t *s = gp(RSX_LOCAL_BASE + d.offset + y * d.pitch);
        uint32_t *o = pixels + y * pw;
        for (uint32_t x = 0; x < pw; x++, s += 4) o[x] = (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | s[3];   /* A R G B (BE) -> BGRX */
    }
    if (shot) { char p[64]; snprintf(p, sizeof p, "shot_%ld.bmp", n); save_bmp(p); printf("[rsx] saved %s\n", p); }
    ReleaseSRWLockExclusive(&px_lock);
    if (wnd) InvalidateRect(wnd, NULL, FALSE);
}
