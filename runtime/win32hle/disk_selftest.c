/* Synthetic directory and registered Win32 shim only; no retail files. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include "win32hle.h"
const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, 0}};
const uint32_t recomp_dispatch_count = 0;
static int failures;
static void check(const char *label, int ok) { if (!ok) { fprintf(stderr, "FAIL %s\n", label); ++failures; } }
static uint32_t call(const char *name, int n, const uint32_t *args) {
    uint32_t va = hle_resolve(name); if (!va) { ++failures; return 0; }
    return hle_call_guest(va, n, args);
}
int main(void) {
    char tmp[] = "/tmp/pcrecomp-disk-integration-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    hle_set_drive('Q', tmp); win32hle_register(win32hle_kernel32_ext); mach_enter();
    struct { char path[80]; unsigned char outputs[32]; } *g = hle_alloc(sizeof *g);
    if (!g) return 1;
    strcpy(g->path, "Q:\\"); memset(g->outputs, 0xa5, sizeof g->outputs);
    uint32_t args[] = { hle_va(g->path), hle_va(g->outputs + 1), hle_va(g->outputs + 10), hle_va(g->outputs + 19) };
    struct statvfs fs; if (statvfs(tmp, &fs)) return 1;
    hle_set_last_error(42); uint32_t status = call("GetDiskFreeSpaceExA", 4, args);
    uint64_t avail, total, free_total; memcpy(&avail, g->outputs+1, 8); memcpy(&total, g->outputs+10, 8); memcpy(&free_total, g->outputs+19, 8);
    uint64_t unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
    check("mapped volume", status && hle_get_last_error() == 42 && total == unit * fs.f_blocks && avail <= free_total && free_total <= total);
    check("output bounds", g->outputs[0] == 0xa5 && g->outputs[9] == 0xa5 && g->outputs[18] == 0xa5 && g->outputs[27] == 0xa5);
    char mapped[1024]; char *expected_root = realpath(tmp, NULL);
    check("resolve synthetic root", expected_root != NULL);
    check("disk path clamps to mapped root", expected_root && hle_host_path_base("Q:\\..\\", mapped, sizeof mapped) && !strcmp(mapped, expected_root));
    free(expected_root);
    strcpy(g->path, "Q:\\..\\");
    check("disk query with clamped path", call("GetDiskFreeSpaceExA", 4, args) != 0);
    uint32_t optional[] = { 0, 0, 0, 0 };
    check("null arguments", call("GetDiskFreeSpaceExA", 4, optional) != 0);
    strcpy(g->path, "Q:\\does-not-exist"); args[1] = args[2] = args[3] = 0;
    check("missing path", !call("GetDiskFreeSpaceExA", 4, args) && hle_get_last_error() == 3);
    strcpy(g->path, "\\\\server\\share\\");
    check("UNC unsupported", !call("GetDiskFreeSpaceExA", 4, args) && hle_get_last_error() == 120);
    args[0] = 0; args[1] = UINT32_MAX;
    check("wrapping output", !call("GetDiskFreeSpaceExA", 4, args) && hle_get_last_error() == 87);
    mach_leave(); hle_set_drive('Q', NULL); rmdir(tmp);
    if (!failures) puts("synthetic registered disk shim: pass");
    return failures ? 1 : 0;
}
