/* Synthetic drive-root and overlay lexical-normalization tests. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "win32hle.h"

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
    char tmp[] = "/tmp/hle-path-root-XXXXXX";
    char *base = mkdtemp(tmp);
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
    if (mkdir(game, 0700) || mkdir(dir, 0700) || mkdir(mod, 0700) || mkdir(outside, 0700) ||
        !put_file(game_file, "base") || !put_file(mod_file, "mod") ||
        !put_file(child_file, "child") || !put_file(outside_file, "outside")) {
        perror("synthetic path fixture");
        return 2;
    }

    char mapped[1024], expected[1024], path[1024], overlay_path[1024];
    char *canonical_game = realpath(game, NULL), *canonical_mod = realpath(mod, NULL);
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

    hle_set_overlay(NULL, NULL);
    hle_set_drive('C', NULL);
    unlink(game_file); unlink(mod_file); unlink(child_file); unlink(outside_file);
    rmdir(dir); rmdir(game); rmdir(mod); rmdir(outside); rmdir(base);
    free(canonical_game); free(canonical_mod);
    if (!fails) puts("hle path root selftest: PASS");
    return fails != 0;
}
