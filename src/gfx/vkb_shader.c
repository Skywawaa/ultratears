// Shaders of the NVN replayer (milestone 3.3 step 4): the game's Maxwell shaders, translated to SPIR-V by Ryujinx's
// translator (MacNativeGames' totk_xlate), become Vulkan shader objects (VK_EXT_shader_object): no pipelines to build
// at draw time, every other state is dynamic.
//  - A shader is identified as MacNativeGames does: SHA-256 of its control section (0x880 bytes) and its code (length
//    at control+0x704); the stage is at control+0x714 (0 vertex, 1 fragment, 5 compute).
//  - [temporary] The translation is the default variant from the translator comparison (work/shaders/v143/ryujinx: every
//    texture 2D-typed by its own reflection, attributes and outputs float). The runtime translation service with
//    variants per bound state (MacNativeGames' totk_shader_rt) replaces it before the open world.
//  - Descriptors: one universal layout. Each stage owns 64 bindings of set 0 (vertex 0-63, fragment 64-127; compute its
//    own layout): +0 Ryujinx support buffer, +1..17 constant buffers c0-c16, +18..33 storage buffers, +34..57 combined
//    textures, +58..61 images. The translator numbers bindings densely per shader; they are rewritten in the SPIR-V
//    when it is loaded, so any vertex shader combines with any fragment shader under one pipeline layout.
#include <windows.h>
#include <bcrypt.h>   // after windows.h (its types)
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "vkb_shader.h"
#include "vkb_spv.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

static VkDescriptorSetLayout g_layout[2];   // 0 graphics, 1 compute
static VkPipelineLayout g_playout[2];

static void make_layouts(void) {
    for (int compute = 0; compute < 2; compute++) {
        VkDescriptorSetLayoutBinding b[128];
        VkDescriptorBindingFlags fl[128];
        uint32_t n = 0;
        for (int st = 0; st < (compute ? 1 : 2); st++) {
            VkShaderStageFlags sf = compute ? VK_SHADER_STAGE_COMPUTE_BIT : st ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            uint32_t base = (uint32_t)st * VKB_STAGE_BINDINGS;
            struct { uint32_t off, count; VkDescriptorType type; } r[] = {
                {VKB_B_SUPPORT, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}, {VKB_B_CBUF, VKB_NCBUF, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
                {VKB_B_SBUF, VKB_NSBUF, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {VKB_B_TEX, VKB_NTEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                {VKB_B_IMG, VKB_NIMG, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}};
            for (unsigned k = 0; k < sizeof r / sizeof *r; k++)
                for (uint32_t i = 0; i < r[k].count; i++) {
                    b[n] = (VkDescriptorSetLayoutBinding){base + r[k].off + i, r[k].type, 1, sf, NULL};
                    fl[n++] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;   // only what the shaders use is written
                }
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                          .bindingCount = n, .pBindingFlags = fl};
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &fi, .bindingCount = n, .pBindings = b};
        if (vkb_db_on()) { ci.pNext = NULL; ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT; }   // vkb_desc.c
        if (!vk_check(vk.vkCreateDescriptorSetLayout(vkc.device, &ci, NULL, &g_layout[compute]), "descriptor set layout")) die("layout");
        VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_layout[compute]};
        if (!vk_check(vk.vkCreatePipelineLayout(vkc.device, &pi, NULL, &g_playout[compute]), "pipeline layout")) die("layout");
    }
    if (vkb_db_on()) vkb_db_layouts(g_layout);
}
VkDescriptorSetLayout vkb_set_layout(int compute) { if (!g_layout[0]) make_layouts(); return g_layout[compute]; }
VkPipelineLayout vkb_pipeline_layout(int compute) { if (!g_layout[0]) make_layouts(); return g_playout[compute]; }

// ---------------- the reflection (<key>.res.json): a small reader for its fixed shape ----------------
static const char *json_array(const char *j, const char *name) {   // just after the '[' of "name": [
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", name);
    const char *p = strstr(j, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), '[');
    return p ? p + 1 : NULL;
}
// next {...} object of an array: its bounds; NULL at the closing ']'
static const char *json_next(const char *p, const char **end) {
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') return NULL;
    int depth = 0;
    for (const char *q = p; *q; q++) {
        if (*q == '{') depth++;
        else if (*q == '}' && !--depth) { *end = q + 1; return p; }
    }
    return NULL;
}
static long json_int(const char *obj, const char *end, const char *key, long dflt) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return dflt;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (!strncmp(p, "true", 4)) return 1;
    if (!strncmp(p, "false", 5)) return 0;
    return strtol(p, NULL, 10);
}
static long json_top_int(const char *j, const char *key, long dflt) { return json_int(j, j + strlen(j), key, dflt); }

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    if (b) b[len] = 0;
    fclose(f);
    if (n) *n = (size_t)len;
    return b;
}

// ---------------- shaders by key ----------------
typedef struct shentry { char key[65]; int c1_set, prewarmed, loading; vkb_shader sh; struct shentry *next; } shentry;   // prewarmed: made ahead (prewarm_thread); loading: being made (vkb_shader_get)
static shentry *g_tab[4096];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_loaded_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_loaded, g_missing;

static BCRYPT_ALG_HANDLE g_sha;   // opened once (opening it per hash cost more than the hash)
static void sha_open(void) { if (BCryptOpenAlgorithmProvider(&g_sha, BCRYPT_SHA256_ALGORITHM, NULL, 0)) die("SHA-256 provider"); }
static void sha256_hex(const void *a, size_t na, const void *b, size_t nb, char out[65]) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, sha_open);
    BCRYPT_ALG_HANDLE alg = g_sha;
    BCRYPT_HASH_HANDLE h;
    uint8_t md[32];
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) die("SHA-256 provider");
    BCryptHashData(h, (PUCHAR)a, (ULONG)na, 0);
    BCryptHashData(h, (PUCHAR)b, (ULONG)nb, 0);
    BCryptFinishHash(h, md, sizeof md, 0);
    BCryptDestroyHash(h);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hex[md[i] >> 4]; out[2 * i + 1] = hex[md[i] & 15]; }
    out[64] = 0;
}

