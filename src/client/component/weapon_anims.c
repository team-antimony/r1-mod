/* ---- weapon swap style: QA or IW4 (Xbox) quick drop/raise clips ----
 * bg_weaponSwapStyle (set by the server, sent like the movement dvars; back to 0 in the lobby):
 *   0  QA: every gun plays the clips its weapon definition names
 *   1  IW4: the quick drop and quick raise play the gun's own "_quick" clip when the client has one
 *
 * Why: a switch from a primary to a pistol is a quick drop (weapon session of 2026-11-04, live). The QA weapon
 * definitions fill the quick slots with the regular putaway and pullout (viewmodel_cheytac_putaway, the
 * over-the-shoulder flip). IW4 had separate quick clips (viewmodel_cheytac_putaway_quick ...) that put the gun away
 * below the hip. The QA zones do NOT carry them: the names exist in the client only as placeholders (0 frames, no
 * bones, made when something requests a name no zone has), and playing one leaves the arms unposed, which is the
 * vertex explosion seen 2026-11-10. So style 1 only uses a "_quick" clip with frames and bones: one built from
 * iw4_quick_xanims.bin (the 1.1.1.13 zones' quick clips, exported by r1-server's codol_xanim_export; looked for in
 * the client mod folder, then next to the DLL), else one a zone really loaded. Without the bundle it changes nothing.
 *
 * QA view-model clip slots (anim index -> timing field via dword_CDD6E0[slot], names from qa_weapon_times.tsv):
 * 15 raise (iRaiseTime 0x750), 16 first raise (0xD4C), 17 alt raise (0x760), 18 drop (iDropTime 0x74C), 21 alt
 * drop (0x754), 22 quick raise (quickRaiseTime 0x75C), 23 quick drop (quickDropTime 0x758). The anim dispatcher
 * sub_557390 plays them through sub_556760 (usercall: eax = view model, then weapon, slot, blend time), which
 * scales a clip to the weapon's time for the slot (rate = clip length / time), so a substituted clip keeps the
 * server's timing and prediction and server still agree.
 *
 * How: sub_556760 is detoured at its entry. For slot 22 or 23 with style 1, the view model's anim set (cg view
 * model info: encrypted table 0x1A19510[0x7817BB8] ^ 0x39641402, 24 bytes per view model, +4 -> -> XAnim_s; clip
 * pointer at +8 + 8 * slot) gets "<clip name>_quick" when the asset table holds a loaded clip of that name. With style 0
 * every replaced slot gets its own clip back on the next clip played. Replaced slots are forgotten in the lobby
 * (the anim sets are rebuilt per map). */
#include "std_include.h"
#include <ctype.h>
#include "game/game.h"
#include "component/components.h"

#define VA_PLAY_SLOT      0x556760u /* usercall (eax = view model, weapon, slot, float blend) */
#define VA_VM_TABLE       0x1A19510u
#define VA_VM_TABLE_KEY   0x7817BB8u
#define VM_XOR            0x39641402u
#define VA_DVAR_REG_INT   0x633AB0u
#define VA_CONNSTATE      0x1A2E7ECu
#define ASSET_XANIM       2
#define SLOT_QUICK_RAISE  22
#define SLOT_QUICK_DROP   23
#define DVAR_VALUE        0x0C
#define DVAR_LATCHED      0x1C
#define MAX_SWAPS         128
#define MAX_LOOKUPS       256

static const unsigned char g_prologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18};

struct swap {
    void **entry;  /* clip pointer in an anim set */
    void *orig, *quick;
};

struct lookup {
    void *parts;   /* a clip */
    void *quick;   /* its _quick clip, NULL when the client has none */
};

static unsigned char *g_style;  /* dvar bg_weaponSwapStyle */
static uintptr_t g_resume;      /* VA_PLAY_SLOT + 6 */
static struct swap g_swaps[MAX_SWAPS];
static int g_swap_n;
static struct lookup g_lookups[MAX_LOOKUPS];
static int g_lookup_n;
static int g_installed;
static volatile LONG g_substituted;

/* The asset table, read without DB_FindAsset / DB_RequestAsset: a request for a name the zones lack makes a
 * placeholder (0 frames, no bones), and DB_FindAsset answers 1 exactly for such placeholders (entry byte +16 == 0),
 * 0 for loaded assets. Bucket = DB hash (sub_49E170: seed = type, h = tolower(c) + 31h, '\' hashed as '/', mod
 * 0x13880) into 0x1BC9F20; entries of 7 dwords at 0x1FC4E28: [0] type, [2] header, byte +16 loaded, [5] next. */
#define VA_DB_BUCKETS 0x1BC9F20u
#define VA_DB_ENTRIES 0x1FC4E28u
#define DB_HASH_SIZE  0x13880u

static void *loaded_xanim(const char *name)
{
    const unsigned *buckets = (const unsigned *)A(VA_DB_BUCKETS);
    const unsigned *entries = (const unsigned *)A(VA_DB_ENTRIES);
    unsigned h = ASSET_XANIM, i, guard;
    const char *c;
    for (c = name; *c; c++)
        h = (*c == '\\' ? '/' : (unsigned)tolower((unsigned char)*c)) + 31u * h;
    for (i = buckets[h % DB_HASH_SIZE], guard = 0; i && guard < 4096; i = entries[7 * i + 5], guard++) {
        const unsigned *e = &entries[7 * i];
        const unsigned char *parts = (const unsigned char *)e[2];
        if (e[0] != ASSET_XANIM || !parts || _stricmp(*(const char *const *)parts, name))
            continue;
        if (!((const unsigned char *)e)[16])
            return NULL; /* a placeholder, not a clip from a zone */
        return (void *)parts;
    }
    return NULL;
}

