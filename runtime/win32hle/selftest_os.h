/*
 * selftest_os.h - host helpers for the selftests only, beyond what hle_os.h
 * offers: a scratch folder for test files, and stand-ins for the few POSIX
 * calls Windows' C runtime lacks (like mkdtemp and setenv). win32hle itself
 * never uses this file.
 */
#ifndef SELFTEST_OS_H
#define SELFTEST_OS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hle_os.h"
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#endif

/* The folder the tests put their scratch files in.
 *   Linux, macOS: $TMPDIR, or /tmp if that isn't set.
 *   Windows:      %TEMP%, or %TMP%, or the current directory if neither is set.
 * Returned with forward slashes and no slash at the end (except a root's
 * own, like C:/). */
static const char *test_tmp(void) {
    static char dir[512];
    if (!dir[0]) {
#ifdef _WIN32
        const char *t = getenv("TEMP");
        if (!t || !*t) t = getenv("TMP");
        if (!t || !*t) t = ".";
#else
        const char *t = getenv("TMPDIR");
        if (!t || !*t) t = "/tmp";
#endif
        snprintf(dir, sizeof dir, "%s", t);
        for (char *s = dir; *s; s++) if (*s == '\\') *s = '/';
        size_t l = strlen(dir);
        while (l > hle_os_root_len(dir) && dir[l - 1] == '/') dir[--l] = 0;
    }
    return dir;
}
/* The path of the file name in the scratch folder, written to out.
 * Returns out. */
static const char *test_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", test_tmp(), name);
    return out;
}
/* Create a new folder with a unique name, like POSIX mkdtemp. tmpl is the
 * path to use, ending in XXXXXX, which is replaced in place with characters
 * that make it unique.
 * Returns tmpl, or NULL on failure. */
static char *test_mkdtemp(char *tmpl) {
#ifdef _WIN32
    if (_mktemp_s(tmpl, strlen(tmpl) + 1) || _mkdir(tmpl)) return NULL;
    return tmpl;
#else
    return mkdtemp(tmpl);
#endif
}
/* Set the environment variable name to value, replacing any existing value.
 * Returns 0 on success. */
static int test_setenv(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value);
#else
    return setenv(name, value, 1);
#endif
}
/* Find needle in hay, ignoring case, like the GNU/BSD strcasestr (which
 * Windows lacks).
 * Returns where it starts in hay, or NULL if it isn't there. */
static const char *test_strcasestr(const char *hay, const char *needle) {
    size_t l = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, l)) return hay;
    return NULL;
}
/* Delete the folder path and everything in it, like rm -r. Does nothing if it
 * doesn't exist. */
static void test_rmtree(const char *path) {
    hle_os_dir *d = hle_os_opendir(path);
    const char *e;
    int is_dir;
    char full[1024];
    while (d && (e = hle_os_readdir(d, &is_dir))) {
        if (!strcmp(e, ".") || !strcmp(e, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", path, e);
        if (is_dir < 0) { struct stat st; is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode); }
        if (is_dir) test_rmtree(full);
        else remove(full);
    }
    hle_os_closedir(d);
    rmdir(path);
}
/* Create the folder path, along with any missing parents, like mkdir -p.
 * Returns 0 if the folder exists afterwards, -1 if not. */
static int test_mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + hle_os_root_len(p) + 1; *s; s++)
        if (*s == '/') { *s = 0; hle_os_mkdir(p); *s = '/'; }
    hle_os_mkdir(p);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

#endif
