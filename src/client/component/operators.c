/* ---- operators ----
 * The operator of character n is characterClasses[n].slots[0] (an int at 3858 + 24 n, encoded like the class ints),
 * and currentCharacterIndex (a plain byte at 4146) picks the character. QA has 12 characters, the menu only adds a
 * newly owned operator to a free one (sub_68A140), so with all 12 taken the remaining operators of
 * mp/coMarketplaceItems.csv ("cha" rows; 27 in this build) never show up. Writing the slot directly swaps one in;
 * sub_593700 then reloads the menu's copy of the characters (0x2E16E50) from playerdata. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define PD_CHARACTERS      12
#define PD_CHARACTER_SLOTS 3858
#define PD_CHARACTER_SIZE  24
#define PD_CURRENT_CHAR    4146
#define VA_CHARS_RELOAD    0x593700u
#define MAX_OPERATORS      64

/* the "cha" rows of the marketplace table: ids[i], names[i]; returns how many */
static int list_operators(int *ids, const char **names, int max)
{
    const unsigned char *t = find_table("mp/coMarketplaceItems.csv");
    int r, n = 0, rows = t ? *(const int *)(t + 8) : 0;
    for (r = 0; r < rows && n < max; r++)
        if (!strcmp(table_cell(t, r, 2), "cha")) {
            ids[n] = atoi(table_cell(t, r, 0));
            names[n++] = table_cell(t, r, 1);
        }
    return n;
}

/* cx_operator [name|id [character]]   no argument lists the characters and every operator; otherwise the operator
 * (cha_ prefix optional) replaces the one of character 1..12, by default the current character */
static void __cdecl cmd_cx_operator(void)
{
    int ids[MAX_OPERATORS], i, c = 0, id = 0;
    const char *names[MAX_OPERATORS];
    int current = *(pd_buf() + PD_CURRENT_CHAR);
    int n = list_operators(ids, names, MAX_OPERATORS);
    if (n == 0) {
        out("cx_operator: mp/coMarketplaceItems.csv is not loaded yet\n");
        return;
    }
    if (Cmd_Argc() < 2) {
        for (c = 0; c < PD_CHARACTERS; c++) {
            int have = pd_get_int(PD_CHARACTER_SLOTS + PD_CHARACTER_SIZE * c);
            const char *name = "(empty)";
            for (i = 0; i < n; i++)
                if (ids[i] == have) name = names[i];
            out(" %c%2d  %s\n", c == current ? '>' : ' ', c + 1, name);
        }
        out("operators:");
        for (i = 0; i < n; i++)
            out(" %s", names[i] + (_strnicmp(names[i], "cha_", 4) ? 0 : 4));
        out("\ncx_operator <name> [1-12] puts one on a character (default: the current one)\n");
        return;
    }
    for (i = 0; i < n && !id; i++) {
        const char *want = Cmd_Argv(1);
        if (atoi(want) == ids[i] || !_stricmp(want, names[i]) ||
            (!_strnicmp(names[i], "cha_", 4) && !_stricmp(want, names[i] + 4)))
            id = ids[i], c = i;
    }
    if (!id) {
        out("cx_operator: no operator %s (cx_operator lists them)\n", Cmd_Argv(1));
        return;
    }
    i = Cmd_Argc() >= 3 ? atoi(Cmd_Argv(2)) - 1 : current;
    if (i < 0 || i >= PD_CHARACTERS) {
        out("cx_operator: character 1..%d\n", PD_CHARACTERS);
        return;
    }
    pd_set_int(PD_CHARACTER_SLOTS + PD_CHARACTER_SIZE * i, id);
    __try {
        ((void(__cdecl *)(void))A(VA_CHARS_RELOAD))();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out("cx_operator: reloading the menu's characters failed; reopen the operator menu\n");
    }
    out("character %d now plays %s%s\n", i + 1, names[c], i == current ? "" : " (not the current character)");
}

void operators_register(void)
{
    command_add("cx_operator", cmd_cx_operator);
}
