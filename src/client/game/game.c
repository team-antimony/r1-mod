/* Engine functions called by r1-mod; addresses in game.h. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

uintptr_t g_delta;
const struct engine *g_engine;

static const struct engine g_qa_3333 = {
    BUILD_QA_3333, "QA 3.3.3.3",
    VA_CMD_ARGV, VA_DVAR_FIND, VA_CMD_HEAD, VA_CMD_DISPATCH, VA_CMD_ARGC, VA_CMD_DEPTH, VA_DVAR_HASH,
};

static const struct engine g_retail_319 = {
    BUILD_RETAIL_319, "3.19.26.22",
    EA319_CMD_ARGV, EA319_DVAR_FIND, EA319_CMD_FUNCTIONS, 0, EA319_CMD_ARGC, EA319_CMD_LEVEL, EA319_DVAR_HASH,
};

typedef const char *(__cdecl *Cmd_Argv_t)(int);
typedef unsigned char *(__cdecl *Dvar_FindVar_t)(const char *);

/* In another exe these addresses may not be mapped at all. Asked of the OS rather than left to a fault:
 * an injector can take an access violation before a __try around the read sees it (frida's default). */
static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect)
        return 0;
    return (const unsigned char *)p + n <= (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
}

/* QA: Cbuf_AddText starts with push ebx / push esi / push edi / push imm32.
 * 3.19: Cmd_Argv is push ebp / mov ebp, esp / mov eax, [cmd level] / mov ecx, [ebp+8] /
 * cmp ecx, [eax*4 + cmd argc]; the two addresses make it specific to this build. */
int engine_detect(void)
{
    const unsigned char *p = (const unsigned char *)A(VA_CBUF_ADDTEXT);
    if (readable(p, 4) && !memcmp(p, "\x53\x56\x57\x68", 4)) {
        g_engine = &g_qa_3333;
        return 1;
    }
    p = (const unsigned char *)A(EA319_CMD_ARGV);
    if (readable(p, 18) && !memcmp(p, "\x55\x8B\xEC\xA1", 4) &&
        *(const unsigned *)(p + 4) == (unsigned)A(EA319_CMD_LEVEL) &&
        !memcmp(p + 8, "\x8B\x4D\x08\x3B\x0C\x85", 6) && *(const unsigned *)(p + 14) == (unsigned)A(EA319_CMD_ARGC)) {
        g_engine = &g_retail_319;
        return 1;
    }
    return 0;
}

/* 3.19 has no Cbuf_AddText to call: the engine inlines it. Same steps here, on cmd_text[0] under the
 * engine's own lock, which the QA call below does without. */
static void cbuf_add_319(const char *text)
{
    struct cmd_text { char *data; int maxsize; int cmdsize; } *ct = (struct cmd_text *)A(EA319_CMD_TEXT);
    CRITICAL_SECTION *lock = (CRITICAL_SECTION *)A(EA319_CMD_TEXT_LOCK);
    int len = (int)strlen(text), added = 0;
    EnterCriticalSection(lock);
    if (ct->data && ct->cmdsize + len < ct->maxsize) {
        memcpy(ct->data + ct->cmdsize, text, (size_t)len + 1);
        ct->cmdsize += len;
        added = 1;
    }
    LeaveCriticalSection(lock);
    if (!added)
        out("r1-mod: the game's command buffer is full or not set up, line dropped\n");
}

void Cbuf_AddText(const char *text)
{
    void *fn;
    if (g_engine->build == BUILD_RETAIL_319) {
        cbuf_add_319(text);
        return;
    }
    fn = (void *)A(VA_CBUF_ADDTEXT);
    __asm {
        mov eax, text
        xor ecx, ecx
        call fn
    }
}

int Cmd_Argc(void)
{
    int depth = *(int *)A(g_engine->cmd_depth);
    return ((int *)A(g_engine->cmd_argc))[depth];
}

const char *Cmd_Argv(int i)
{
    return ((Cmd_Argv_t)A(g_engine->cmd_argv))(i);
}

unsigned char *Dvar_FindVar(const char *name)
{
    return ((Dvar_FindVar_t)A(g_engine->dvar_find))(name);
}

const char *name_dvar_value(void)
{
    unsigned char *d = *(unsigned char **)A(VA_NAME_DVAR);
    const char *v;
    if (!d)
        return "";
    __try {
        v = *(const char **)(d + 12);
        return v ? v : "";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
}

int dw_live_state(void)
{
    __try {
        unsigned char *o = *(unsigned char **)A(VA_DW_LIVE);
        return o ? *(int *)(o + 500) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const char *table_cell(const unsigned char *t, int row, int col)
{
    int cols = *(const int *)(t + 4), rows = *(const int *)(t + 8);
    const char *s;
    if (row < 0 || row >= rows || col < 0 || col >= cols)
        return "";
    s = *(const char *const *)(*(const unsigned char *const *)(t + 12) + 8 * (row * cols + col));
    return s ? s : "";
}

const unsigned char *find_table(const char *name)
{
    typedef void *(__cdecl *req_t)(int, const char *, int);
    __try {
        return (const unsigned char *)((req_t)A(VA_DB_REQUEST))(37, name, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
}
