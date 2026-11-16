/*
 * dinput.c - DirectInput (3, 5 and 8): the system mouse and keyboard.
 *
 * Both devices read the input state the window manager keeps (user32.c:
 * the cursor, and the key and button bytes GetAsyncKeyState answers from),
 * which the host feeds from SDL or a script with hle_input_mouse and
 * hle_input_key. So a game that reads DirectInput and one that reads its
 * window's messages see the same mouse and keyboard.
 *
 * The mouse reports relative axes, each read the movement since the last,
 * unless the game sets DIPROP_AXISMODE to absolute; three buttons. The
 * movement does not stop at the screen's edge (hle_input_raw), and while a
 * relative mouse is held exclusively the host is asked for movement alone
 * (hle_input_relative_hook), as Windows gives it. The
 * keyboard reports DIK scan codes. Both answer GetDeviceState (immediate)
 * and GetDeviceData (buffered: each change as an event, recorded as input
 * arrives, with the device's notification event set then).
 *
 * A game controller the host reports (hle_input_pad) is a third device, a
 * gamepad shown as Windows shows an Xbox 360 controller to DirectInput:
 * the left stick X and Y, the right stick Rx and Ry, the triggers sharing Z
 * (the left one up from the middle, the right one down), the d-pad the
 * first POV hat, and ten buttons, A B X Y LB RB Back Start and the sticks
 * pressed, in that order. Its axes run 0..65535 until the game sets a
 * range (DIPROP_RANGE), with a dead zone if it sets one; its state comes in
 * whatever data format the game declares, object by object (DIJOYSTATE,
 * DIJOYSTATE2 or its own). It is enumerated, and can be made, only while
 * the host says one is connected.
 *
 * DirectInput 8 (DirectInput8Create) is the same devices behind its own
 * interfaces: IDirectInput8A is IDirectInput2A and two more methods
 * (EnumDevicesBySemantics, ConfigureDevices: action maps, which nothing
 * here has), IDirectInputDevice8A is IDirectInputDevice2A and five more (an
 * effects file, action maps, an image), and devices are described with
 * DirectInput 8's types (DI8DEVTYPE_MOUSE 0x12, _KEYBOARD 0x13) where 3 and 5
 * use DIDEVTYPE_MOUSE 2 and _KEYBOARD 3.
 *
 * HLE_DITRACE=1 prints the calls that set a device up.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"

#define DI_OK                0u
#define DI_BUFFEROVERFLOW    1u
#define DI_NOEFFECT          1u
#define DIERR_UNSUPPORTED    0x80004001u
#define DIERR_NOINTERFACE    0x80004002u
#define DIERR_INVALIDPARAM   0x80070057u
#define DIERR_DEVICENOTREG   0x80040154u
#define DIERR_OBJECTNOTFOUND 0x80070002u

static int g_trace = -1;
static int trace(void) { if (g_trace < 0) g_trace = getenv("HLE_DITRACE") != NULL; return g_trace; }
#define T(...) do { if (trace()) fprintf(stderr, "[dinput] " __VA_ARGS__); } while (0)

enum { MOUSE = 1, KEYBOARD = 2, PAD = 3 };
/* A pad's objects: the five axes (X Y Z Rx Ry, then Rz and two sliders it
 * has not got), the hat, the ten buttons. */
enum { O_NONE, O_AXIS, O_POV, O_BUTTON };
#define PAD_AXES 5
#define PAD_BUTTONS 10
#define MAX_OBJ 64
typedef struct { uint32_t ofs; uint8_t what, index; } pobj_t;   /* where in the game's format, and which */
#define QUEUE 256
typedef struct { uint32_t ofs, data, time, seq; } ev_t;    /* DIDEVICEOBJECTDATA */
typedef struct {
    int kind, acquired, absolute, exclusive;
    int v8;                                              /* made by DirectInput 8: its device types */
    uint32_t data_size;                                  /* SetDataFormat's dwDataSize */
    uint32_t buffer;                                     /* DIPROP_BUFFERSIZE */
    int last_x, last_y, last_z;                          /* GetDeviceState's: where the last read left the cursor (and wheel) */
    int buf_x, buf_y, buf_z;                             /* GetDeviceData's */
    uint8_t buf_buttons[3], buf_keys[256];               /* what the last buffered read saw */
    /* a pad's */
    pobj_t obj[MAX_OBJ];
    int nobj;
    int32_t range_min[8], range_max[8];                  /* by axis: X Y Z Rx Ry Rz s0 s1 */
    uint32_t dead[8];                                    /* DIPROP_DEADZONE, 0..10000 */
    uint32_t buf_obj[MAX_OBJ];                           /* what the last buffered read saw, by object */
    ev_t q[QUEUE];
    int qn;
    int overflow;
    uint32_t event;                                      /* SetEventNotification's */
} dev_t_;

static uint32_t g_vt_di, g_vt_dev, g_vt_di8, g_vt_dev8, g_seq;
/* The devices made, so input arriving can reach them. */
#define MAX_DEV 16
static uint32_t g_devs[MAX_DEV];
#define DEV(o) ((dev_t_ *)hle_ptr(MEM32((o) + 8)))

/* A device's DIDEVTYPE: DirectInput 8's codes for its devices
 * (DI8DEVTYPEMOUSE_TRADITIONAL, DI8DEVTYPEKEYBOARD_PCENH). */
