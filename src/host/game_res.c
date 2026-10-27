// Render and output resolution for TOTK 1.4.3 (tools/lifter/patches.py), from MacNativeGames
// games/totk/runtime/game_patches_v143.c and scripts/make_output_res_overlay.py (MIT).
//  - TOTK_RENDER_RES=<w>x<h> or "display": the size the 3D scene is rendered at (the game's own: 1600x900 docked).
//  - TOTK_OUTPUT_RES=<w>x<h> or "display": the game's canvas, where it composes the scene, the HUD and the menus
//    (the game's own: 1920x1080). The window shows the canvas scaled to its size.
// Both are read once at start. Bigger targets need bigger texture heaps: the game's heap sizes are data
// (System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs), so fs.c serves that file with DynamicTextureAllocatorMem and
// Graphics grown by runtime_heap_extra_bytes(), and the application pool (svc.c) grows by the same amount.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#define CANVAS_W 1920
#define SCENE_W 1600
#define SCENE_H 900

int mtl_display_size(unsigned *w, unsigned *h);   // window_sdl.c

/* The data the patched game code reads (guest addresses are host addresses): +16 the scene size as floats, divided by
   the canvas's scale (the game multiplies its scene by canvas width / 1920); +32 the canvas as h << 32 | w. */
uint64_t totk_res_page[8];

static int parse(const char *name, unsigned *w, unsigned *h, unsigned min_w, unsigned min_h, unsigned max_w, unsigned max_h) {
    const char *e = getenv(name);
    if (!e || !*e || !strcmp(e, "0")) return 0;
    char rest;
    if (!strcmp(e, "display")) {
        if (!mtl_display_size(w, h)) return 0;
    } else if (sscanf(e, "%ux%u%c", w, h, &rest) != 2) {
        fprintf(stderr, "[totk] %s=%s: not <width>x<height> or display; the game's own size\n", name, e);
        return 0;
    }
    if (*w < min_w || *h < min_h || *w > max_w || *h > max_h) {
        fprintf(stderr, "[totk] %s=%ux%u: outside %ux%u..%ux%u; the game's own size\n", name, *w, *h, min_w, min_h, max_w, max_h);
        return 0;
    }
    return 1;
}

static int decided, output_on, render_on;
static unsigned out_w = CANVAS_W, out_h = 1080, scene_w = SCENE_W, scene_h = SCENE_H;
static void decide(void) {
    if (decided) return;
    decided = 1;
    output_on = parse("TOTK_OUTPUT_RES", &out_w, &out_h, 640, 360, 7680, 4320);
    if (!output_on) { out_w = CANVAS_W; out_h = 1080; }
    render_on = parse("TOTK_RENDER_RES", &scene_w, &scene_h, 64, 64, 4096, 4096);
    float k = out_w / (float)CANVAS_W;
    // without TOTK_RENDER_RES the game scales its own 1600x900 with the canvas (3200x1800 on a 4K one): the heaps count it
    if (!render_on) { scene_w = (unsigned)(SCENE_W * k + 0.5f); scene_h = (unsigned)(SCENE_H * k + 0.5f); }
    float v[2] = {scene_w / k, scene_h / k};
    memcpy((uint8_t *)totk_res_page + 16, v, sizeof v);
    totk_res_page[4] = (uint64_t)out_h << 32 | out_w;
    fprintf(stderr, "[totk] canvas %ux%u%s, 3D scene %ux%u%s (TOTK_OUTPUT_RES, TOTK_RENDER_RES)\n", out_w, out_h,
            output_on ? "" : " (the game's)", scene_w, scene_h, render_on ? "" : " (the game's, scaled with the canvas)");
}
int totk_output_res_on(void) { decide(); return output_on; }
int totk_render_res_on(void) { decide(); return render_on; }
uint64_t totk_output_w(void) { decide(); return out_w; }
uint64_t totk_output_h(void) { decide(); return out_h; }

/* KiB added to DynamicTextureAllocatorMem (113664 KiB shipped) and Graphics: each of the canvas and the scene grows the
   heap by its pixel ratio above the shipped size, a bigger scene with a 1.6 margin (MacNativeGames measured a 3440x1440
   canvas with a 2580x1080 scene crash at +258 MiB and load at +400 MiB). */
