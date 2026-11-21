/*
 * hle_os.h - win32hle's host operating system interface.
 *
 * win32hle handles a guest's Win32 calls with libc and SDL2.
 * Some things that libc leaves to the OS, such as reserving and protecting
 * pages, canonical paths, listing a folder, disk space, and loading a library,
 * go through here so the rest of the layer can remain platform independent.
 *
 * hle_os_posix.c handles Linux, macOS and the browser (Emscripten).
 * hle_os_win32.c handles Windows.
 *
 * Threads, clocks, sleeps, and plain file calls (open, read, stat, unlink,
 * rename) don't come through here at all; they're the same on every host thanks
 * to the CRT and [win]pthreads.
 *
 * Host paths: on Windows they are canonical with forward slashes,
 * for instance `D:/games/tpw`, which makes splitting on '/' work everywhere.
 * Only a path's root differs (see hle_os_root_len).
 */
#ifndef HLE_OS_H
#define HLE_OS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>

/* ---- memory ---- */
/* The host's page size: 4 KB on x86, 16 KB on Apple Silicon. */
size_t hle_os_page_size(void);

/* Based mode: We reserve one 4GB block of address space for the guest to use.
 * Sections of it are made available to the guest as it asks for memory, and
 * released again when it's done. */

/* Reserve size bytes, inaccessible until hle_os_commit(...).
 *
 * When at is NULL, the reservation is made wherever it fits.
 *
 * When at contains an address, try to place the reservation there.
 * If it can't be placed there, e.g. something's already mapped there,
 * return NULL. */
void *hle_os_reserve(void *at, size_t size);
/* Zero out [p, p+size) in the reservation and make it readable & writable.
 * If already committed, zeroes it again.
 * p and p+size must be multiples of page size.
 * Return 0 on success. */
int hle_os_commit(void *p, size_t size);
/* Make [p, p+size) inaccessible and give its memory back to the OS (though it
 * stays reserved.)
 * p and p+size must be multiples of page size.
 * Return 0 on success. */
int hle_os_decommit(void *p, size_t size);

/* Flat mode: on a 32-bit host, a guest address is a host address. */

/* Map size bytes of zeroed, readable & writable memory.
 *
 * When at is NULL, the mapping is made wherever it fits.
 *
 * When at contains an address, try to place the mapping there.
 * If it can't be placed there (e.g. something's already mapped there),
 * return NULL. */
void *hle_os_map(void *at, size_t size);
/* Unmap [p, p+size), giving its memory back to the OS.
 * p and size must be exactly as mapped by hle_os_map */
void hle_os_unmap(void *p, size_t size);
/* Zero out [p, p+size), letting the OS drop its memory where it can.
 * It stays mapped, readable & writable. */
void hle_os_discard(void *p, size_t size);

/* Put len bytes of the file fd, from offset off, at [p, p+len).
 * p must already be committed, and p and off must be multiples of page size.
 *
 * When copy is 0, writes to memory reach the file (if writable).
 * When copy is 1, writes stay in memory (copy-on-write).
 *
 * On Windows and in the browser, a file can't be put at an address, so the
 * bytes are always copied in, and writes never reach the file.
 * Return 0 on success. */
int hle_os_map_file(void *p, size_t len, int fd, uint64_t off, int writable, int copy);

/* ---- files and paths ---- */
/* Flags passed to open().
 * O_NOINHERIT is the Windows equivalent of O_CLOEXEC.
 * Additionally, the CRT on Windows opens text mode by default, so we add O_BINARY. */
#ifdef _WIN32
#define HLE_O_HOST (O_BINARY | O_NOINHERIT)
#else
#define HLE_O_HOST O_CLOEXEC
#endif
/* The full absolute path of path, with "..", "." and symlinks resolved.
 * Returns NULL if path doesn't exist.
 * The result must be freed by the caller. */
char *hle_os_realpath(const char *path);
/* The current directory, as hle_os_realpath would give it, written to buf.
 * Returns buf, or NULL on error (e.g. it doesn't fit in n bytes). */
char *hle_os_getcwd(char *buf, size_t n);
/* Create the folder path. Its parent must already exist.
 * Returns 0 on success, else -1 with errno set. */
int hle_os_mkdir(const char *path);
/* The path of this program's own executable, as hle_os_realpath would give
 * it, written to out.
 * Returns 1 on success, 0 if it can't be found or doesn't fit in n bytes. */
int hle_os_exe_path(char *out, size_t n);
/* Read up to n bytes of fd from offset off into buf, leaving fd's position
 * where it was.
 * On Windows the position is moved and put back, so fd mustn't be used by
 * another thread at the same time.
 * Returns the number of bytes read, 0 at the end of the file, -1 on error. */
ssize_t hle_os_pread(int fd, void *buf, size_t n, int64_t off);
/* The space on the drive holding path, in bytes: available to this user,
 * total, and free overall (which can be more than available, e.g. space
 * kept back for root on Linux).
 * Returns 0 on success, -1 with errno set. */
int hle_os_disk_space(const char *path, uint64_t *avail, uint64_t *total, uint64_t *free_bytes);