static uint32_t dev_type(int kind, int v8) {
    if (kind == PAD) return 0x00010000u | (v8 ? 0x0215u : 0x0804u);   /* HID; DI8DEVTYPE_GAMEPAD standard, DIDEVTYPE_JOYSTICK gamepad */
    return kind == MOUSE ? (v8 ? 0x0212u : 0x0102u) : (v8 ? 0x0413u : 0x0403u);
}

/* ---- the pad ---- */
static hle_pad_t g_pad;
static void arrived(void);
void hle_input_pad(const hle_pad_t *pad) {
    int was = g_pad.connected;
    g_pad = *pad;
    if (!g_pad.name[0]) strcpy(g_pad.name, "Controller (XBOX 360 For Windows)");
    if (g_pad.connected != was) T("a controller %s: %s\n", g_pad.connected ? "connected" : "gone", g_pad.name);
    arrived();
}
/* The instance (made up) and product (VID 045E, PID 028E: an Xbox 360
 * controller, as its PIDVID GUID) DirectInput names the pad by. */
static const uint8_t k_pad_instance[16] = { 0x30, 0x9A, 0x7E, 0x9E, 0x5A, 0x1D, 0xD1, 0x11, 0x80, 0x01, 0x48, 0x4C, 0x45, 0x50, 0x41, 0x44 };
static const uint8_t k_pad_product[16] = { 0x5E, 0x04, 0x8E, 0x02, 0, 0, 0, 0, 0, 0, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 };
/* An axis's value, -32768..32767 before its range: X Y Z Rx Ry. */
static int pad_raw(int axis) {
    switch (axis) {
    case 0: return g_pad.lx;
    case 1: return g_pad.ly;
    case 2: return (g_pad.lt - g_pad.rt) * 128;          /* the left trigger up from the middle, the right down */
    case 3: return g_pad.rx;
    case 4: return g_pad.ry;
    default: return 0;
    }
}
static uint32_t pad_axis(const dev_t_ *d, int axis) {
    int raw = axis < PAD_AXES && g_pad.connected ? pad_raw(axis) : 0;
    if (d->dead[axis] && (int64_t)(raw < 0 ? -raw : raw) * 10000 < (int64_t)d->dead[axis] * 32768) raw = 0;
    int64_t lo = d->range_min[axis], hi = d->range_max[axis];
    return (uint32_t)(int32_t)(lo + ((int64_t)raw + 32768) * (hi - lo) / 65535);
}
static uint32_t pad_pov(int index) {                     /* hundredths of a degree from up, clockwise; centred all ones */
    if (index || !g_pad.connected) return 0xFFFFFFFFu;
    int u = !!(g_pad.buttons & HLE_PAD_UP), dn = !!(g_pad.buttons & HLE_PAD_DOWN);
    int l = !!(g_pad.buttons & HLE_PAD_LEFT), r = !!(g_pad.buttons & HLE_PAD_RIGHT);
    static const int32_t deg[3][3] = { { 31500, 0, 4500 }, { 27000, -1, 9000 }, { 22500, 18000, 13500 } };
    int32_t v = deg[1 + dn - u][1 + r - l];
    return v < 0 ? 0xFFFFFFFFu : (uint32_t)v;
}
static uint32_t pad_button(int index) {
    return index < PAD_BUTTONS && g_pad.connected && (g_pad.buttons & (1u << index)) ? 0x80u : 0u;
}
static uint32_t pad_obj(const dev_t_ *d, const pobj_t *o) {
    return o->what == O_AXIS ? pad_axis(d, o->index) : o->what == O_POV ? pad_pov(o->index) : pad_button(o->index);
}
/* The game's data format, object by object: what each is (by its GUID, or
 * else its type) and which instance (its own, or the next of that kind).
 * GUID_XAxis A36D02E0, Y E1, Z E2, Rx F4, Ry F5, Rz F7, Slider E4, Button
 * F0, POV F2 (the rest of the GUID is the same). */
static void pad_format(dev_t_ *d, uint32_t objs, uint32_t n, uint32_t objsize) {
    int next[4] = { 0 };
    d->nobj = 0;
    for (uint32_t i = 0; i < n && d->nobj < MAX_OBJ; i++) {
        uint32_t o = objs + i * (objsize ? objsize : 16), g = MEM32(o), ofs = MEM32(o + 4), type = MEM32(o + 8);
        uint32_t d1 = g ? MEM32(g) : 0, inst = (type >> 8) & 0xFFFF;
        int what = O_NONE, index = -1;
        if (d1 == 0xA36D02E0u || d1 == 0xA36D02E1u || d1 == 0xA36D02E2u) what = O_AXIS, index = (int)(d1 - 0xA36D02E0u);
        else if (d1 == 0xA36D02F4u || d1 == 0xA36D02F5u) what = O_AXIS, index = 3 + (int)(d1 - 0xA36D02F4u);
        else if (d1 == 0xA36D02F7u) what = O_AXIS, index = 5;
        else if (d1 == 0xA36D02E4u) what = O_AXIS, index = 6 + next[O_AXIS]++ % 2;   /* sliders: none here */
        else if (d1 == 0xA36D02F0u) what = O_BUTTON;
        else if (d1 == 0xA36D02F2u) what = O_POV;
        else if (!g) what = (type & 0x10) ? O_POV : (type & 0x0C) ? O_BUTTON : (type & 0x03) ? O_AXIS : O_NONE;
        if (what == O_NONE) continue;
        if (index < 0) index = inst != 0xFFFF ? (int)inst : next[what]++;
        d->obj[d->nobj++] = (pobj_t){ ofs, (uint8_t)what, (uint8_t)(index > 255 ? 255 : index) };
    }
}

