/* ---- console commands ----
 * QA 3.3.3.3: each command needs a node in both lists: the executor walks cmd_dispatch, while
 * Cmd_FindCommand and the registration code use cmd_head. A node can only be in one list.
 * 3.19.26.22 has one list (cmd_dispatch is 0); its executor moves the command it runs to the front. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

/* 2026.11.11 shipped with 32, fewer than it registers: the last five (cx_swapstyle, cx_movement, cx_operator,
 * cx_yawspeed, cx_pitchspeed) were dropped, and with cx_swapstyle the swap style and reload fix never installed. */
#define MAX_COMMANDS 96

static struct { const char *name; command_fn fn; } g_defs[MAX_COMMANDS];
static int g_count;
static unsigned g_nodes[MAX_COMMANDS][2][3];

void command_add(const char *name, command_fn fn)
{
    if (g_count < MAX_COMMANDS) {
        g_defs[g_count].name = name;
        g_defs[g_count].fn = fn;
        g_count++;
    } else {
        out("r1-mod: too many commands, %s not registered\n", name);
    }
}

static int in_list(unsigned **head, const unsigned *node)
{
    int count = 0;
    __try {
        const unsigned *n = *head;
        for (; n && count < 4096; n = (const unsigned *)n[0], count++)
            if (n == node)
                return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

/* The game thread relinks these lists while it runs commands, and this runs on r1-mod's thread: a
 * node put in between two of its writes drops out again. So link, look, and link again if needed. */
static int link_all(unsigned **head, int list)
{
    int i, linked = 0;
    for (i = 0; i < g_count; i++) {
        unsigned *node = g_nodes[i][list];
        if (in_list(head, node))
            continue;
        node[1] = (unsigned)g_defs[i].name;
        node[2] = (unsigned)g_defs[i].fn;
        do
            node[0] = (unsigned)*head;
        while (InterlockedCompareExchange((volatile LONG *)head, (LONG)node, (LONG)node[0]) != (LONG)node[0]);
        linked++;
    }
    return linked;
}

void command_register_all(void)
{
    unsigned **heads[2];
    int lists = 0, l, pass;
    if (g_engine->cmd_dispatch)
        heads[lists++] = (unsigned **)A(g_engine->cmd_dispatch);
    heads[lists++] = (unsigned **)A(g_engine->cmd_head);
    for (l = 0; l < lists; l++) {
        for (pass = 0; pass < 5 && link_all(heads[l], l); pass++)
            Sleep(20);
    }
}

int command_engine_count(void)
{
    unsigned *n = *(unsigned **)A(g_engine->cmd_head);
    int count = 0;
    while (n && count < 4096) {
        n = (unsigned *)n[0];
        count++;
    }
    return count;
}
