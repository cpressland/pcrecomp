/*
 * hle_os_win32.c - the hle_os.h functions for Windows (64-bit, built with a
 * MinGW compiler such as llvm-mingw). See hle_os_posix.c for everything else.
 *
 * This is the only win32hle file that includes <windows.h>. The rest of
 * win32hle defines the Win32 names its guest uses itself, and they would
 * clash with the real ones.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <errno.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "hle_os.h"

/* ---- memory ----
 * These map directly onto Windows' own terms: hle_os_reserve is
 * VirtualAlloc(MEM_RESERVE), hle_os_commit is MEM_COMMIT and hle_os_decommit
 * is MEM_DECOMMIT. */

size_t hle_os_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}

/* VirtualAlloc never replaces anything: if at is in use it fails. It also
 * rounds at down to a multiple of 64 KB, so a reservation that didn't land
 * exactly at at is given back and counts as failed. */
void *hle_os_reserve(void *at, size_t size) {
    void *p = VirtualAlloc(at, size, MEM_RESERVE, PAGE_NOACCESS);
    if (p && at && p != at) { VirtualFree(p, 0, MEM_RELEASE); return NULL; }
    return p;
}

int hle_os_decommit(void *p, size_t size) {
    return VirtualFree(p, size, MEM_DECOMMIT) ? 0 : -1;
}
/* Committing pages that are already committed keeps their old contents, so
 * they're decommitted first. Windows always gives newly committed pages
 * back zeroed. */
int hle_os_commit(void *p, size_t size) {
    if (hle_os_decommit(p, size)) return -1;
    return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) == p ? 0 : -1;
}

/* Flat mode, used only by a 32-bit build. Reserved and committed in one go;
 * the address works as in hle_os_reserve. */
void *hle_os_map(void *at, size_t size) {
    void *p = VirtualAlloc(at, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p && at && p != at) { VirtualFree(p, 0, MEM_RELEASE); return NULL; }
    return p;
}
/* MEM_RELEASE always frees a whole mapping, so size isn't needed. */
void hle_os_unmap(void *p, size_t size)   { (void)size; VirtualFree(p, 0, MEM_RELEASE); }
/* The whole pages inside the range are decommitted and committed again, so
 * their memory goes back to Windows and they come back zeroed. The partial
 * pages at either end are zeroed by hand (VirtualFree would decommit all of
 * any page it touches, zeroing bytes outside the range). */
void hle_os_discard(void *p, size_t size) {
    uintptr_t lo = (uintptr_t)p, hi = lo + size, page = hle_os_page_size();
    uintptr_t plo = (lo + page - 1) & ~(page - 1), phi = hi & ~(page - 1);
    if (plo >= phi || hle_os_commit((void *)plo, phi - plo)) { memset(p, 0, size); return; }
    memset(p, 0, plo - lo);
    memset((void *)phi, 0, hi - phi);
}

/* A file view (MapViewOfFileEx) can only go into free address space, and the
 * guest's 4 GB is all one reservation, so the file's bytes are copied in
 * instead. Writes to the memory never reach the file, and if the file is
 * shorter than len, the rest stays zero.
 * Windows 10 1803+ could do real views here, by reserving the 4 GB as
 * placeholders (VirtualAlloc2) and mapping over one (MapViewOfFile3). */
int hle_os_map_file(void *p, size_t len, int fd, uint64_t off, int writable, int copy) {
    (void)writable, (void)copy;
    for (size_t done = 0; done < len;) {
        ssize_t r = hle_os_pread(fd, (uint8_t *)p + done, len - done, (int64_t)(off + done));
        if (r <= 0) break;
        done += (size_t)r;
    }
    return 0;
}

/* ---- files and paths ---- */

/* Turn every backslash in s into a forward slash: win32hle's host paths
 * always use forward slashes (see hle_os.h). */
static void forward_slashes(char *s) {
    for (; *s; s++) if (*s == '\\') *s = '/';
}

/* The path is opened and Windows is asked for the opened file's final path,
 * so symlinks and junctions are resolved, as POSIX realpath resolves
 * symlinks. Windows gives that path with a \\?\ prefix (like \\?\C:\Users),
 * or \\?\UNC\ for a network path (like \\?\UNC\server\share\folder).
 *
 * A drive mapped to a network share (net use) also comes back as its network
 * path, which the rest of win32hle can't use as a root. In that case the
 * drive letter path is kept instead, with "." and ".." resolved but not
 * junctions. */