/* ---- virtual keys to DirectInput's scan codes ---- */
static uint8_t g_dik[256];                                /* vk -> DIK */
static void dik_table(void) {
    if (g_dik[0x1B]) return;
    static const struct { uint8_t vk, dik; } t[] = {
        { 0x1B, 0x01 }, { 0x31, 0x02 }, { 0x32, 0x03 }, { 0x33, 0x04 }, { 0x34, 0x05 }, { 0x35, 0x06 }, { 0x36, 0x07 },
        { 0x37, 0x08 }, { 0x38, 0x09 }, { 0x39, 0x0A }, { 0x30, 0x0B }, { 0xBD, 0x0C }, { 0xBB, 0x0D }, { 0x08, 0x0E },
        { 0x09, 0x0F }, { 'Q', 0x10 }, { 'W', 0x11 }, { 'E', 0x12 }, { 'R', 0x13 }, { 'T', 0x14 }, { 'Y', 0x15 },
        { 'U', 0x16 }, { 'I', 0x17 }, { 'O', 0x18 }, { 'P', 0x19 }, { 0xDB, 0x1A }, { 0xDD, 0x1B }, { 0x0D, 0x1C },
        { 0xA2, 0x1D }, { 'A', 0x1E }, { 'S', 0x1F }, { 'D', 0x20 }, { 'F', 0x21 }, { 'G', 0x22 }, { 'H', 0x23 },
        { 'J', 0x24 }, { 'K', 0x25 }, { 'L', 0x26 }, { 0xBA, 0x27 }, { 0xDE, 0x28 }, { 0xC0, 0x29 }, { 0xA0, 0x2A },
        { 0xDC, 0x2B }, { 'Z', 0x2C }, { 'X', 0x2D }, { 'C', 0x2E }, { 'V', 0x2F }, { 'B', 0x30 }, { 'N', 0x31 },
        { 'M', 0x32 }, { 0xBC, 0x33 }, { 0xBE, 0x34 }, { 0xBF, 0x35 }, { 0xA1, 0x36 }, { 0x6A, 0x37 }, { 0xA4, 0x38 },
        { 0x20, 0x39 }, { 0x14, 0x3A }, { 0x70, 0x3B }, { 0x71, 0x3C }, { 0x72, 0x3D }, { 0x73, 0x3E }, { 0x74, 0x3F },
        { 0x75, 0x40 }, { 0x76, 0x41 }, { 0x77, 0x42 }, { 0x78, 0x43 }, { 0x79, 0x44 }, { 0x90, 0x45 }, { 0x91, 0x46 },
        { 0x67, 0x47 }, { 0x68, 0x48 }, { 0x69, 0x49 }, { 0x6D, 0x4A }, { 0x64, 0x4B }, { 0x65, 0x4C }, { 0x66, 0x4D },
        { 0x6B, 0x4E }, { 0x61, 0x4F }, { 0x62, 0x50 }, { 0x63, 0x51 }, { 0x60, 0x52 }, { 0x6E, 0x53 }, { 0x7A, 0x57 },
        { 0x7B, 0x58 }, { 0xA3, 0x9D }, { 0x6F, 0xB5 }, { 0xA5, 0xB8 }, { 0x13, 0xC5 }, { 0x24, 0xC7 }, { 0x26, 0xC8 },
        { 0x21, 0xC9 }, { 0x25, 0xCB }, { 0x27, 0xCD }, { 0x23, 0xCF }, { 0x28, 0xD0 }, { 0x22, 0xD1 }, { 0x2D, 0xD2 },
        { 0x2E, 0xD3 }, { 0x5B, 0xDB }, { 0x5C, 0xDC }, { 0x5D, 0xDD },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) g_dik[t[i].vk] = t[i].dik;
}
/* The keyboard as DIK bytes: a key's byte 0x80 while it is down. The
 * generic Shift, Control and Alt count as the left ones. */
static void keyboard(uint8_t out[256]) {
    dik_table();
    memset(out, 0, 256);
    for (int vk = 1; vk < 256; vk++)
        if ((hle_input_vk((uint32_t)vk) & 0x80) && g_dik[vk]) out[g_dik[vk]] = 0x80;
    if (hle_input_vk(0x10) & 0x80 && !out[0x36]) out[0x2A] = 0x80;
    if (hle_input_vk(0x11) & 0x80 && !out[0x9D]) out[0x1D] = 0x80;
    if (hle_input_vk(0x12) & 0x80 && !out[0xB8]) out[0x38] = 0x80;
}
static void buttons(uint8_t out[3]) {
    out[0] = hle_input_vk(1) & 0x80, out[1] = hle_input_vk(2) & 0x80, out[2] = hle_input_vk(4) & 0x80;
}

/* The mouse's axes: the cursor, absolute; else the movement, which does not
 * stop at the screen's edge (hle_input_raw). */
static void axes(const dev_t_ *d, int *x, int *y) {
    if (d->absolute) hle_input_position(x, y); else hle_input_raw(x, y);
}
/* A relative mouse held exclusively: the host gives movement alone, as
 * Windows does (the desktop's pointer stops at an edge; the mouse does not).
 * A host whose game takes the mouse non-exclusively but draws its own
 * cursor (Star Trek: Legacy, turning its camera by the mouse) says so with
 * hle_dinput_relative_shared: Windows gives such a device movement past the
 * edge too, and only the system pointer, which that game hides, is lost. */