/* a clip that can stand in for a view-model clip: frames and bones (XAnimParts: frames +14, bone counts +17..+26) */
static int usable_clip(const unsigned char *parts)
{
    int i, bones = 0;
    for (i = 0; i < 10; i++)
        bones += parts[17 + i];
    return *(const unsigned short *)(parts + 14) > 0 && bones > 0;
}

/* ---- IW4 quick clips from a bundle (r1-server tools: codol_xanim_export, R1XA v1) ----
 * QA's zones lack the quick clips; the 1.1.1.13 zones have them, with the same bone names as QA's own clips (QA's
 * viewmodel_cheytac_putaway is 1.1.1.13's with the sleeve bones stripped). The bundle holds each clip's serialized
 * zone bytes (the 88-byte XAnimParts header, then inline name, bone names, notifies, delta part, the six data streams
 * and the frame indices, in the order the zone loader reads them) and the bone and notify names as text. Each clip is
 * rebuilt here as a runtime XAnimParts: header copied, every inline array copied to 16-byte aligned memory and its
 * pointer set, names turned into this client's script strings (SL_GetStringOfSize 0x5F73A0, as the zone loader
 * sub_485710 does). A clip with a back-referenced array or a delta part is refused rather than guessed. */
#define VA_SL_GET_STRING_OF_SIZE 0x5F73A0u /* cdecl unsigned (const char *, int user, size_t size with NUL) */
#define MAX_BUNDLE_CLIPS 512
#define XA_INLINE 0xFFFFFFFFu

struct bundle_clip {
    char name[96];
    unsigned char *parts; /* runtime XAnimParts, 88 bytes */
};

static struct bundle_clip g_bundle[MAX_BUNDLE_CLIPS];
static int g_bundle_n;

static unsigned rd16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return rd16(p) | rd16(p + 2) << 16; }

static void *aligned_copy(const unsigned char *src, size_t n)
{
    void *p = _aligned_malloc(n ? n : 1, 16);
    if (p && n)
        memcpy(p, src, n);
    return p;
}

static unsigned sl_string(const char *s)
{
    return ((unsigned(__cdecl *)(const char *, int, size_t))A(VA_SL_GET_STRING_OF_SIZE))(s, 4, strlen(s) + 1) & 0xFFFF;
}

/* one clip: d/n = serialized bytes, names = bone names then notify names. NULL when it cannot be rebuilt. */
static unsigned char *build_clip(const char *name, const unsigned char *d, size_t n, char **names, unsigned nnames)
{
    static const unsigned count_at[6] = {4, 6, 8, 32, 10, 12}, width[6] = {1, 2, 4, 2, 1, 4};
    unsigned char *h;
    size_t pos = 88, len;
    unsigned bones, notifies, frames, iw, i;
    if (n < 88)
        return NULL;
    bones = d[26];
    notifies = d[27];
    frames = rd16(d + 14);
    iw = frames < 256 ? 1 : 2;
    if (nnames != bones + notifies || rd32(d + 84) != 0)
        return NULL; /* names do not match, or a delta part */
    for (i = 48; i <= 80; i += 4)
        if (rd32(d + i) != 0 && rd32(d + i) != XA_INLINE)
            return NULL; /* back-referenced array */
    h = (unsigned char *)_aligned_malloc(88, 16);
    if (!h)
        return NULL;
    memcpy(h, d, 88);
    if (rd32(d) == XA_INLINE) { /* inline name */
        const unsigned char *z = (const unsigned char *)memchr(d + pos, 0, n - pos);
        if (!z)
            goto fail;
        pos = (size_t)(z - d) + 1;
    }
    *(char **)h = _strdup(name);
    if (rd32(d + 48)) {
        unsigned short *ids;
        if (pos + 2 * bones > n || !(ids = (unsigned short *)_aligned_malloc(2 * bones + 2, 16)))
            goto fail;
        for (i = 0; i < bones; i++)
            ids[i] = (unsigned short)sl_string(names[i]);
        *(void **)(h + 48) = ids;
        pos += 2 * bones;
    }
    if (rd32(d + 80)) {
        unsigned char *nt;
        if (pos + 8 * notifies > n || !(nt = (unsigned char *)aligned_copy(d + pos, 8 * notifies)))
            goto fail;
        for (i = 0; i < notifies; i++)
            *(unsigned short *)(nt + 8 * i) = (unsigned short)sl_string(names[bones + i]);
        *(void **)(h + 80) = nt;
        pos += 8 * notifies;
    }
    for (i = 0; i < 6; i++) {
        unsigned count = i == 3 ? rd32(d + count_at[i]) : rd16(d + count_at[i]);
        if (!rd32(d + 52 + 4 * i))
            continue;
        len = (size_t)count * width[i];
        if (pos + len > n)
            goto fail;
        *(void **)(h + 52 + 4 * i) = aligned_copy(d + pos, len);
        pos += len;
    }
    if (rd32(d + 76)) {
        len = (size_t)rd32(d + 36) * iw;
        if (pos + len > n)
            goto fail;
        *(void **)(h + 76) = aligned_copy(d + pos, len);
        pos += len;
    }
    if (pos != n)
        goto fail;
    *(void **)(h + 84) = NULL;
    h[29] = 0; /* isDefault */
    return h;
fail:
    out("r1-mod: quick clip %s does not rebuild (at byte %u of %u)\n", name, (unsigned)pos, (unsigned)n);
    return NULL; /* allocations made so far are left: this only happens with a damaged bundle */
}

