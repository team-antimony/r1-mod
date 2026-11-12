/* ---- movement dvars (iw4x PlayerMovement) ----
 * The server decides movement (the native server runs its own C1 pmove); the client only predicts it. These dvars
 * change the QA client's prediction the same way the server's copies change its pmove, and the native server sends
 * its values to each client (`s <name> "<value>"`, codol_movement_dvars.c in r1-server), so the two agree.
 * Values a server sets stay until the client is back in the lobby, then the defaults return (movement_keep).
 *
 *   bg_bounces 0/1/2            jump steps may land on a non-walkable slope (1); 2 also keeps the jump state when
 *                               Jump_ApplySlowdown would clear it (double bounces)
 *   bg_bouncesAllAngles 0/1/2   PM_ProjectVelocity always projects (1); 2 also takes steep surfaces in
 *                               PM_StepSlideMove (with bg_bounces)
 *   bg_disableLandingSlowdown   PM_CrashLand keeps the velocity
 *   bg_bunnyHopAuto             holding jump jumps again on landing
 *   bg_playerCollision          0: prediction moves through other players
 *   bg_climbAnything            every surface is a ladder
 *   player_duckedSpeedScale     PM_CmdScaleForStance crouch scale (0.65)
 *   player_proneSpeedScale      PM_CmdScaleForStance prone scale (0.15)
 *   cg_noclip_scaler            noclip speed scale (3)
 *   cg_ufo_scaler               ufo speed scale (6)
 *   bg_elevators 0/1/2          PM_CorrectAllSolid 0x418240 (iw4x Elevators): 1 takes a corrected spot even when the
 *                               trace down from it starts solid (IW4's elevators), 2 also a spot that is itself
 *                               solid; 0 keeps both tests (C1/QA stock). Easy mode's stand-up rule is server-only
 *   player_sprintUnlimited      PM_UpdateSprint 0x4160F0 never times out (the jl at 0x4161C7) and the sprint-left
 *                               query 0x415D40 always answers 0x3FFF, the engine's own "unlimited"
 *
 * QA sites (IDA): PM_StepSlideMove 0x421680, PM_ProjectVelocity 0x415BC0, Jump_ApplySlowdown 0x40E5D0, Jump_Check
 * 0x40EAC0, PM_CrashLand 0x417E30, PM_CheckLadderMove 0x41C200, PM_CmdScaleForStance 0x416960, the move scales
 * 0x416670 / 0x416820 / 0x416AA0, CG_ClipMoveToEntity 0x458C30. The constant operands (3.0, 6.0, 0.65, 0.15) are
 * pointed at the dvars' current value (+0x0C), so those need no hook; the doubles become float loads (DC 0D fmul m64
 * -> D8 0D fmul m32, same length). bg_rocketJump and bg_playerEjection are server-only. */
#include "std_include.h"
#include "game/game.h"
#include "utils/hook.h"
#include "component/components.h"

#define VA_DVAR_REGISTER_BOOL  0x633A00u /* cdecl dvar *(name, char value, flags, desc) */
#define VA_DVAR_REGISTER_INT   0x633AB0u /* cdecl dvar *(name, value, min, max, flags, desc) */
#define VA_DVAR_REGISTER_FLOAT 0x633B60u /* cdecl dvar *(name, float value, float min, float max, flags, desc) */
#define VA_CONNSTATE           0x1A2E7ECu
#define VA_CG_ENTITIES         0x1A19510u /* encrypted cg_entities pointers, indexed by [VA_CG_ENTITIES_KEY] */
#define VA_CG_ENTITIES_KEY     0x7817BB0u
#define CG_ENTITY_XOR          0x2168F672u
#define CG_ENTITY_SIZE         568
#define DVAR_VALUE             0x0C
#define DVAR_LATCHED           0x1C

typedef unsigned char *(__cdecl *dvar_register_bool_t)(const char *, char, int, const char *);
typedef unsigned char *(__cdecl *dvar_register_int_t)(const char *, int, int, int, int, const char *);
typedef unsigned char *(__cdecl *dvar_register_float_t)(const char *, float, float, float, int, const char *);