static int g_shared_relative;
static void relative(void);
void hle_dinput_relative_shared(int on) { g_shared_relative = on != 0; relative(); }
static void relative(void) {
    int on = 0;
    for (int i = 0; i < MAX_DEV; i++) {
        if (!g_devs[i]) continue;
        dev_t_ *d = (dev_t_ *)hle_ptr(MEM32(g_devs[i] + 8));
        if (d->kind == MOUSE && d->acquired && (d->exclusive || g_shared_relative) && !d->absolute) on = 1;
    }
    static int was = -1;
    if (on != was && hle_input_relative_hook) hle_input_relative_hook(on);
    was = on;
}
static void push(dev_t_ *d, uint32_t ofs, uint32_t data) {
    if (d->qn >= QUEUE || (d->buffer && (uint32_t)d->qn >= d->buffer)) { d->overflow = 1; return; }
    ev_t *e = &d->q[d->qn++];
    e->ofs = ofs, e->data = data, e->time = hle_ticks_ms(), e->seq = ++g_seq;
}
/* What changed since the last buffered read, as events. */
static void gather(dev_t_ *d) {
    if (d->kind == MOUSE) {
        int x, y;
        uint8_t b[3];
        axes(d, &x, &y);
        if (x != d->buf_x) push(d, 0, d->absolute ? (uint32_t)x : (uint32_t)(x - d->buf_x));
        if (y != d->buf_y) push(d, 4, d->absolute ? (uint32_t)y : (uint32_t)(y - d->buf_y));
        d->buf_x = x, d->buf_y = y;
        int z = hle_input_wheel_total();                 /* the wheel: always relative, as on Windows */
        if (z != d->buf_z) push(d, 8, (uint32_t)(z - d->buf_z)), d->buf_z = z;
        buttons(b);
        for (int i = 0; i < 3; i++)
            if (b[i] != d->buf_buttons[i]) push(d, 12u + (uint32_t)i, b[i]), d->buf_buttons[i] = b[i];
    } else if (d->kind == PAD) {
        for (int i = 0; i < d->nobj; i++) {
            uint32_t v = pad_obj(d, &d->obj[i]);
            if (v != d->buf_obj[i]) push(d, d->obj[i].ofs, v), d->buf_obj[i] = v;
        }
    } else {
        uint8_t k[256];
        keyboard(k);
        for (int i = 0; i < 256; i++)
            if (k[i] != d->buf_keys[i]) push(d, (uint32_t)i, k[i]), d->buf_keys[i] = k[i];
    }
}
/* Input arrived (hle_input_hook, holding the machine): each acquired device
 * records what changed as events now, so a press and release between two
 * reads are both kept, and its notification event is set, as DirectInput
 * sets it when data arrives. */
static void arrived(void) {
    for (int i = 0; i < MAX_DEV; i++) {
        if (!g_devs[i]) continue;
        dev_t_ *d = (dev_t_ *)hle_ptr(MEM32(g_devs[i] + 8));
        if (!d->acquired) continue;
        int before = d->qn;
        gather(d);
        if (d->qn != before && d->event) hle_set_event(d->event);
    }
}
static void baseline(dev_t_ *d) {
    axes(d, &d->last_x, &d->last_y);
    d->buf_x = d->last_x, d->buf_y = d->last_y;
    d->last_z = d->buf_z = hle_input_wheel_total();
    buttons(d->buf_buttons);
    keyboard(d->buf_keys);
    for (int i = 0; i < d->nobj; i++) d->buf_obj[i] = pad_obj(d, &d->obj[i]);
    d->qn = 0, d->overflow = 0;
}

/* ---- IDirectInputDevice(2)A ---- */
static void dv_QueryInterface(void) {
    uint32_t d1 = MEM32(A32(1));
    /* IUnknown, IDirectInputDeviceA, IDirectInputDevice2A (or 8A): one object */
    if (d1 == 0 || (!DEV(A32(0))->v8 && (d1 == 0x5944E680u || d1 == 0x5944E682u)) ||
        (DEV(A32(0))->v8 && d1 == 0x54D41080u)) { MEM32(A32(2)) = A32(0); MEM32(A32(0) + 4)++; RET(DI_OK, 3); }
    MEM32(A32(2)) = 0;
    RET(DIERR_NOINTERFACE, 3);
}
static void dv_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) {
        for (int i = 0; i < MAX_DEV; i++) if (g_devs[i] == self) g_devs[i] = 0;
        hle_free(DEV(self));
        hle_com_free(self);
        relative();
    }
    RET(n, 1);
}
static void dv_GetCapabilities(void) {                   /* DIDEVCAPS */
    dev_t_ *d = DEV(A32(0));
    uint32_t c = A32(1), size = MEM32(c) ? MEM32(c) : 24;
    memset(hle_ptr(c + 4), 0, size - 4);
    MEM32(c + 4) = 1;                                    /* DIDC_ATTACHED */
    MEM32(c + 8) = dev_type(d->kind, d->v8);             /* a mouse, a keyboard: their DIDEVTYPEs */
    MEM32(c + 12) = d->kind == MOUSE ? 3 : d->kind == PAD ? PAD_AXES : 0;            /* axes */
    MEM32(c + 16) = d->kind == MOUSE ? 3 : d->kind == PAD ? PAD_BUTTONS : 128;       /* buttons */
    if (size >= 24) MEM32(c + 20) = d->kind == PAD;      /* POV hats */
    RET(DI_OK, 2);
}
/* Which axis a property header names: the device (-1: all), an offset in
 * the game's format (DIPH_BYOFFSET), or an object's type id (DIPH_BYID). */