/* <client mod>\<file>, else <dll dir>\<file>. Returns clips built. Bundles: iw4_quick_xanims.bin (IW4 quick
 * clips) and r1_xanims_319.bin (clips from your own 3.19.26.22 zones, such as weapon inspects; made with a local
 * export tool that is not part of r1-mod). */
static int load_bundle(const char *file)
{
    char path[MAX_PATH], magic[4];
    unsigned char hdr[8], *data = NULL;
    FILE *f = NULL;
    unsigned count, c, i;
    int before = g_bundle_n;
    if (fsmod_dir()) {
        _snprintf(path, sizeof path, "%s\\%s", fsmod_dir(), file);
        path[sizeof path - 1] = 0;
        f = fopen(path, "rb");
    }
    if (!f) {
        _snprintf(path, sizeof path, "%s%s", g_dir, file);
        path[sizeof path - 1] = 0;
        f = fopen(path, "rb");
    }
    if (!f)
        return 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "R1XA", 4) || fread(hdr, 1, 8, f) != 8 || rd32(hdr) != 1) {
        out("r1-mod: %s is not an R1XA v1 bundle\n", path);
        fclose(f);
        return 0;
    }
    count = rd32(hdr + 4);
    for (c = 0; c < count && g_bundle_n < MAX_BUNDLE_CLIPS; c++) {
        unsigned char b2[2], b4[4];
        char name[96], *names[512];
        unsigned nlen, size, nn;
        unsigned char *parts;
        if (fread(b2, 1, 2, f) != 2 || (nlen = rd16(b2)) >= sizeof name || fread(name, 1, nlen, f) != nlen)
            break;
        name[nlen] = 0;
        if (fread(b4, 1, 4, f) != 4 || (size = rd32(b4)) > (1u << 24) || !(data = (unsigned char *)malloc(size)) ||
            fread(data, 1, size, f) != size || fread(b2, 1, 2, f) != 2 || (nn = rd16(b2)) > 512)
            break;
        for (i = 0; i < nn; i++) {
            unsigned sl_len;
            names[i] = NULL;
            if (fread(b2, 1, 2, f) != 2)
                break;
            sl_len = rd16(b2);
            names[i] = (char *)malloc(sl_len + 1);
            if (!names[i] || fread(names[i], 1, sl_len, f) != sl_len) {
                free(names[i]);
                break;
            }
            names[i][sl_len] = 0;
        }
        parts = i == nn ? build_clip(name, data, size, names, nn) : NULL;
        while (i > 0)
            free(names[--i]);
        free(data);
        data = NULL;
        if (parts && usable_clip(parts)) {
            strcpy(g_bundle[g_bundle_n].name, name);
            g_bundle[g_bundle_n].parts = parts;
            g_bundle_n++;
        }
    }
    free(data);
    fclose(f);
    out("r1-mod: %d clip(s) built from %s\n", g_bundle_n - before, path);
    return g_bundle_n - before;
}

static int install_inspect_key(void);
static int install_dispatch_hook(void);
static void inspect_watch(void);
static volatile LONG g_inspecting; /* 1 while cx_inspect's clip is the view model's clip */

static void *bundle_clip(const char *name)
{
    int i;
    for (i = 0; i < g_bundle_n; i++)
        if (!_stricmp(g_bundle[i].name, name))
            return g_bundle[i].parts;
    return NULL;
}

static void *quick_clip(void *parts)
{
    char name[160];
    const char *base;
    void *quick = NULL;
    size_t n;
    int i;
    for (i = 0; i < g_lookup_n; i++)
        if (g_lookups[i].parts == parts)
            return g_lookups[i].quick;
    base = *(const char *const *)parts; /* XAnimParts: name at +0 */
    n = base ? strlen(base) : 0;
    if (n && n + 7 < sizeof name && !strstr(base, "_quick")) {
        memcpy(name, base, n);
        memcpy(name + n, "_quick", 7);
        quick = bundle_clip(name);
        if (!quick)
            quick = loaded_xanim(name);
        if (quick && !usable_clip((const unsigned char *)quick))
            quick = NULL;
    }
    if (g_lookup_n < MAX_LOOKUPS) {
        g_lookups[g_lookup_n].parts = parts;
        g_lookups[g_lookup_n].quick = quick;
        g_lookup_n++;
    }
    return quick;
}