static unsigned char *g_bounces, *g_all_angles, *g_no_landing, *g_bhop, *g_collision, *g_climb;
static unsigned char *g_ducked, *g_prone, *g_noclip, *g_ufo, *g_elevators, *g_sprint_unlimited;
static int g_installed;

#define MOVEMENT_SITES 24

/* jump targets inside the patched functions, rebased at install */
static uintptr_t g_step_normal, g_step_accept, g_step_reject, g_project_apply, g_slowdown_resume;
static uintptr_t g_bhop_held, g_bhop_jump, g_crashland, g_player_trace, g_clip_entity;
static uintptr_t g_elev_take, g_elev_next, g_elev_accept, g_sprint_expire, g_sprint_continue, g_sprint_left_body;

static int write_code(unsigned va, const void *bytes, size_t n)
{
    unsigned char *p = (unsigned char *)A(va);
    DWORD old;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return 1;
}

/* jmp to a stub over len bytes (the rest NOPs), when the site still holds the expected bytes */
static int patch_jmp(unsigned va, const char *expect, size_t len, void *stub)
{
    unsigned char code[16];
    int rel;
    if (len < 5 || len > sizeof code || memcmp((void *)A(va), expect, len))
        return 0;
    memset(code, 0x90, len);
    code[0] = 0xE9;
    rel = (int)((uintptr_t)stub - (A(va) + 5));
    memcpy(code + 1, &rel, 4);
    return write_code(va, code, len);
}

/* an fld/fmul with an absolute operand (6 bytes: opcode, modrm, disp32) now reads a dvar's float value */
static int patch_operand(unsigned va, unsigned char opcode, unsigned char modrm, unsigned constant_va,
                         const unsigned char *dvar)
{
    const unsigned char *p = (const unsigned char *)A(va);
    unsigned char code[6];
    uintptr_t value = (uintptr_t)dvar + DVAR_VALUE;
    if ((p[0] != 0xD9 && p[0] != 0xDC) || p[1] != modrm || *(const unsigned *)(p + 2) != (unsigned)A(constant_va))
        return 0;
    code[0] = opcode;
    code[1] = modrm;
    memcpy(code + 2, &value, 4);
    return write_code(va, code, sizeof code);
}

/* PM_StepSlideMove 0x4219D8: after a down trace that hit a non-walkable surface, `if (jumpStep) reject` and then
 * `if (normal.z < 0.3) reject`. bg_bounces skips the first test; with bg_bouncesAllAngles 2 the second as well. */
static __declspec(naked) void step_reject_stub(void)
{
    __asm {
        push eax
        mov eax, g_bounces
        cmp dword ptr [eax + DVAR_VALUE], 0
        jle l_original
        mov eax, g_all_angles
        cmp dword ptr [eax + DVAR_VALUE], 2
        pop eax
        je l_accept
        jmp dword ptr [g_step_normal]
    l_accept:
        jmp dword ptr [g_step_accept]
    l_original:
        pop eax
        cmp dword ptr [ebp - 0x88], 0
        jnz l_reject
        jmp dword ptr [g_step_normal]
    l_reject:
        jmp dword ptr [g_step_reject]
    }
}

/* PM_ProjectVelocity 0x415C94: the return that leaves the velocity alone when the projection would neither slow
 * it nor turn it downward. bg_bouncesAllAngles projects anyway. */
static __declspec(naked) void project_stub(void)
{
    __asm {
        push eax
        mov eax, g_all_angles
        cmp dword ptr [eax + DVAR_VALUE], 0
        pop eax
        jne l_project
        fstp st(0)
        pop ebx
        mov esp, ebp
        pop ebp
        ret
    l_project:
        jmp dword ptr [g_project_apply]
    }
}

/* Jump_ApplySlowdown 0x40E5E3: the inlined Jump_ClearState (PMF_JUMPING off, jumpOriginZ 0) once pm_time is above
 * 1800. bg_bounces 2 keeps the jump state, so the next jump step can bounce again. */
static __declspec(naked) void slowdown_clear_stub(void)
{
    __asm {
        push eax
        mov eax, g_bounces
        cmp dword ptr [eax + DVAR_VALUE], 2
        pop eax
        je l_keep
        and dword ptr [ecx + 0x0C], 0xFFFFDFFF
        mov dword ptr [ecx + 0xC0], 0
    l_keep:
        jmp dword ptr [g_slowdown_resume]
    }
}