static int prop_axis(const dev_t_ *d, uint32_t h) {
    uint32_t obj = MEM32(h + 8), how = MEM32(h + 12);
    if (how == 1) {
        for (int i = 0; i < d->nobj; i++) if (d->obj[i].ofs == obj && d->obj[i].what == O_AXIS) return d->obj[i].index;
        return -2;
    }
    if (how == 2) return (obj & 3) ? (int)((obj >> 8) & 0xFFFF) : -2;
    return -1;
}
static void dv_GetProperty(void) {                       /* (this, guid or id, DIPROPHEADER*) */
    dev_t_ *d = DEV(A32(0));
    uint32_t id = A32(1), h = A32(2);
    if (id == 1) { MEM32(h + 16) = d->buffer; RET(DI_OK, 3); }          /* DIPROP_BUFFERSIZE */
    if (id == 2) { MEM32(h + 16) = (uint32_t)d->absolute; RET(DI_OK, 3); }   /* DIPROP_AXISMODE */
    if (d->kind == PAD && (id == 4 || id == 5)) {
        int a = prop_axis(d, h);
        if (a < 0) a = 0;
        if (id == 4) MEM32(h + 16) = (uint32_t)d->range_min[a], MEM32(h + 20) = (uint32_t)d->range_max[a];   /* DIPROP_RANGE */
        else MEM32(h + 16) = d->dead[a];                                                            /* DIPROP_DEADZONE */
        RET(DI_OK, 3);
    }
    RET(DIERR_UNSUPPORTED, 3);
}
static void dv_SetProperty(void) {
    dev_t_ *d = DEV(A32(0));
    uint32_t id = A32(1), h = A32(2);
    T("SetProperty %u = %u\n", id, h ? MEM32(h + 16) : 0);
    if (id == 1) d->buffer = MEM32(h + 16) > QUEUE ? QUEUE : MEM32(h + 16);
    else if (id == 2) d->absolute = MEM32(h + 16) == 1, relative();   /* DIPROPAXISMODE_ABS */
    else if (d->kind == PAD && (id == 4 || id == 5)) {   /* DIPROP_RANGE, DIPROP_DEADZONE */
        int a = prop_axis(d, h);
        if (a == -2 || a > 7) RET(DIERR_OBJECTNOTFOUND, 3);
        for (int i = a < 0 ? 0 : a; i <= (a < 0 ? 7 : a); i++) {
            if (id == 4) d->range_min[i] = (int32_t)MEM32(h + 16), d->range_max[i] = (int32_t)MEM32(h + 20);
            else d->dead[i] = MEM32(h + 16) > 10000 ? 10000 : MEM32(h + 16);
        }
        T("the pad's %s, %s: %d..%d\n", id == 4 ? "range" : "dead zone", a < 0 ? "every axis" : "an axis",
          (int)MEM32(h + 16), id == 4 ? (int)MEM32(h + 20) : 0);
    }
    RET(DI_OK, 3);
}
static void dv_Acquire(void) {
    dev_t_ *d = DEV(A32(0));
    if (!d->acquired) baseline(d), d->acquired = 1;
    relative();
    RET(DI_OK, 1);
}
static void dv_Unacquire(void) { DEV(A32(0))->acquired = 0; relative(); RET(DI_OK, 1); }
static void dv_GetDeviceState(void) {                    /* (this, size, data) */
    dev_t_ *d = DEV(A32(0));
    uint32_t n = A32(1), p = A32(2);
    if (!p || !n) RET(DIERR_INVALIDPARAM, 3);
    memset(hle_ptr(p), 0, n);
    if (d->kind == MOUSE) {                              /* DIMOUSESTATE: lX, lY, lZ, four buttons */
        int x, y;
        uint8_t b[3];
        axes(d, &x, &y);
        buttons(b);
        int z = hle_input_wheel_total();
        if (n >= 12) {
            MEM32(p) = (uint32_t)(d->absolute ? x : x - d->last_x);
            MEM32(p + 4) = (uint32_t)(d->absolute ? y : y - d->last_y);
            MEM32(p + 8) = (uint32_t)(z - d->last_z);
        }
        d->last_x = x, d->last_y = y, d->last_z = z;
        for (uint32_t i = 0; i < 3 && 12 + i < n; i++) MEM8(p + 12 + i) = b[i];
    } else if (d->kind == PAD) {                         /* the game's format, object by object */
        for (int i = 0; i < d->nobj; i++) {
            const pobj_t *o = &d->obj[i];
            uint32_t v = pad_obj(d, o);
            if (o->what == O_BUTTON) { if (o->ofs < n) MEM8(p + o->ofs) = (uint8_t)v; }
            else if (o->ofs + 4 <= n) MEM32(p + o->ofs) = v;
        }
    } else {
        uint8_t k[256];
        keyboard(k);
        memcpy(hle_ptr(p), k, n < 256 ? n : 256);
    }
    RET(DI_OK, 3);
}
static void dv_GetDeviceData(void) {                     /* (this, objsize, rgdod, &inout, flags) */
    dev_t_ *d = DEV(A32(0));
    uint32_t sz = A32(1), out = A32(2), io = A32(3), flags = A32(4);
    if (out && !io) RET(DIERR_INVALIDPARAM, 5);          /* reading needs a count to read up to */
    uint32_t want = io ? MEM32(io) : 0;
    gather(d);
    uint32_t hr = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    if (!out) {                                          /* a count, or a flush */
        uint32_t n = (uint32_t)d->qn;
        if (!(flags & 1)) d->qn = 0, d->overflow = 0;    /* not DIGDD_PEEK: they are gone */
        if (io) MEM32(io) = want == 0xFFFFFFFFu || n < want ? n : want;
        RET(hr, 5);
    }
    uint32_t n = (uint32_t)d->qn < want ? (uint32_t)d->qn : want;
    if (sz < 16) sz = 16;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = out + i * sz;
        memset(hle_ptr(e), 0, sz);
        MEM32(e) = d->q[i].ofs, MEM32(e + 4) = d->q[i].data, MEM32(e + 8) = d->q[i].time, MEM32(e + 12) = d->q[i].seq;
    }
    if (!(flags & 1)) {
        memmove(d->q, d->q + n, sizeof *d->q * (size_t)(d->qn - (int)n));
        d->qn -= (int)n;
        d->overflow = 0;
    }
    if (trace() && n) {
        fprintf(stderr, "[dinput] GetDeviceData %s:", d->kind == MOUSE ? "mouse" : d->kind == PAD ? "pad" : "keyboard");
        for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %X=%X", MEM32(out + i * sz), MEM32(out + i * sz + 4));
        fprintf(stderr, "%s\n", flags & 1 ? " (peek)" : "");
    }
    MEM32(io) = n;
    RET(hr, 5);
}
static void dv_SetDataFormat(void) {                     /* DIDATAFORMAT: size, objsize, flags, datasize, nobjs, objs */
    dev_t_ *d = DEV(A32(0));
    uint32_t f = A32(1);
    d->data_size = MEM32(f + 12);
    if (d->kind == PAD) {
        pad_format(d, MEM32(f + 20), MEM32(f + 16), MEM32(f + 4));
        for (int i = 0; i < d->nobj; i++) d->buf_obj[i] = pad_obj(d, &d->obj[i]);
    } else if (MEM32(f + 8) & 1) d->absolute = 1, relative();   /* DIDF_ABSAXIS */
    T("SetDataFormat on the %s: %u bytes, %u objects, flags %X\n", d->kind == MOUSE ? "mouse" : d->kind == PAD ? "pad" : "keyboard",
      d->data_size, MEM32(f + 16), MEM32(f + 8));
    RET(DI_OK, 2);
}
static void dv_SetEventNotification(void) { DEV(A32(0))->event = A32(1); RET(DI_OK, 2); }
static void dv_SetCooperativeLevel(void) {             /* (this, hwnd, flags): DISCL_EXCLUSIVE is 1 */
    T("SetCooperativeLevel %X\n", A32(2));
    DEV(A32(0))->exclusive = (A32(2) & 1) != 0;
    relative();
    RET(DI_OK, 3);
}
static void dv_GetObjectInfo(void) { RET(DIERR_OBJECTNOTFOUND, 4); }
/* (this, callback, ref, flags): the pad's axes, hat and buttons, each a
 * DIDEVICEOBJECTINSTANCEA at its place in DIJOYSTATE; the mouse and the
 * keyboard enumerate nothing. flags: DIDFT_ALL 0, or the kinds wanted. */
