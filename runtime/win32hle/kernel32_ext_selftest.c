/*
 * kernel32_ext_selftest.c - drives the file-I/O, Global*, and .ini shims from a
 * synthetic lifted guest: write a file and read it back, round-trip a Global
 * block, and write then read an .ini key. Headless, no game.
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "win32hle.h"
#include "selftest_os.h"

#define ENTRY_VA 0x00401000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_READ  0x80000000u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u

static int fails;
static void ok(const char *w, int c) { if (!c) { printf("FAIL %s\n", w); fails++; } }
#define GUEST_PROLOGUE(va) \
    uint32_t _cf = 0; uint32_t _flag_k = FK_NONE; uint32_t _flag_a = 0, _flag_b = 0; RECOMP_ENTER(va)

static uint32_t V_Create, V_Read, V_Write, V_Close, V_GAlloc, V_GLock, V_GFree,
                V_WriteIni, V_GetIni;

/* scratch guest memory (VAs, made in main); the files are in the scratch
 * folder, which the guest can access as T: */
static char g_ini_host[600];
static uint32_t g_path, g_ini, g_sec, g_key, g_val, g_payload, g_readbuf, g_inibuf;
static uint32_t g_io;     /* bytes read/written out-param */

static int mem_eq;
static uint32_t g_global_ptr;
static int ini_ok;

static void guest(void) {
    GUEST_PROLOGUE(ENTRY_VA);

    /* h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0) */
    PUSH32(esp, 0); PUSH32(esp, 0); PUSH32(esp, CREATE_ALWAYS); PUSH32(esp, 0);
    PUSH32(esp, 0); PUSH32(esp, GENERIC_WRITE); PUSH32(esp, g_path);
    RECOMP_ICALL(V_Create);
    uint32_t h = eax;
    /* WriteFile(h, payload, 16, &io, 0) */
    PUSH32(esp, 0); PUSH32(esp, g_io); PUSH32(esp, 16);
    PUSH32(esp, g_payload); PUSH32(esp, h);
    RECOMP_ICALL(V_Write);
    PUSH32(esp, h); RECOMP_ICALL(V_Close);

    /* h = CreateFileA(path, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0); ReadFile */
    PUSH32(esp, 0); PUSH32(esp, 0); PUSH32(esp, OPEN_EXISTING); PUSH32(esp, 0);
    PUSH32(esp, 0); PUSH32(esp, GENERIC_READ); PUSH32(esp, g_path);
    RECOMP_ICALL(V_Create);
    h = eax;
    PUSH32(esp, 0); PUSH32(esp, g_io); PUSH32(esp, 16);
    PUSH32(esp, g_readbuf); PUSH32(esp, h);
    RECOMP_ICALL(V_Read);
    PUSH32(esp, h); RECOMP_ICALL(V_Close);
    mem_eq = (MEM32(g_io) == 16) && memcmp(hle_ptr(g_payload), hle_ptr(g_readbuf), 16) == 0;

    /* p = GlobalAlloc(GMEM_ZEROINIT, 32); GlobalLock(p); write; GlobalFree */
    PUSH32(esp, 32); PUSH32(esp, 0x40); RECOMP_ICALL(V_GAlloc);
    uint32_t p = eax;
    PUSH32(esp, p); RECOMP_ICALL(V_GLock);
    uint32_t locked = eax;
    MEM32(locked) = 0xABCD1234u;
    g_global_ptr = (p && locked == p && MEM32(p) == 0xABCD1234u);
    PUSH32(esp, p); RECOMP_ICALL(V_GFree);

    /* WritePrivateProfileStringA(sec,key,val,ini); GetPrivateProfileStringA back */
    PUSH32(esp, g_ini); PUSH32(esp, g_val);
    PUSH32(esp, g_key); PUSH32(esp, g_sec);
    RECOMP_ICALL(V_WriteIni);
    PUSH32(esp, g_ini); PUSH32(esp, 64);
    PUSH32(esp, g_inibuf); PUSH32(esp, 0 /*default*/);
    PUSH32(esp, g_key); PUSH32(esp, g_sec);
    RECOMP_ICALL(V_GetIni);
    ini_ok = strcmp((const char *)hle_ptr(g_inibuf), "1024") == 0;

    eax = 0;
}

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { ENTRY_VA, guest } };
const uint32_t recomp_dispatch_count = 1;

