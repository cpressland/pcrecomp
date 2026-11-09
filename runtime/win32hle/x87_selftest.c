/*
 * x87_selftest.c - the lifted x87 stack: pushes, pops, fxch and the
 * fild-qword shadow behave alike with the fixed eight registers and with
 * RECOMP_X87_WINDOW's sliding window, including across the window's
 * recentring at either end of its ring.
 */
#include <stdio.h>
#include <stdint.h>
#include "win32hle.h"
#include "recomp_types.h"

#ifdef RECOMP_X87_WINDOW
#define ST(i)  g_stp[i]
#define STI(i) g_stip[i]
#else
#define ST(i)  g_st[i]
#define STI(i) g_st_i64[i]
#endif
#ifdef RECOMP_X87_WINDOW
#define PUSH(v) fp_push_impl(0, &g_fp_top, (v))
#define POP()   fp_pop_impl(0, &g_fp_top)
#else
#define PUSH(v) fp_push_impl(g_st, &g_fp_top, (v))
#define POP()   fp_pop_impl(g_st, &g_fp_top)
#endif

/* No lifted program: an empty dispatch table for the runtime to link. */
const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    int top = g_fp_top;

    /* eight deep, read back in order */
    for (int i = 0; i < 8; i++) PUSH(i + 0.5);
    for (int i = 0; i < 8; i++) CHECK(ST(i) == 7 - i + 0.5);
    CHECK(g_fp_top == top + 8);
    for (int i = 7; i >= 0; i--) CHECK(POP() == i + 0.5);
    CHECK(g_fp_top == top);

    /* many pushes then pops, and many pops then pushes, as unbalanced lifted
     * code can leave it: the window crosses both ends of its ring (64)
     * several times. The x87 has eight registers, so only the top eight of
     * a deep push come back. */
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 200; i++) {
            PUSH((double)i);
            CHECK(ST(0) == (double)i);
            if (i >= 7) CHECK(ST(7) == (double)(i - 7));
        }
        for (int i = 199; i >= 192; i--) CHECK(POP() == (double)i);
        for (int i = 0; i < 342; i++) (void)POP();
        for (int i = 0; i < 8; i++) PUSH(i * 2.0);
        for (int i = 0; i < 8; i++) CHECK(ST(i) == (7 - i) * 2.0);
        for (int i = 0; i < 8; i++) (void)POP();
    }

    /* fxch keeps each value's shadow with it */
    PUSH(1.0); STI(0) = 111;
    PUSH(2.0); STI(0) = 222;
    {
        double t = ST(0); ST(0) = ST(1); ST(1) = t;
        int64_t ti = STI(0); STI(0) = STI(1); STI(1) = ti;
    }
    CHECK(ST(0) == 1.0 && STI(0) == 111);
    CHECK(ST(1) == 2.0 && STI(1) == 222);
    /* a push clears the new slot's shadow; the one below keeps its own */
    PUSH(3.0);
    CHECK(STI(0) == 0 && STI(1) == 111 && STI(2) == 222);
    (void)POP(); (void)POP(); (void)POP();

    if (fails) { fprintf(stderr, "x87_selftest: %d failed\n", fails); return 1; }
    printf("x87_selftest: ok\n");
    return 0;
}
