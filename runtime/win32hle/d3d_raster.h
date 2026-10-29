/*
 * d3d_raster.h - the software rasteriser behind d3d.c's DrawPrimitive: one
 * triangle at a time, in screen space, into a DirectDraw surface.
 */
#ifndef WIN32HLE_D3D_RASTER_H
#define WIN32HLE_D3D_RASTER_H
#include <stdint.h>
#include "ddraw_int.h"

/* A vertex in screen space: position, 1/w, and what is interpolated. */
typedef struct {
    float x, y, z, rhw;
    float r, g, b, a;                    /* diffuse, 0..1 */
    float sr, sg, sb, fog;               /* specular, and fog factor (specular alpha) */
    float u, v;
} rvert_t;

/* What a triangle is drawn with: the device's render and texture-stage
 * states, read out by d3d.c. */
/* A pixel format, decoded once: each channel's shift and width. */
typedef struct { int sh[4], bits[4]; uint32_t mask[4]; } rfmt_t;

typedef struct {
    surf_t *target, *zbuf, *tex;
    rfmt_t tfmt;                         /* the target's format (d3d_raster_prepare) */
    int clip_l, clip_t, clip_r, clip_b;  /* the viewport, clipped to the target */
    int zenable, zwrite, zfunc;
    int alphatest, alphafunc; float alpharef;
    int blend, srcblend, dstblend;
    int colorkey;
    int fog; float fog_r, fog_g, fog_b;
    int specular;
    int flat;
    int cull;                            /* 1 none, 2 cw, 3 ccw */
    int bilinear;
    int wrap_u, wrap_v;                  /* 1 wrap, else clamp */
    int colorop, colorarg1, colorarg2;
    int alphaop, alphaarg1, alphaarg2;
    float tfactor[4];
} rstate_t;

/* Before drawing with a state: decode the target's format and bring the
 * texture's ARGB cache up to date. */
void d3d_raster_prepare(rstate_t *st);
void d3d_raster_triangle(const rstate_t *st, const rvert_t *a, const rvert_t *b, const rvert_t *c);
void d3d_raster_line(const rstate_t *st, const rvert_t *a, const rvert_t *b);

/* Deferred drawing: triangles and lines recorded, drawn by every core at
 * the next flush (which must come before anything else touches a surface). */
void d3d_raster_submit_triangle(const rstate_t *st, const rvert_t *a, const rvert_t *b, const rvert_t *c);
void d3d_raster_submit_line(const rstate_t *st, const rvert_t *a, const rvert_t *b);
void d3d_raster_flush(void);

/* A texel of any 8/16/32-bit surface, as 0..1 ARGB; sets *keyed if it is the
 * surface's source colour key. */
void d3d_texel(const surf_t *s, int x, int y, float out[4], int *keyed);
/* Pack 0..1 RGB into a surface's pixel format. */
uint32_t d3d_pack(const surf_t *s, float r, float g, float b, float a);

#endif
