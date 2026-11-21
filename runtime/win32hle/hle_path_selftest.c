/* Synthetic drive-root and overlay lexical-normalization tests. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "selftest_os.h"

/* No guest code: the layer links against an empty dispatch table. */
const recomp_dispatch_entry_t recomp_dispatch_table[] = {{0, 0}};
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(label, condition) do { \
    if (!(condition)) { fprintf(stderr, "FAIL %s\n", label); ++fails; } \
} while (0)

static int put_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

int main(void) {
    char tmp[600];
    test_path(tmp, sizeof tmp, "hle-path-root-XXXXXX");
    char *base = test_mkdtemp(tmp);
    if (!base) { perror("mkdtemp"); return 2; }
    char game[512], dir[512], mod[512], outside[512];
    char game_file[600], mod_file[600], child_file[600], outside_file[600];
    snprintf(game, sizeof game, "%s/game", base);
    snprintf(dir, sizeof dir, "%s/dir", game);
    snprintf(mod, sizeof mod, "%s/mod", base);
    snprintf(outside, sizeof outside, "%s/outside", base);
    snprintf(game_file, sizeof game_file, "%s/file.txt", game);
    snprintf(mod_file, sizeof mod_file, "%s/file.txt", mod);
    snprintf(child_file, sizeof child_file, "%s/child.txt", dir);
    snprintf(outside_file, sizeof outside_file, "%s/outside.txt", outside);
    if (hle_os_mkdir(game) || hle_os_mkdir(dir) || hle_os_mkdir(mod) || hle_os_mkdir(outside) ||
        !put_file(game_file, "base") || !put_file(mod_file, "mod") ||
        !put_file(child_file, "child") || !put_file(outside_file, "outside")) {
        perror("synthetic path fixture");
        return 2;
    }

    char mapped[1024], expected[1024], path[1024], overlay_path[1024];
    char *canonical_game = hle_os_realpath(game), *canonical_mod = hle_os_realpath(mod);
    if (!canonical_game || !canonical_mod) { free(canonical_game); free(canonical_mod); return 2; }
    snprintf(expected, sizeof expected, "%s/file.txt", canonical_game);
    snprintf(mod_file, sizeof mod_file, "%s/file.txt", canonical_mod);
    hle_set_drive('C', game);
    hle_set_overlay(game, mod);

    CHECK("single parent clamps at mapped drive root",
          hle_host_path_base("C:\\..\\file.txt", mapped, sizeof mapped) &&
          !strcmp(mapped, expected));
    CHECK("repeated parent clamps at mapped drive root",
          hle_host_path_base("C:\\..\\..\\file.txt", mapped, sizeof mapped) &&
          !strcmp(mapped, expected));
    CHECK("mixed separators keep drive-root anchor",
          hle_host_path_base("C:/../file.txt", mapped, sizeof mapped) &&
          !strcmp(mapped, expected));
    CHECK("normal in-root parent resolves",
          hle_host_path_base("C:\\dir\\..\\file.txt", mapped, sizeof mapped) &&
          !strcmp(mapped, expected));
    CHECK("missing component canceled by parent is normalized before existence",
          hle_host_path_base("C:\\missing\\..\\file.txt", mapped, sizeof mapped) &&
          !strcmp(mapped, expected));
    CHECK("parent chain cannot climb above drive root",
          hle_host_path_base("C:\\dir\\..\\..\\file.txt", mapped, sizeof mapped) &&
          !strcmp(mapped, expected));

    CHECK("drive path still selects overlay file",
          hle_host_path("C:\\..\\file.txt", path, sizeof path) &&
          !strcmp(path, mod_file));
    snprintf(path, sizeof path, "%s/../outside/outside.txt", canonical_game);
    CHECK("overlay classification rejects lexical path outside its base",
          hle_overlay_of(path, overlay_path, sizeof overlay_path) == -1);
    snprintf(path, sizeof path, "%s/dir/../file.txt", canonical_game);
    CHECK("overlay accepts normalized path still under base",
          hle_overlay_of(path, overlay_path, sizeof overlay_path) == 1 &&
          !strcmp(overlay_path, mod_file));

    char cwd[1024];
    int got_cwd = getcwd(cwd, sizeof cwd) != NULL;
    CHECK("save cwd for relative-path case", got_cwd);
    if (got_cwd && chdir(dir) == 0) {
        CHECK("undriven relative parent remains relative to cwd",
              hle_host_path_base("..\\file.txt", mapped, sizeof mapped) &&
              !strcmp(mapped, "../file.txt"));
        chdir(cwd);
    }

    /* host -> guest -> host: under a mapped drive, through Z:, and under a
     * drive mapped to a host root (like / or C:/) */
    {
        char guest[1024], back[1024], *real_outside = hle_os_realpath(outside_file);
        CHECK("get outside file's host path", real_outside != NULL);
        hle_guest_path(expected, guest, sizeof guest);
        CHECK("file under C: maps to C:\\file.txt", !strcmp(guest, "C:\\file.txt"));
        if (real_outside) {
            hle_guest_path(real_outside, guest, sizeof guest);
#ifdef _WIN32
            char z_prefix[8];
            snprintf(z_prefix, sizeof z_prefix, "Z:\\%c", real_outside[0]);
            CHECK("file outside all drives maps to Z:\\<drive>\\...", !strncmp(guest, z_prefix, 3 + 1) && guest[4] == '\\');
#else
            CHECK("file outside all drives maps to Z:\\...", !strncmp(guest, "Z:\\", 3));
#endif
            CHECK("Z: path maps back to original host path", hle_host_path_base(guest, back, sizeof back) && !strcmp(back, real_outside));

            size_t rl = hle_os_root_len(real_outside);
            char host_root[8];
            snprintf(host_root, sizeof host_root, "%.*s", (int)rl, real_outside);
            hle_set_drive('Y', host_root);
            hle_guest_path(real_outside, guest, sizeof guest);
            CHECK("with Y: mapped to the file's host root, the file maps to Y:\\...", !strncmp(guest, "Y:\\", 3) && guest[3] != '\\');
            CHECK("Y: path maps back to original host path", hle_host_path_base(guest, back, sizeof back) && !strcmp(back, real_outside));
            hle_set_drive('Y', NULL);
        }
        CHECK("Z:\\ alone goes nowhere on Windows (needs a drive directory), but maps to / elsewhere",
#ifdef _WIN32
              !hle_host_path_base("Z:\\", back, sizeof back)
#else
              hle_host_path_base("Z:\\", back, sizeof back) && !strcmp(back, "/")
#endif
              );
        free(real_outside);
    }

    hle_set_overlay(NULL, NULL);
    hle_set_drive('C', NULL);
    unlink(game_file); unlink(mod_file); unlink(child_file); unlink(outside_file);
    rmdir(dir); rmdir(game); rmdir(mod); rmdir(outside); rmdir(base);
    free(canonical_game); free(canonical_mod);
    if (!fails) puts("hle path root selftest: PASS");
    return fails != 0;
}
