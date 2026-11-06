/*
 * eh32.c - guest C++ exceptions (MSVC's frame-based EH) on win32hle.
 *
 * The same scheme as runtime/native32/eh32.c, which has the whole story; in
 * short: a guest `throw` reaches _CxxThrowException, and rather than raise a
 * host exception (which would never find the guest's frames), the throw is
 * dispatched here from guest data, as __CxxFrameHandler would:
 *
 *   1. walk the fs:[0] chain for a C++ frame (its handler a stub
 *      `mov eax, FuncInfo; jmp __CxxFrameHandler`) with a try block covering
 *      its state and a catch matching a type in the ThrowInfo, by decorated
 *      name, since the thrower and the catcher can be different modules;
 *   2. run the unwind funclets of every frame above it, and its own down to
 *      the try block;
 *   3. store the object in the catch's variable and run the catch funclet as
 *      guest code with the frame's ebp; it returns where the function goes on;
 *   4. longjmp into the lifted body that owns the frame, whose landing pad
 *      (RECOMP_EH_ENTER, tools/lift/eh32.py) goes to that label.
 *
 * What differs from native32: guest memory is not host memory (APTR/hle_ptr,
 * a mapped-range check in place of IsBadReadPtr), the frames are per host
 * thread with C11 _Thread_local, and a throw nothing catches stops the run
 * naming its type, where native32 hands it to the real msvcrt.
 *
 * The host answers `_CxxThrowException` with hle_eh_throw: its import for a
 * program bound to win32hle_eh32[], and, when the program's C runtime DLL is
 * lifted with it, that DLL's own export too (a throw inside the runtime, a
 * bad_alloc from operator new, calls it directly).
 *
 * HLE_TRACE_EH=1: one line per throw, and where it lands.
 *
 * ponytail: C++ frames only. A __try/__except (SEH, _except_handler3) frame
 * is passed over, not asked, and its __finally blocks do not run on the way
 * past; RaiseException still ends the run. Add when a title needs them.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"

#define EH_MAGIC_VC6   0x19930520u
#define EH_MAGIC_VC7   0x19930521u
#define EH_MAGIC_VC8   0x19930522u

static _Thread_local recomp_ehframe_t *t_frames;    /* newest first */
/* The exception a running catch funclet holds, for a bare `throw;`. */
static _Thread_local uint32_t t_cur_obj, t_cur_info;

void recomp_eh_push(recomp_ehframe_t *f) { f->prev = t_frames; t_frames = f; }

void recomp_eh_pop(recomp_ehframe_t *f) {
    /* A longjmp past this frame already dropped it. */
    for (recomp_ehframe_t *p = t_frames; p; p = p->prev)
        if (p == f) { t_frames = f->prev; return; }
}

/* Guest setjmp/longjmp: the newest frame entered above a saved esp is the
 * one that called setjmp (native32/eh32.c). */
recomp_ehframe_t *recomp_eh_owner(uint32_t esp) {
    for (recomp_ehframe_t *f = t_frames; f; f = f->prev)
        if (f->entry_esp > esp) return f;
    return NULL;
}

void recomp_eh_resume(recomp_ehframe_t *f, uint32_t resume) {
    t_frames = f;
    f->resume = resume;
    g_cur_func = f->func;
    longjmp(f->jb, 1);
}

/* The FuncInfo a C++ frame's handler stub names, or 0 if it is not one. */
static uint32_t funcinfo(uint32_t handler) {
    if (handler < 0x10000u || !hle_vm_readable(handler, 11)) return 0;
    const uint8_t *h = (const uint8_t *)hle_ptr(handler);
    if (h[0] != 0xB8) return 0;
    if (h[5] != 0xE9 && !(h[5] == 0xFF && h[6] == 0x25)) return 0;
    uint32_t fi = (uint32_t)h[1] | (uint32_t)h[2] << 8 | (uint32_t)h[3] << 16 | (uint32_t)h[4] << 24;
    if (!hle_vm_readable(fi, 20)) return 0;
    uint32_t magic = MEM32(fi);
    return magic == EH_MAGIC_VC6 || magic == EH_MAGIC_VC7 || magic == EH_MAGIC_VC8 ? fi : 0;
}

/* Run guest code at va as MSVC's _CallSettingFrame does: ebp = the frame's,
 * on the current guest stack. Returns eax; every other register is kept. */
static uint32_t call_funclet(uint32_t va, uint32_t frame_ebp) {
    recomp_func_t fn = recomp_lookup(va);
    if (!fn) hle_fatal("[eh32] funclet 0x%08X is not lifted", va);
    uint32_t esp = g_esp, ebp = g_ebp, ebx = g_ebx, esi = g_esi, edi = g_edi, cur = g_cur_func;
    g_ebp = frame_ebp;
    g_esp -= 4;
    MEM32(g_esp) = 0;                   /* return address: the funclet's ret pops it */
    fn();
    uint32_t eax = g_eax;
    g_esp = esp, g_ebp = ebp, g_ebx = ebx, g_esi = esi, g_edi = edi, g_cur_func = cur;
    return eax;
}

/* Destroy a C++ frame's locals from its current state down to `to`. */
static void unwind_to(uint32_t rec, uint32_t fi, int to) {
    int s = (int)MEM32(rec + 8);
    uint32_t map = MEM32(fi + 8);
    int max = (int)MEM32(fi + 4);
    while (s != to && s >= 0 && s < max) {
        int next = (int)MEM32(map + 8 * s);
        uint32_t action = MEM32(map + 8 * s + 4);
        MEM32(rec + 8) = (uint32_t)next;
        if (action) call_funclet(action, rec + 12);
        s = next;
    }
}