static const char *cache_dir(void) {
    static const char *d;
    if (!d) { d = getenv("TOTK_SHADER_DIR"); if (!d) d = "data/shaders/ryujinx"; }
    return d;
}

// ---------------- the shader pack (<cache>/shaders.pack, tools/pack_shaders.py) ----------------
// One file instead of a .spv and a .res.json per shader (~57,000 files: slow to copy, back up and sync). Layout,
// little-endian: "UTSHPK01", u32 count, u32 0, then count entries sorted by key {u8 key[32] (the SHA-256); u64 offset;
// u32 SPIR-V bytes; u32 reflection bytes} (the SPIR-V at offset, its reflection right after it), then the data.
// Without a pack, or for a key not in it, the loose files are read as before.
typedef struct { uint8_t key[32]; uint64_t off; uint32_t nspv, njson; } pack_entry;
_Static_assert(sizeof(pack_entry) == 48, "shader pack entry layout");
static HANDLE g_pack = INVALID_HANDLE_VALUE;
static pack_entry *g_pack_ix;
static uint32_t g_pack_n;
static int read_at(HANDLE h, uint64_t off, void *buf, uint32_t n) {   // positioned read: safe from several threads
    OVERLAPPED o = {0};
    o.Offset = (DWORD)off;
    o.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    return ReadFile(h, buf, n, &got, &o) && got == n;
}
static void pack_open(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/shaders.pack", cache_dir());
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    uint8_t hd[16];
    uint32_t n = 0;
    if (read_at(h, 0, hd, sizeof hd) && !memcmp(hd, "UTSHPK01", 8)) memcpy(&n, hd + 8, 4);
    pack_entry *ix = n && n < (1u << 24) ? malloc((size_t)n * sizeof *ix) : NULL;
    if (!ix || !read_at(h, sizeof hd, ix, n * (uint32_t)sizeof *ix)) {
        VLOG("%s: not a shader pack, the loose files are used", path);
        free(ix);
        CloseHandle(h);
        return;
    }
    g_pack = h;
    g_pack_ix = ix;
    g_pack_n = n;
    VLOG("shader pack: %u shaders in %s", n, path);
}
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int key_bytes(const char *key, uint8_t k[32]) {   // the 64 hex digits of a key as its 32 bytes
    for (int i = 0; i < 32; i++) {
        int hi = hexval(key[2 * i]), lo = hi < 0 ? -1 : hexval(key[2 * i + 1]);
        if (lo < 0) return 0;
        k[i] = (uint8_t)(hi << 4 | lo);
    }
    return 1;
}
static int pack_read(const char *key, char **json, uint32_t **spv, size_t *nspv) {
    uint8_t k[32];
    if (!key_bytes(key, k)) return 0;
    size_t lo = 0, hi = g_pack_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = memcmp(g_pack_ix[mid].key, k, 32);
        if (c < 0) { lo = mid + 1; continue; }
        if (c > 0) { hi = mid; continue; }
        const pack_entry *e = &g_pack_ix[mid];
        char *s = malloc((size_t)e->nspv + 1), *j = malloc((size_t)e->njson + 1);
        if (!s || !j || !read_at(g_pack, e->off, s, e->nspv) || !read_at(g_pack, e->off + e->nspv, j, e->njson)) {
            free(s);
            free(j);
            return 0;
        }
        s[e->nspv] = 0;
        j[e->njson] = 0;
        *spv = (uint32_t *)s;
        *json = j;
        *nspv = e->nspv;
        return 1;
    }
    return 0;
}
// A shader's translation (SPIR-V) and reflection (JSON), malloc'd and NUL-terminated: from the pack, else the loose files.
static pthread_once_t g_pack_once = PTHREAD_ONCE_INIT;
static void read_translation(const char *key, char **json, uint32_t **spv, size_t *nspv) {
    pthread_once(&g_pack_once, pack_open);
    if (g_pack_n && strlen(key) >= 64 && pack_read(key, json, spv, nspv)) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.res.json", cache_dir(), key);
    *json = read_file(path, NULL);
    snprintf(path, sizeof path, "%s/%s.spv", cache_dir(), key);
    *spv = (uint32_t *)read_file(path, nspv);
}

// The translator declares gl_PerVertex with ClipDistance[8] and CullDistance[1]: 9 > maxCombinedClipAndCullDistances (8)
// on NVIDIA, though no shader writes the cull distance. The CullDistance member (the block's last) is dropped: its
// OpTypeStruct operand, OpMemberName and OpMemberDecorate. Returns the new word count.
static size_t drop_cull_distance(uint32_t *w, size_t n) {
    uint32_t st = 0, mem = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) return n;
        if (op == 72 && wc == 5 && w[i + 3] == 11 && w[i + 4] == 4) { st = w[i + 1]; mem = w[i + 2]; }   // BuiltIn CullDistance
        i += wc;
    }
    if (!st) return n;
    size_t o = 5;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        int drop = (op == 72 || op == 6) && wc >= 3 && w[i + 1] == st && w[i + 2] == mem;   // OpMemberDecorate / OpMemberName
        if (op == 30 && w[i + 1] == st && wc - 2 == mem + 1) {   // OpTypeStruct: the member is the last operand
            memmove(&w[o], &w[i], (wc - 1) * 4);
            w[o] = ((wc - 1) << 16) | 30;
            o += wc - 1;
        } else if (!drop) {
            memmove(&w[o], &w[i], wc * 4);
            o += wc;
        }
        i += wc;
    }
    return o;
}

