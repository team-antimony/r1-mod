/* r1-mod - injected client extension for the CoD Online 3.3.3.3 QA build.
 *
 * DllMain installs the loopback redirects (they must precede Demonware start-up) and starts main_thread, which
 * waits for the engine's commands, then installs every component in a fixed order. Addresses are in game/game.h
 * and in each component (IDA base 0x400000, rebased from the loaded module).
 *
 * In a 3.19.26.22 client only the console and the commands that need nothing but the command and dvar layer
 * start (main_thread, BUILD_RETAIL_319): the other components hold QA addresses. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"
#include "component/overlay.h"

HMODULE g_self;
char g_dir[MAX_PATH];
static int g_redirects, g_scaleform, g_onboarding, g_intro, g_fsmod;

/* log fatal exceptions with IDA-relative addresses, then let the process die as usual */
static LONG CALLBACK crash_logger(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        out("EXCEPTION 0x%08x at ida 0x%08x (eip 0x%08x) fault addr 0x%08x\n", (unsigned)code,
            (unsigned)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - g_delta),
            (unsigned)(uintptr_t)ep->ExceptionRecord->ExceptionAddress,
            (unsigned)(ep->ExceptionRecord->NumberParameters > 1
                           ? ep->ExceptionRecord->ExceptionInformation[1] : 0));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* A QA exe is recognised at once. A packed 3.19 exe unpacks its code after its modules load, so a DLL
 * that is in before that has to look again for a while. */
static int wait_for_build(void)
{
    int tries;
    for (tries = 0; tries < 240; tries++) {
        if (g_engine || engine_detect())
            return 1;
        Sleep(250);
    }
    return 0;
}

static void wait_for_commands(int *count_out)
{
    int stable = 0, last = -1;
    for (;;) {
        int count = command_engine_count();
        if (count == last && count > 100) {
            if (++stable >= 5)
                break;
        } else {
            stable = 0;
            last = count;
        }
        Sleep(500);
    }
    *count_out = last;
}

/* 3.19.26.22: the console window, r1-mod_in.txt and the commands of console.c. Nothing is hooked or patched. */
static DWORD run_retail_319(void)
{
    int count;
    wait_for_commands(&count);
    AddVectoredExceptionHandler(1, crash_logger);
    console_register();
    command_register_all();
    out("r1-mod ready on %s: %d engine commands, base 0x%08x. Console and dvars only (type 'help');\n"
        "the QA components (overlay, connect, playerdata, ...) are not ported to this build.\n",
        g_engine->label, count, (unsigned)A(IDA_BASE));
    CreateThread(NULL, 0, console_file_thread, NULL, 0, NULL);
    console_input_thread(NULL);
    return 0;
}

static void register_commands(void)
{
    console_register();
    direct_connect_register();
    compat_register();
    trace_register();
    render_register();
    name_register();
    onboarding_register();
    playerdata_register();
    unlock_register();
    checksum_register();
    match_register();
    stats_register();
    fps_register();
    rcon_register();
    chat_register();
    textcolor_register();
    localize_register();
    mod_register();
    fsmod_register();
    weapon_anims_register();
    movement_register();
    operators_register();
    turn_speed_register();
    command_register_all();
}

static DWORD WINAPI main_thread(LPVOID unused)
{
    int last;
    (void)unused;
    console_open_log();

    /* the loosefs exe declares a different ImageBase than the IDB, so rebase from IDA_BASE */
    g_delta = (uintptr_t)GetModuleHandleA(NULL) - IDA_BASE;
    if (!wait_for_build()) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        out("r1-mod: this exe is neither QA 3.3.3.3 nor 3.19.26.22 (code signatures differ); refusing to hook\n");
        return 1;
    }
    console_open_window();
    if (g_engine->build == BUILD_RETAIL_319)
        return run_retail_319();

    /* wait for the engine to finish registering its commands */
    wait_for_commands(&last);

    AddVectoredExceptionHandler(1, crash_logger);
    if (!direct_connect_install())
        out("r1-mod: net hook install failed, connect unavailable\n");
    redirects_report(g_redirects);
    scaleform_report(g_scaleform);
    fsmod_report(g_fsmod);
    onboarding_report(g_onboarding);
    intro_report(g_intro);
    unlock_startup();
    stats_startup();
    checksum_startup();
    if (!render_install_frame_hook())
        out("r1-mod: frame hook signature mismatch, version banner unavailable\n");
    if (!trace_install())
        out("r1-mod: Com_sprintf signature mismatch, cx_trace unavailable\n");
    if (console_install_gamemsg_hooks() != console_gamemsg_site_count())
        out("r1-mod: some game message call sites not recognised, the overlay console misses their text\n");
    overlay_start();
    if (!render_install_endframe_hook())
        out("r1-mod: end-of-frame call not recognised, the in-game console and browser cannot draw\n");
    register_commands();
    movement_start();
    weapon_anims_start();
    fsmod_start();
    out("r1-mod ready: %d engine commands, base 0x%08x (type 'help')\n", last,
        (unsigned)A(IDA_BASE));
    CreateThread(NULL, 0, console_file_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, onboarding_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, name_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, playerdata_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, stats_thread, NULL, 0, NULL);
    fps_load_config();
    turn_speed_load_config();
    CreateThread(NULL, 0, timescale_guard, NULL, 0, NULL);
    console_input_thread(NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = (HMODULE)inst;
        DisableThreadLibraryCalls(inst);
        /* only memory patches here (no loader calls beyond GetModuleHandle/GetProcAddress of loaded modules) */
        g_delta = (uintptr_t)GetModuleHandleA(NULL) - IDA_BASE;
        if (engine_detect() && g_engine->build == BUILD_QA_3333) {
            g_redirects = redirects_install();
            g_scaleform = scaleform_install();
            g_fsmod = fsmod_install();
            g_onboarding = onboarding_install();
            g_intro = intro_install();
        }
        CreateThread(NULL, 0, main_thread, NULL, 0, NULL);
    }
    return TRUE;
}
