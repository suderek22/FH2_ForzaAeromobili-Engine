// Forza Aeromobili Engine - FH2 mod platform mod.
// An experimental arcade "flight engine" for Forza Horizon 2. When switched on (Pause > Settings > MODS)
// it drives the car you are currently in like a GTA V style plane: RT throttle, LT airbrake,
// left stick pitch/roll, right stick yaw. No new cars, no models - this is the physics/controls test bed.
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "fh2mod.h"
#include "fh2game.h"

#define MOD_VERSION "0.1.0"

// Game offsets (Forza Horizon 2 PC, build the platform 2.8.0 targets)
#define RVA_UPDATE_DYNAMICS 0x12C1EA0   // CCarDynamics::UpdateDynamics
#define CAR_KIND            0x4D08      // 0 = local player's car
#define CAR_STATE_PTR       0x4C08
#define STATE_POS           0xEF0
#define STATE_ROT           0xE70       // 3 rows of a float3, 16 byte stride: right, up, forward
#define DYN_POS             0x10
#define DYN_VEL             0x50
#define DYN_ANG             0x60

static const FH2ModApi *g_api;
static FH2Mod *g_mod;
static unsigned char *g_game;
static char g_ini[MAX_PATH];

// ---- configuration (ForzaAeromobiliEngine.ini) ----
struct Cfg {
    float max_accel, max_speed, coast_drag, airbrake;
    float takeoff_speed, air_speed, lift_factor;
    float grip, alignment_force, climb_loss;
    float pitch_rate, roll_rate, yaw_rate, ang_drag, level_rate, level_pitch;
    float bank_yaw, bank_pitch, sense, roll_dir, pitch_dir, yaw_dir;
} g_cfg;

static float cfgf(const char *k, float d) {
    char b[64], def[64];
    snprintf(def, sizeof def, "%g", d);
    GetPrivateProfileStringA("flight", k, def, b, sizeof b, g_ini);
    return (float)atof(b);
}

static void load_cfg() {
    g_cfg.max_accel = cfgf("max_accel", 22);       g_cfg.max_speed = cfgf("max_speed", 95);
    g_cfg.coast_drag = cfgf("coast_drag", 0.25f);  g_cfg.airbrake = cfgf("airbrake", 0.8f);
    g_cfg.takeoff_speed = cfgf("takeoff_speed", 38); g_cfg.air_speed = cfgf("air_speed", 22); g_cfg.lift_factor = cfgf("lift_factor", 1.0f);
    g_cfg.grip = cfgf("grip", 3.0f);               g_cfg.alignment_force = cfgf("alignment_force", 1.5f); g_cfg.climb_loss = cfgf("climb_loss", 0.5f);
    g_cfg.pitch_rate = cfgf("pitch_rate", 1.2f);   g_cfg.roll_rate = cfgf("roll_rate", 2.2f); g_cfg.yaw_rate = cfgf("yaw_rate", 0.6f);
    g_cfg.ang_drag = cfgf("ang_drag", 40);         g_cfg.level_rate = cfgf("level_rate", 2.0f); g_cfg.level_pitch = cfgf("level_pitch", 1.2f);
    g_cfg.bank_yaw = cfgf("bank_yaw", 0.6f);       g_cfg.bank_pitch = cfgf("bank_pitch", 0.05f); g_cfg.sense = cfgf("sense", -1.f);
    g_cfg.roll_dir = cfgf("roll_dir", 1);          g_cfg.pitch_dir = cfgf("pitch_dir", 1);    g_cfg.yaw_dir = cfgf("yaw_dir", 1);
}

static void logf_(const char *fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    g_api->log(g_mod, b);
}