// ---------------- compiled shaders kept between runs ----------------
// [Windows port] Creating a shader object from SPIR-V compiles it in the driver, a few ms each and hundreds at once where
// a place is first seen: freezes of 0.2 to 2 s in play (2026-10-27). Every shader made from SPIR-V gives its compiled
// binary (vkGetShaderBinaryDataEXT), appended to <cache>/binaries-<the driver's shaderBinaryUUID>.bin; a later run
// creates it from that binary, which is not compiled again. A new driver (another UUID or binary version) starts a new
// file. Layout: "UTSHBN01", u32 shaderBinaryVersion, u32 0, then records {u8 key[32]; u32 size; u32 0; data[size]}.
// A record cut short (the game closed while writing) ends the file there. TOTK_SHADER_BINARIES=0: off.
typedef struct { uint8_t key[32]; uint64_t off; uint32_t size; } bin_entry;
static HANDLE g_bin = INVALID_HANDLE_VALUE;
static bin_entry *g_bin_e;
static uint32_t g_bin_n, g_bin_cap, *g_bin_slot, g_bin_slots;   // open-addressing index into g_bin_e (+1; 0 = empty)
static uint64_t g_bin_hits;
static pthread_mutex_t g_bin_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t bin_hash(const uint8_t k[32]) { uint32_t h; memcpy(&h, k, 4); return h; }   // the key is a SHA-256
static void bin_index(uint32_t i) {   // g_bin_mu held (or before any thread)
    if (2 * (g_bin_n + 1) > g_bin_slots) {
        uint32_t n = g_bin_slots ? g_bin_slots * 2 : 1u << 16, *s = calloc(n, sizeof *s);
        if (!s) die("shader binary index");
        for (uint32_t j = 0; j < g_bin_slots; j++)
            if (g_bin_slot[j]) { uint32_t h = bin_hash(g_bin_e[g_bin_slot[j] - 1].key) & (n - 1); while (s[h]) h = (h + 1) & (n - 1); s[h] = g_bin_slot[j]; }
        free(g_bin_slot);
        g_bin_slot = s;
        g_bin_slots = n;
    }
    uint32_t h = bin_hash(g_bin_e[i].key) & (g_bin_slots - 1);
    while (g_bin_slot[h]) h = (h + 1) & (g_bin_slots - 1);
    g_bin_slot[h] = i + 1;
    g_bin_n++;
}
static int bin_add_entry(const uint8_t k[32], uint64_t off, uint32_t size) {   // g_bin_mu held
    if (g_bin_n == g_bin_cap) {
        g_bin_cap = g_bin_cap ? g_bin_cap * 2 : 1u << 14;
        g_bin_e = realloc(g_bin_e, g_bin_cap * sizeof *g_bin_e);
        if (!g_bin_e) die("shader binary index");
    }
    memcpy(g_bin_e[g_bin_n].key, k, 32);
    g_bin_e[g_bin_n].off = off;
    g_bin_e[g_bin_n].size = size;
    bin_index(g_bin_n);
    return 1;
}
static const bin_entry *bin_find_k(const uint8_t k[32]) {   // g_bin_mu held
    if (!g_bin_slots) return NULL;
    for (uint32_t h = bin_hash(k) & (g_bin_slots - 1); g_bin_slot[h]; h = (h + 1) & (g_bin_slots - 1))
        if (!memcmp(g_bin_e[g_bin_slot[h] - 1].key, k, 32)) return &g_bin_e[g_bin_slot[h] - 1];
    return NULL;
}
static void bin_open(void) {
    const char *e = getenv("TOTK_SHADER_BINARIES");
    if (e && *e == '0') return;
    char path[1024], uuid[33];
    for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", vkc.so.shaderBinaryUUID[i]);
    snprintf(path, sizeof path, "%s/binaries-%s.bin", cache_dir(), uuid);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { VLOG("shader binaries: cannot open %s, shaders are compiled every run", path); return; }
    LARGE_INTEGER size;
    GetFileSizeEx(h, &size);
    uint8_t hd[16], want[16] = "UTSHBN01";
    memcpy(want + 8, &vkc.so.shaderBinaryVersion, 4);
    uint64_t end = 16;
    if (size.QuadPart < 16 || !read_at(h, 0, hd, 16) || memcmp(hd, want, 16)) {   // new, or another binary version: start over
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        SetEndOfFile(h);
        DWORD w;
        WriteFile(h, want, 16, &w, NULL);
    } else {
        uint8_t rh[40];
        while (end + 40 <= (uint64_t)size.QuadPart && read_at(h, end, rh, 40)) {
            uint32_t n;
            memcpy(&n, rh + 32, 4);
            if (end + 40 + n > (uint64_t)size.QuadPart) break;
            bin_add_entry(rh, end + 40, n);
            end += 40 + n;
        }
    }
    g_bin = h;
    VLOG("shader binaries: %u kept from earlier runs (%.0f MiB, %s)", g_bin_n, end / 1048576.0, path);
}
static pthread_once_t g_bin_once = PTHREAD_ONCE_INIT;
static void bin_store(const char *key, VkShaderEXT obj) {
    uint8_t k[32];
    if (g_bin == INVALID_HANDLE_VALUE || !key_bytes(key, k)) return;
    size_t n = 0;
    if (vk.vkGetShaderBinaryDataEXT(vkc.device, obj, &n, NULL) != VK_SUCCESS || !n || n > (64u << 20)) return;
    uint8_t *rec = malloc(40 + n);
    if (!rec) return;
    if (vk.vkGetShaderBinaryDataEXT(vkc.device, obj, &n, rec + 40) != VK_SUCCESS) { free(rec); return; }
    uint32_t size = (uint32_t)n, zero = 0;
    memcpy(rec, k, 32);
    memcpy(rec + 32, &size, 4);
    memcpy(rec + 36, &zero, 4);
    pthread_mutex_lock(&g_bin_mu);
    if (!bin_find_k(k)) {   // the game and the compiler process append to one file: at its end, under a file lock
        OVERLAPPED lk = {0}, o = {0};
        LARGE_INTEGER end;
        if (LockFileEx(g_bin, LOCKFILE_EXCLUSIVE_LOCK, 0, 16, 0, &lk)) {
            DWORD w = 0;
            if (GetFileSizeEx(g_bin, &end)) {
                o.Offset = (DWORD)end.QuadPart;
                o.OffsetHigh = (DWORD)(end.QuadPart >> 32);
                if (WriteFile(g_bin, rec, (DWORD)(40 + n), &w, &o) && w == 40 + n) {
                    bin_add_entry(k, (uint64_t)end.QuadPart + 40, size);
                }
            }
            UnlockFileEx(g_bin, 0, 16, 0, &lk);
        }
    }
    pthread_mutex_unlock(&g_bin_mu);
    free(rec);
}
static int bin_have(const char *key) {
    uint8_t k[32];
    if (g_bin == INVALID_HANDLE_VALUE || !key_bytes(key, k)) return 0;
    pthread_mutex_lock(&g_bin_mu);
    int have = bin_find_k(k) != NULL;
    pthread_mutex_unlock(&g_bin_mu);
    return have;
}