#define DTA_KIB 113664
static uint64_t extra_kib(void) {
    decide();
    double grow = 0;
    if ((double)out_w * out_h > 1920.0 * 1080.0) grow += (double)out_w * out_h / (1920.0 * 1080.0) - 1;
    int scene = (double)scene_w * scene_h > (double)SCENE_W * SCENE_H;
    if (scene) grow += (double)scene_w * scene_h / ((double)SCENE_W * SCENE_H) - 1;
    if (grow <= 0) return 0;
    if (scene) grow *= 1.6;
    return (uint64_t)ceil(DTA_KIB * (1 + grow) / 1024.0) * 1024 - DTA_KIB;
}
uint64_t runtime_heap_extra_bytes(void) { return extra_kib() << 10; }

/* BYML v7 ("YB"): hash nodes (0xC1) of {u24 key index, u8 type, u32 value}; ints (0xD1) inline. */
typedef struct { uint8_t *b; size_t n; uint32_t keys, nkeys; } Byml;
static uint32_t rd32(const Byml *y, size_t at) { uint32_t v = 0; if (at + 4 <= y->n) memcpy(&v, y->b + at, 4); return v; }
static const char *key_name(const Byml *y, uint32_t i) {
    if (i >= y->nkeys) return "";
    size_t at = y->keys + rd32(y, y->keys + 4 + 4 * (size_t)i);
    return at < y->n && memchr(y->b + at, 0, y->n - at) ? (const char *)y->b + at : "";
}
static size_t field(const Byml *y, size_t node, const char *name, uint8_t type) {   // offset of the value, 0 = none
    if (node + 4 > y->n || y->b[node] != 0xc1) return 0;
    uint32_t count = rd32(y, node) >> 8;
    for (uint32_t i = 0; i < count; i++) {
        size_t e = node + 4 + 8 * (size_t)i;
        if (e + 8 > y->n) return 0;
        if (!strcmp(key_name(y, rd32(y, e) & 0xffffff), name)) return y->b[e + 3] == type ? e + 4 : 0;
    }
    return 0;
}
static size_t child(const Byml *y, size_t node, const char *name) { size_t f = field(y, node, name, 0xc1); return f ? rd32(y, f) : 0; }

/* The HeapDef with the bigger texture heaps, as a new zstd frame (malloc'd), or NULL to serve the original. */
uint8_t *totk_heapdef_patch(const uint8_t *zs, size_t n, size_t *out_n) {
    uint64_t extra = extra_kib();
    if (!extra) return NULL;
    unsigned long long size = ZSTD_getFrameContentSize(zs, n);
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (64u << 20) || ZSTD_getDictID_fromFrame(zs, n)) {
        fprintf(stderr, "[totk] HeapDef: not the expected zstd frame; heaps not grown (expect a crash above 1600x900 / 1080p)\n");
        return NULL;
    }
    Byml y = {malloc(size), size, 0, 0};
    if (!y.b || ZSTD_isError(ZSTD_decompress(y.b, size, zs, n)) || memcmp(y.b, "YB\x07\x00", 4)) goto fail;
    y.keys = rd32(&y, 4);
    y.nkeys = rd32(&y, y.keys) >> 8;
    size_t heaps = child(&y, rd32(&y, 12), "Heaps");
    size_t dta = heaps ? field(&y, child(&y, heaps, "DynamicTextureAllocatorMem"), "Size", 0xd1) : 0;
    size_t gfx = heaps ? field(&y, child(&y, heaps, "Graphics"), "Size", 0xd1) : 0;
    if (!dta || !gfx) goto fail;
    int32_t a = (int32_t)rd32(&y, dta), g = (int32_t)rd32(&y, gfx), a2 = a + (int32_t)extra, g2 = g + (int32_t)extra;
    memcpy(y.b + dta, &a2, 4);
    memcpy(y.b + gfx, &g2, 4);
    size_t cap = ZSTD_compressBound(size);
    uint8_t *out = malloc(cap);
    size_t z = out ? ZSTD_compress(out, cap, y.b, size, 3) : 0;
    free(y.b);
    if (!out || ZSTD_isError(z)) { free(out); return NULL; }
    fprintf(stderr, "[totk] HeapDef: DynamicTextureAllocatorMem %d -> %d KiB, Graphics %d -> %d KiB (canvas %ux%u, scene %ux%u)\n",
            a, a2, g, g2, out_w, out_h, scene_w, scene_h);
    *out_n = z;
    return out;
fail:
    free(y.b);
    fprintf(stderr, "[totk] HeapDef: layout not recognised; heaps not grown\n");
    return NULL;
}
