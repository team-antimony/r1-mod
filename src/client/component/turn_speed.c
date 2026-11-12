/* ---- turn speed ----
 * QA CL_AdjustAngles (sub_45EA10) turns the view while +left/+right (yaw) or +lookup/+lookdown (pitch) are held,
 * at dvar "965" (cl_yawspeed; pointer at 0x1A27114) and "966" (cl_pitchspeed; 0x1A2724C) degrees per second,
 * both registered by sub_4691C0 as archived floats, default 140; +speed multiplies them by "967" (1.5), and a
 * positive cap the game sets (cl +0x3618 yaw, +0x3614 pitch) still limits them. cx_yawspeed and cx_pitchspeed
 * set and keep them; [r1-mod] yaw_speed= / pitch_speed= in r1-mod.ini apply them at start (unset: the game's
 * own value is left alone). */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define VA_YAWSPEED_DVAR 0x1A27114u
#define VA_PITCHSPEED_DVAR 0x1A2724Cu
#define TURN_SPEED_LIMIT 10000.0f
static struct {
    const char *what, *key, *dvar;
    unsigned va;
    volatile float value; /* < 0: not kept */
} g_turn[2] = {
    {"yaw speed", "yaw_speed", "965", VA_YAWSPEED_DVAR, -1.0f},
    {"pitch speed", "pitch_speed", "966", VA_PITCHSPEED_DVAR, -1.0f},
};

static void apply_turn(int i)
{
    unsigned char *d = *(unsigned char **)A(g_turn[i].va);
    float value = g_turn[i].value;
    if (!d || value < 0.0f)
        return;
    __try {
        if (*(float *)(d + 0x0C) != value) {
            *(float *)(d + 0x0C) = value;
            *(float *)(d + 0x1C) = value;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

/* called by the timescale guard every 50 ms */
void turn_speed_keep(void)
{
    apply_turn(0);
    apply_turn(1);
}

void turn_speed_load_config(void)
{
    char ini[MAX_PATH];
    int i;
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
    for (i = 0; i < 2; i++) {
        char text[32];
        GetPrivateProfileStringA("r1-mod", g_turn[i].key, "", text, sizeof text, ini);
        if (text[0]) {
            float value = (float)atof(text);
            if (value >= 0.0f && value <= TURN_SPEED_LIMIT) {
                g_turn[i].value = value;
                out("r1-mod: %s %g from r1-mod.ini\n", g_turn[i].what, (double)value);
            }
        }
    }
}

/* cx_yawspeed / cx_pitchspeed [degrees per second|off] */
static void turn_command(int i)
{
    char ini[MAX_PATH], text[32];
    unsigned char *d;
    float value;
    _snprintf(ini, sizeof ini, "%sr1-mod.ini", g_dir);
    if (Cmd_Argc() >= 2) {
        const char *arg = Cmd_Argv(1);
        if (!_stricmp(arg, "off")) {
            g_turn[i].value = -1.0f;
            WritePrivateProfileStringA("r1-mod", g_turn[i].key, NULL, ini);
        } else {
            value = (float)atof(arg);
            if (!(value >= 0.0f))
                value = 0.0f;
            if (value > TURN_SPEED_LIMIT)
                value = TURN_SPEED_LIMIT;
            g_turn[i].value = value;
            apply_turn(i);
            _snprintf(text, sizeof text, "%g", (double)value);
            WritePrivateProfileStringA("r1-mod", g_turn[i].key, text, ini);
        }
    }
    d = *(unsigned char **)A(g_turn[i].va);
    if (!d) {
        out("%s dvar %s is not registered yet\n", g_turn[i].what, g_turn[i].dvar);
        return;
    }
    __try {
        value = *(float *)(d + 0x0C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out("%s dvar %s is unreadable\n", g_turn[i].what, g_turn[i].dvar);
        return;
    }
    out("%s %g degrees per second (dvar %s, default 140)%s\n", g_turn[i].what, (double)value, g_turn[i].dvar,
        g_turn[i].value >= 0.0f ? ", kept by r1-mod and saved to r1-mod.ini" : ", the game's own setting");
}

static void __cdecl cmd_cx_yawspeed(void)
{
    turn_command(0);
}

static void __cdecl cmd_cx_pitchspeed(void)
{
    turn_command(1);
}

void turn_speed_register(void)
{
    command_add("cx_yawspeed", cmd_cx_yawspeed);
    command_add("cx_pitchspeed", cmd_cx_pitchspeed);
}