char *hle_os_realpath(const char *path) {
    HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS /* needed to open a folder */, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    char full[MAX_PATH * 4];
    DWORD n = GetFinalPathNameByHandleA(h, full, sizeof full, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= sizeof full) return NULL;
    char *s = full;
    if (!strncmp(s, "\\\\?\\UNC\\", 8)) {
        char lettered[MAX_PATH * 4];
        DWORD m = GetFullPathNameA(path, sizeof lettered, lettered, NULL);
        if (m && m < sizeof lettered && lettered[1] == ':') memcpy(full, lettered, m + 1);
        else s += 6, s[0] = '\\';                          /* turn \\?\UNC\server into \\server */
    }
    else if (!strncmp(s, "\\\\?\\", 4)) s += 4;
    forward_slashes(s);
    /* The drive letter in capitals (c:/ becomes C:/), so the same folder
     * always gives the same string: win32hle compares paths as strings. */
    if (s[0] && s[1] == ':') s[0] = (char)(s[0] & ~0x20);
    /* No slash at the end, except a root's own (C:/ stays as it is). */
    size_t l = strlen(s);
    while (l > hle_os_root_len(s) && s[l - 1] == '/') s[--l] = 0;
    return _strdup(s);
}

char *hle_os_getcwd(char *buf, size_t n) {
    char *real = hle_os_realpath(".");
    if (!real || strlen(real) >= n) { free(real); return NULL; }
    memcpy(buf, real, strlen(real) + 1);
    free(real);
    return buf;
}

int hle_os_mkdir(const char *path) { return _mkdir(path); }

int hle_os_exe_path(char *out, size_t n) {
    char raw[MAX_PATH * 4];
    DWORD len = GetModuleFileNameA(NULL, raw, sizeof raw);
    if (!len || len >= sizeof raw) return 0;
    /* GetModuleFileName gives the path the program was started by, which may
     * go through a junction: resolve it as hle_os_realpath would. */
    char *real = hle_os_realpath(raw);
    int fits = real && strlen(real) < n;
    if (fits) memcpy(out, real, strlen(real) + 1);
    free(real);
    return fits;
}

/* Windows' C runtime has no pread, so this seeks to off, reads, and seeks
 * back to where fd was. Putting the position back matters when the guest
 * reads from the same file itself (like a Bink movie opened by the game).
 * It isn't safe if another thread uses fd at the same time. */
ssize_t hle_os_pread(int fd, void *buf, size_t n, int64_t off) {
    int64_t was = _lseeki64(fd, 0, SEEK_CUR);
    if (was < 0 || _lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int r = _read(fd, buf, n > 0x7FFFFFFFu ? 0x7FFFFFFFu : (unsigned)n);
    _lseeki64(fd, was, SEEK_SET);
    return r;
}

/* The errno closest to a Windows error code, for the few errors the callers
 * tell apart; anything else is EINVAL. */
static int errno_of(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH: return ENOENT;
    case ERROR_ACCESS_DENIED: return EACCES;
    case ERROR_NOT_READY: return EIO;
    default: return EINVAL;
    }
}

int hle_os_disk_space(const char *path, uint64_t *avail, uint64_t *total, uint64_t *free_bytes) {
    ULARGE_INTEGER a, t, f;
    if (!GetDiskFreeSpaceExA(path, &a, &t, &f)) { errno = errno_of(GetLastError()); return -1; }
    *avail = a.QuadPart, *total = t.QuadPart, *free_bytes = f.QuadPart;
    return 0;
}

/* FindFirstFile both opens the listing and returns its first entry, so the
 * first entry is kept until the first hle_os_readdir. */
struct hle_os_dir { HANDLE h; WIN32_FIND_DATAA fd; int first; };
/* Lists everything in the folder: FindFirstFile is given path with a "*"
 * wildcard added as its last component. */
hle_os_dir *hle_os_opendir(const char *path) {
    char pat[MAX_PATH * 4];
    size_t l = strlen(path);
    if (l + 3 > sizeof pat) return NULL;
    snprintf(pat, sizeof pat, "%s%s*", path, l && (path[l - 1] == '/' || path[l - 1] == '\\') ? "" : "/");
    hle_os_dir *d = (hle_os_dir *)malloc(sizeof *d);
    if (!d) return NULL;
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}
const char *hle_os_readdir(hle_os_dir *d, int *is_dir) {
    if (!d) return NULL;
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->first = 0;
    if (is_dir) *is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return d->fd.cFileName;
}
void hle_os_closedir(hle_os_dir *d) {
    if (!d) return;
    FindClose(d->h);
    free(d);
}

/* ---- time ---- */

struct tm *hle_os_localtime(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }

