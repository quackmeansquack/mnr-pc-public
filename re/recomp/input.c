/* Game controllers through the Windows joystick API (winmm): wheels, pedal sets, sticks, button boxes - anything
 * Windows lists as a game controller (DirectInput devices included; Xbox pads keep using XInput in host.c).
 * MNR_INPUT_CONFIG=<file> (written by the launcher's Controls page) maps their axes and buttons onto the PS3 pad:
 *
 *     # control | device | kind | index | invert | deadzone | low | high
 *     lx  | 046D:C262 | axis   | 0 | 0 | 0.02 | 0 | 65535      steering wheel -> left stick X
 *     r2  | 046D:C262 | axis   | 1 | 1 | 0.05 | 0 | 65535      accelerator   -> R2 (analog)
 *     cross | 046D:C262 | button | 0 | 0 | 0 | 0 | 0
 *
 * device: USB vendor:product (hex) of the controller, or #<n> for the n-th joystick. kind: axis (index 0-5 = X Y Z R U V),
 * button (0-31) or pov (0 up, 1 right, 2 down, 3 left). An axis is scaled from [low, high] to [0, 1] (flipped by invert).
 * controls: lx ly rx ry (sticks, full range), lx- lx+ ly- ly+ ... (one direction), l2 r2 (analog pressure + button),
 * cross circle square triangle l1 r1 start select up down left right l3 r3.
 *
 * Options (one per line, "option | name | value"):
 *     wheel_degrees  the wheel's rotation (e.g. 900) and steer_lock  the rotation that gives full lock in the game
 *                    (e.g. 270): steering on lx is scaled by wheel_degrees / steer_lock; steer_boost (0-100) starts
 *                    steering at that percentage as soon as the wheel leaves the centre (the game ignores small stick moves)
 *     ffb 1          force feedback through DirectInput on ffb_device (vendor:product; default: the steering's device):
 *                    the wheel's own autocentre is switched off and replaced by a centring spring (ffb_spring, 0-100)
 *                    and a damper (ffb_damper, 0-100), smooth instead of the stiff default */
#define DIRECTINPUT_VERSION 0x0800
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>
#include <dinput.h>
#include "input.h"
#include "present.h"

static struct { float wheel_degrees, steer_lock, ffb, ffb_spring, ffb_damper; unsigned ffb_vid, ffb_pid; float steer_boost; } opt = {0, 0, 0, 40, 20, 0, 0, 0};
static void ffb_start(void);

typedef struct { char control[16]; unsigned vid, pid; int joy_index; int kind, index, invert; float dead; float lo, hi; } bind_t;
enum { K_AXIS, K_BUTTON, K_POV };
static bind_t binds[128];
static int nbinds = -1;
static struct { int present, xinput; unsigned vid, pid; JOYINFOEX st; } joys[16];
static DWORD last_scan;

static void trim(char *s) {
    char *a = s; while (*a == ' ' || *a == '\t') a++;
    memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

static void load_config(void) {
    nbinds = 0;
    const char *path = getenv("MNR_INPUT_CONFIG");
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) { printf("[input] can't open %s\n", path); return; }
    char line[512];
    while (fgets(line, sizeof line, f) && nbinds < 128) {
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *field[8]; int n = 0; char *p = line;
        while (n < 8) { field[n++] = p; char *bar = strchr(p, '|'); if (!bar) break; *bar = 0; p = bar + 1; }
        for (int i = 0; i < n; i++) trim(field[i]);
        if (n >= 3 && !strcmp(field[0], "option")) {   /* option | name | value */
            const char *k = field[1]; float v = (float)atof(field[2]);
            if (!strcmp(k, "wheel_degrees")) opt.wheel_degrees = v;
            else if (!strcmp(k, "steer_lock")) opt.steer_lock = v;
            else if (!strcmp(k, "steer_boost")) opt.steer_boost = v;
            else if (!strcmp(k, "ffb")) opt.ffb = v;
            else if (!strcmp(k, "ffb_spring")) opt.ffb_spring = v;
            else if (!strcmp(k, "ffb_damper")) opt.ffb_damper = v;
            else if (!strcmp(k, "ffb_device")) sscanf(field[2], "%x:%x", &opt.ffb_vid, &opt.ffb_pid);
            continue;
        }
        if (n < 4) continue;
        bind_t *b = &binds[nbinds];
        memset(b, 0, sizeof *b);
        snprintf(b->control, sizeof b->control, "%s", field[0]);
        b->joy_index = -1;
        if (field[1][0] == '#') b->joy_index = atoi(field[1] + 1);
        else if (sscanf(field[1], "%x:%x", &b->vid, &b->pid) != 2) continue;
        b->kind = !strcmp(field[2], "axis") ? K_AXIS : !strcmp(field[2], "button") ? K_BUTTON : !strcmp(field[2], "pov") ? K_POV : -1;
        if (b->kind < 0) continue;
        b->index = atoi(field[3]);
        b->invert = n > 4 ? atoi(field[4]) : 0;
        b->dead = n > 5 ? (float)atof(field[5]) : 0;
        b->lo = n > 6 ? (float)atof(field[6]) : 0;
        b->hi = n > 7 ? (float)atof(field[7]) : 65535;
        if (b->hi == b->lo) b->hi = b->lo + 1;
        nbinds++;
    }
    fclose(f);
    printf("[input] %d control bindings from %s\n", nbinds, path);
    if (!opt.ffb_vid) for (int i = 0; i < nbinds; i++) if (!strcmp(binds[i].control, "lx") && binds[i].vid) { opt.ffb_vid = binds[i].vid; opt.ffb_pid = binds[i].pid; break; }
    if (opt.wheel_degrees > 0 && opt.steer_lock > 0) printf("[input] wheel %.0f degrees, full lock at %.0f\n", opt.wheel_degrees, opt.steer_lock);
    if (opt.ffb > 0) ffb_start();
}