// key: the shader's key, for the binaries kept between runs (NULL: a variant, always compiled)
/* [Windows port] The translator writes a gather with four offsets (Maxwell TLD4 with per-texel offsets: GLSL
   textureGatherOffsets) as OpImageGather / OpImageDrefGather with ConstOffsets whose offsets are module-level
   OpConstantComposites of the registers' OpLoads (ids defined later, inside the function): invalid SPIR-V, on which
   NVIDIA's compiler crashes (shaders b940aa86..., dfe7662f..., 2026-10-27). Rewritten as drivers do for
   textureGatherOffsets: those "constants" are dropped, and each such gather becomes four gathers with one Offset each
   (ImageGatherExtended), offset k built where the gather is (OpCompositeConstruct of the same values), texel k from
   gather k. A composite constant inside a function becomes OpCompositeConstruct. *spvp may be replaced (malloc'd);
   returns the new word count. */
static size_t fix_gather_offsets(uint32_t **spvp, size_t n) {
    uint32_t *w = *spvp, bound = w[3];
    size_t first_fn = 0, fixes = 0, dropped = 0;
    int has_ext = 0;
    uint32_t *vec_of = calloc(bound, sizeof *vec_of), *def_at = calloc(bound, sizeof *def_at);
    uint8_t *is_const = calloc(bound, 1), *bad = calloc(bound, 1);   // module constants; module "constants" of non-constants
    if (!vec_of || !def_at || !is_const || !bad) die("SPIR-V rewrite");
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) break;
        if (op == 17 && w[i + 1] == 25) has_ext = 1;   // OpCapability ImageGatherExtended
        if (op == 54 && !first_fn) first_fn = i;       // OpFunction
        if (op == 23 && w[i + 1] < bound) vec_of[w[i + 1]] = w[i + 2];   // OpTypeVector: component type
        if (!first_fn && (op == 41 || op == 42 || op == 43 || op == 44 || op == 46) && wc >= 3 && w[i + 2] < bound) {
            is_const[w[i + 2]] = 1;
            def_at[w[i + 2]] = (uint32_t)i;
        }
        i += wc;
    }
    for (int changed = 1; changed;) {   // a composite of anything but constants is not a constant (transitively)
        changed = 0;
        for (uint32_t id = 0; id < bound; id++) {
            if (!is_const[id] || bad[id] || (w[def_at[id]] & 0xffff) != 44) continue;
            uint32_t at = def_at[id], wc = w[at] >> 16;
            for (uint32_t k = 3; k < wc; k++)
                if (w[at + k] >= bound || !is_const[w[at + k]] || bad[w[at + k]]) { bad[id] = 1; changed = 1; break; }
        }
    }
    for (uint32_t id = 0; id < bound; id++) dropped += bad[id];
    size_t cap = n + 2 + 4096, o = 0;
    uint32_t *r = malloc(cap * 4);
    if (!r) die("SPIR-V rewrite");
    memcpy(r, w, 5 * 4);
    o = 5;
    if (dropped && !has_ext) { r[o++] = (2u << 16) | 17; r[o++] = 25; }
    size_t changed = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) break;
        if (o + 128 > cap) { cap *= 2; if (!(r = realloc(r, cap * 4))) die("SPIR-V rewrite"); }
        if (op == 44 && (!first_fn || i < first_fn) && w[i + 2] < bound && bad[w[i + 2]]) { i += wc; changed++; continue; }
        if (op == 44 && first_fn && i > first_fn) {   // a "constant" inside a function: a composite of what it holds
            memcpy(r + o, w + i, wc * 4);
            r[o] = (wc << 16) | 80;
            o += wc; i += wc; changed++;
            continue;
        }
        uint32_t offs = (op == 96 || op == 97) && wc == 8 && w[i + 6] == 0x20 ? w[i + 7] : 0;
        if (offs && offs < bound && bad[offs] && (w[def_at[offs]] >> 16) == 7 && w[i + 1] < bound && vec_of[w[i + 1]]) {
            uint32_t ty = w[i + 1], res = w[i + 2], si = w[i + 3], coord = w[i + 4], comp = w[i + 5], sc = vec_of[ty], x[4];
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t in = w[def_at[offs] + 3 + k], e = in, g = bound++;
                if (in < bound && bad[in] && (w[def_at[in]] >> 16) == 5) {   // offset k: the ivec2 of its two values, here
                    e = bound++;
                    r[o++] = (5u << 16) | 80; r[o++] = w[def_at[in] + 1]; r[o++] = e; r[o++] = w[def_at[in] + 3]; r[o++] = w[def_at[in] + 4];
                }
                x[k] = bound++;
                r[o++] = (8u << 16) | op; r[o++] = ty; r[o++] = g; r[o++] = si; r[o++] = coord; r[o++] = comp;   // gather with it
                r[o++] = 0x10; r[o++] = e;
                r[o++] = (5u << 16) | 81; r[o++] = sc; r[o++] = x[k]; r[o++] = g; r[o++] = k;                 // its texel k
            }
            r[o++] = (7u << 16) | 80; r[o++] = ty; r[o++] = res;
            for (int k = 0; k < 4; k++) r[o++] = x[k];
            i += wc; changed++; fixes++;
            continue;
        }
        memcpy(r + o, w + i, wc * 4);
        o += wc; i += wc;
    }
    if (changed) {
        r[3] = bound;
        free(*spvp);
        *spvp = r;
        n = o;
        static int said;
        if (said++ < 16) VLOG("shader: %zu four-offset gather(s) rewritten, %zu invalid constant(s) dropped", fixes, dropped);
    } else free(r);
    free(vec_of); free(def_at); free(is_const); free(bad);
    return n;
}

