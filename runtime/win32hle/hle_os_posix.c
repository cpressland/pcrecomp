/*
 * hle_os_posix.c - the hle_os.h functions for Linux, macOS and the browser
 * (Emscripten). See hle_os_win32.c for Windows.
 */
#ifndef _WIN32
/* For the calls beyond standard C used here (like pread, realpath, timegm
 * and tm_gmtoff), even if the compiler is set to a strict C standard. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>   /* for _NSGetExecutablePath, in hle_os_exe_path */
#endif
#include "hle_os.h"

/* Older Linux headers lack MAP_FIXED_NOREPLACE (Linux 4.17+). Its value is
 * passed anyway: an older kernel ignores it and treats the address as a
 * hint, and the callers check where the memory actually landed. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
/* Not every OS has MAP_NORESERVE (don't set memory aside for this mapping
 * up front). Without it, mappings work the same, just without that hint. */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/* ---- memory ---- */

size_t hle_os_page_size(void) { return (size_t)getpagesize(); }

void *hle_os_reserve(void *at, size_t size) {
    /* at is asked for, not forced (no MAP_FIXED), so nothing already mapped
     * there is replaced. Linux is told so with MAP_FIXED_NOREPLACE; macOS
     * uses the address as long as it's free. If the memory lands somewhere
     * else, it's given back and the reservation fails. */
    void *p = mmap(at, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
#ifdef __linux__
                   | (at ? MAP_FIXED_NOREPLACE : 0)
#endif
                   , -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (at && p != at) { munmap(p, size); return NULL; }
    return p;
}

/* Maps new pages over the range, replacing whatever was there, so they're
 * always zeroed (even if they were already committed). */
int hle_os_commit(void *p, size_t size) {
    return mmap(p, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == p ? 0 : -1;
}
int hle_os_decommit(void *p, size_t size) {
    return mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == p ? 0 : -1;
}

void *hle_os_map(void *at, size_t size) {
    void *p = mmap(at, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (at ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (at && p != at) { munmap(p, size); return NULL; }
    return p;
}
void hle_os_unmap(void *p, size_t size)   { munmap(p, size); }
/* madvise(MADV_DONTNEED) gives the whole pages inside the range back, and on
 * Linux they read back as zero afterwards. It only takes whole pages (it
 * fails if p isn't at a page start, and rounds the length up, which would
 * zero bytes past the end), so the partial pages at either end are zeroed by
 * hand. In the browser madvise does nothing, so everything is zeroed by
 * hand. */
void hle_os_discard(void *p, size_t size) {
#ifndef __EMSCRIPTEN__
    uintptr_t lo = (uintptr_t)p, hi = lo + size, page = hle_os_page_size();
    uintptr_t plo = (lo + page - 1) & ~(page - 1), phi = hi & ~(page - 1);
    if (plo < phi && madvise((void *)plo, phi - plo, MADV_DONTNEED) == 0) {
        memset(p, 0, plo - lo);
        memset((void *)phi, 0, hi - phi);
        return;
    }
#endif
    memset(p, 0, size);
}

int hle_os_map_file(void *p, size_t len, int fd, uint64_t off, int writable, int copy) {
#ifdef __EMSCRIPTEN__
    /* The browser can't map a file at a chosen address, so the bytes are
     * copied in instead, and writes to the memory never reach the file. If
     * the file is shorter than len, the rest stays zero. */
    (void)writable, (void)copy;
    for (size_t done = 0; done < len;) {
        ssize_t r = pread(fd, (uint8_t *)p + done, len - done, (off_t)(off + done));
        if (r <= 0) break;
        done += (size_t)r;
    }
    return 0;
#else
    int prot = PROT_READ | (writable || copy ? PROT_WRITE : 0);
    return mmap(p, len, prot, MAP_FIXED | (copy ? MAP_PRIVATE : MAP_SHARED), fd, (off_t)off) == MAP_FAILED ? -1 : 0;
#endif
}

/* ---- files and paths ---- */

char *hle_os_realpath(const char *path) { return realpath(path, NULL); }
char *hle_os_getcwd(char *buf, size_t n) { return getcwd(buf, n); }
int hle_os_mkdir(const char *path)       { return mkdir(path, 0755); }

int hle_os_exe_path(char *out, size_t n) {
#ifdef __APPLE__
    char raw[4096], real[4096];
    uint32_t len = sizeof raw;
    if (!n || _NSGetExecutablePath(raw, &len) != 0 || !realpath(raw, real) || strlen(real) >= n) return 0;
    memcpy(out, real, strlen(real) + 1);
    return 1;
#else
    /* readlink doesn't fail when the path is too long for out: it fills out
     * with as much as fits. So a result that fills out completely may have
     * been cut short, and counts as not fitting. */
    ssize_t len = n ? readlink("/proc/self/exe", out, n) : -1;
    if (len <= 0 || (size_t)len >= n) return 0;
    out[len] = 0;
    return 1;
#endif
}

ssize_t hle_os_pread(int fd, void *buf, size_t n, int64_t off) { return pread(fd, buf, n, (off_t)off); }

/* *out = a * b. Returns 0 if that overflows 64 bits. */
static int mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a && b > UINT64_MAX / a) return 0;
    *out = a * b;
    return 1;
}
int hle_os_disk_space(const char *path, uint64_t *avail, uint64_t *total, uint64_t *free_bytes) {
    struct statvfs info;
    if (statvfs(path, &info) != 0) return -1;
    uint64_t unit = info.f_frsize ? info.f_frsize : info.f_bsize;
    if (!unit || !mul_u64(unit, info.f_bavail, avail) || !mul_u64(unit, info.f_blocks, total) ||
        !mul_u64(unit, info.f_bfree, free_bytes)) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

struct hle_os_dir { DIR *d; };
hle_os_dir *hle_os_opendir(const char *path) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    hle_os_dir *h = (hle_os_dir *)malloc(sizeof *h);
    if (!h) { closedir(d); return NULL; }
    h->d = d;
    return h;
}
const char *hle_os_readdir(hle_os_dir *h, int *is_dir) {
    struct dirent *e = h ? readdir(h->d) : NULL;
    if (!e) return NULL;
    if (is_dir) *is_dir = e->d_type == DT_DIR ? 1 : e->d_type == DT_UNKNOWN ? -1 : 0;
    return e->d_name;
}
void hle_os_closedir(hle_os_dir *h) {
    if (!h) return;
    closedir(h->d);
    free(h);
}

/* ---- time ---- */

struct tm *hle_os_localtime(const time_t *t, struct tm *out) { return localtime_r(t, out); }
struct tm *hle_os_gmtime(const time_t *t, struct tm *out)    { return gmtime_r(t, out); }
time_t hle_os_timegm(struct tm *t)                            { return timegm(t); }
long hle_os_utc_offset(void) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return (long)t.tm_gmtoff;
}

/* macOS names struct stat's nanosecond times st_mtimespec and st_atimespec,
 * where Linux has st_mtim and st_atim. */
#ifdef __APPLE__
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#endif
void hle_os_stat_times(const struct stat *st, struct timespec *mtime, struct timespec *atime) {
    if (mtime) *mtime = st->st_mtim;
    if (atime) *atime = st->st_atim;
}
int hle_os_set_file_times(int fd, const struct timespec *atime, const struct timespec *mtime) {
    struct timespec ts[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
    if (atime) ts[0] = *atime;
    if (mtime) ts[1] = *mtime;
    return futimens(fd, ts);
}

/* ---- CPUs ---- */

int hle_os_cpu_count(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }

/* ---- fonts ---- */

/* Tried in order, stopping at the first that exists:
 *   1. Liberation Sans, Serif or Mono, as fontconfig (fc-match) finds it.
 *      fc-match always gives its closest match, so with fontconfig installed
 *      this nearly always finds something, even if Liberation isn't there.
 *   2. Liberation in the usual Linux font folders.
 *   3. DejaVu Sans (for every kind) in the same folders.
 *   4. macOS's own Arial, Times New Roman or Courier New. */
const char *hle_os_font_file(int kind, int bold, int italic, char *path, size_t n) {
    const char *family = kind == HLE_OS_FONT_SERIF ? "Liberation Serif" : kind == HLE_OS_FONT_MONO ? "Liberation Mono" : "Liberation Sans";
    const char *style = bold && italic ? "Bold Italic" : bold ? "Bold" : italic ? "Italic" : "Regular";
    char cmd[256];
    snprintf(cmd, sizeof cmd, "fc-match -f '%%{file}' '%s:style=%s' 2>/dev/null", family, style);
    FILE *p = popen(cmd, "r");
    if (p) {
        size_t got = fread(path, 1, n - 1, p);
        pclose(p);
        path[got] = 0;
        if (got && access(path, R_OK) == 0) return path;
    }
    static const char *const dirs[] = { "/usr/share/fonts/truetype/liberation/", "/usr/share/fonts/truetype/liberation2/",
                                        "/usr/share/fonts/liberation/", "/usr/share/fonts/liberation-sans/",
                                        "/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/dejavu/" };
    char file[64];
    snprintf(file, sizeof file, "%s-%s.ttf", kind == HLE_OS_FONT_SERIF ? "LiberationSerif" : kind == HLE_OS_FONT_MONO ? "LiberationMono" : "LiberationSans",
             bold && italic ? "BoldItalic" : bold ? "Bold" : italic ? "Italic" : "Regular");
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, n, "%s%s", dirs[i], file);
        if (access(path, R_OK) == 0) return path;
    }
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(path, n, "%s%s", dirs[i], bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf");
        if (access(path, R_OK) == 0) return path;
    }
    const char *mac = kind == HLE_OS_FONT_SERIF ? "Times New Roman" : kind == HLE_OS_FONT_MONO ? "Courier New" : "Arial";
    snprintf(path, n, "/System/Library/Fonts/Supplemental/%s%s.ttf", mac,
             bold && italic ? " Bold Italic" : bold ? " Bold" : italic ? " Italic" : "");
    if (access(path, R_OK) == 0) return path;
    return NULL;
}

#endif /* !_WIN32 */