/* An Xbox-style controller shows in Windows as a HID device whose path has "IG_" after its VID/PID; host.c already
 * reads those through XInput */
static int is_xinput(unsigned vid, unsigned pid) {
    UINT n = 0;
    if (GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || !n) return 0;
    RAWINPUTDEVICELIST *l = malloc(n * sizeof *l);
    int found = 0;
    if (l && (n = GetRawInputDeviceList(l, &n, sizeof *l)) != (UINT)-1) {
        char want[32]; snprintf(want, sizeof want, "VID_%04X&PID_%04X&IG_", vid, pid);
        for (UINT i = 0; i < n && !found; i++) {
            char name[512]; UINT sz = sizeof name;
            if (GetRawInputDeviceInfoA(l[i].hDevice, RIDI_DEVICENAME, name, &sz) == (UINT)-1) continue;
            for (char *c = name; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
            found = strstr(name, want) != NULL;
        }
    }
    free(l);
    return found;
}
static void scan(void) {
    JOYCAPSA caps;
    for (UINT id = 0; id < 16; id++) {
        joys[id].present = 0;
        JOYINFOEX ji = {sizeof ji, JOY_RETURNALL};
        if (joyGetPosEx(id, &ji) != JOYERR_NOERROR) continue;
        if (joyGetDevCapsA(id, &caps, sizeof caps) != JOYERR_NOERROR) continue;
        joys[id].present = 1; joys[id].vid = caps.wMid; joys[id].pid = caps.wPid;
        joys[id].xinput = is_xinput(caps.wMid, caps.wPid);
    }
    last_scan = GetTickCount();
}

static int joy_for(const bind_t *b) {
    if (b->joy_index >= 0) return b->joy_index < 16 && joys[b->joy_index].present ? b->joy_index : -1;
    for (int id = 0; id < 16; id++) if (joys[id].present && joys[id].vid == b->vid && joys[id].pid == b->pid) return id;
    return -1;
}

/* the bound control's value in [0, 1] (buttons: 0 or 1) */
static int value(const bind_t *b, float *v) {
    int id = joy_for(b);
    if (id < 0) return 0;
    const JOYINFOEX *s = &joys[id].st;
    float x = 0;
    if (b->kind == K_AXIS) {
        DWORD raw[6] = {s->dwXpos, s->dwYpos, s->dwZpos, s->dwRpos, s->dwUpos, s->dwVpos};
        if (b->index < 0 || b->index > 5) return 0;
        x = ((float)raw[b->index] - b->lo) / (b->hi - b->lo);
        x = x < 0 ? 0 : x > 1 ? 1 : x;
    } else if (b->kind == K_BUTTON) {
        x = b->index >= 0 && b->index < 32 && (s->dwButtons >> b->index & 1) ? 1.0f : 0.0f;
    } else {
        DWORD pov = s->dwPOV;   /* hundredths of a degree, 0xFFFF centred */
        if (pov != 0xFFFF) {
            int dir = b->index * 9000, d = abs((int)pov - dir); if (d > 18000) d = 36000 - d;
            x = d < 6750 ? 1.0f : 0.0f;   /* within 67.5 degrees: diagonals count for both */
        }
    }
    if (b->invert) x = 1 - x;
    *v = x;
    return 1;
}

/* Split screen: when a controller set up on the launcher's Controls page (a wheel, a PlayStation-style pad, ...) is
 * connected, it is player 1 and the first Xbox controller becomes player 2 (host.c) */
int input_bound_present(void) {
    if (nbinds < 0) load_config();
    if (!nbinds) return 0;
    if (!last_scan || GetTickCount() - last_scan > 2000) scan();
    for (int i = 0; i < nbinds; i++) { int id = joy_for(&binds[i]); if (id >= 0 && !joys[id].xinput) return 1; }
    return 0;
}
int input_read(input_state_t *out) {
    memset(out, 0, sizeof *out);
    out->lx = out->ly = out->rx = out->ry = -1; out->l2 = out->r2 = -1;
    if (nbinds < 0) load_config();
    if (!nbinds) return 0;
    if (!last_scan || GetTickCount() - last_scan > 2000) scan();   /* devices plugged in later */
    for (UINT id = 0; id < 16; id++) if (joys[id].present) {
        joys[id].st.dwSize = sizeof joys[id].st; joys[id].st.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &joys[id].st) != JOYERR_NOERROR) joys[id].present = 0;
    }
    static const struct { const char *name; int word; unsigned bit; } btn[] = {
        {"select", 1, 0x01}, {"l3", 1, 0x02}, {"r3", 1, 0x04}, {"start", 1, 0x08}, {"up", 1, 0x10}, {"right", 1, 0x20}, {"down", 1, 0x40}, {"left", 1, 0x80},
        {"l2b", 2, 0x01}, {"r2b", 2, 0x02}, {"l1", 2, 0x04}, {"r1", 2, 0x08}, {"triangle", 2, 0x10}, {"circle", 2, 0x20}, {"cross", 2, 0x40}, {"square", 2, 0x80}};
    float half[4][2] = {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};   /* lx ly rx ry: (minus, plus) */
    int any = 0;
    for (int i = 0; i < nbinds; i++) {
        bind_t *b = &binds[i]; float v;
        if (!value(b, &v)) continue;
        any = 1;
        const char *c = b->control;
        int stick = !strncmp(c, "lx", 2) ? 0 : !strncmp(c, "ly", 2) ? 1 : !strncmp(c, "rx", 2) ? 2 : !strncmp(c, "ry", 2) ? 3 : -1;
        if (stick >= 0) {
            int *dst = stick == 0 ? &out->lx : stick == 1 ? &out->ly : stick == 2 ? &out->rx : &out->ry;
            if (c[2] == 0) {   /* full axis, centre deadzone */
                float d = v - 0.5f;
                if (stick == 0 && opt.wheel_degrees > 0 && opt.steer_lock > 0) {   /* wheel: full lock within steer_lock degrees */
                    d *= opt.wheel_degrees / opt.steer_lock;
                    if (d > 0.5f) d = 0.5f; else if (d < -0.5f) d = -0.5f;
                }
                if (d > -b->dead / 2 && d < b->dead / 2) d = 0;
                if (stick == 0 && opt.steer_boost > 0 && d != 0) {   /* skip the game's own stick deadzone: the smallest turn already steers */
                    float m = (d < 0 ? -d : d) * 2, b0 = opt.steer_boost / 100.0f;
                    m = b0 + (1 - b0) * m; if (m > 1) m = 1;
                    d = (d < 0 ? -m : m) / 2;
                }
                int u = (int)((d + 0.5f) * 255 + 0.5f);
                *dst = u < 0 ? 0 : u > 255 ? 255 : u;
            } else {
                float m = v < b->dead ? 0 : v;
                if (c[2] == '-') { if (m > half[stick][0]) half[stick][0] = m; } else if (m > half[stick][1]) half[stick][1] = m;
            }
            continue;
        }
        if (!strcmp(c, "l2") || !strcmp(c, "r2")) {   /* analog pedal/trigger: pressure, and the button past 10% */
            float m = v < b->dead ? 0 : (v - b->dead) / (1 - b->dead);
            int p = (int)(m * 255 + 0.5f);
            int *dst = c[0] == 'l' ? &out->l2 : &out->r2;
            if (p > *dst) *dst = p;
            if (m > 0.1f) out->d2 |= c[0] == 'l' ? 0x01 : 0x02;
            continue;
        }
        for (unsigned k = 0; k < sizeof btn / sizeof btn[0]; k++)
            if (!strcmp(c, btn[k].name) && v > 0.5f) { if (btn[k].word == 1) out->d1 |= btn[k].bit; else out->d2 |= btn[k].bit; }
    }
    for (int s = 0; s < 4; s++) {   /* one-direction bindings (e.g. steering on two buttons) */
        if (half[s][0] < 0 && half[s][1] < 0) continue;
        float m = (half[s][1] > 0 ? half[s][1] : 0) - (half[s][0] > 0 ? half[s][0] : 0);
        int *dst = s == 0 ? &out->lx : s == 1 ? &out->ly : s == 2 ? &out->rx : &out->ry;
        *dst = (int)((m + 1) * 127.5f + 0.5f);
    }
    return any;
}