static VkResult create_object(const char *key, VkShaderStageFlagBits stage, const uint32_t *spv, size_t bytes, VkShaderEXT *obj) {
    pthread_once(&g_bin_once, bin_open);
    VkDescriptorSetLayout layout = vkb_set_layout(stage == VK_SHADER_STAGE_COMPUTE_BIT);
    VkShaderCreateInfoEXT ci = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = stage,
        .nextStage = stage == VK_SHADER_STAGE_VERTEX_BIT ? VK_SHADER_STAGE_FRAGMENT_BIT : 0,
        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT, .codeSize = bytes, .pCode = spv, .pName = "main",
        .setLayoutCount = 1, .pSetLayouts = &layout};
    uint8_t k[32];
    if (key && g_bin != INVALID_HANDLE_VALUE && key_bytes(key, k)) {
        pthread_mutex_lock(&g_bin_mu);
        const bin_entry *b = bin_find_k(k);
        bin_entry be = b ? *b : (bin_entry){{0}, 0, 0};
        pthread_mutex_unlock(&g_bin_mu);
        void *data = b ? malloc(be.size) : NULL;
        if (data && read_at(g_bin, be.off, data, be.size)) {
            VkShaderCreateInfoEXT bi = ci;
            bi.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT;
            bi.codeSize = be.size;
            bi.pCode = data;
            VkResult r = vk.vkCreateShadersEXT(vkc.device, 1, &bi, NULL, obj);
            free(data);
            if (r == VK_SUCCESS) { __atomic_fetch_add(&g_bin_hits, 1, __ATOMIC_RELAXED); return r; }
        } else free(data);
    }
    VkResult r = vk.vkCreateShadersEXT(vkc.device, 1, &ci, NULL, obj);
    if (r == VK_SUCCESS && key) bin_store(key, *obj);
    return r;
}

// A vertex shader for integer attributes at the locations in intmask (signed where sintmask has the bit): those
// inputs read the raw bits (vkb_spv.c). Made on first use and kept; 0 masks give the shader itself.
// [temporary] made during play (a compile on first use); the variant list and building ahead of time - phase 5.
VkShaderEXT vkb_shader_variant(const vkb_shader *cs, uint32_t intmask, uint32_t sintmask) {
    vkb_shader *s = (vkb_shader *)cs;
    intmask &= s->in_mask;
    sintmask &= intmask;
    if (!intmask || !s->spv) return s->obj;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < s->nvariants; i++)
        if (s->variants[i].intmask == intmask && s->variants[i].sintmask == sintmask) { pthread_mutex_unlock(&g_mu); return s->variants[i].obj; }
    pthread_mutex_unlock(&g_mu);
    uint32_t *out = NULL;
    size_t nw = 0;
    VkShaderEXT obj = VK_NULL_HANDLE;
    if (vkb_spv_int_inputs(s->spv, s->nspv_words, intmask, sintmask, &out, &nw)) {
        if (getenv("TOTK_SPV_DUMP")) {   // development: the variant's SPIR-V for spirv-val / spirv-dis
            char path[256];
            snprintf(path, sizeof path, "%s/%.16s-i%x-s%x.spv", getenv("TOTK_SPV_DUMP"), s->key ? s->key : "x", intmask, sintmask);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(out, 4, nw, f); fclose(f); }
        }
        if (create_object(NULL, VK_SHADER_STAGE_VERTEX_BIT, out, nw * 4, &obj) != VK_SUCCESS) obj = VK_NULL_HANDLE;
        free(out);
    }
    if (!obj) {
        static int said;
        if (said++ < 8) VLOG("[temporary] shader %.12s: no variant for integer inputs %#x: the float reading stays", s->key ? s->key : "?", intmask);
        obj = s->obj;
    } else {
        static int made;
        if (made++ < 16) VLOG("shader %.12s: variant for integer inputs %#x (signed %#x)", s->key ? s->key : "?", intmask, sintmask);
    }
    pthread_mutex_lock(&g_mu);
    if (s->nvariants < VKB_VARIANTS) s->variants[s->nvariants++] = (vkb_variant){intmask, sintmask, obj};
    pthread_mutex_unlock(&g_mu);
    return obj;
}