/* Jump_Check 0x40EB0D: a jump held since the last command is dropped. bg_bunnyHopAuto jumps anyway. */
static __declspec(naked) void bhop_stub(void)
{
    __asm {
        push eax
        mov eax, g_bhop
        cmp byte ptr [eax + DVAR_VALUE], 0
        pop eax
        jne l_jump
        test dword ptr [esi + 0x34], 0x400
        jz l_jump
        jmp dword ptr [g_bhop_held]
    l_jump:
        jmp dword ptr [g_bhop_jump]
    }
}

/* PM_CrashLand (usercall: edi = playerState, one stack argument the caller pops), called once at 0x418731. Landing
 * changes the velocity only by scaling it; bg_disableLandingSlowdown puts the velocity back afterwards. */
static __declspec(naked) void crashland_stub(void)
{
    __asm {
        sub esp, 12
        mov eax, [edi + 40]
        mov [esp], eax
        mov eax, [edi + 44]
        mov [esp + 4], eax
        mov eax, [edi + 48]
        mov [esp + 8], eax
        push dword ptr [esp + 16]
        call dword ptr [g_crashland]
        add esp, 4
        mov eax, g_no_landing
        cmp byte ptr [eax + DVAR_VALUE], 0
        je l_done
        mov eax, [esp]
        mov [edi + 40], eax
        mov eax, [esp + 4]
        mov [edi + 44], eax
        mov eax, [esp + 8]
        mov [edi + 48], eax
    l_done:
        add esp, 12
        ret
    }
}

/* PM_playerTrace (usercall: esi = pmove, six stack arguments the caller pops) inside PM_CheckLadderMove: with
 * bg_climbAnything every hit counts as a ladder surface (trace +0x10, surface flag 8). */
static __declspec(naked) void ladder_trace_stub(void)
{
    __asm {
        push dword ptr [esp + 24]
        push dword ptr [esp + 24]
        push dword ptr [esp + 24]
        push dword ptr [esp + 24]
        push dword ptr [esp + 24]
        push dword ptr [esp + 24]
        call dword ptr [g_player_trace]
        add esp, 24
        push eax
        mov eax, g_climb
        cmp byte ptr [eax + DVAR_VALUE], 0
        je l_done
        mov eax, [esp + 8]
        or dword ptr [eax + 0x10], 8
    l_done:
        pop eax
        ret
    }
}

/* CG_ClipMoveToEntity(ecx = trace, edx = clip, [esp+4] = entity number; the caller pops it): with bg_playerCollision
 * 0, a player's move (clip passEntityNum +108 a client) passes through players (cg entity eType 1 at +244, model
 * handle +0x194 not the brush marker). */