int main(void) {
    hle_set_drive('T', test_tmp());
    test_path(g_ini_host, sizeof g_ini_host, "win32hle_k32ext.ini");
    remove(g_ini_host);   /* the append-writer starts clean */
    static const uint8_t payload[16] = { 1,2,3,4,5,6,7,8, 9,10,11,12,13,14,15,16 };
    g_path = hle_guest_strdup("T:\\win32hle_k32ext.dat"), g_ini = hle_guest_strdup("T:\\win32hle_k32ext.ini");
    g_sec = hle_guest_strdup("video"), g_key = hle_guest_strdup("width"), g_val = hle_guest_strdup("1024");
    g_payload = hle_va(hle_alloc(16)), memcpy(hle_ptr(g_payload), payload, 16);
    g_readbuf = hle_va(hle_alloc(16)), g_inibuf = hle_va(hle_alloc(64)), g_io = hle_va(hle_alloc(4));
    win32hle_register(win32hle_kernel32);
    win32hle_register(win32hle_kernel32_ext);
    V_Create   = hle_resolve("CreateFileA");
    V_Read     = hle_resolve("ReadFile");
    V_Write    = hle_resolve("WriteFile");
    V_Close    = hle_resolve("CloseHandle");
    V_GAlloc   = hle_resolve("GlobalAlloc");
    V_GLock    = hle_resolve("GlobalLock");
    V_GFree    = hle_resolve("GlobalFree");
    V_WriteIni = hle_resolve("WritePrivateProfileStringA");
    V_GetIni   = hle_resolve("GetPrivateProfileStringA");
    ok("all kernel32_ext shims resolve",
       V_Create && V_Read && V_Write && V_Close && V_GAlloc && V_GLock && V_GFree && V_WriteIni && V_GetIni);

    mach_enter();
    guest();
    mach_leave();

    ok("file write then read round-trips", mem_eq);
    ok("GlobalAlloc/Lock write-through",    g_global_ptr);
    ok(".ini write then read round-trips",  ini_ok);

    /* A mod's overlay: its file read instead of the game's, the game's where
     * it has none, writes into the overlay (the game's file copied first when
     * kept), and the game's files untouched. */
    {
        char p[1024], q[1024], ov[600], game[700], mod[700], at[800];
        FILE *f;
        test_path(ov, sizeof ov, "hleov");
        snprintf(game, sizeof game, "%s/game", ov), snprintf(mod, sizeof mod, "%s/mod", ov);
        test_rmtree(ov);
        snprintf(at, sizeof at, "%s/maps", game), test_mkdirs(at), test_mkdirs(mod);
        /* Paths come back resolved (macOS's /tmp is /private/tmp), and a
         * case-insensitive disk (APFS, NTFS) finds a file by the spelling asked. */
        char root[4096], want[4200];
        char *real = hle_os_realpath(ov);
        snprintf(root, sizeof root, "%s", real ? real : ov);
        free(real);
        snprintf(at, sizeof at, "%s/rules.ini", game), f = fopen(at, "w"); fputs("base", f); fclose(f);
        snprintf(at, sizeof at, "%s/maps/a.map", game), f = fopen(at, "w"); fputs("map", f); fclose(f);
        snprintf(at, sizeof at, "%s/RULES.INI", mod), f = fopen(at, "w"); fputs("mod", f); fclose(f);
        hle_set_drive('C', game);
        hle_set_overlay(game, mod);
        ok("overlay: the mod's file is read", hle_host_path("C:\\rules.ini", p, sizeof p) && test_strcasestr(p, "/mod/RULES.INI"));
        ok("overlay: the game's where the mod has none", hle_host_path("C:\\maps\\a.map", p, sizeof p) && strstr(p, "/game/maps/a.map"));
        hle_host_path_for_write("C:\\saves\\one.sav", p, sizeof p, 0);
        snprintf(want, sizeof want, "%s/mod/saves/one.sav", root);
        snprintf(at, sizeof at, "%s/saves", mod);
        ok("overlay: a new file goes to the mod, its directory made", !strcmp(p, want) && access(at, F_OK) == 0);
        hle_host_path_for_write("C:\\maps\\a.map", p, sizeof p, 1);
        f = fopen(p, "r");
        ok("overlay: a kept file is copied into the mod first", f && fgets(q, sizeof q, f) && !strcmp(q, "map") &&
           strstr(p, "/mod/maps/a.map"));
        if (f) fclose(f);
        snprintf(at, sizeof at, "%s/saves", game);
        ok("overlay: the game's files untouched", access(at, F_OK) != 0);
        snprintf(want, sizeof want, "%s/game", root);
        ok("overlay: a directory stays the game's (the cwd)", hle_host_path("C:\\", p, sizeof p) && !strcmp(p, want));
        chdir(game);
        ok("overlay: a name relative to the game's folder", hle_host_path("rules.ini", p, sizeof p) && test_strcasestr(p, "/mod/RULES.INI"));
        hle_set_overlay(NULL, NULL);
        ok("overlay off: the game's file again", hle_host_path("C:\\rules.ini", p, sizeof p) && strstr(p, "/game/rules.ini"));
    }

    /* The guest's printf: arguments read from guest memory, and a spec of 19
     * flags, a 10-digit * width and ll stays inside its buffer (under ASan
     * this overflowed it). */
    {
        char out[64];
        uint32_t a = hle_va(hle_alloc(16));
        MEM32(a) = 42, MEM32(a + 4) = hle_guest_strdup("ok");
        hle_guest_format(out, sizeof out, "%04d %s", a);
        ok("guest format: %04d %s", !strcmp(out, "0042 ok"));
        MEM32(a) = (uint32_t)-100000000, MEM32(a + 4) = 7, MEM32(a + 8) = 0;
        hle_guest_format(out, sizeof out, "%-------------------*lld", a);
        ok("guest format: a long spec stays in its buffer", out[0] == '7');
    }

    if (fails == 0) printf("kernel32_ext_selftest: all checks passed\n");
    return fails != 0;
}