static const char *type_name(uint32_t td) {
    return hle_vm_readable(td + 8, 1) ? (const char *)hle_ptr(td + 8) : "?";
}

/* The ThrowInfo's CatchableType that a catch of `type` takes, or 0. */
static uint32_t catchable(uint32_t info, uint32_t type) {
    uint32_t arr = MEM32(info + 12);
    int n = (int)MEM32(arr);
    for (int i = 0; i < n; i++) {
        uint32_t ct = MEM32(arr + 4 + 4 * i);
        uint32_t td = MEM32(ct + 4);
        if (td == type || !strcmp(type_name(td), type_name(type))) return ct;
    }
    return 0;
}

/* Store the thrown object into the catch's variable. */
static void bind_catch_object(uint32_t handler, uint32_t ct, uint32_t obj, uint32_t frame_ebp) {
    uint32_t adj = MEM32(handler), disp = MEM32(handler + 8);
    if (!MEM32(handler + 4) || !disp || !ct) return;       /* catch(...) or no variable */
    uint32_t dst = frame_ebp + disp;
    uint32_t src = obj + MEM32(ct + 8);                     /* this-displacement (mdisp) */
    uint32_t size = MEM32(ct + 20), copy = MEM32(ct + 24);
    if (adj & 8) MEM32(dst) = src;                          /* by reference */
    else if (MEM32(ct) & 1 || !copy) memmove(hle_ptr(dst), hle_ptr(src), size);
    else {                                                  /* copy constructor, thiscall */
        uint32_t ecx = g_ecx;
        g_ecx = dst;
        g_esp -= 4;
        MEM32(g_esp) = src;
        call_funclet(copy, g_ebp);   /* ret 4 pops the argument */
        g_ecx = ecx;
    }
}

static int eh_trace = -1;

static const char *throw_type(uint32_t info) {
    uint32_t arr = info ? MEM32(info + 12) : 0;
    return arr && MEM32(arr) ? type_name(MEM32(MEM32(arr + 4) + 4)) : "?";
}

static void dispatch(uint32_t obj, uint32_t info) {
    uint32_t top = MEM32(g_fs_base);
    for (uint32_t rec = top; rec && rec != 0xFFFFFFFFu && hle_vm_readable(rec, 16); rec = MEM32(rec)) {
        uint32_t fi = funcinfo(MEM32(rec + 4));
        if (!fi) continue;
        int state = (int)MEM32(rec + 8);
        uint32_t tries = MEM32(fi + 16);
        int ntry = (int)MEM32(fi + 12);
        for (int t = 0; t < ntry; t++) {
            uint32_t tb = tries + 20 * t;
            int lo = (int)MEM32(tb), hi = (int)MEM32(tb + 4), ncatch = (int)MEM32(tb + 12);
            if (state < lo || state > hi) continue;
            for (int c = 0; c < ncatch; c++) {
                uint32_t h = MEM32(tb + 16) + 16 * c;
                uint32_t type = MEM32(h + 4);
                uint32_t ct = type ? catchable(info, type) : 0;
                if (type && !ct) continue;
                if (eh_trace)
                    fprintf(stderr, "[eh32] %s caught by the frame at 0x%08X (catch funclet 0x%08X)\n",
                            throw_type(info), rec + 12, MEM32(h + 12));
                /* Caught: unwind everything above, then this frame to the try. */
                for (uint32_t r = top; r != rec; r = MEM32(r)) {
                    uint32_t f = funcinfo(MEM32(r + 4));
                    if (f) unwind_to(r, f, -1);
                }
                unwind_to(rec, fi, lo);
                MEM32(g_fs_base) = rec;
                MEM32(rec + 8) = (uint32_t)(hi + 1);
                uint32_t ebp = rec + 12;
                bind_catch_object(h, ct, obj, ebp);
                uint32_t prev_obj = t_cur_obj, prev_info = t_cur_info;
                t_cur_obj = obj, t_cur_info = info;
                uint32_t resume = call_funclet(MEM32(h + 12), ebp);
                t_cur_obj = prev_obj, t_cur_info = prev_info;
                /* The catch returned: continue in the frame's own function,
                 * on its stack as its prologue left it ([ebp-0x10]). */
                for (recomp_ehframe_t *f = t_frames; f; f = f->prev) {
                    if (f->entry_esp != ebp + 4) continue;
                    t_frames = f;
                    g_ebp = ebp;
                    g_esp = MEM32(ebp - 0x10);
                    g_eax = resume;
                    f->resume = resume;
                    g_cur_func = f->func;
                    longjmp(f->jb, 1);
                }
                hle_fatal("[eh32] %s caught by the frame at 0x%08X, but no lifted body owns it "
                          "(resume 0x%08X): its function has no landing pad", throw_type(info), ebp, resume);
            }
        }
    }
}

/* _CxxThrowException(object, ThrowInfo), stdcall. Never returns. */
void hle_eh_throw(void) {
    uint32_t obj = A32(0), info = A32(1);
    if (eh_trace < 0) eh_trace = getenv("HLE_TRACE_EH") != NULL;
    if (eh_trace)
        fprintf(stderr, "[eh32] %s 0x%08X from sub_%08X%s\n", info ? "throw" : "rethrow",
                info ? obj : t_cur_obj, g_cur_func, !info && !t_cur_info ? " with no exception active" : "");
    if (!info) obj = t_cur_obj, info = t_cur_info;          /* `throw;` */
    g_esp += 4;                                             /* the call's return address */
    if (info) dispatch(obj, info);
    hle_fatal("[eh32] an uncaught C++ exception (%s) from sub_%08X", info ? throw_type(info) : "none active",
              g_cur_func);
}

const win32hle_shim win32hle_eh32[] = {
    { "_CxxThrowException", hle_eh_throw },
    { NULL, NULL }
};