// Reads the reflection, rewrites the SPIR-V bindings into the universal layout, creates the shader object.
static int load(vkb_shader *s, const char *key, int nvn_stage) {
    char *j = NULL;
    uint32_t *spv = NULL;
    size_t nspv = 0;
    read_translation(key, &j, &spv, &nspv);
    if (!j || !spv || nspv < 20 || (nspv & 3) || spv[0] != 0x07230203) { free(j); free(spv); return 0; }
    s->stage = nvn_stage;
    uint32_t base = nvn_stage == 1 ? VKB_STAGE_BINDINGS : 0;
    uint32_t remap[256];   // translator binding -> universal binding (UINT32_MAX: unknown)
    for (int i = 0; i < 256; i++) remap[i] = UINT32_MAX;
    remap[0] = base + VKB_B_SUPPORT;
    const char *p, *e;
    // constant buffers: slot = NVN constant buffer index
    if ((p = json_array(j, "cbufs")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), slot = json_int(p, e, "slot", -1);
            if (b < 0 || b > 255 || slot < 0 || slot >= VKB_NCBUF) continue;
            remap[b] = base + VKB_B_CBUF + (uint32_t)slot;
            s->cbuf_mask |= 1u << slot;
        }
    // storage buffers: the address comes from the NVN driver bank c0, 16 bytes per slot: the stage's storage buffer
    // table (vertex 0x110, fragment 0x510, compute 0x310) or, for compute, its uniform buffer table (0x210): TOTK's
    // compute shaders also use uniform buffers as storage (MacNativeGames' "ubo_ptr")
    int sb_base = nvn_stage == 0 ? 0x110 : nvn_stage == 1 ? 0x510 : 0x310;
    if ((p = json_array(j, "sbufs")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), off = json_int(p, e, "cb_offset", -1) * 4;
            if (b < 0 || b > 255 || s->nsbuf >= VKB_NSBUF) { s->unsupported = 1; continue; }
            long cb = json_int(p, e, "cb_slot", 0);
            int write = (json_int(p, e, "flags", 0) & 1) != 0;   // Ryujinx BufferUsageFlags.Write
            if (write) s->stores = 1;
            int slot = -1, kind = VKB_SB_TABLE;
            if (cb == 0 && off >= sb_base && off < sb_base + 0x100 && !(off & 15)) slot = (int)(off - sb_base) / 16;
            else if (cb == 0 && nvn_stage == 5 && off >= 0x210 && off < 0x310 && !(off & 15)) { slot = (int)(off - 0x210) / 16; kind = VKB_SB_UBO; }
            else if (cb >= 3 && cb <= 16 && off >= 0 && off < 0x10000) { slot = (int)cb - 3; kind = VKB_SB_UBO_PTR; }
            if (slot < 0 || slot >= VKB_NSBUF) {
                static int said; if (said++ < 8) VLOG("shader %s: storage buffer address from c%ld+%#lx: unknown source", key, cb, off);
                slot = 0xFF;
            }
            remap[b] = base + VKB_B_SBUF + (uint32_t)s->nsbuf;
            s->sbuf[s->nsbuf].slot = (uint8_t)slot;
            s->sbuf[s->nsbuf].kind = (uint8_t)kind;
            s->sbuf[s->nsbuf].write = (uint8_t)write;
            s->sbuf[s->nsbuf++].off = (uint16_t)(kind == VKB_SB_UBO_PTR ? off : 0);
        }
    // textures: the handle is a word offset into the NVN driver bank c2 - combined textures at 0x20 (8 bytes per slot),
    // separate textures at 0x168 (MacNativeGames' totk_xlate Service.cs)
    if ((p = json_array(j, "textures")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), h = json_int(p, e, "handle", -1) * 4, type = json_int(p, e, "type", 3);
            if (b < 0 || b > 255 || s->ntex >= VKB_NTEX || h < 0x20) { s->unsupported = 1; continue; }
            vkb_tex_ref *t = &s->tex[s->ntex];
            t->separate = h >= 0x168;
            t->slot = (uint8_t)((h - (t->separate ? 0x168 : 0x20)) / 8);
            t->type = (uint16_t)type;
            remap[b] = base + VKB_B_TEX + (uint32_t)s->ntex++;
        }
    if ((p = json_array(j, "images")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), h = json_int(p, e, "handle", -1) * 4, type = json_int(p, e, "type", 3);
            if (b < 0 || b > 255 || s->nimg >= VKB_NIMG || h < 0x120) { s->unsupported = 1; continue; }
            s->img[s->nimg] = (vkb_tex_ref){(uint8_t)((h - 0x120) / 8), 0, (uint16_t)type};
            s->stores = 1;   // [temporary] images are taken as written (the reflection has no read-only mark)
            remap[b] = base + VKB_B_IMG + (uint32_t)s->nimg++;
        }
    s->frag_outputs = (uint32_t)json_top_int(j, "frag_out_map", 0);
    free(j);
    // rewrite Binding decorations (OpDecorate %id Binding n: opcode 71, decoration 33)
    size_t nw = drop_cull_distance(spv, nspv / 4);
    nw = fix_gather_offsets(&spv, nw);
    nspv = nw * 4;
    for (size_t i = 5; i < nw;) {
        uint32_t op = spv[i] & 0xffff, wc = spv[i] >> 16;
        if (!wc || i + wc > nw) break;
        if (op == 71 && wc == 4 && spv[i + 2] == 33) {
            uint32_t old = spv[i + 3];
            if (old > 255 || remap[old] == UINT32_MAX) {
                VLOG("shader %s: binding %u not in its reflection", key, old);
                s->unsupported = 1;
            } else spv[i + 3] = remap[old];
        }
        i += wc;
    }
    VkShaderStageFlagBits stage = nvn_stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : nvn_stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    VkResult r = create_object(key, stage, spv, nspv, &s->obj);
    if (r == VK_SUCCESS) {   // input / output locations: vertex variants; fragment inputs no vertex shader writes (audit)
        s->in_mask = vkb_spv_input_mask(spv, nspv / 4);
        s->out_mask = vkb_spv_output_mask(spv, nspv / 4);
        uint32_t reads = vkb_spv_sbuf_reads(spv, nspv / 4, base + VKB_B_SBUF, s->nsbuf);
        for (int i = 0; i < s->nsbuf; i++) s->sbuf[i].read = (uint8_t)(reads >> i & 1);
    }
    if (stage == VK_SHADER_STAGE_VERTEX_BIT && r == VK_SUCCESS) {   // kept for attribute variants (vkb_shader_variant)
        s->spv = spv;
        s->nspv_words = nspv / 4;
    } else free(spv);
    if (r != VK_SUCCESS) { VLOG("shader %s: vkCreateShadersEXT %d", key, (int)r); return 0; }
    if (getenv("TOTK_VK_VALIDATION")) VLOG("shader %.12s = VkShaderEXT %#llx", key, (unsigned long long)(uintptr_t)s->obj);   // names validation messages
    s->vk_stage = stage;
    return 1;
}

// ---- shaders made ahead: creating a shader object compiles it in the driver, ~17% of the recording thread on the way
// into the world, where new shaders arrive in bursts (submissions held up to 2.3 s, Great Sky Island, 2026-10-24).
// Every shader loaded is listed (<cache>/used.txt: key and NVN stage, appended); at the first shader lookup of a run,
// PREWARM_THREADS low-priority threads create the listed ones that are not loaded yet. The first run fills the list;
// later runs find those shaders made (as SavageWind's pipeline list). The constants c1 come from the game's program
// at the first real lookup (c1_set).
enum { PREWARM_THREADS = 2 };
static uint64_t g_prewarm_made, g_prewarm_hits;
uint64_t vkb_shader_load_ns;   // shaders made when first drawn with (long submission log, backend_vk.c)
static char *g_list;   // used.txt, read once
static size_t g_list_n;
static _Atomic size_t g_list_at;
static void list_add(const char *key, int stage) {   // g_mu held
    char path[1024];
    snprintf(path, sizeof path, "%s/used.txt", cache_dir());
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, "%s %d\n", key, stage);
    fclose(f);
}
/* [Windows port] Every shader of the pack compiled once, ahead, in another process: `UltraTears --compile-shaders <game
   pid>` (started by the game while <cache>/binaries-<uuid>.done is missing) runs `--compile-shaders-child`, which compiles
   each shader that has no kept binary yet at below-normal priority and keeps its binary (the object is destroyed: only
   the binary is wanted). A driver that crashes compiling a shader takes only that process down (seen once with NVIDIA,
   2026-10-27, on one of the ~28,700 shaders): the supervisor puts the shader each thread was compiling
   (<cache>/compiling-<n>.txt) on <cache>/bad-shaders.txt and starts it again. It stops with the game (the next run goes
   on where it stopped) and writes the .done file when the pack is through. Log: <cache>/shader-compile.log.
   TOTK_SHADER_PREWARM=list: only the list above, nothing compiled ahead. */
