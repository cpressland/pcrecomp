/*
 * eh32_selftest.c - a guest C++ throw caught two frames up, headless.
 *
 * Synthetic "lifted" functions written as generate.py writes them, over
 * MSVC's EH data laid out in guest memory as a compiler leaves it:
 *
 *   catcher (try { middle(); } catch (Other) {} catch (Base &b) { ... })
 *     -> middle (a local with a destructor, no try)
 *        -> thrower (throw Derived(0x1234), Derived : Base)
 *
 * The throw must skip the catch of Other, take the catch of Base by the
 * name in Derived's CatchableTypeArray, run middle's destructor on the way,
 * bind the object into the catch's variable, run the catch funclet with the
 * catcher's ebp, and land at the label the funclet returns, on the stack the
 * catcher's prologue saved, with fs:[0] back to the catcher's record. Twice,
 * so the per-thread frame list is left as it was found.
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"

static int fails;
static void ok(const char *w, int c) { if (!c) { printf("FAIL %s\n", w); fails++; } }

#define CATCHER  0x00401000u
#define RESUME   0x00401050u
#define FUNCLET  0x00401100u
#define MIDDLE   0x00401200u
#define DTOR     0x00401300u
#define THROWER  0x00401400u

#define D        0x00500000u            /* the EH data, as in .rdata */
#define TD_BASE  (D + 0x100)
#define TD_OTHER (D + 0x140)
#define TD_DER   (D + 0x180)
#define CT_DER   (D + 0x200)
#define CT_BASE  (D + 0x240)
#define CTA      (D + 0x280)
#define TINFO    (D + 0x2C0)
#define FI1      (D + 0x300)
#define UM1      (D + 0x340)
#define TB1      (D + 0x360)
#define HA1      (D + 0x380)
#define H1       (D + 0x400)
#define FI2      (D + 0x500)
#define UM2      (D + 0x540)
#define H2       (D + 0x580)
#define OBJ      (D + 0x800)            /* the thrown object */

#define LOCALS \
    int _fpu_cmp = 0; uint32_t _cf = 0; int _df = 1; uint32_t _flag_k = FK_NONE; \
    uint32_t _flag_a = 0, _flag_b = 0; uint32_t _itail_tgt = 0; \
    (void)_fpu_cmp; (void)_cf; (void)_df; (void)_flag_a; (void)_flag_b

static uint32_t VA_throw;
static int g_dtor_ran, g_resumed, g_after_throw, g_after_middle;
static uint32_t g_caught, g_dtor_ebp, g_middle_ebp, g_resume_eax, g_resume_esp, g_saved_esp;

/* push ebp; mov ebp, esp; push -1; push handler; push fs:[0]; mov fs:[0], esp;
 * sub esp, 0x10; mov [ebp-0x10], esp */
#define EH_PROLOGUE(handler) do { \
    PUSH32(esp, ebp); ebp = esp; PUSH32(esp, 0xFFFFFFFFu); PUSH32(esp, handler); \
    eax = MEM32(FS_BASE); PUSH32(esp, eax); MEM32(FS_BASE) = esp; \
    esp -= 0x10; MEM32(ebp - 0x10) = esp; } while (0)
/* mov fs:[0], [ebp-0xC]; mov esp, ebp; pop ebp; ret */
#define EH_EPILOGUE() do { \
    MEM32(FS_BASE) = MEM32(ebp - 0xC); esp = ebp; ebp = MEM32(esp); esp += 4; esp += 4; } while (0)

static void sub_middle(void);
static void sub_thrower(void);

static void sub_catcher(void) {
    LOCALS;
    RECOMP_ENTER(CATCHER);
    RECOMP_EH_ENTER();
    EH_PROLOGUE(H1);
    g_saved_esp = esp;
    MEM32(ebp - 4) = 0;                       /* try { */
    RECOMP_CALL(sub_middle);
    g_after_middle = 1;                       /* not reached: middle() throws */
    MEM32(ebp - 4) = 0xFFFFFFFFu;
    goto out;
L_00401050:                                   /* after the catch */
    g_resumed++;
    g_resume_eax = eax;
    g_resume_esp = esp;
out:
    EH_EPILOGUE();
    eax = 7;
    return;
_ljump:
    switch (_itail_tgt) { case RESUME: goto L_00401050; }
    abort();
}

static void sub_funclet(void) {               /* catch (Base &b): b is at [ebp-0x14] */
    LOCALS;
    RECOMP_ENTER(FUNCLET);
    g_caught = MEM32(ebp - 0x14);
    eax = RESUME;
    esp += 4;
    (void)_itail_tgt; (void)_flag_k;
}

static void sub_middle(void) {
    LOCALS;
    RECOMP_ENTER(MIDDLE);
    EH_PROLOGUE(H2);
    g_middle_ebp = ebp;
    MEM32(ebp - 4) = 0;                       /* the local is constructed */
    RECOMP_CALL(sub_thrower);
    g_after_throw = 1;                        /* not reached */
    EH_EPILOGUE();
    (void)_itail_tgt; (void)_flag_k;
}