static void dv_EnumObjects(void) {
    dev_t_ *d = DEV(A32(0));
    uint32_t cb = A32(1), ref = A32(2), want = A32(3) & 0xFF;
    if (d->kind != PAD) RET(DI_OK, 4);
    static const char *const an[PAD_AXES] = { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation" };
    static const uint8_t ag[PAD_AXES] = { 0xE0, 0xE1, 0xE2, 0xF4, 0xF5 };
    uint32_t mark = hle_scratch_mark(), p = hle_scratch(316);
    for (int k = 0; k < PAD_AXES + 1 + PAD_BUTTONS; k++) {
        int axis = k < PAD_AXES, pov = k == PAD_AXES, b = k - PAD_AXES - 1;
        uint32_t type = axis ? 0x02u | ((uint32_t)k << 8) : pov ? 0x10u : 0x04u | ((uint32_t)b << 8);
        if (want && !(type & want & 0x1F)) continue;
        memset(hle_ptr(p), 0, 316);
        MEM32(p) = 316;
        uint8_t g[16] = { 0xE0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
        g[0] = axis ? ag[k] : pov ? 0xF2 : 0xF0;
        memcpy(hle_ptr(p + 4), g, 16);
        MEM32(p + 20) = axis ? (uint32_t)(k * 4) : pov ? 32u : 48u + (uint32_t)b;   /* DIJOYSTATE's offsets */
        MEM32(p + 24) = type;
        if (axis) snprintf((char *)hle_ptr(p + 32), 260, "%s", an[k]);
        else if (pov) snprintf((char *)hle_ptr(p + 32), 260, "Hat Switch");
        else snprintf((char *)hle_ptr(p + 32), 260, "Button %d", b);
        uint32_t a[2] = { p, ref };
        if (!hle_call_guest(cb, 2, a)) break;            /* DIENUM_STOP */
    }
    hle_scratch_release(mark);
    RET(DI_OK, 4);
}
static void put_instance(uint32_t p, int kind, int v8) { /* DIDEVICEINSTANCEA */
    uint32_t size = MEM32(p) ? MEM32(p) : 0x234;
    memset(hle_ptr(p + 4), 0, size - 4);
    uint8_t g[16] = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    if (kind == KEYBOARD) g[0] = 0x61;                   /* GUID_SysMouse, GUID_SysKeyboard */
    memcpy(hle_ptr(p + 4), kind == PAD ? k_pad_instance : g, 16);
    memcpy(hle_ptr(p + 20), kind == PAD ? k_pad_product : g, 16);
    MEM32(p + 36) = dev_type(kind, v8);
    const char *name = kind == MOUSE ? "Mouse" : kind == KEYBOARD ? "Keyboard" : g_pad.name;
    snprintf((char *)hle_ptr(p + 40), 260, "%s", name);
    snprintf((char *)hle_ptr(p + 300), 260, "%s", name);
    if (kind == PAD && size >= 0x244) MEM16(p + 0x240) = 0x01, MEM16(p + 0x242) = 0x05;   /* HID usage page 1, usage 5: a game pad */
}
static void dv_GetDeviceInfo(void) { dev_t_ *d = DEV(A32(0)); put_instance(A32(1), d->kind, d->v8); RET(DI_OK, 2); }
static void dv_ok2(void) { RET(DI_OK, 2); }
static void dv_ok3(void) { RET(DI_OK, 3); }
static void dv_ok4(void) { RET(DI_OK, 4); }
static void dv_none1(void) { RET(DI_NOEFFECT, 1); }
static void dv_no2(void) { RET(DIERR_UNSUPPORTED, 2); }
static void dv_no3(void) { RET(DIERR_UNSUPPORTED, 3); }
static void dv_no4(void) { RET(DIERR_UNSUPPORTED, 4); }
static void dv_no5(void) { RET(DIERR_UNSUPPORTED, 5); }
static void dv_ok5(void) { RET(DI_OK, 5); }
static void dv_ok6(void) { RET(DI_OK, 6); }

/* ---- IDirectInput(2)A ---- */
/* The IDirectInput object's word at +8: 8 if DirectInput8Create made it. */
#define DI_V8(o) (MEM32((o) + 8) == 8)
static void di_QueryInterface(void) {
    uint32_t d1 = MEM32(A32(1)), self = A32(0);
    if (d1 == 0 || (!DI_V8(self) && (d1 == 0x89521360u || d1 == 0x5944E662u)) || (DI_V8(self) && d1 == 0xBF798030u)) { MEM32(A32(2)) = A32(0); MEM32(A32(0) + 4)++; RET(DI_OK, 3); }
    MEM32(A32(2)) = 0;
    RET(DIERR_NOINTERFACE, 3);
}
static void di_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) hle_com_free(self);
    RET(n, 1);
}
static void di_CreateDevice(void) {                      /* (this, guid, &device, outer) */
    uint32_t d1 = MEM32(A32(1));
    int kind = d1 == 0x6F1D2B60u ? MOUSE : d1 == 0x6F1D2B61u ? KEYBOARD : 0;
    if (!kind && !memcmp(hle_ptr(A32(1)), k_pad_instance, 16)) kind = PAD;
    if (!kind) { MEM32(A32(2)) = 0; RET(DIERR_DEVICENOTREG, 4); }
    dev_t_ *d = (dev_t_ *)hle_alloc(sizeof *d);
    d->kind = kind;
    for (int i = 0; i < 8; i++) d->range_min[i] = 0, d->range_max[i] = 65535;
    d->v8 = DI_V8(A32(0));
    baseline(d);
    uint32_t o = hle_com_new(d->v8 ? g_vt_dev8 : g_vt_dev, 12);
    MEM32(o + 8) = hle_va(d);
    MEM32(A32(2)) = o;
    for (int i = 0; i < MAX_DEV; i++) if (!g_devs[i]) { g_devs[i] = o; break; }
    hle_input_hook = arrived;
    T("CreateDevice: the %s -> 0x%08X\n", kind == MOUSE ? "mouse" : kind == PAD ? "pad" : "keyboard", o);
    RET(DI_OK, 4);
}
static void di_EnumDevices(void) {                       /* (this, type, callback, ctx, flags) */
    /* The type is DIDEVTYPE_MOUSE 2, _KEYBOARD 3 or _JOYSTICK 4, or
     * (DirectInput 8) DI8DEVCLASS_POINTER 2, _KEYBOARD 3 or _GAMECTRL 4: the
     * same numbers; or one of DirectInput 8's types (a joystick 0x14, a
     * gamepad 0x15). The pad only while one is connected. */
    uint32_t type = A32(1) & 0xFF, mark = hle_scratch_mark(), p = hle_scratch(0x244);
    int v8 = DI_V8(A32(0));
    T("EnumDevices: type %X, flags %X%s\n", type, A32(4), g_pad.connected ? " (a controller is connected)" : "");
    for (int kind = MOUSE; kind <= PAD; kind++) {
        if (kind == PAD && !g_pad.connected) continue;
        if (type && type != (kind == MOUSE ? 2u : kind == KEYBOARD ? 3u : 4u) &&
            !(kind == PAD && v8 && (type == 0x14 || type == 0x15))) continue;
        MEM32(p) = 0x244;
        put_instance(p, kind, v8);
        uint32_t a[2] = { p, A32(3) };
        if (!hle_call_guest(A32(2), 2, a)) break;        /* DIENUM_STOP */
    }
    hle_scratch_release(mark);
    RET(DI_OK, 5);
}
static void di_GetDeviceStatus(void) { RET(DI_OK, 2); }
static void di_FindDevice(void) { RET(DIERR_DEVICENOTREG, 4); }