static int __cdecl skip_clip(const unsigned char *clip, int entnum)
{
    uintptr_t entities, ent;
    int pass;
    if (!g_collision || g_collision[DVAR_VALUE])
        return 0;
    __try {
        pass = *(const int *)(clip + 108);
        if (pass < 0 || pass >= 64 || entnum < 0 || entnum >= 1024)
            return 0;
        entities = *(volatile unsigned *)(A(VA_CG_ENTITIES) + 4 * *(volatile unsigned *)A(VA_CG_ENTITIES_KEY)) ^
                   CG_ENTITY_XOR;
        ent = entities + CG_ENTITY_SIZE * (uintptr_t)entnum;
        if (!*(unsigned *)(ent + 0x194) || *(unsigned *)(ent + 0x194) == 0xFFFFFFu)
            return 0;
        return *(int *)(ent + 244) == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static __declspec(naked) void clip_stub(void)
{
    __asm {
        push ecx
        push edx
        push dword ptr [esp + 12]
        push edx
        call skip_clip
        add esp, 8
        pop edx
        pop ecx
        test eax, eax
        jnz l_skip
        jmp dword ptr [g_clip_entity]
    l_skip:
        ret
    }
}

/* PM_CorrectAllSolid 0x4182B2: `if (trace.startsolid) next spot` after the trace at a corrected spot (eax =
 * trace). bg_elevators 2 takes the spot anyway. */
static __declspec(naked) void elevator_spot_stub(void)
{
    __asm {
        push edx
        mov edx, g_elevators
        cmp dword ptr [edx + DVAR_VALUE], 2
        pop edx
        jge l_take
        cmp byte ptr [eax + 0x25], 0
        jnz l_next
    l_take:
        jmp dword ptr [g_elev_take]
    l_next:
        jmp dword ptr [g_elev_next]
    }
}

/* PM_CorrectAllSolid 0x4182FF: the trace down from the spot must not start solid either. bg_elevators 1 and 2
 * accept the spot without that test (IW4's elevators). */
static __declspec(naked) void elevator_down_stub(void)
{
    __asm {
        push edx
        mov edx, g_elevators
        cmp dword ptr [edx + DVAR_VALUE], 1
        pop edx
        jge l_accept
        cmp byte ptr [eax + 0x25], 0
        jz l_accept
        jmp dword ptr [g_elev_next]
    l_accept:
        jmp dword ptr [g_elev_accept]
    }
}

/* PM_UpdateSprint 0x4161C1: `if (time - lastSprintStart < sprintStartMaxLength) keep sprinting` (edx = elapsed,
 * esi = playerState). player_sprintUnlimited keeps sprinting. */
static __declspec(naked) void sprint_timeout_stub(void)
{
    __asm {
        push eax
        mov eax, g_sprint_unlimited
        cmp byte ptr [eax + DVAR_VALUE], 0
        pop eax
        jne l_continue
        cmp edx, [esi + 0x24C]
        jl l_continue
        jmp dword ptr [g_sprint_expire]
    l_continue:
        jmp dword ptr [g_sprint_continue]
    }
}

/* the sprint-left query 0x415D40 (usercall: eax = playerState, one stack argument the caller pops): with
 * player_sprintUnlimited it answers 0x3FFF, as the engine does for an unlimited sprint time */
static __declspec(naked) void sprint_left_stub(void)
{
    __asm {
        push eax
        mov eax, g_sprint_unlimited
        cmp byte ptr [eax + DVAR_VALUE], 0
        pop eax
        je l_original
        mov eax, 0x3FFF
        ret
    l_original:
        push ebp
        mov ebp, esp
        sub esp, 8
        jmp dword ptr [g_sprint_left_body]
    }
}

static void register_dvars(void)
{
    dvar_register_bool_t reg_bool = (dvar_register_bool_t)A(VA_DVAR_REGISTER_BOOL);
    dvar_register_int_t reg_int = (dvar_register_int_t)A(VA_DVAR_REGISTER_INT);
    dvar_register_float_t reg_float = (dvar_register_float_t)A(VA_DVAR_REGISTER_FLOAT);
    g_bounces = reg_int("bg_bounces", 0, 0, 2, 0, "Bounces: 0 disabled, 1 enabled, 2 double (set by the server)");
    g_all_angles = reg_int("bg_bouncesAllAngles", 0, 0, 2, 0,
                           "Force bounces from all angles: 0 disabled, 1 simple, 2 all surfaces (set by the server)");
    g_no_landing = reg_bool("bg_disableLandingSlowdown", 0, 0, "Toggle landing slowdown (set by the server)");
    g_bhop = reg_bool("bg_bunnyHopAuto", 0, 0, "Constantly jump when holding jump (set by the server)");
    g_collision = reg_bool("bg_playerCollision", 1, 0, "Players collide with each other (set by the server)");
    g_climb = reg_bool("bg_climbAnything", 0, 0, "Treat any surface as a ladder (set by the server)");
    g_ducked = reg_float("player_duckedSpeedScale", 0.65f, 0.0f, 5.0f, 0,
                         "The scale applied to the player speed when ducking (set by the server)");
    g_prone = reg_float("player_proneSpeedScale", 0.15f, 0.0f, 5.0f, 0,
                        "The scale applied to the player speed when crawling (set by the server)");
    g_noclip = reg_float("cg_noclip_scaler", 3.0f, 0.001f, 1000.0f, 0,
                         "The speed at which noclip camera moves (set by the server)");
    g_ufo = reg_float("cg_ufo_scaler", 6.0f, 0.001f, 1000.0f, 0,
                      "The speed at which ufo camera moves (set by the server)");
    g_elevators = reg_int("bg_elevators", 0, 0, 2, 0, "Elevators: 0 off, 1 normal, 2 easy (set by the server)");
    g_sprint_unlimited = reg_bool("player_sprintUnlimited", 0, 0, "Sprint never runs out (set by the server)");
}

/* returns the number of sites patched (of MOVEMENT_SITES) */
static int install_patches(void)
{
    static const struct { unsigned va, constant_va; unsigned char opcode, modrm; unsigned char **dvar; } ops[] = {
        {0x416774u, 0xC854C8u, 0xD8, 0x0D, &g_noclip}, {0x416787u, 0xC854D0u, 0xD8, 0x0D, &g_ufo},
        {0x4168BEu, 0xC854C8u, 0xD8, 0x0D, &g_noclip}, {0x4168CFu, 0xC854D0u, 0xD8, 0x0D, &g_ufo},
        {0x416BD4u, 0xC854C8u, 0xD8, 0x0D, &g_noclip}, {0x416BE7u, 0xC854D0u, 0xD8, 0x0D, &g_ufo},
        {0x4169CDu, 0xC85F48u, 0xD8, 0x0D, &g_ducked}, {0x4169D5u, 0xC85C58u, 0xD8, 0x0D, &g_prone},
        {0x416A4Cu, 0xC85C58u, 0xD8, 0x0D, &g_prone},  {0x416A54u, 0xC85F48u, 0xD8, 0x0D, &g_ducked},
        {0x416A88u, 0xC85B38u, 0xD9, 0x05, &g_ducked}, {0x416A93u, 0xC8594Cu, 0xD9, 0x05, &g_prone},
    };
    int i, n = 0;
    g_step_normal = A(0x4219E1u);
    g_step_accept = A(0x421A32u);
    g_step_reject = A(0x4219F1u);
    g_project_apply = A(0x415C9Du);
    g_slowdown_resume = A(0x40E5F2u);
    g_bhop_held = A(0x40EB16u);
    g_bhop_jump = A(0x40EB26u);
    g_elev_take = A(0x4182B8u);
    g_elev_next = A(0x418305u);
    g_elev_accept = A(0x418356u);
    g_sprint_expire = A(0x4161C9u);
    g_sprint_continue = A(0x4161E9u);
    g_sprint_left_body = A(0x415D46u);
    n += patch_jmp(0x4182B2u, "\x80\x78\x25\x00\x75\x4D", 6, elevator_spot_stub);
    n += patch_jmp(0x4182FFu, "\x80\x78\x25\x00\x74\x51", 6, elevator_down_stub);
    n += patch_jmp(0x4161C1u, "\x3B\x96\x4C\x02\x00\x00\x7C\x20", 8, sprint_timeout_stub);
    n += patch_jmp(0x415D40u, "\x55\x8B\xEC\x83\xEC\x08", 6, sprint_left_stub);
    n += patch_jmp(0x4219D8u, "\x83\xBD\x78\xFF\xFF\xFF\x00\x75\x10", 9, step_reject_stub);
    n += patch_jmp(0x415C94u, "\xDD\xD8\x5B\x8B\xE5\x5D\xC3", 7, project_stub);
    n += patch_jmp(0x40E5E3u, "\xD9\xEE\x81\x61\x0C\xFF\xDF\xFF\xFF\xD9\x99\xC0\x00\x00\x00", 15, slowdown_clear_stub);
    n += patch_jmp(0x40EB0Du, "\xF7\x46\x34\x00\x04\x00\x00\x74\x10", 9, bhop_stub);
    n += patch_call(0x418731u, 0x417E30u, crashland_stub, (void **)&g_crashland);
    n += patch_call(0x41C39Bu, 0x415A10u, ladder_trace_stub, (void **)&g_player_trace);
    n += patch_call(0x41C44Bu, 0x415A10u, ladder_trace_stub, (void **)&g_player_trace);
    n += patch_call(0x458EDEu, 0x458C30u, clip_stub, (void **)&g_clip_entity);
    for (i = 0; i < (int)(sizeof ops / sizeof ops[0]); i++)
        n += patch_operand(ops[i].va, ops[i].opcode, ops[i].modrm, ops[i].constant_va, *ops[i].dvar);
    return n;
}

static void set_int(unsigned char *d, int v)
{
    *(int *)(d + DVAR_VALUE) = v;
    *(int *)(d + DVAR_LATCHED) = v;
}

static void set_float(unsigned char *d, float v)
{
    *(float *)(d + DVAR_VALUE) = v;
    *(float *)(d + DVAR_LATCHED) = v;
}

/* back in the lobby: the next server may not send these, so the stock values return */
static void reset_defaults(void)
{
    set_int(g_bounces, 0);
    set_int(g_all_angles, 0);
    g_no_landing[DVAR_VALUE] = g_no_landing[DVAR_LATCHED] = 0;
    g_bhop[DVAR_VALUE] = g_bhop[DVAR_LATCHED] = 0;
    g_collision[DVAR_VALUE] = g_collision[DVAR_LATCHED] = 1;
    g_climb[DVAR_VALUE] = g_climb[DVAR_LATCHED] = 0;
    set_float(g_ducked, 0.65f);
    set_float(g_prone, 0.15f);
    set_float(g_noclip, 3.0f);
    set_float(g_ufo, 6.0f);
    set_int(g_elevators, 0);
    g_sprint_unlimited[DVAR_VALUE] = g_sprint_unlimited[DVAR_LATCHED] = 0;
}

/* called by the timescale guard every 50 ms */
void movement_keep(void)
{
    static int was_connected;
    int state;
    if (g_installed <= 0)
        return;
    __try {
        state = *(volatile int *)A(VA_CONNSTATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (state > 0) {
        was_connected = 1;
    } else if (was_connected) {
        was_connected = 0;
        reset_defaults();
    }
}

static void print_values(void)
{
    out("bg_bounces %d  bg_bouncesAllAngles %d  bg_disableLandingSlowdown %d  bg_bunnyHopAuto %d\n",
        *(int *)(g_bounces + DVAR_VALUE), *(int *)(g_all_angles + DVAR_VALUE), g_no_landing[DVAR_VALUE],
        g_bhop[DVAR_VALUE]);
    out("bg_playerCollision %d  bg_climbAnything %d  player_duckedSpeedScale %g  player_proneSpeedScale %g\n",
        g_collision[DVAR_VALUE], g_climb[DVAR_VALUE], (double)*(float *)(g_ducked + DVAR_VALUE),
        (double)*(float *)(g_prone + DVAR_VALUE));
    out("cg_noclip_scaler %g  cg_ufo_scaler %g\n", (double)*(float *)(g_noclip + DVAR_VALUE),
        (double)*(float *)(g_ufo + DVAR_VALUE));
    out("bg_elevators %d  player_sprintUnlimited %d\n", *(int *)(g_elevators + DVAR_VALUE),
        g_sprint_unlimited[DVAR_VALUE]);
}

/* cx_movement            show the movement dvars (the server sets them; see the header)
 * cx_movement install    register them and patch pmove; r1-mod queues it once at start so that it runs on the
 *                        game thread (Dvar_Register* keeps state in the calling thread's TLS) */
static void __cdecl cmd_cx_movement(void)
{
    int n;
    if (Cmd_Argc() >= 2 && !strcmp(Cmd_Argv(1), "install")) {
        if (g_installed)
            return;
        register_dvars();
        if (!g_bounces || !g_all_angles || !g_no_landing || !g_bhop || !g_collision || !g_climb || !g_ducked ||
            !g_prone || !g_noclip || !g_ufo || !g_elevators || !g_sprint_unlimited) {
            g_installed = -1;
            out("r1-mod: movement dvars could not be registered; movement patches not installed\n");
            return;
        }
        n = install_patches();
        g_installed = 1;
        if (n == MOVEMENT_SITES)
            out("r1-mod: movement dvars registered, %d pmove sites patched\n", n);
        else
            out("r1-mod: movement dvars registered, but only %d of %d pmove sites matched (wrong build?)\n", n,
                MOVEMENT_SITES);
        return;
    }
    if (g_installed <= 0) {
        out("cx_movement: not installed\n");
        return;
    }
    print_values();
}

void movement_register(void)
{
    command_add("cx_movement", cmd_cx_movement);
}

void movement_start(void)
{
    Cbuf_AddText("cx_movement install\n");
}