/* style 0: every replaced slot gets its own clip back at once */
static void restore_all(void)
{
    while (g_swap_n > 0) {
        struct swap *s = &g_swaps[--g_swap_n];
        __try {
            if (*s->entry == s->quick)
                *s->entry = s->orig;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

static struct swap *swap_find(void **entry)
{
    int i;
    for (i = 0; i < g_swap_n; i++)
        if (g_swaps[i].entry == entry)
            return &g_swaps[i];
    return NULL;
}

/* ---- bg_weaponAnimFix: one clip family per staged reload ----
 * A part can swap a gun's clips for its own versions (the vertical grip: viewmodel_m1014_grip_*), but QA ships some
 * of those sets incomplete: the gripped M1014 has grip_reload_loop and no grip intro or outro, so a reload goes base
 * intro (hand on the pump) -> grip loop (hand on the grip) -> base outro and the hand jumps between stages (seen
 * 2026-11-04). With bg_weaponAnimFix 1 (default; 0 = stock QA) a staged reload keeps the family of its start clip:
 * when the clip of slot 9/10/11/12 (reload, the next three reload stages) and the start clip (slot 13, else the end
 * clip 14) name different families ("viewmodel_m1014_grip_" vs "viewmodel_m1014_" before "reload"), the slot gets the
 * start family's clip of the same stage when the asset table has it loaded. Timing is unchanged (clips are scaled to
 * the weapon's time for the slot). Replaced slots are kept in g_fixes and put back when the dvar goes to 0. */
#define SLOT_RELOAD_START 13
#define SLOT_RELOAD_END   14
#define MAX_FIXES         128

static unsigned char *g_fix;    /* dvar bg_weaponAnimFix */
static struct swap g_fixes[MAX_FIXES];
static int g_fix_n;
static volatile LONG g_fixed;

static const char *clip_name(void *parts)
{
    return parts ? *(const char *const *)parts : NULL;
}

/* "viewmodel_m1014_grip_reload_loop" -> length of "viewmodel_m1014_grip_" (the text before "reload"), -1 without */
static int family_len(const char *name)
{
    const char *r = name ? strstr(name, "reload") : NULL;
    return r ? (int)(r - name) : -1;
}

static void restore_fixes(void)
{
    while (g_fix_n > 0) {
        struct swap *s = &g_fixes[--g_fix_n];
        __try {
            if (*s->entry == s->quick)
                *s->entry = s->orig;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

static void fix_reload_family(unsigned char *anims, int slot)
{
    void **entry = (void **)(anims + 8 + 8 * slot), *parts = *entry, *start, *fixed;
    const char *have, *want;
    char name[160];
    int hl, wl, i;
    if (!parts)
        return;
    for (i = 0; i < g_fix_n; i++)
        if (g_fixes[i].entry == entry && parts == g_fixes[i].quick)
            return; /* already fixed */
    start = *(void **)(anims + 8 + 8 * SLOT_RELOAD_START);
    if (!start)
        start = *(void **)(anims + 8 + 8 * SLOT_RELOAD_END);
    have = clip_name(parts);
    want = clip_name(start);
    hl = family_len(have);
    wl = family_len(want);
    if (hl < 0 || wl < 0 || (hl == wl && !strncmp(have, want, (size_t)hl)))
        return; /* not a staged reload, or already one family */
    if ((size_t)wl + strlen(have + hl) + 1 > sizeof name)
        return;
    memcpy(name, want, (size_t)wl);
    strcpy(name + wl, have + hl); /* start family + this stage ("reload_loop") */
    fixed = loaded_xanim(name);
    if (!fixed || !usable_clip((const unsigned char *)fixed) || g_fix_n == MAX_FIXES)
        return;
    g_fixes[g_fix_n].entry = entry;
    g_fixes[g_fix_n].orig = parts;
    g_fixes[g_fix_n].quick = fixed;
    g_fix_n++;
    *entry = fixed;
    InterlockedIncrement(&g_fixed);
}

/* cx_swapstyle reloads: for every view model of this map, the staged-reload stages whose family differs from the
 * start clip's and the clip bg_weaponAnimFix would put in (or why not). Reads only; nothing is replaced. */
static void reload_report(void)
{
    unsigned key, base;
    int vm, slot, mixed = 0, fixable = 0, models = 0;
    __try {
        key = *(volatile unsigned *)A(VA_VM_TABLE_KEY);
        base = ((volatile unsigned *)A(VA_VM_TABLE))[key] ^ VM_XOR;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out("cx_swapstyle reloads: no view-model table\n");
        return;
    }
    for (vm = 0; vm < 2048; vm++) {
        __try {
            unsigned char **pp = *(unsigned char ***)(base + 24u * (unsigned)vm + 4);
            unsigned char *anims;
            void *start;
            const char *want;
            int wl;
            if (!pp || !(anims = *pp))
                continue;
            models++;
            start = *(void **)(anims + 8 + 8 * SLOT_RELOAD_START);
            if (!start)
                start = *(void **)(anims + 8 + 8 * SLOT_RELOAD_END);
            want = clip_name(start);
            if ((wl = family_len(want)) < 0)
                continue;
            for (slot = 9; slot <= 12; slot++) {
                void *parts = *(void **)(anims + 8 + 8 * slot);
                const char *have = clip_name(parts);
                char name[160];
                void *fixed;
                int hl = family_len(have), k, done = 0;
                if (hl < 0)
                    continue;
                for (k = 0; k < g_fix_n; k++)
                    if (g_fixes[k].entry == (void **)(anims + 8 + 8 * slot) && g_fixes[k].quick == parts)
                        done = 1;
                if (!done && hl == wl && !strncmp(have, want, (size_t)hl))
                    continue;
                mixed++;
                if (done) {
                    fixable++;
                    out("  vm %d slot %d: %s (already fixed, start %s)\n", vm, slot, have, want);
                    continue;
                }
                if ((size_t)wl + strlen(have + hl) + 1 > sizeof name)
                    continue;
                memcpy(name, want, (size_t)wl);
                strcpy(name + wl, have + hl);
                fixed = loaded_xanim(name);
                if (fixed && usable_clip((const unsigned char *)fixed)) {
                    fixable++;
                    out("  vm %d slot %d: %s -> %s (start %s)\n", vm, slot, have, name, want);
                } else {
                    out("  vm %d slot %d: %s, start %s: %s not loaded, left as is\n", vm, slot, have, want, name);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    out("cx_swapstyle reloads: %d view models, %d mixed reload stages, %d fixable\n", models, mixed, fixable);
}

/* runs on the game thread before sub_556760 plays a slot */
/* the view model and weapon the clip player was last called with (the local player's: only its view model has an
 * anim set), for cx_inspect */
static volatile int g_last_vm = -1, g_last_weapon;

static void __cdecl before_play(int view_model, int weapon, int slot)
{
    struct swap *s;
    void **entry, *parts, *quick;
    int style;
    g_last_vm = view_model;
    g_last_weapon = weapon;
    g_inspecting = 0; /* the game (or cx_inspect itself, which sets it again after) plays another clip */
    if (!g_style)
        return;
    if (g_fix) {
        if (*(int *)(g_fix + DVAR_VALUE) == 0) {
            if (g_fix_n)
                restore_fixes();
        } else if (slot >= 9 && slot <= 12) {
            __try {
                unsigned key = *(volatile unsigned *)A(VA_VM_TABLE_KEY);
                uintptr_t info = (((volatile unsigned *)A(VA_VM_TABLE))[key] ^ VM_XOR) + 24u * (unsigned)view_model;
                fix_reload_family(**(unsigned char ***)(info + 4), slot);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    style = *(int *)(g_style + DVAR_VALUE);
    if (style != 1) {
        if (g_swap_n)
            restore_all();
        return;
    }
    if (slot != SLOT_QUICK_RAISE && slot != SLOT_QUICK_DROP)
        return;
    __try {
        unsigned key = *(volatile unsigned *)A(VA_VM_TABLE_KEY);
        uintptr_t info = (((volatile unsigned *)A(VA_VM_TABLE))[key] ^ VM_XOR) + 24u * (unsigned)view_model;
        unsigned char *anims = **(unsigned char ***)(info + 4);
        entry = (void **)(anims + 8 + 8 * slot);
        parts = *entry;
        if (!parts)
            return;
        s = swap_find(entry);
        if (s && parts == s->quick)
            return; /* already the quick clip */
        quick = quick_clip(parts);
        if (!quick || g_swap_n == MAX_SWAPS)
            return;
        if (!s)
            s = &g_swaps[g_swap_n++];
        s->entry = entry;
        s->orig = parts;
        s->quick = quick;
        *entry = quick;
        InterlockedIncrement(&g_substituted);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static __declspec(naked) void play_slot_detour(void)
{
    __asm {
        pushad
        pushfd
        push dword ptr [esp + 44]  /* slot   (36 saved + return address + weapon) */
        push dword ptr [esp + 44]  /* weapon */
        push eax                   /* view model */
        call before_play
        add esp, 12
        popfd
        popad
        push ebp
        mov ebp, esp
        sub esp, 18h
        jmp dword ptr [g_resume]
    }
}

static int install_detour(void)
{
    unsigned char *p = (unsigned char *)A(VA_PLAY_SLOT);
    unsigned char code[6];
    DWORD old;
    if (memcmp(p, g_prologue, sizeof g_prologue) != 0)
        return 0;
    g_resume = A(VA_PLAY_SLOT) + sizeof g_prologue;
    code[0] = 0xE9;
    *(int *)(code + 1) = (int)((uintptr_t)play_slot_detour - ((uintptr_t)p + 5));
    code[5] = 0x90;
    if (!VirtualProtect(p, sizeof code, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(p, code, sizeof code);
    VirtualProtect(p, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof code);
    return 1;
}

/* timescale guard (50 ms): back in the lobby, style 0 and forget the anim sets of the last map */
void weapon_anims_keep(void)
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
        inspect_watch();
    } else if (was_connected) {
        was_connected = 0;
        *(int *)(g_style + DVAR_VALUE) = *(int *)(g_style + DVAR_LATCHED) = 0;
        if (g_fix)
            *(int *)(g_fix + DVAR_VALUE) = *(int *)(g_fix + DVAR_LATCHED) = 1;
        g_swap_n = 0;
        g_fix_n = 0;
        g_lookup_n = 0;
    }
}

/* cx_swapstyle            show the style and the clips replaced
 * cx_swapstyle install    register bg_weaponSwapStyle and hook the clip player (queued once at start, so that it
 *                         runs on the game thread: Dvar_Register* keeps state in the caller's TLS) */
static void __cdecl cmd_cx_swapstyle(void)
{
    int i;
    if (Cmd_Argc() >= 2 && !strcmp(Cmd_Argv(1), "install")) {
        typedef unsigned char *(__cdecl *reg_int_t)(const char *, int, int, int, int, const char *);
        if (g_installed)
            return;
        g_style = ((reg_int_t)A(VA_DVAR_REG_INT))("bg_weaponSwapStyle", 0, 0, 1, 0,
                                                  "Weapon swap clips: 0 QA, 1 IW4 quick drop/raise (set by the server)");
        g_fix = ((reg_int_t)A(VA_DVAR_REG_INT))("bg_weaponAnimFix", 1, 0, 1, 0,
                                                "Staged reloads keep one clip family: 1 on, 0 stock QA (set by the server)");
        if (!g_style || !install_detour()) {
            g_installed = -1;
            out("r1-mod: weapon swap style not installed (dvar or clip player not recognised)\n");
            return;
        }
        g_installed = 1;
        load_bundle("iw4_quick_xanims.bin");
        if (load_bundle("r1_xanims_319.bin")) {
            if (!install_dispatch_hook())
                out("r1-mod: inspect off (view-weapon anim dispatcher not recognised)\n");
            else if (!install_inspect_key())
                out("r1-mod: inspect key not hooked (bind handler not recognised); use cx_inspect\n");
        }
        out("r1-mod: weapon swap style ready (bg_weaponSwapStyle)\n");
        return;
    }
    if (g_installed <= 0) {
        out("cx_swapstyle: not installed\n");
        return;
    }
    if (Cmd_Argc() >= 2 && !strcmp(Cmd_Argv(1), "reloads")) {
        reload_report();
        return;
    }
    out("bg_weaponSwapStyle %d (0 QA, 1 IW4 quick drop/raise); %ld quick clips put in, %d slots replaced now\n",
        *(int *)(g_style + DVAR_VALUE), g_substituted, g_swap_n);
    out("bg_weaponAnimFix %d; %ld reload clips made one family, %d slots replaced now\n",
        g_fix ? *(int *)(g_fix + DVAR_VALUE) : -1, g_fixed, g_fix_n);
    for (i = 0; i < g_fix_n; i++)
        __try {
            out("  %s -> %s\n", *(const char *const *)g_fixes[i].orig, *(const char *const *)g_fixes[i].quick);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    for (i = 0; i < g_swap_n; i++)
        __try {
            out("  %s -> %s\n", *(const char *const *)g_swaps[i].orig, *(const char *const *)g_swaps[i].quick);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
}

/* ---- cx_inspect: play the held gun's inspect clip (test) ----
 * QA ships no inspect clips for its guns (one unused viewmodel_m4a1tech_inspect); 3.19.26.22 has them per gun
 * (viewmodel_cheytac_inspect ...). r1_xanims_319.bin brings them in as built clips. cx_inspect takes the held gun's
 * idle clip name, "viewmodel_<gun>_idle" -> "viewmodel_<gun>_inspect", puts that clip (bundle first, else a zone's
 * loaded clip) into view-model slot 50 and plays it. Slot 50 is weapon anim 0x29 in the dispatcher 0x557390 and has no
 * timing field (dword_CDD6E0[50] = -1), so the clip plays at its own rate. Client side only: other players do not see
 * it.
 *
 * Rules, as Modern Warfare Remastered plays an inspect: it starts only with the gun ready (weapon state 0, which also
 * rules out firing, reloading, raising, dropping and sprinting) and not aimed (fWeaponPosFrac 0); a press while one
 * plays does nothing; aiming down the sights ends it at once (the game draws no clip for ADS, so without this the
 * inspect kept playing in the scope); any clip the game plays itself (fire, reload, sprint, switch) ends it too.
 * Predicted player state = cg (0x1A19510[*0x7817BA8] ^ 0xFBB2C97A): weapState[0] at +620 (weapAnim +620, state
 * +636), weapCommon.fWeaponPosFrac +844 (the QA netfield table, tools\net\qa_netfields_aligned.json). */
#define SLOT_INSPECT 50
#define SLOT_IDLE 1
#define VA_CG_TABLE 0x1A19510u
#define VA_CG_KEY 0x7817BA8u
#define CG_XOR 0xFBB2C97Au
#define PS_WEAPON_STATE 636
#define PS_WEAPON_POS_FRAC 844

static volatile DWORD g_inspect_until;  /* GetTickCount when it ends by itself */

/* The clip player 0x556760 may only run inside the cgame frame: it reads the viewed entity's client info through a
 * base pointer (0x7817B80) that is set only while cgame draws a frame, and from a console command it was null
 * ("EXCEPTION at 0x5569E1, fault addr 0x979F0" = 952 * client + 618016 from base 0, 2026-11-11). So cx_inspect only
 * asks; the view-weapon anim dispatcher 0x557390 (cdecl, called each frame from 0x55C5F0) is detoured at its entry
 * and plays or stops the inspect there, and checks the ADS / weapon-state rules every frame. */
#define VA_ANIM_DISPATCH 0x557390u
static const unsigned char g_dispatch_prologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C};
static uintptr_t g_dispatch_resume;     /* VA_ANIM_DISPATCH + 6 */
static struct {
    volatile LONG play, stop;
    int vm, weapon, forced;
    unsigned char *anims;
    void *clip;
    char name[160];
} g_req;

/* 1 and the values when the predicted player state can be read */
static int weapon_ready(int *state, float *ads)
{
    __try {
        uintptr_t cg = ((volatile unsigned *)A(VA_CG_TABLE))[*(volatile unsigned *)A(VA_CG_KEY)] ^ CG_XOR;
        *state = *(volatile int *)(cg + PS_WEAPON_STATE);
        *ads = *(volatile float *)(cg + PS_WEAPON_POS_FRAC);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int inspect_playing(void)
{
    return g_inspecting && (LONG)(g_inspect_until - GetTickCount()) > 0;
}

/* called from weapon_anims_keep (50 ms); the frame hook below does the checking now */
static void inspect_watch(void)
{
}

static void play_slot(int view_model, int weapon, int slot, float blend)
{
    uintptr_t fn = A(VA_PLAY_SLOT);
    __asm {
        push blend
        push slot
        push weapon
        mov eax, view_model
        call fn
        add esp, 12
    }
}

/* cx_inspect          inspect the held gun (MWR rules above); quiet when a rule says no
 * cx_inspect <clip>   play that clip in the inspect slot, rules ignored (testing)
 * cx_inspect stop     end the inspect now
 * cx_inspect state    the weapon state and ADS fraction the rules read */
static void __cdecl cmd_cx_inspect(void)
{
    char name[160];
    const char *idle;
    unsigned char *anims;
    const unsigned char *parts;
    void *clip;
    size_t n;
    int vm = g_last_vm, weapon = g_last_weapon, state = -1, forced = 0;
    float ads = -1.0f, rate;
    if (Cmd_Argc() >= 2 && !strcmp(Cmd_Argv(1), "state")) {
        weapon_ready(&state, &ads);
        out("cx_inspect: weapon state %d, ADS %.2f, inspecting %d (%ld ms left)\n", state, ads, inspect_playing(),
            inspect_playing() ? (long)(g_inspect_until - GetTickCount()) : 0L);
        return;
    }
    if (Cmd_Argc() >= 2 && !strcmp(Cmd_Argv(1), "stop")) {
        if (inspect_playing())
            g_req.stop = 1;
        return;
    }
    if (g_installed <= 0 || vm < 0) {
        out("cx_inspect: no weapon clip played yet (join a match and raise a gun)\n");
        return;
    }
    forced = Cmd_Argc() >= 2;
    if (g_req.play)
        return; /* one already asked for, played on the next frame */
    if (!forced) {
        if (inspect_playing())
            return; /* one at a time: a press while it plays does nothing */
        if (!weapon_ready(&state, &ads) || state != 0 || ads > 0.0f)
            return; /* not ready, or aimed */
    }
    __try {
        unsigned key = *(volatile unsigned *)A(VA_VM_TABLE_KEY);
        uintptr_t info = (((volatile unsigned *)A(VA_VM_TABLE))[key] ^ VM_XOR) + 24u * (unsigned)vm;
        anims = **(unsigned char ***)(info + 4);
        idle = clip_name(*(void **)(anims + 8 + 8 * 1));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out("cx_inspect: no anim set for view model %d\n", vm);
        return;
    }
    n = idle ? strlen(idle) : 0;
    if (Cmd_Argc() >= 2) {
        _snprintf(name, sizeof name - 1, "%s", Cmd_Argv(1));
        name[sizeof name - 1] = 0;
    } else if (n > 5 && n + 4 < sizeof name && !strcmp(idle + n - 5, "_idle")) {
        memcpy(name, idle, n - 5);
        strcpy(name + n - 5, "_inspect");
    } else {
        out("cx_inspect: the held gun's idle clip (%s) does not name an inspect clip\n", idle ? idle : "none");
        return;
    }
    clip = bundle_clip(name);
    if (!clip)
        clip = loaded_xanim(name);
    if (!clip || !usable_clip((const unsigned char *)clip)) {
        static char last_missing[160];
        if (forced || strcmp(last_missing, name)) /* once per gun, not once per press */
            out("cx_inspect: %s is not loaded (add it to r1_xanims_319.bin)\n", name);
        strcpy(last_missing, name);
        return;
    }
    (void)parts;
    (void)rate;
    g_req.vm = vm;
    g_req.weapon = weapon;
    g_req.forced = forced;
    g_req.anims = anims;
    g_req.clip = clip;
    strcpy(g_req.name, name);
    g_req.stop = 0;
    g_req.play = 1; /* played by the frame hook */
}

/* inside the cgame frame, before the view weapon's anims are dispatched */
static void __cdecl on_dispatch(void)
{
    int state, ok;
    float ads;
    if (g_req.play) {
        const unsigned char *parts = (const unsigned char *)g_req.clip;
        float rate = *(const float *)(parts + 40); /* XAnimParts framerate */
        g_req.play = 0;
        ok = weapon_ready(&state, &ads);
        if (!g_req.forced && (!ok || state != 0 || ads > 0.0f))
            return; /* the gun changed between the press and this frame */
        __try {
            *(void **)(g_req.anims + 8 + 8 * SLOT_INSPECT) = g_req.clip;
            play_slot(g_req.vm, g_req.weapon, SLOT_INSPECT, 0.2f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out("cx_inspect: playing %s failed\n", g_req.name);
            return;
        }
        /* after the play: the clip player's detour clears g_inspecting for every clip, ours included */
        g_inspect_until = GetTickCount() +
            (DWORD)(rate > 1.0f ? 1000.0f * (float)(*(const unsigned short *)(parts + 14)) / rate : 3000.0f);
        g_inspecting = 1;
        if (g_req.forced)
            out("cx_inspect: %s (%u frames) on view model %d, weapon %d\n", g_req.name,
                *(const unsigned short *)(parts + 14), g_req.vm, g_req.weapon);
        return;
    }
    if (!inspect_playing())
        return;
    if (g_req.stop || (weapon_ready(&state, &ads) && (ads > 0.0f || state != 0))) {
        g_req.stop = 0;
        g_inspecting = 0;
        __try {
            play_slot(g_last_vm, g_last_weapon, SLOT_IDLE, 0.1f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

static __declspec(naked) void dispatch_detour(void)
{
    __asm {
        pushad
        pushfd
        call on_dispatch
        popfd
        popad
        push ebp
        mov ebp, esp
        sub esp, 2Ch
        jmp dword ptr [g_dispatch_resume]
    }
}

static int install_dispatch_hook(void)
{
    unsigned char *p = (unsigned char *)A(VA_ANIM_DISPATCH);
    unsigned char code[6];
    DWORD old;
    if (memcmp(p, g_dispatch_prologue, sizeof g_dispatch_prologue) != 0)
        return 0;
    g_dispatch_resume = A(VA_ANIM_DISPATCH) + sizeof g_dispatch_prologue;
    code[0] = 0xE9;
    *(int *)(code + 1) = (int)((uintptr_t)dispatch_detour - ((uintptr_t)p + 5));
    code[5] = 0x90;
    if (!VirtualProtect(p, sizeof code, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(p, code, sizeof code);
    VirtualProtect(p, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof code);
    return 1;
}

/* ---- QA's own inspect key ----
 * A key bound to +inspectWeapon (bind table 0xCE1CC0, index 73 down / 74 up) goes to the bind-action handler 0x460870
 * (cdecl: action, key; ecx = local client), which only presses a key button (case 73: 0x45E890). QA has nothing behind
 * that button for its guns, so a press showed nothing. The handler's entry is detoured: a 73 queues cx_inspect. The
 * first 11 bytes (push ebp; mov ebp, esp; push ecx; cmp byte ptr [connected flag], 0) run in a trampoline that jumps
 * back to +11; the cmp is last there because the handler's jz after it uses its flags. */
#define VA_BIND_ACTION 0x460870u
#define BIND_INSPECT_DOWN 73

static const unsigned char g_bind_prologue[5] = {0x55, 0x8B, 0xEC, 0x51, 0x80};
static unsigned char *g_bind_tramp;
static volatile LONG g_inspect_presses;

/* 1 = the action is ours and the game's handler is skipped. The inspect key's own button (kbutton at
 * 0x1A27250 + 680 * client + 640) becomes usercmd button 0x1000000 (CL_CmdButtons 0x45F660), which the QA client's
 * prediction and r1-server (C1 game logic, where the bit means something else) both acted on: a press from the key
 * misbehaved where cx_inspect from the console did not (2026-11-11). So the key's press and release never reach the
 * game; only cx_inspect runs. */
static int __cdecl on_bind_action(int action)
{
    if (action == BIND_INSPECT_DOWN) {
        InterlockedIncrement(&g_inspect_presses);
        Cbuf_AddText("cx_inspect\n");
        return 1;
    }
    return action == BIND_INSPECT_DOWN + 1;
}

static __declspec(naked) void bind_action_detour(void)
{
    __asm {
        pushad
        pushfd
        push dword ptr [esp + 40]  /* action (36 saved + return address) */
        call on_bind_action
        add esp, 4
        test eax, eax
        jnz swallow
        popfd
        popad
        jmp dword ptr [g_bind_tramp]
    swallow:
        popfd
        popad
        xor eax, eax
        ret                        /* cdecl: the caller removes its arguments */
    }
}

static int install_inspect_key(void)
{
    unsigned char *p = (unsigned char *)A(VA_BIND_ACTION);
    unsigned char code[11];
    DWORD old;
    int i;
    if (memcmp(p, g_bind_prologue, sizeof g_bind_prologue) != 0 || p[5] != 0x3D || p[10] != 0x00)
        return 0;
    g_bind_tramp = (unsigned char *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_bind_tramp)
        return 0;
    memcpy(g_bind_tramp, p, 11);                              /* the cmp keeps its (already relocated) address */
    g_bind_tramp[11] = 0xE9;
    *(int *)(g_bind_tramp + 12) = (int)((uintptr_t)(p + 11) - ((uintptr_t)g_bind_tramp + 16));
    code[0] = 0xE9;
    *(int *)(code + 1) = (int)((uintptr_t)bind_action_detour - ((uintptr_t)p + 5));
    for (i = 5; i < 11; i++)
        code[i] = 0x90;
    if (!VirtualProtect(p, sizeof code, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(p, code, sizeof code);
    VirtualProtect(p, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof code);
    return 1;
}

void weapon_anims_register(void)
{
    command_add("cx_swapstyle", cmd_cx_swapstyle);
    command_add("cx_inspect", cmd_cx_inspect);
}

void weapon_anims_start(void)
{
    Cbuf_AddText("cx_swapstyle install\n");
}