/* UTC conversions are done with plain date arithmetic, not gmtime_s and
 * _mkgmtime, because those fail for any date before 1970, and guests use
 * earlier ones (a FILETIME of 0 is 1601-01-01).
 * The arithmetic is Howard Hinnant's days_from_civil and civil_from_days
 * (https://howardhinnant.github.io/date_algorithms.html), which convert
 * between a calendar date and days since 1970-01-01. */
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
struct tm *hle_os_gmtime(const time_t *t, struct tm *out) {
    int64_t days = *t / 86400, secs = *t % 86400;
    if (secs < 0) secs += 86400, days--;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    int64_t y = yoe + era * 400 + (m <= 2);
    memset(out, 0, sizeof *out);
    out->tm_year = (int)(y - 1900), out->tm_mon = m - 1, out->tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->tm_hour = (int)(secs / 3600), out->tm_min = (int)(secs / 60 % 60), out->tm_sec = (int)(secs % 60);
    out->tm_wday = (int)(((days + 4) % 7 + 7) % 7);         /* 1970-01-01 was a Thursday */
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
    return out;
}
/* Works like POSIX timegm: fields out of range carry over (January 32nd is
 * February 1st), and t is rewritten with them normalized. */
time_t hle_os_timegm(struct tm *t) {
    int64_t mon = t->tm_mon, y = (int64_t)t->tm_year + 1900 + (mon >= 0 ? mon / 12 : (mon - 11) / 12);
    mon -= (y - 1900 - t->tm_year) * 12;
    int64_t s = (days_from_civil(y, (int)mon + 1, 1) + t->tm_mday - 1) * 86400
              + (int64_t)t->tm_hour * 3600 + (int64_t)t->tm_min * 60 + t->tm_sec;
    time_t r = (time_t)s;
    hle_os_gmtime(&r, t);
    return r;
}
long hle_os_utc_offset(void) {
    time_t now = time(NULL);
    struct tm t;
    if (localtime_s(&t, &now)) return 0;
    /* Read the local time's fields as if they were UTC: the difference from
     * the real time is how far local time is ahead of UTC. */
    return (long)(hle_os_timegm(&t) - now);
}

/* Windows' stat only has whole seconds, so the nanoseconds are always 0. */
void hle_os_stat_times(const struct stat *st, struct timespec *mtime, struct timespec *atime) {
    if (mtime) mtime->tv_sec = st->st_mtime, mtime->tv_nsec = 0;
    if (atime) atime->tv_sec = st->st_atime, atime->tv_nsec = 0;
}
/* A Unix time as a FILETIME: 100-nanosecond steps since 1601-01-01, which is
 * 11644473600 seconds before 1970-01-01. */
static FILETIME filetime_of(const struct timespec *t) {
    uint64_t ft = ((uint64_t)t->tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t->tv_nsec / 100u;
    FILETIME f = { (DWORD)ft, (DWORD)(ft >> 32) };
    return f;
}
int hle_os_set_file_times(int fd, const struct timespec *atime, const struct timespec *mtime) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    FILETIME a, m;
    if (atime) a = filetime_of(atime);
    if (mtime) m = filetime_of(mtime);
    return SetFileTime(h, NULL, atime ? &a : NULL, mtime ? &m : NULL) ? 0 : -1;
}

/* ---- CPUs ---- */

/* ALL_PROCESSOR_GROUPS: Windows splits machines with more than 64 logical
 * CPUs into groups, and this counts every group, not just the program's own. */
int hle_os_cpu_count(void) { return (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); }

/* ---- fonts ---- */

/* Windows ships the fonts themselves, in its Fonts folder: arial.ttf,
 * times.ttf and cour.ttf, with bd, i or bi added for bold, italic or both
 * (like arialbd.ttf). */
const char *hle_os_font_file(int kind, int bold, int italic, char *path, size_t n) {
    char fonts[MAX_PATH];
    UINT l = GetWindowsDirectoryA(fonts, sizeof fonts);
    if (!l || l >= sizeof fonts) return NULL;
    const char *base = kind == HLE_OS_FONT_SERIF ? "times" : kind == HLE_OS_FONT_MONO ? "cour" : "arial";
    snprintf(path, n, "%s/Fonts/%s%s.ttf", fonts, base, bold && italic ? "bi" : bold ? "bd" : italic ? "i" : "");
    forward_slashes(path);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? path : NULL;
}

/* ---- shared libraries ---- */

/* LoadLibrary searches the program's own folder, the system folders, the
 * current directory and PATH, in that order. */
void *hle_os_dlopen(const char *const *names, int count) {
    HMODULE lib = NULL;
    for (int i = 0; i < count && !lib; i++) lib = LoadLibraryA(names[i]);
    return (void *)lib;
}
void *hle_os_dlsym(void *lib, const char *name) { return (void *)(uintptr_t)GetProcAddress((HMODULE)lib, name); }

#endif /* _WIN32 */