/* The length of a host path's root, or 0 if it has none (it's relative).
 *   POSIX:   "/home" -> 1
 *   Windows: "C:/Users" or "C:\Users" -> 3
 *            "/Users" or "\Users" -> 1 (the current drive's root)
 *            "C:Users" -> 2 (relative to C:'s current directory, so not
 *            absolute: see hle_os_path_is_abs)
 * Network paths ("//server/share/...") aren't understood: they get 1, as if
 * "/server" were a folder. */
static inline size_t hle_os_root_len(const char *p) {
#ifdef _WIN32
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') return p[2] == '/' || p[2] == '\\' ? 3 : 2;
    return p[0] == '/' || p[0] == '\\';
#else
    return p[0] == '/';
#endif
}
/* Whether a host path is absolute: it has a root, and isn't relative to a
 * drive's current directory (like C:Users). */
static inline int hle_os_path_is_abs(const char *p) {
    size_t r = hle_os_root_len(p);
    return r && !(r == 2 && p[1] == ':');
}
/* Whether a path is a root and nothing more (like / or C:/). */
static inline int hle_os_is_root(const char *p) { return *p && hle_os_root_len(p) == strlen(p); }

/* List a folder's entries: open it, read one name per call until NULL, then
 * close it. The name stays valid until the next read or the close.
 * "." and ".." are included, except at a Windows drive root (as on Windows).
 * If is_dir isn't NULL, it's set to 1 for a folder, 0 for anything else, or
 * -1 when the OS doesn't say (some Linux filesystems): stat the entry then. */
typedef struct hle_os_dir hle_os_dir;
hle_os_dir *hle_os_opendir(const char *path);
const char *hle_os_readdir(hle_os_dir *d, int *is_dir);
void hle_os_closedir(hle_os_dir *d);

/* ---- time ---- */
/* Split *t into calendar fields in out, as local time.
 * Returns out, or NULL if the time can't be converted. */
struct tm *hle_os_localtime(const time_t *t, struct tm *out);
/* Split *t into calendar fields in out, as UTC. Works for any date, including
 * before 1970 (a FILETIME of 0 is 1601).
 * Returns out. */
struct tm *hle_os_gmtime(const time_t *t, struct tm *out);
/* The reverse of hle_os_gmtime: calendar fields in UTC to a time_t.
 * Fields out of range carry over (January 32nd is February 1st), and t is
 * rewritten with them normalized. */
time_t hle_os_timegm(struct tm *t);
/* How far local time is ahead of UTC right now, in seconds, including summer
 * time (e.g. +3600 in the UK in summer, -18000 in New York in winter). */
long hle_os_utc_offset(void);

/* A file's last write time (mtime) and last access time (atime), taken from
 * its stat. Either can be NULL if not wanted.
 * Linux and macOS give nanoseconds; Windows' stat only has whole seconds. */
struct stat;
void hle_os_stat_times(const struct stat *st, struct timespec *mtime, struct timespec *atime);
/* Set an open file's last access time (atime) and last write time (mtime).
 * NULL leaves that time as it is.
 * On Windows, fd must be open for writing.
 * Returns 0 on success, -1 on failure. */
int hle_os_set_file_times(int fd, const struct timespec *atime, const struct timespec *mtime);

/* ---- CPUs ---- */
/* The number of logical CPUs (hardware threads) online, or less than 1 if
 * the OS won't say. */
int hle_os_cpu_count(void);

/* ---- fonts ---- */
/* Find a font file on the host to draw a guest's text with, written to path.
 * kind is sans (for Arial), serif (for Times New Roman) or mono (for Courier
 * New).
 *   Windows: Arial/Times New Roman/Courier New from %windir%/Fonts
 *   macOS:   The same fonts as windows, from /System/Library/Fonts/Supplemental
 *            If fontconfig is installed, the closest match to Liberation is used instead.
 *   Linux:   Liberation Sans, Serif or Mono via fontconfig or the usual folders.
 *            Failing that, DejaVu Sans for everything.
 * Returns path, or NULL if nothing was found. */
enum { HLE_OS_FONT_SANS, HLE_OS_FONT_SERIF, HLE_OS_FONT_MONO };
const char *hle_os_font_file(int kind, int bold, int italic, char *path, size_t n);

/* ---- shared libraries ---- */
/* Load a shared library (.so or .dll), trying each of names in turn until one
 * loads successfully.
 * Returns the library, or NULL if none of them load.
 * hle_os_dlsym finds a function in it by name, or NULL. */
#ifdef _WIN32
void *hle_os_dlopen(const char *const *names, int count);
void *hle_os_dlsym(void *lib, const char *name);
#define HLE_OS_VULKAN_LIBS "vulkan-1.dll"
#else
/* Declared inline so that only programs that need them (e.g. builds using the
 * Vulkan renderer on glibc < 2.34) require -ldl. */
#include <dlfcn.h>
static inline void *hle_os_dlopen(const char *const *names, int count) {
    void *lib = NULL;
    for (int i = 0; i < count && !lib; i++) lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    return lib;
}
static inline void *hle_os_dlsym(void *lib, const char *name) { return dlsym(lib, name); }
#define HLE_OS_VULKAN_LIBS "libvulkan.so.1", "libvulkan.so"
#endif

#endif