static _Atomic uint32_t g_pack_at;
static _Atomic uint64_t g_pack_made;
static char *g_bad;   // bad-shaders.txt: keys never compiled ahead (65 bytes a line)
static size_t g_bad_n;
static int spv_nvn_stage(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc) break;
        if (op == 15 && wc >= 3) return w[i + 1] == 0 ? 0 : w[i + 1] == 4 ? 1 : w[i + 1] == 5 ? 5 : -1;   // OpEntryPoint
        i += wc;
    }
    return -1;
}
static int is_bad(const char *key) {
    for (size_t at = 0; at + 64 <= g_bad_n; at += 65) if (!memcmp(g_bad + at, key, 64)) return 1;
    return 0;
}
static void done_path(char *path, size_t n) {
    char uuid[33];
    for (int i = 0; i < 16; i++) snprintf(uuid + 2 * i, 3, "%02x", vkc.so.shaderBinaryUUID[i]);
    snprintf(path, n, "%s/binaries-%s.done", cache_dir(), uuid);
}
static void *compile_thread(void *arg) {
    int me = (int)(intptr_t)arg;
    char mine[1024];
    snprintf(mine, sizeof mine, "%s/compiling-%d.txt", cache_dir(), me);
    uint64_t t0 = runtime_now_ns();
    for (uint32_t i; (i = atomic_fetch_add(&g_pack_at, 1)) < g_pack_n;) {
        static const char hex[] = "0123456789abcdef";
        char key[65];
        for (int b = 0; b < 32; b++) { key[2 * b] = hex[g_pack_ix[i].key[b] >> 4]; key[2 * b + 1] = hex[g_pack_ix[i].key[b] & 15]; }
        key[64] = 0;
        if (bin_have(key) || is_bad(key)) continue;
        char *j = NULL;
        uint32_t *spv = NULL;
        size_t nspv = 0;
        read_translation(key, &j, &spv, &nspv);
        int stage = spv && nspv >= 20 ? spv_nvn_stage(spv, nspv / 4) : -1;
        free(j);
        free(spv);
        FILE *f = fopen(mine, "wb");   // what this thread compiles: on a crash, the supervisor skips it next time
        if (f) { fprintf(f, "%s\n", key); fclose(f); }
        vkb_shader tmp = {0};
        tmp.key = key;
        if (stage < 0 || !load(&tmp, key, stage)) continue;
        vk.vkDestroyShaderEXT(vkc.device, tmp.obj, NULL);
        free(tmp.spv);
        uint64_t n = atomic_fetch_add(&g_pack_made, 1) + 1;
        if (n % 1000 == 0) VLOG("shaders compiled ahead: %llu (%.0f s), %u of %u looked at", (unsigned long long)n, (runtime_now_ns() - t0) / 1e9, i + 1, g_pack_n);
    }
    remove(mine);
    return NULL;
}
static void *watch_game(void *arg) {   // the child ends with the game
    HANDLE g = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)(uintptr_t)arg);
    if (g) WaitForSingleObject(g, INFINITE);
    ExitProcess(3);
    return NULL;
}
static void log_to_cache(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s/shader-compile.log", cache_dir());
    if (freopen(path, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
}
static int child_main(DWORD game) {
    log_to_cache();
    if (!vk_init(NULL)) return 2;
    pthread_once(&g_pack_once, pack_open);
    pthread_once(&g_bin_once, bin_open);
    if (g_bin == INVALID_HANDLE_VALUE || !g_pack_n) return 2;
    char path[1024];
    snprintf(path, sizeof path, "%s/bad-shaders.txt", cache_dir());
    g_bad = read_file(path, &g_bad_n);
    if (!g_bad) g_bad_n = 0;
    pthread_t w;
    if (game) pthread_create(&w, NULL, watch_game, (void *)(uintptr_t)game);
    const char *e = getenv("TOTK_SHADER_COMPILE_THREADS");
    int n = e && atoi(e) > 0 ? atoi(e) : 4;
    pthread_t t[16];
    if (n > 16) n = 16;
    VLOG("compiling the shader pack ahead: %u shaders, %u with binaries, %zu skipped as bad, %d threads", g_pack_n, g_bin_n, g_bad_n / 65, n);
    for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, compile_thread, (void *)(intptr_t)i);
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    done_path(path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (f) fclose(f);
    VLOG("shader pack compiled ahead: %llu new binaries (%u kept in all)", (unsigned long long)atomic_load(&g_pack_made), g_bin_n);
    return 0;
}
static int supervisor_main(DWORD game) {
    log_to_cache();
    HANDLE one = CreateMutexA(NULL, TRUE, "UltraTears-shader-compile");
    if (!one || GetLastError() == ERROR_ALREADY_EXISTS) return 0;   // another one is at it
    char exe[MAX_PATH], cmd[MAX_PATH + 64], path[1024];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    for (int run = 0; run < 64; run++) {
        snprintf(cmd, sizeof cmd, "\"%s\" --compile-shaders-child %lu", exe, (unsigned long)game);
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) return 1;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (code == 0 || code == 2 || code == 3) return 0;   // done, nothing to do, or the game ended
        fprintf(stderr, "[totk] shader compiler process ended with %#lx: the shaders it was compiling are skipped from now on\n", (unsigned long)code);
        snprintf(path, sizeof path, "%s/bad-shaders.txt", cache_dir());
        FILE *bad = fopen(path, "ab");
        for (int i = 0; i < 16; i++) {
            char mine[1024], key[80] = "";
            snprintf(mine, sizeof mine, "%s/compiling-%d.txt", cache_dir(), i);
            FILE *f = fopen(mine, "rb");
            if (!f) continue;
            if (fgets(key, sizeof key, f) && strlen(key) >= 64 && bad) { fprintf(bad, "%.64s\n", key); fprintf(stderr, "[totk]   %.64s\n", key); }
            fclose(f);
            remove(mine);
        }
        if (bad) fclose(bad);
    }
    return 1;
}
int vkb_shader_cache_main(int argc, char **argv) {
    DWORD game = argc > 2 ? (DWORD)strtoul(argv[2], NULL, 10) : 0;
    return !strcmp(argv[1], "--compile-shaders-child") ? child_main(game) : supervisor_main(game);
}
static void compile_ahead_start(void) {   // the game: start the supervisor while the pack is not through
    const char *e = getenv("TOTK_SHADER_PREWARM");
    char path[1024];
    if (e && !strcmp(e, "list")) return;
    done_path(path, sizeof path);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    char exe[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" --compile-shaders %lu", exe, (unsigned long)GetCurrentProcessId());
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        VLOG("shaders: the pack is compiled ahead by another process (log: %s/shader-compile.log)", cache_dir());
    }
}
static void *prewarm_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-shader-prewarm");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        size_t at = atomic_fetch_add(&g_list_at, 67);   // one line: 64 hex digits, space, stage, newline (67 bytes)
        if (at + 67 > g_list_n) break;
        const char *l = g_list + at;
        if (l[64] != ' ' || l[66] != '\n') continue;
        char key[65];
        memcpy(key, l, 64); key[64] = 0;
        int stage = l[65] - '0';
        uint32_t h = (uint32_t)strtoul(key + 56, NULL, 16) & 4095;
        pthread_mutex_lock(&g_mu);
        shentry *e = g_tab[h];
        while (e && strcmp(e->key, key)) e = e->next;
        pthread_mutex_unlock(&g_mu);
        if (e) continue;
        shentry *n = calloc(1, sizeof *n);
        if (!n) die("shader entry");
        memcpy(n->key, key, sizeof key);
        n->sh.key = n->key;
        if (!load(&n->sh, key, stage)) { free(n); continue; }
        n->prewarmed = 1;
        pthread_mutex_lock(&g_mu);
        e = g_tab[h];
        while (e && strcmp(e->key, key)) e = e->next;
        if (!e) { n->next = g_tab[h]; g_tab[h] = n; g_prewarm_made++; g_loaded++; }
        pthread_mutex_unlock(&g_mu);
        if (e) { vk.vkDestroyShaderEXT(vkc.device, n->sh.obj, NULL); free(n->sh.spv); free(n); }   // loaded meanwhile
    }
    static _Atomic int left = PREWARM_THREADS;
    if (atomic_fetch_sub(&left, 1) == 1) VLOG("shaders made ahead from the list: %llu", (unsigned long long)g_prewarm_made);
    return NULL;
}
static void prewarm_start(void) {
    const char *e = getenv("TOTK_SHADER_PREWARM");   // =0: shaders are made only when first drawn with (A/B)
    if (e && *e == '0') return;
    compile_ahead_start();
    char path[1024];
    snprintf(path, sizeof path, "%s/used.txt", cache_dir());
    size_t n = 0;
    g_list = read_file(path, &n);   // none yet: the threads go straight to the pack (pack_compile)
    g_list_n = g_list ? n : 0;
    VLOG("shader list: %zu shaders to make ahead", g_list_n / 67);
    for (int i = 0; i < PREWARM_THREADS; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, prewarm_thread, NULL) == 0) pthread_detach(t);
    }
}

