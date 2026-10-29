/* Synthetic guest arguments, temporary host directory and real statvfs only. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../runtime/win32hle/kernel32_ext.c"
uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
ptrdiff_t g_mem_base;
static uint32_t test_error;
void hle_set_last_error(uint32_t v) { test_error = v; }
static char host_root[1024];
int hle_host_path_base(const char *g, char *out, size_t size) {
    if (!strcmp(g, "C:\\") || !strcmp(g, "C:\\synthetic")) { snprintf(out, size, "%s", host_root); return 1; }
    if (!strcmp(g, "C:\\file")) { snprintf(out, size, "%s/file", host_root); return 1; }
    out[0] = 0; return 0;
}
void hle_guest_path(const char *host, char *out, size_t size) { (void)host; snprintf(out, size, "C:\\work"); }
static unsigned char guest[8192];
static uint32_t call(uint32_t path, uint32_t free_avail, uint32_t total, uint32_t free_total) {
    g_esp = 0x100; MEM32(0x104) = path; MEM32(0x108) = free_avail;
    MEM32(0x10c) = total; MEM32(0x110) = free_total;
    k_GetDiskFreeSpaceExA();
    if (g_esp != 0x114) return 0xffffffffu; return g_eax;
}
static uint64_t read64(uint32_t at) { uint64_t v; memcpy(&v, &guest[at], 8); return v; }
int main(void) {
    g_mem_base = (ptrdiff_t)(uintptr_t)guest;
    char tmp[] = "/tmp/pcrecomp-disk-test-XXXXXX"; if (!mkdtemp(tmp)) return 1;
    snprintf(host_root, sizeof host_root, "%s", tmp);
    struct statvfs fs; if (statvfs(tmp, &fs)) return 2;
    uint64_t unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
    uint64_t a, b, c, result = 55;
    if (!disk_mul_u64(unit, fs.f_bavail, &a) || !disk_mul_u64(unit, fs.f_blocks, &b) || !disk_mul_u64(unit, fs.f_bfree, &c)) return 3;
    if (disk_mul_u64(UINT64_MAX, 2, &result) || result != 55) return 4;
    if (!disk_mul_u64(UINT64_MAX, 1, &result) || result != UINT64_MAX) return 5;
    strcpy((char *)&guest[0x200], "C:\\synthetic"); test_error = 42;
    if (call(0x200, 0x301, 0x311, 0x321) != 1 || test_error != 42 || read64(0x311)!=b || read64(0x301)>read64(0x321) || read64(0x321)>read64(0x311)) return 6;
    if (call(0, 0, 0, 0) != 1 || test_error != 42) return 7;
    for (unsigned mask = 0; mask < 8; ++mask) {
        memset(&guest[0x300], 0xa5, 48); test_error = 42;
        if (call(0x200, mask & 1 ? 0x301 : 0, mask & 2 ? 0x311 : 0, mask & 4 ? 0x321 : 0) != 1 || test_error != 42) return 20;
        if ((mask & 1) ? read64(0x301) > b : read64(0x301) != UINT64_C(0xa5a5a5a5a5a5a5a5)) return 21;
        if ((mask & 2) ? read64(0x311) != b : read64(0x311) != UINT64_C(0xa5a5a5a5a5a5a5a5)) return 22;
        if ((mask & 4) ? read64(0x321) > b : read64(0x321) != UINT64_C(0xa5a5a5a5a5a5a5a5)) return 23;
    }
    strcpy((char *)&guest[0x200], "C:\\absent"); if (call(0x200, 0, 0, 0) != 0 || test_error != 3) return 8;
    strcpy((char *)&guest[0x200], "\\\\\\\\server\\\\share\\\\"); if (call(0x200, 0, 0, 0) != 0 || test_error != 120) return 9;
    if (call(0, UINT32_MAX, 0, 0) != 0 || test_error != 87) return 10;
    char path[1100]; snprintf(path, sizeof path, "%s/file", tmp); FILE *f = fopen(path, "w"); if (!f) return 11; fclose(f);
    strcpy((char *)&guest[0x200], "C:\\file"); if (call(0x200, 0, 0, 0) != 0 || test_error != 267) return 12;
    unlink(path); rmdir(tmp); puts("synthetic disk free space: pass"); return 0;
}