/* ---- force feedback (DirectInput): a smooth centring spring and a damper instead of the wheel's stiff autocentre */
static LPDIRECTINPUT8 ff_di;
static LPDIRECTINPUTDEVICE8 ff_dev;

static BOOL CALLBACK ff_enum(LPCDIDEVICEINSTANCEA inst, LPVOID ref) {
    (void)ref;
    LPDIRECTINPUTDEVICE8 d;
    if (FAILED(IDirectInput8_CreateDevice(ff_di, &inst->guidInstance, &d, NULL))) return DIENUM_CONTINUE;
    DIPROPDWORD p = {{sizeof p, sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, 0};
    unsigned vid = 0, pid = 0;
    if (SUCCEEDED(IDirectInputDevice8_GetProperty(d, DIPROP_VIDPID, &p.diph))) { vid = LOWORD(p.dwData); pid = HIWORD(p.dwData); }
    if (!opt.ffb_vid || (vid == opt.ffb_vid && pid == opt.ffb_pid)) { ff_dev = d; printf("[ffb] using %s (%04X:%04X)\n", inst->tszProductName, vid, pid); return DIENUM_STOP; }
    IDirectInputDevice8_Release(d);
    return DIENUM_CONTINUE;
}

static LPDIRECTINPUTEFFECT ff_condition(REFGUID kind, float strength) {   /* strength 0-100 -> DirectInput coefficient */
    LONG c = (LONG)(strength * 100.0f); if (c < 0) c = 0; if (c > DI_FFNOMINALMAX) c = DI_FFNOMINALMAX;
    DICONDITION cond = {0, c, c, DI_FFNOMINALMAX, DI_FFNOMINALMAX, 0};
    DWORD axes[1] = {DIJOFS_X}; LONG dir[1] = {0};
    DIEFFECT e = {0};
    e.dwSize = sizeof e; e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS; e.dwDuration = INFINITE; e.dwGain = DI_FFNOMINALMAX;
    e.dwTriggerButton = DIEB_NOTRIGGER; e.cAxes = 1; e.rgdwAxes = axes; e.rglDirection = dir;
    e.cbTypeSpecificParams = sizeof cond; e.lpvTypeSpecificParams = &cond;
    LPDIRECTINPUTEFFECT fx = NULL;
    if (FAILED(IDirectInputDevice8_CreateEffect(ff_dev, kind, &e, &fx, NULL))) { printf("[ffb] effect not supported\n"); return NULL; }
    IDirectInputEffect_Start(fx, 1, 0);
    return fx;
}

static DWORD WINAPI ff_thread(void *p) {
    (void)p;
    for (int i = 0; i < 200 && !rsx_window; i++) Sleep(50);   /* exclusive access needs our window */
    if (!rsx_window) { printf("[ffb] no window\n"); return 0; }
    if (FAILED(DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&ff_di, NULL))) { printf("[ffb] DirectInput unavailable\n"); return 0; }
    IDirectInput8_EnumDevices(ff_di, DI8DEVCLASS_GAMECTRL, ff_enum, NULL, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    if (!ff_dev) { printf("[ffb] no force feedback wheel found\n"); return 0; }
    IDirectInputDevice8_SetDataFormat(ff_dev, &c_dfDIJoystick2);
    if (FAILED(IDirectInputDevice8_SetCooperativeLevel(ff_dev, rsx_window, DISCL_EXCLUSIVE | DISCL_BACKGROUND))) printf("[ffb] couldn't get exclusive access\n");
    DIPROPDWORD ac = {{sizeof ac, sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, DIPROPAUTOCENTER_OFF};
    IDirectInputDevice8_SetProperty(ff_dev, DIPROP_AUTOCENTER, &ac.diph);   /* the stiff built-in centring off */
    if (FAILED(IDirectInputDevice8_Acquire(ff_dev))) { printf("[ffb] couldn't acquire the wheel\n"); return 0; }
    if (opt.ffb_spring > 0) ff_condition(&GUID_Spring, opt.ffb_spring);
    if (opt.ffb_damper > 0) ff_condition(&GUID_Damper, opt.ffb_damper);
    printf("[ffb] spring %.0f%%, damper %.0f%%\n", opt.ffb_spring, opt.ffb_damper);
    for (;;) {   /* re-acquire after the wheel is lost (another app, unplug) */
        Sleep(1000);
        if (IDirectInputDevice8_Poll(ff_dev) == DIERR_INPUTLOST || IDirectInputDevice8_Poll(ff_dev) == DIERR_NOTACQUIRED) IDirectInputDevice8_Acquire(ff_dev);
    }
    return 0;
}
static void ffb_start(void) { CreateThread(NULL, 0, ff_thread, NULL, 0, NULL); }