const vkb_shader *vkb_shader_get(const NVNshaderData *sd) {
    const uint8_t *control = sd->control;
    if (!control || !sd->data) return NULL;
    uint32_t length, stage;
    memcpy(&length, control + 0x704, 4);
    memcpy(&stage, control + 0x714, 4);
    if (!length || length > (16u << 20) || (stage != 0 && stage != 1 && stage != 5)) return NULL;
    char key[65];
    sha256_hex(control, 0x880, (const void *)(uintptr_t)sd->data, length, key);
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, prewarm_start);
    uint32_t h = (uint32_t)strtoul(key + 56, NULL, 16) & 4095;
    pthread_mutex_lock(&g_mu);
    shentry *e = g_tab[h];
    while (e && strcmp(e->key, key)) e = e->next;
    if (!e) {
        /* [Windows port] made outside g_mu: a compile no longer holds up the other recording threads' lookups (each
           waited for every shader any thread was compiling). A thread wanting this shader meanwhile waits for it. */
        e = calloc(1, sizeof *e);
        if (!e) die("shader entry");
        memcpy(e->key, key, sizeof key);
        e->sh.key = e->key;
        e->loading = 1;
        e->next = g_tab[h];
        g_tab[h] = e;
        pthread_mutex_unlock(&g_mu);
        uint64_t t0 = runtime_now_ns();
        int ok = load(&e->sh, key, (int)stage);
        __atomic_fetch_add(&vkb_shader_load_ns, runtime_now_ns() - t0, __ATOMIC_RELAXED);
        pthread_mutex_lock(&g_mu);
        if (ok) {
            g_loaded++;
            list_add(key, (int)stage);
        }
        else {
            e->sh.obj = VK_NULL_HANDLE;
            if (g_missing++ < 16) VLOG("shader %s (stage %u): no translation in %s", key, stage, cache_dir());
        }
        e->loading = 0;
        pthread_cond_broadcast(&g_loaded_cv);
    }
    while (e->loading) pthread_cond_wait(&g_loaded_cv, &g_mu);
    if (e->sh.obj && !e->c1_set) {   // loaded now or ahead (prewarm): the constants come with the game's program
        e->c1_set = 1;
        if (e->prewarmed) g_prewarm_hits++;
        uint32_t c1off;
        memcpy(&c1off, control + 0x700, 4);   // the shader's constants: the code's tail (c1)
        if (c1off < length) {   // copied: the program's memory may be freed and reused while the key stays cached
            uint8_t *c = malloc(length - c1off);
            if (!c) die("shader constants");
            memcpy(c, (const uint8_t *)(uintptr_t)sd->data + c1off, length - c1off);
            e->sh.c1 = c;
            e->sh.c1_size = length - c1off;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return e->sh.obj ? &e->sh : NULL;
}

void vkb_shader_stats(uint64_t *loaded, uint64_t *missing) { *loaded = g_loaded; *missing = g_missing; }