// ---- gamepad (polled on a worker thread) ----
typedef struct { DWORD pk; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XSTATE;
typedef DWORD(WINAPI *XGetState)(DWORD, XSTATE *);
static volatile LONG g_lx, g_ly, g_rx, g_lt, g_rt;
static volatile LONG g_flight;        // the Mods-page switch

static DWORD WINAPI pad_thread(void *) {
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    XGetState get = m ? (XGetState)GetProcAddress(m, "XInputGetState") : 0;
    if (!get) { logf_("XInput not available"); return 0; }
    for (;;) {
        XSTATE s;
        if (get(0, &s) == 0) { g_lx = s.lx; g_ly = s.ly; g_rx = s.rx; g_lt = s.lt; g_rt = s.rt; }
        Sleep(4);
    }
}

static float stick(LONG v) {
    float f = v / 32767.f;
    if (fabsf(f) < 0.12f) return 0;
    return f > 0 ? (f - 0.12f) / 0.88f : (f + 0.12f) / 0.88f;
}

// ---- safe memory access ----
static int safe_read(const void *src, void *dst, size_t n) {
    __try { memcpy(dst, src, n); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int safe_write(void *dst, const void *src, size_t n) {
    __try { memcpy(dst, src, n); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int plausible_ptr(unsigned long long p) { return p > 0x10000 && p < 0x00007FFFFFFFFFFFull && (p & 7) == 0; }

static unsigned char *g_dyn;          // CCarDynamics (rigid body) of the player's car
static unsigned char *g_pstate;       // player's car state

// =====================================================================================
// GTA V style arcade flight model. The game's own rigid body is driven with artificial
// forces/torques (no aerodynamic lift formulas):
//   Throttle (RT)  : forward thrust, hard speed cap, drag when released
//   Airbrake (LT)  : extra drag (the stock brakes still work on the ground)
//   Pitch/Roll     : left stick -> target body rates
//   Yaw            : right stick -> target body rate
//   AlignmentForce : passive torque turning the nose toward the velocity vector
//   Faked bank turn: rolling adds yaw + a little nose-up
//   Angular drag   : body rate is pulled hard toward the commanded rate
//   Auto-level     : centred sticks bring wings and nose back to the horizon
// Below air_speed the car keeps its stock handling; the flight model blends in above it.
// All tunables are in ForzaAeromobiliEngine.ini ([flight]).
// =====================================================================================
static LONGLONG g_last_t;
static float g_sdt = 1.f / 60.f;      // smoothed time between calls
static float g_wsign;                 // +1/-1: sense of the game's angular velocity vs the rotation matrix
static float g_prev_m[3][3];
static int g_have_prev_m;

static void cross3(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static void flight_step() {
    unsigned char *dyn = g_dyn, *state = g_pstate;
    if (!dyn || !state) return;

    LARGE_INTEGER t, fq;
    QueryPerformanceCounter(&t); QueryPerformanceFrequency(&fq);
    float dtr = g_last_t ? (float)(t.QuadPart - g_last_t) / (float)fq.QuadPart : 1.f / 60.f;
    g_last_t = t.QuadPart;
    if (dtr > 0.f && dtr < 0.1f) g_sdt += (dtr - g_sdt) * 0.1f;
    float dt = clampf(dtr > 0.f && dtr < 0.1f ? dtr : g_sdt, 0.001f, 0.05f);

    float v[3], m[3][3], cur[3];
    if (!safe_read(dyn + DYN_VEL, v, 12) || !safe_read(dyn + DYN_ANG, cur, 12)) return;
    for (int r = 0; r < 3; r++)
        if (!safe_read(state + STATE_ROT + r * 16, m[r], 12)) return;

    // basis: row 1 = up, row 2 = forward, right = up x forward
    float right[3], up[3], fwd[3];
    for (int i = 0; i < 3; i++) { up[i] = m[1][i]; fwd[i] = m[2][i]; }
    cross3(up, fwd, right);

    // learn the angular velocity convention from how the basis really rotates (db/dt = w x b)
    if (g_have_prev_m) {
        float we[3] = {0, 0, 0};
        for (int r = 0; r < 3; r++) {
            float db[3] = {(m[r][0] - g_prev_m[r][0]) / dt, (m[r][1] - g_prev_m[r][1]) / dt, (m[r][2] - g_prev_m[r][2]) / dt};
            float c[3];
            cross3(m[r], db, c);
            for (int i = 0; i < 3; i++) we[i] += 0.5f * c[i];
        }
        float d = dot3(we, cur), mag = sqrtf(dot3(we, we)), cm = sqrtf(dot3(cur, cur));
        if (mag > 0.05f && cm > 0.3f && fabsf(d) > 0.5f * mag * cm) {
            static float acc;
            acc = acc * 0.9f + (d > 0 ? 1.f : -1.f);
            if (fabsf(acc) > 3.f) {
                float s = acc > 0 ? 1.f : -1.f;
                if (s != g_wsign) { g_wsign = s; logf_("angular velocity sense calibrated: %+.0f", s); }
            }
        }
    }
    memcpy(g_prev_m, m, sizeof m);
    g_have_prev_m = 1;
    float ws = (g_wsign != 0 ? g_wsign : 1.f) * g_cfg.sense;

    // ---- inputs ----
    float rt = g_rt / 255.f, lt = g_lt / 255.f;
    float roll = stick(g_lx) * g_cfg.roll_dir;
    float noseup = -stick(g_ly) * g_cfg.pitch_dir;   // stick up = nose down
    float yaw = stick(g_rx) * g_cfg.yaw_dir;

    float speed = sqrtf(dot3(v, v));
    float vf = dot3(v, fwd);
    // 0 = on the ground (stock car handling), 1 = flying
    float air = clampf((speed - g_cfg.air_speed) / g_cfg.air_speed, 0.f, 1.f);

    // ---- linear forces ----
    // thrust + quadratic drag (terminal speed = max_speed) + drag when off the throttle
    float accel = rt * g_cfg.max_accel - (g_cfg.max_accel / (g_cfg.max_speed * g_cfg.max_speed)) * speed * vf;
    float lin_drag = (rt < 0.05f ? g_cfg.coast_drag : 0.f) + g_cfg.airbrake * lt;
    // fake lift: cancels gravity along world up, scaled by how close we are to takeoff speed
    float lift = 9.8f * clampf((vf * vf) / (g_cfg.takeoff_speed * g_cfg.takeoff_speed), 0.f, 1.f) * g_cfg.lift_factor;
    float v2[3];
    for (int i = 0; i < 3; i++) v2[i] = v[i] + fwd[i] * accel * dt;
    v2[1] += lift * dt * (0.2f + 0.8f * air);
    float s2 = sqrtf(dot3(v2, v2));
    // grip: the velocity vector follows the nose (arcade "plane flies where it points")
    if (s2 > 1.f && air > 0.f) {
        float f = clampf(g_cfg.grip * air * dt, 0.f, 1.f);
        for (int i = 0; i < 3; i++) v2[i] = v2[i] * (1.f - f) + fwd[i] * s2 * f;
        float s3 = sqrtf(dot3(v2, v2));
        if (s3 > 0.01f) for (int i = 0; i < 3; i++) v2[i] *= s2 / s3;      // keep speed
        // climbing costs speed, diving gains it
        float climb = fwd[1] * g_cfg.climb_loss * air * dt;
        for (int i = 0; i < 3; i++) v2[i] *= (1.f - climb);
    }
    float drag = clampf(1.f - lin_drag * dt, 0.f, 1.f);
    for (int i = 0; i < 3; i++) v2[i] *= drag;
    float sc = sqrtf(dot3(v2, v2));
    if (sc > g_cfg.max_speed) for (int i = 0; i < 3; i++) v2[i] *= g_cfg.max_speed / sc;  // hard speed cap
    safe_write(dyn + DYN_VEL, v2, 12);

    // ---- angular: target body rates, then high angular drag toward them ----
    float bank = right[1];                         // > 0: right wing is up
    // The roll correction fades out near vertical, where "bank" is undefined.
    float horiz = sqrtf(clampf(1.f - fwd[1] * fwd[1], 0.f, 1.f));
    float bank_l = bank * horiz;
    if (up[1] < 0.f) bank_l = (bank >= 0.f ? 1.f : -1.f) * horiz;   // inverted: roll the shortest way back upright
    float level = (fabsf(roll) < 0.1f) ? clampf(bank_l * g_cfg.level_rate, -1.5f, 1.5f) : 0.f;
    float pitch_ang = asinf(clampf(fwd[1], -1.f, 1.f));
    float plevel = (fabsf(noseup) < 0.1f) ? clampf(-pitch_ang * g_cfg.level_pitch, -1.2f, 1.2f) : 0.f;
    float pitch_cmd = noseup * g_cfg.pitch_rate + plevel + fabsf(bank) * g_cfg.bank_pitch * air;
    float roll_cmd = roll * g_cfg.roll_rate + level;
    float yaw_cmd = yaw * g_cfg.yaw_rate - bank * g_cfg.bank_yaw * air;   // right wing down -> turn right
    // alignment: torque that turns the nose toward the velocity vector
    float al[3] = {0, 0, 0};
    if (speed > 5.f && sc > 0.01f) {
        float vd[3] = {v2[0] / sc, v2[1] / sc, v2[2] / sc};
        cross3(fwd, vd, al);
        float k = g_cfg.alignment_force * clampf(speed / g_cfg.max_speed, 0.f, 1.f);
        for (int i = 0; i < 3; i++) al[i] *= k;
    }
    float target[3];
    for (int i = 0; i < 3; i++)
        target[i] = ws * (-pitch_cmd * right[i] - roll_cmd * fwd[i] + yaw_cmd * up[i]) + ws * al[i] * air;
    // controls take over quickly once the plane is moving
    float k = clampf(g_cfg.ang_drag * dt, 0.f, 1.f) * clampf(air * 4.f, 0.f, 1.f);
    float w[3];
    for (int i = 0; i < 3; i++) w[i] = cur[i] + (target[i] - cur[i]) * k;
    safe_write(dyn + DYN_ANG, w, 12);
}

// ---- hooks ----
typedef void (*DynFn)(void *, void *, void *, void *);
static DynFn g_orig_dyn;
// extra args are forwarded untouched: the game function may take more than `this`
static void detour_dyn(void *self, void *a2, void *a3, void *a4) {
    g_orig_dyn(self, a2, a3, a4);
    // remember the player's dynamics: its position matches the player's car state
    unsigned char *st = g_pstate;
    float pos[3], p3[3];
    if (st && safe_read((unsigned char *)self + DYN_POS, pos, 12) && safe_read(st + STATE_POS, p3, 12) &&
        fabsf(pos[0] - p3[0]) + fabsf(pos[2] - p3[2]) < 6.f)
        g_dyn = (unsigned char *)self;
}

typedef void (*UpdateFn)(void *, void *, void *, void *);
static UpdateFn g_orig_update;
static void detour_update(void *a, void *b, void *c, void *d) {
    g_orig_update(a, b, c, d);
    unsigned char *car = (unsigned char *)a;
    int kind;
    unsigned long long sp;
    if (!safe_read(car + CAR_KIND, &kind, 4) || kind != 0) return;
    if (!safe_read(car + CAR_STATE_PTR, &sp, 8) || !plausible_ptr(sp)) return;
    g_pstate = (unsigned char *)sp;
    if (g_flight) flight_step();
}

static DWORD WINAPI hook_thread(void *) {
    g_game = (unsigned char *)g_api->wait_for_code(60000);
    if (!g_game) { logf_("wait_for_code timed out"); return 0; }
    if (memcmp(g_game + FH2_GAME_FORZACAR_UPDATE, FH2_GAME_FORZACAR_UPDATE_BYTES, sizeof FH2_GAME_FORZACAR_UPDATE_BYTES - 1) != 0) {
        logf_("ForzaCar::Update bytes mismatch; flight engine disabled");
        return 0;
    }
    FH2Hook *h = g_api->hook_function(g_mod, "ForzaCar::Update", g_game + FH2_GAME_FORZACAR_UPDATE, (void *)detour_update, (void **)&g_orig_update);
    FH2Hook *h2 = g_api->hook_function(g_mod, "CCarDynamics::UpdateDynamics", g_game + RVA_UPDATE_DYNAMICS, (void *)detour_dyn, (void **)&g_orig_dyn);
    if (!h || !h2 || !g_api->enable_hooks(g_mod, 0)) { logf_("hook failed"); return 0; }
    logf_("hooked ForzaCar::Update and CCarDynamics::UpdateDynamics");
    return 0;
}

// ---- Mods page switch ----
static void on_switch(void *, const char *key, int value) {
    g_flight = value;
    WritePrivateProfileStringA("engine", "enabled", value ? "1" : "0", g_ini);
    if (value) load_cfg();   // re-read the tunables each time the engine is switched on
}

static void cmd_flight(void *, const char *, const char *, char *reply, size_t n) {
    load_cfg();
    snprintf(reply, n, "flight engine is %s (tunables reloaded)", g_flight ? "on" : "off");
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi *api) {
    g_api = api;
    FH2ModInfo info = {};
    info.size = sizeof info;
    info.name = "Forza Aeromobili Engine";
    info.version = MOD_VERSION;
    info.platform_version = api->platform_version;
    info.github_repo = "suderek22/FH2_ForzaAeromobili-Engine";
    info.check_updates = 1;
    info.description = "Experimental GTA V style flight engine: turns the car you are driving into an arcade plane.";
    info.offline_only = 1;
    g_mod = api->register_mod(&info);
    if (!g_mod) return 0;

    HMODULE self;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&fh2mod_init, &self);
    GetModuleFileNameA(self, g_ini, MAX_PATH);
    char *dot = strrchr(g_ini, '.');
    if (dot) strcpy(dot, ".ini");
    load_cfg();
    g_flight = GetPrivateProfileIntA("engine", "enabled", 0, g_ini) ? 1 : 0;   // default: off

    FH2Setting s = {};
    s.size = sizeof s;
    s.key = "FlightEngine";
    s.label = "Flight engine";
    s.description = "Makes the car you are driving fly: RT throttle, LT airbrake, left stick pitch and roll, right stick yaw.";
    s.value = (int)g_flight;
    s.on_change = on_switch;
    if (!api->add_setting(g_mod, &s)) logf_("could not add the Mods page switch");

    api->add_command(g_mod, "aero_flight", "reload the flight tunables from the ini", cmd_flight, 0);
    CreateThread(0, 0, pad_thread, 0, 0, 0);
    CreateThread(0, 0, hook_thread, 0, 0, 0);
    logf_("Forza Aeromobili Engine %s loaded (flight engine %s)", MOD_VERSION, g_flight ? "on" : "off");
    return 0;  // 0 = success (the platform treats anything else as a failed start)
}