static void sub_dtor(void) {                  /* middle's unwind action */
    LOCALS;
    RECOMP_ENTER(DTOR);
    g_dtor_ran++;
    g_dtor_ebp = ebp;
    esp += 4;
    (void)_itail_tgt; (void)_flag_k;
}

static void sub_thrower(void) {
    LOCALS;
    RECOMP_ENTER(THROWER);
    MEM32(OBJ) = 0x1234;
    PUSH32(esp, TINFO);
    PUSH32(esp, OBJ);
    RECOMP_ICALL(VA_throw);
    g_after_throw = 1;                        /* not reached */
    esp += 4;
    (void)_itail_tgt; (void)_flag_k;
}

const recomp_dispatch_entry_t recomp_dispatch_table[] = {
    { CATCHER, sub_catcher }, { FUNCLET, sub_funclet }, { MIDDLE, sub_middle },
    { DTOR, sub_dtor }, { THROWER, sub_thrower },
};
const uint32_t recomp_dispatch_count = 5;

static void w32(uint32_t va, const uint32_t *v, int n) { for (int i = 0; i < n; i++) MEM32(va + 4 * i) = v[i]; }
static void type_desc(uint32_t va, const char *name) { MEM32(va) = 0; MEM32(va + 4) = 0; strcpy((char *)hle_ptr(va + 8), name); }
static void stub(uint32_t va, uint32_t fi) {        /* mov eax, fi; jmp rel32 */
    uint8_t *p = (uint8_t *)hle_ptr(va);
    p[0] = 0xB8; memcpy(p + 1, &fi, 4); p[5] = 0xE9; memset(p + 6, 0, 4);
}

int main(void) {
    win32hle_register(win32hle_kernel32);
    win32hle_register(win32hle_eh32);
    VA_throw = hle_resolve("_CxxThrowException");
    ok("_CxxThrowException resolves", VA_throw != 0);
    ok("the EH data's page", hle_vm_reserve(D, 0x1000) == D);
    ok("an unreserved page is not readable", !hle_vm_readable(D + 0x10000, 4));
    ok("a reserved one is", hle_vm_readable(D + 0x10, 4));

    type_desc(TD_BASE, ".?AVBase@@");
    type_desc(TD_OTHER, ".?AVOther@@");
    type_desc(TD_DER, ".?AVDerived@@");
    w32(CT_DER, (uint32_t[]){ 1, TD_DER, 0, 0xFFFFFFFFu, 0, 4, 0 }, 7);
    w32(CT_BASE, (uint32_t[]){ 1, TD_BASE, 0, 0xFFFFFFFFu, 0, 4, 0 }, 7);
    w32(CTA, (uint32_t[]){ 2, CT_DER, CT_BASE }, 3);
    w32(TINFO, (uint32_t[]){ 0, 0, 0, CTA }, 4);
    /* catcher: one state, one try over it, catch (Other), then catch (Base &) */
    w32(FI1, (uint32_t[]){ 0x19930521u, 1, UM1, 1, TB1 }, 5);
    w32(UM1, (uint32_t[]){ 0xFFFFFFFFu, 0 }, 2);
    w32(TB1, (uint32_t[]){ 0, 0, 1, 2, HA1 }, 5);
    w32(HA1, (uint32_t[]){ 0, TD_OTHER, 0, 0x00401900u,  8, TD_BASE, (uint32_t)-0x14, FUNCLET }, 8);
    stub(H1, FI1);
    /* middle: state 0 destroys a local (DTOR), no try */
    w32(FI2, (uint32_t[]){ 0x19930521u, 1, UM2, 0, 0 }, 5);
    w32(UM2, (uint32_t[]){ 0xFFFFFFFFu, DTOR }, 2);
    stub(H2, FI2);

    for (int round = 1; round <= 2; round++) {
        mach_enter();
        uint32_t fs_before = MEM32(FS_BASE);
        mach_leave();
        uint32_t r = hle_call_guest(CATCHER, 0, NULL);
        mach_enter();
        uint32_t fs_after = MEM32(FS_BASE);
        mach_leave();
        ok("the catcher returned normally", r == 7);
        ok("resumed after the catch", g_resumed == round);
        ok("the funclet's return address is where it went on", g_resume_eax == RESUME);
        ok("on the stack its prologue saved", g_resume_esp == g_saved_esp);
        ok("the catch got the object, by reference", g_caught == OBJ);
        ok("the middle frame's destructor ran once", g_dtor_ran == round);
        ok("with the middle frame's ebp", g_dtor_ebp == g_middle_ebp);
        ok("nothing ran past the throw", !g_after_throw && !g_after_middle);
        ok("fs:[0] is as it was", fs_after == fs_before);
    }

    if (fails == 0) printf("eh32_selftest: all checks passed\n");
    return fails != 0;
}
