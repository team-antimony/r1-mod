/* Engine access for the CoD Online 3.3.3.3 QA build, and the command and dvar layer of 3.19.26.22.
 *
 * Every VA_ address is a QA 3.3.3.3 IDA address (base 0x400000) and is
 * rebased from the loaded module, because the exe is relocated at runtime. Addresses used by one
 * component only are defined in that component.
 *
 * What both builds have goes through g_engine, which engine_detect() points at the profile of the
 * loaded exe. Components that use VA_ addresses directly are QA-only and main.c does not start them
 * on another build. */
#ifndef R1_GAME_H
#define R1_GAME_H

#include "std_include.h"
#include "engine_map.h"     /* researched engine API as named reference constants: QA 3.3.3.3 */
#include "engine_map_319.h" /* and 3.19.26.22 */

#define IDA_BASE 0x400000u

enum engine_build { BUILD_QA_3333 = 1, BUILD_RETAIL_319 };

/* The command and dvar addresses of one build; the functions below use them. */
struct engine {
    enum engine_build build;
    const char *label;
    unsigned cmd_argv;     /* cdecl const char *(int) */
    unsigned dvar_find;    /* cdecl dvar *(const char *) */
    unsigned cmd_head;     /* command list head: node {next, name, func} */
    unsigned cmd_dispatch; /* second list, walked by the executor; 0 when the executor walks cmd_head */
    unsigned cmd_argc;     /* int[], indexed by [cmd_depth] */
    unsigned cmd_depth;
    unsigned dvar_hash;    /* dvar *[1024] by name hash; chain next at +0x48 */
};

extern const struct engine *g_engine; /* NULL until engine_detect() succeeds */

/* Recognises the loaded exe by its code and sets g_engine; 0 when it is neither build (or a packed
 * 3.19 exe has not unpacked its code yet: call again). Needs g_delta. */
int engine_detect(void);

#define VA_CBUF_ADDTEXT 0x58D840u /* usercall: eax = text, ecx = local client */
#define VA_CMD_ARGV     0x43B260u /* cdecl const char *(int) */
#define VA_DVAR_FIND    0x632440u /* cdecl dvar *(const char *) */
#define VA_CMD_HEAD     0x2E01224u /* command list head: node {next, name, func} */
#define VA_CMD_DISPATCH 0x2E0111Cu /* list walked by the command executor (0x58D910) */
#define VA_CMD_ARGC     0x2E011A4u /* int[], indexed by [VA_CMD_DEPTH] */
#define VA_CMD_DEPTH    0x2E01120u
#define VA_DVAR_HASH    0x369BC30u /* dvar *[1024] by name hash (Dvar_FindVar 0x632440); chain next at +0x48 */
#define VA_NAME_DVAR    0x1A2E800u /* dvar * holding the player name (userinfo "name") */
#define VA_DW_LIVE      0xCFAFE0u  /* Demonware live singleton pointer; state at +500 (2 = connected) */
#define VA_PD_OBJECT    0xC95BD80u /* Demonware stats object: playerdata at +144, secure data at +37304 */
#define VA_DB_REQUEST   0x49E7D0u  /* cdecl void *(int type, const char *name, int) */

/* module state, set by main.c */
extern uintptr_t g_delta;      /* runtime base - IDA_BASE */
extern HMODULE g_self;         /* r1-mod.dll */
extern char g_dir[MAX_PATH];   /* folder of r1-mod.dll, ends with a backslash */

#define A(va) ((uintptr_t)(va) + g_delta)

void Cbuf_AddText(const char *text);
int Cmd_Argc(void);
const char *Cmd_Argv(int i);
unsigned char *Dvar_FindVar(const char *name);

/* "name" dvar value, "" when not registered or unreadable */
const char *name_dvar_value(void);
/* Demonware live state (2 = connected), -1 when not readable */
int dw_live_state(void);

/* StringTable assets (type 37): {name, columns, rows, cells}, cell = {const char *, hash} */
const unsigned char *find_table(const char *name);
const char *table_cell(const unsigned char *t, int row, int col);

#endif
