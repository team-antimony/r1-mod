/* ---- loose Scaleform files ----
 * Every file the UI opens (scaleform/default/codo.gfx = the whole frontend, codoingame.gfx, fonts, IME, XML) goes
 * through the game's FileOpener: vtable 0xC7C4C8, slot 1 = 0x6B94A0, thiscall File *(this, path, flags, mode). It
 * takes the file from its rawfile asset (DB_RequestAsset 36), inflates it into a static buffer and wraps that in a
 * Scaleform MemoryFile (28 bytes from the Scaleform global heap, constructor 0x5EB6A0). The slot is replaced so
 * that <dll dir>\ui\<path> (e.g. ui\scaleform\default\codo.gfx) is served the same way when it exists; the client
 * mod's ui\ folder (fsmod.c) is looked at first. A file is
 * read once and kept for the rest of the session, because the MemoryFile does not own its data. Installed from
 * DllMain: the frontend movie loads before main_thread is ready. */
#include "std_include.h"
#include "game/game.h"
#include "component/components.h"

#define VA_OPENER_SLOT  (0xC7C4C8u + 4) /* FileOpener vtable, slot 1 */
#define VA_OPENER_OPEN  0x6B94A0u
#define VA_GFX_HEAP     0xCEE208u       /* Scaleform global heap object; vtable +0x28 = Alloc(size, flags) */
#define VA_MEMFILE_CTOR 0x5EB6A0u       /* thiscall MemoryFile *(this, const char *name, const void *data, int size) */
#define MEMFILE_SIZE    0x1C
#define MAX_FILES       32

/* thiscall functions called and replaced through __fastcall with an unused edx */
typedef void *(__fastcall *open_fn)(void *self, void *edx, const char *path, int flags, int mode);
typedef void *(__fastcall *alloc_fn)(void *heap, void *edx, unsigned size, unsigned flags);
typedef void *(__fastcall *memfile_fn)(void *self, void *edx, const char *name, const void *data, int size);

struct loose_file {
    char path[128];
    unsigned char *data; /* NULL: no loose copy */
    int size;
};

static open_fn g_orig_open;
static CRITICAL_SECTION g_lock;
static struct loose_file g_files[MAX_FILES];
static int g_file_count;

/* -> the cached entry for path, reading <dll dir>\ui\<path> on first use; NULL when the cache is full */
static struct loose_file *loose_lookup(const char *path)
{
    struct loose_file *f;
    char disk[MAX_PATH], *p;
    FILE *fp;
    long size;
    int i;
    for (i = 0; i < g_file_count; i++)
        if (_stricmp(g_files[i].path, path) == 0)
            return &g_files[i];
    if (g_file_count == MAX_FILES || strlen(path) >= sizeof f->path)
        return NULL;
    f = &g_files[g_file_count++];
    strcpy(f->path, path);
    if (strstr(path, ".."))
        return f;
    fp = NULL;
    if (fsmod_dir() && _snprintf(disk, sizeof disk, "%s\\ui\\%s", fsmod_dir(), path) > 0) {
        for (p = disk; *p; p++)
            if (*p == '/')
                *p = '\\';
        fp = fopen(disk, "rb"); /* the client mod's ui\ first */
    }
    if (!fp) {
        if (_snprintf(disk, sizeof disk, "%sui\\%s", g_dir, path) < 0)
            return f;
        for (p = disk; *p; p++)
            if (*p == '/')
                *p = '\\';
        fp = fopen(disk, "rb");
    }
    if (!fp)
        return f;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size > 0 && (f->data = (unsigned char *)malloc(size)) != NULL) {
        if (fread(f->data, 1, size, fp) == (size_t)size) {
            f->size = (int)size;
            out("r1-mod: UI file %s loaded from %s (%ld bytes)\n", path, disk, size);
        } else {
            free(f->data);
            f->data = NULL;
        }
    }
    fclose(fp);
    return f;
}

static void *__fastcall open_hook(void *self, void *edx, const char *path, int flags, int mode)
{
    struct loose_file *f;
    void *heap, *file;
    if (!path || !g_dir[0]) /* g_dir is set by main_thread */
        return g_orig_open(self, edx, path, flags, mode);
    EnterCriticalSection(&g_lock);
    f = loose_lookup(path);
    LeaveCriticalSection(&g_lock);
    if (!f || !f->data)
        return g_orig_open(self, edx, path, flags, mode);
    heap = *(void **)A(VA_GFX_HEAP);
    file = (*(alloc_fn *)(*(unsigned char **)heap + 0x28))(heap, NULL, MEMFILE_SIZE, 0);
    if (!file)
        return NULL;
    return ((memfile_fn)A(VA_MEMFILE_CTOR))(file, NULL, path, f->data, f->size);
}

/* 1 when the FileOpener slot is hooked */
int scaleform_install(void)
{
    open_fn *slot = (open_fn *)A(VA_OPENER_SLOT);
    DWORD old;
    if (*slot != (open_fn)A(VA_OPENER_OPEN) || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old))
        return 0;
    InitializeCriticalSection(&g_lock);
    g_orig_open = *slot;
    *slot = open_hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

/* installed_at_load: what DllMain got; after a late injection the hook only affects files opened from now on */
void scaleform_report(int installed_at_load)
{
    if (installed_at_load)
        out("r1-mod: UI files are read from %sui\\ when present\n", g_dir);
    else if (scaleform_install())
        out("r1-mod: late injection, UI files are read from %sui\\ for files opened from now on\n", g_dir);
    else
        out("r1-mod: FileOpener slot not recognised, loose UI files unavailable\n");
}