static void vtables(void) {
    if (g_vt_di) return;
    static const win32hle_shim di[] = {
        { "IDirectInputA::QueryInterface", di_QueryInterface }, { "IDirectInputA::AddRef", hle_com_AddRef },
        { "IDirectInputA::Release", di_Release }, { "IDirectInputA::CreateDevice", di_CreateDevice },
        { "IDirectInputA::EnumDevices", di_EnumDevices }, { "IDirectInputA::GetDeviceStatus", di_GetDeviceStatus },
        { "IDirectInputA::RunControlPanel", dv_ok3 }, { "IDirectInputA::Initialize", dv_ok3 },
        { "IDirectInput2A::FindDevice", di_FindDevice }, { 0, 0 } };
    static const win32hle_shim dv[] = {
        { "IDirectInputDeviceA::QueryInterface", dv_QueryInterface }, { "IDirectInputDeviceA::AddRef", hle_com_AddRef },
        { "IDirectInputDeviceA::Release", dv_Release }, { "IDirectInputDeviceA::GetCapabilities", dv_GetCapabilities },
        { "IDirectInputDeviceA::EnumObjects", dv_EnumObjects }, { "IDirectInputDeviceA::GetProperty", dv_GetProperty },
        { "IDirectInputDeviceA::SetProperty", dv_SetProperty }, { "IDirectInputDeviceA::Acquire", dv_Acquire },
        { "IDirectInputDeviceA::Unacquire", dv_Unacquire }, { "IDirectInputDeviceA::GetDeviceState", dv_GetDeviceState },
        { "IDirectInputDeviceA::GetDeviceData", dv_GetDeviceData }, { "IDirectInputDeviceA::SetDataFormat", dv_SetDataFormat },
        { "IDirectInputDeviceA::SetEventNotification", dv_SetEventNotification },
        { "IDirectInputDeviceA::SetCooperativeLevel", dv_SetCooperativeLevel },
        { "IDirectInputDeviceA::GetObjectInfo", dv_GetObjectInfo }, { "IDirectInputDeviceA::GetDeviceInfo", dv_GetDeviceInfo },
        { "IDirectInputDeviceA::RunControlPanel", dv_ok3 }, { "IDirectInputDeviceA::Initialize", dv_ok4 },
        /* IDirectInputDevice2A: force feedback (none), Poll, SendDeviceData */
        { "IDirectInputDevice2A::CreateEffect", dv_no5 }, { "IDirectInputDevice2A::EnumEffects", dv_ok4 },
        { "IDirectInputDevice2A::GetEffectInfo", dv_no3 }, { "IDirectInputDevice2A::GetForceFeedbackState", dv_no2 },
        { "IDirectInputDevice2A::SendForceFeedbackCommand", dv_no2 },
        { "IDirectInputDevice2A::EnumCreatedEffectObjects", dv_ok4 }, { "IDirectInputDevice2A::Escape", dv_no2 },
        { "IDirectInputDevice2A::Poll", dv_none1 }, { "IDirectInputDevice2A::SendDeviceData", dv_no5 }, { 0, 0 } };
    g_vt_di = hle_com_vtable(di);
    g_vt_dev = hle_com_vtable(dv);
    /* DirectInput 8: the same slots, and its own at the end. */
    static win32hle_shim di8[16], dv8[40];
    int n = 0;
    for (; di[n].name; n++) di8[n] = di[n];
    di8[n++] = (win32hle_shim){ "IDirectInput8A::EnumDevicesBySemantics", dv_ok6 };
    di8[n++] = (win32hle_shim){ "IDirectInput8A::ConfigureDevices", dv_ok5 };
    di8[n] = (win32hle_shim){ 0, 0 };
    for (n = 0; dv[n].name; n++) dv8[n] = dv[n];
    dv8[n++] = (win32hle_shim){ "IDirectInputDevice7A::EnumEffectsInFile", dv_ok5 };
    dv8[n++] = (win32hle_shim){ "IDirectInputDevice7A::WriteEffectToFile", dv_no5 };
    dv8[n++] = (win32hle_shim){ "IDirectInputDevice8A::BuildActionMap", dv_no4 };
    dv8[n++] = (win32hle_shim){ "IDirectInputDevice8A::SetActionMap", dv_no4 };
    dv8[n++] = (win32hle_shim){ "IDirectInputDevice8A::GetImageInfo", dv_no2 };
    dv8[n] = (win32hle_shim){ 0, 0 };
    g_vt_di8 = hle_com_vtable(di8);
    g_vt_dev8 = hle_com_vtable(dv8);
    (void)dv_ok2;
}

static void x_DirectInputCreateA(void) {                 /* (hinst, version, &out, outer) */
    vtables();
    T("DirectInputCreateA version %X\n", A32(1));
    MEM32(A32(2)) = hle_com_new(g_vt_di, 12);
    RET(DI_OK, 4);
}

/* DirectInput8Create(hinst, version, riid, &out, outer): IID_IDirectInput8A. */
static void x_DirectInput8Create(void) {
    vtables();
    uint32_t d1 = A32(2) ? MEM32(A32(2)) : 0;
    T("DirectInput8Create version %X\n", A32(1));
    if (d1 != 0xBF798030u) { MEM32(A32(3)) = 0; RET(DIERR_NOINTERFACE, 5); }   /* the A interface only */
    uint32_t o = hle_com_new(g_vt_di8, 12);
    MEM32(o + 8) = 8;
    MEM32(A32(3)) = o;
    RET(DI_OK, 5);
}

const win32hle_shim win32hle_dinput[] = {
    { "DirectInputCreateA", x_DirectInputCreateA }, { "DirectInputCreate", x_DirectInputCreateA },
    { "DirectInput8Create", x_DirectInput8Create },
    { 0, 0 }
};
