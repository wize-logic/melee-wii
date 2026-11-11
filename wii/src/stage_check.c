/* Is a modded stage file the original stage with another look? (for
 * mod_stages.c; wii/tools/stage_check.py is the same comparison, and says
 * what is compared and why.)
 *
 * The two files are walked together by their structures. Everything must be
 * equal but what is only looks: a joint's display objects and render-pass
 * flags, material and shape animations, lights, fog, shadows, the particles'
 * textures. The songs in the stage's rows may differ too: they are returned,
 * for the original's to be given to the game in their place.
 *
 * No platform's headers: it is built for the PC too, to be tested against
 * the Python one on every file. */

#include "stage_check.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned int u32;

#define JOINT_LOOKS (0x80u | 0x100u | 0x10000u | 0x40000u | 0x80000u | 0x100000u | 0x70000000u)
#define JOINT_PTCL 0x20u
#define JOINT_SPLINE 0x4000u
#define ROW 0x64u
#define ROW_SONGS 4u
#define ROW_SONGS_END 0x18u

enum { K_WHOLE = 1, K_JOINT, K_AOBJ, K_FOBJ, K_ANIMJOINT, K_ROBJANIM, K_ARTICLE, K_STATES, K_ITEMMODEL };

typedef struct Dat {
    const u8* d; /* the data */
    u32 size;
    u8* rel; /* a bit per word: a pointer */
    u32* targets;
    u32 target_count;
    const u8* roots; /* {offset, name} each */
    u32 root_count;
    const char* names;
    u32 names_size;
} Dat;

#define STACK 8192
typedef struct Walk {
    Dat a, b;
    /* the pairs walked: for the original's word, the modded file's it was
     * walked with (+ 1) and as what */
    u32* seen;
    u8* seen_kind;
    u32 (*stack)[2];
    StagePatch* patches;
    int patch_max, patch_count;
    const char* why;
    jmp_buf fail;
} Walk;

#define FAIL(w, text) ((w)->why = (text), longjmp((w)->fail, 1))

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

static int compare_u32(const void* a, const void* b)
{
    u32 x = *(const u32*) a, y = *(const u32*) b;
    return x < y ? -1 : x > y;
}

/* 0, or -1: not a stage file (or no memory) */
static int dat_open(Dat* f, const u8* raw, u32 raw_size)
{
    u32 size, dsize, nrel, nroot, nref, table, names, n = 0;
    memset(f, 0, sizeof(*f));
    if (raw_size < 0x20) {
        return -1;
    }
    size = be32(raw);
    dsize = be32(raw + 4);
    nrel = be32(raw + 8);
    nroot = be32(raw + 12);
    nref = be32(raw + 16);
    if (size != raw_size || dsize > raw_size - 0x20 || (dsize & 3) != 0 || nrel > raw_size / 4 || nroot > 4096 ||
        nref > 4096)
    {
        return -1;
    }
    table = 0x20 + dsize;
    names = table + 4 * nrel + 8 * (nroot + nref);
    if (names > raw_size) {
        return -1;
    }
    f->d = raw + 0x20;
    f->size = dsize;
    f->roots = raw + table + 4 * nrel;
    f->root_count = nroot;
    f->names = (const char*) raw + names;
    f->names_size = raw_size - names;
    f->rel = calloc(1, dsize / 32 + 1);
    f->targets = malloc((nrel + nroot + 1) * sizeof(u32));
    if (f->rel == NULL || f->targets == NULL) {
        return -1;
    }
    for (u32 i = 0; i < nrel; i++) {
        u32 at = be32(raw + table + 4 * i);
        if ((at & 3) != 0 || at > dsize - 4 || dsize < 4) {
            return -1;
        }
        f->rel[at / 32] |= (u8) (1u << (at / 4 % 8));
        if (be32(f->d + at) <= dsize) {
            f->targets[n++] = be32(f->d + at);
        }
    }
    for (u32 i = 0; i < nroot; i++) {
        u32 off = be32(f->roots + 8 * i), name = be32(f->roots + 8 * i + 4);
        if (off >= dsize || name >= f->names_size || memchr(f->names + name, 0, f->names_size - name) == NULL) {
            return -1;
        }
        f->targets[n++] = off;
    }
    f->targets[n++] = dsize;
    qsort(f->targets, n, sizeof(u32), compare_u32);
    f->target_count = n;
    return 0;
}

static void dat_close(Dat* f)
{
    free(f->rel);
    free(f->targets);
}

static const char* root_name(const Dat* f, u32 i)
{
    return f->names + be32(f->roots + 8 * i + 4);
}

static int find_root(const Dat* f, const char* name, u32* off)
{
    for (u32 i = 0; i < f->root_count; i++) {
        if (strcmp(root_name(f, i), name) == 0) {
            *off = be32(f->roots + 8 * i);
            return 1;
        }
    }
    return 0;
}

static void need(Walk* w, const Dat* f, u32 off, u32 n)
{
    if (off > f->size || n > f->size - off) {
        FAIL(w, "a structure runs past the file");
    }
}

static u32 word(Walk* w, const Dat* f, u32 off)
{
    need(w, f, off, 4);
    return be32(f->d + off);
}

static int is_ptr(const Dat* f, u32 off)
{
    return (off & 3) == 0 && off < f->size && (f->rel[off / 32] >> (off / 4 % 8) & 1) != 0;
}

/* The pointers at oa and ob: 1 with both (in *pa, *pb), 0 with neither */
static int ptrs(Walk* w, u32 oa, u32 ob, u32* pa, u32* pb, const char* what)
{
    int ia, ib;
    need(w, &w->a, oa, 4);
    need(w, &w->b, ob, 4);
    ia = is_ptr(&w->a, oa);
    ib = is_ptr(&w->b, ob);
    if (ia != ib) {
        FAIL(w, what);
    }
    if (!ia) {
        return 0;
    }
    *pa = be32(w->a.d + oa);
    *pb = be32(w->b.d + ob);
    if (*pa >= w->a.size || *pb >= w->b.size) {
        FAIL(w, "a structure runs past the file");
    }
    return 1;
}

/* The same, for a structure: at a word's start (text may be anywhere) */
static int struct_ptrs(Walk* w, u32 oa, u32 ob, u32* pa, u32* pb, const char* what)
{
    if (!ptrs(w, oa, ob, pa, pb, what)) {
        return 0;
    }
    if (((*pa | *pb) & 3) != 0) {
        FAIL(w, what);
    }
    return 1;
}

/* 1 when this pair is to be walked: not walked yet. A word of the original
 * walked before with another word of the modded file, or as something else,
 * is two structures where the original has one. */
static int first(Walk* w, int kind, u32 pa, u32 pb, const char* what)
{
    if (w->seen[pa / 4] != 0) {
        if (w->seen[pa / 4] != pb + 1) {
            FAIL(w, what);
        }
        if (w->seen_kind[pa / 4] == kind) {
            return 0;
        }
        /* (a block compared whole, then by its structure, or the reverse:
         * walked again) */
    }
    w->seen[pa / 4] = pb + 1;
    w->seen_kind[pa / 4] = (u8) kind;
    return 1;
}

/* n bytes equal, and pointers at the same places */
static void same(Walk* w, u32 oa, u32 ob, u32 n, const char* what)
{
    need(w, &w->a, oa, n);
    need(w, &w->b, ob, n);
    if (memcmp(w->a.d + oa, w->b.d + ob, n) != 0) {
        FAIL(w, what);
    }
    for (u32 i = 0; i + 4 <= n; i += 4) {
        if (is_ptr(&w->a, oa + i) != is_ptr(&w->b, ob + i)) {
            FAIL(w, what);
        }
    }
}

/* n bytes equal, whatever they are (an array's or a track's data) */
static void same_bytes(Walk* w, u32 oa, u32 ob, u32 n, const char* what)
{
    need(w, &w->a, oa, n);
    need(w, &w->b, ob, n);
    if (memcmp(w->a.d + oa, w->b.d + ob, n) != 0) {
        FAIL(w, what);
    }
}

/* Where the data at `off` ends, as far as the file says: the next address
 * something points at */
static u32 block_end(const Dat* f, u32 off)
{
    u32 lo = 0, hi = f->target_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (f->targets[mid] <= off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < f->target_count ? f->targets[lo] : f->size;
}

/* A part of unknown structure: equal block by block, through its pointers */
static void whole(Walk* w, u32 pa, u32 pb, const char* what)
{
    u32 base = 0, top = 0;
    /* (called while another walk's entries are on the stack: above them) */
    while (base < STACK && w->stack[base][0] != 0xFFFFFFFFu) {
        base++;
    }
    top = base;
#define PUSH(x, y) \
    do { \
        if (top + 1 >= STACK) { \
            FAIL(w, "too deep a structure"); \
        } \
        w->stack[top][0] = (x); \
        w->stack[top][1] = (y); \
        top++; \
        w->stack[top][0] = 0xFFFFFFFFu; \
    } while (0)
    PUSH(pa, pb);
    while (top > base) {
        u32 n;
        top--;
        pa = w->stack[top][0];
        pb = w->stack[top][1];
        w->stack[top][0] = 0xFFFFFFFFu;
        n = block_end(&w->a, pa) - pa;
        if (n != block_end(&w->b, pb) - pb) {
            FAIL(w, what);
        }
        if (((pa | pb) & 3) != 0) {
            /* text, or bytes: no pointer in it */
            if ((pa & 3) != (pb & 3) || memcmp(w->a.d + pa, w->b.d + pb, n) != 0) {
                FAIL(w, what);
            }
            for (u32 i = (4 - (pa & 3)) & 3; i + 4 <= n; i += 4) {
                if (is_ptr(&w->a, pa + i) || is_ptr(&w->b, pb + i)) {
                    FAIL(w, what);
                }
            }
            continue;
        }
        if (!first(w, K_WHOLE, pa, pb, what)) {
            continue;
        }
        for (u32 i = 0; i + 4 <= n; i += 4) {
            u32 qa, qb;
            if (ptrs(w, pa + i, pb + i, &qa, &qb, what)) {
                PUSH(qa, qb);
            } else if (memcmp(w->a.d + pa + i, w->b.d + pb + i, 4) != 0) {
                FAIL(w, what);
            }
        }
        if (memcmp(w->a.d + pa + (n & ~3u), w->b.d + pb + (n & ~3u), n & 3) != 0) {
            FAIL(w, what);
        }
    }
}

static void joint(Walk* w, u32 pa, u32 pb, const char* what)
{
    static const u32 DATA[] = { 0, 0x38, 0x3C }; /* its class's name, its matrix, its constraints */
    u32 base = 0, top;
    while (base < STACK && w->stack[base][0] != 0xFFFFFFFFu) {
        base++;
    }
    top = base;
    PUSH(pa, pb);
    while (top > base) {
        u32 fa, fb, qa, qb;
        top--;
        pa = w->stack[top][0];
        pb = w->stack[top][1];
        w->stack[top][0] = 0xFFFFFFFFu;
        if (!first(w, K_JOINT, pa, pb, what)) {
            continue;
        }
        fa = word(w, &w->a, pa + 4);
        fb = word(w, &w->b, pb + 4);
        if ((fa | JOINT_LOOKS) != (fb | JOINT_LOOKS)) {
            FAIL(w, what);
        }
        same_bytes(w, pa + 0x14, pb + 0x14, 0x24, what);
        /* (the entries pushed below stay under what these walks push) */
        for (u32 i = 0; i < 2; i++) {
            if (struct_ptrs(w, pa + 8 + 4 * i, pb + 8 + 4 * i, &qa, &qb, what)) {
                PUSH(qa, qb);
            }
        }
        for (u32 i = 0; i < sizeof(DATA) / sizeof(DATA[0]); i++) {
            if (struct_ptrs(w, pa + DATA[i], pb + DATA[i], &qa, &qb, what)) {
                whole(w, qa, qb, what);
            }
        }
        if ((fa & (JOINT_PTCL | JOINT_SPLINE)) != 0) {
            if (struct_ptrs(w, pa + 0x10, pb + 0x10, &qa, &qb, what)) {
                whole(w, qa, qb, what);
            }
        }
        /* (else its display objects: looks) */
    }
}

/* An HSD_AObjDesc: its tracks, and the joint it moves */
static void anim_object(Walk* w, u32 pa, u32 pb, const char* what)
{
    u32 fa, fb, qa, qb;
    if (!first(w, K_AOBJ, pa, pb, what)) {
        return;
    }
    same(w, pa, pb, 8, what);
    if (struct_ptrs(w, pa + 8, pb + 8, &fa, &fb, what)) {
        for (;;) {
            u32 n;
            if (!first(w, K_FOBJ, fa, fb, what)) {
                break;
            }
            same_bytes(w, fa + 4, fb + 4, 12, what);
            n = word(w, &w->a, fa + 4);
            if (struct_ptrs(w, fa + 0x10, fb + 0x10, &qa, &qb, what)) {
                same_bytes(w, qa, qb, n, what);
            }
            if (!struct_ptrs(w, fa, fb, &qa, &qb, what)) {
                break;
            }
            fa = qa;
            fb = qb;
        }
    }
    if (struct_ptrs(w, pa + 0xC, pb + 0xC, &qa, &qb, what)) {
        joint(w, qa, qb, what);
    }
}

static void anim_joint(Walk* w, u32 pa, u32 pb, const char* what)
{
    u32 base = 0, top;
    while (base < STACK && w->stack[base][0] != 0xFFFFFFFFu) {
        base++;
    }
    top = base;
    PUSH(pa, pb);
    while (top > base) {
        u32 qa, qb, ra, rb;
        top--;
        pa = w->stack[top][0];
        pb = w->stack[top][1];
        w->stack[top][0] = 0xFFFFFFFFu;
        if (!first(w, K_ANIMJOINT, pa, pb, what)) {
            continue;
        }
        if (word(w, &w->a, pa + 0x10) != word(w, &w->b, pb + 0x10)) {
            FAIL(w, what);
        }
        for (u32 i = 0; i < 2; i++) {
            if (struct_ptrs(w, pa + 4 * i, pb + 4 * i, &qa, &qb, what)) {
                PUSH(qa, qb);
            }
        }
        if (struct_ptrs(w, pa + 8, pb + 8, &qa, &qb, what)) {
            anim_object(w, qa, qb, what);
        }
        if (struct_ptrs(w, pa + 0xC, pb + 0xC, &ra, &rb, what)) {
            while (first(w, K_ROBJANIM, ra, rb, what)) {
                if (struct_ptrs(w, ra + 4, rb + 4, &qa, &qb, what)) {
                    anim_object(w, qa, qb, what);
                }
                if (!struct_ptrs(w, ra, rb, &qa, &qb, what)) {
                    break;
                }
                ra = qa;
                rb = qb;
            }
        }
    }
}
#undef PUSH

/* A list of joint animations ended by a null (or none); how many */
static u32 anim_list(Walk* w, u32 oa, u32 ob, const char* what)
{
    u32 pa, pb, qa, qb, count = 0;
    if (!struct_ptrs(w, oa, ob, &pa, &pb, what)) {
        return 0;
    }
    while (struct_ptrs(w, pa + 4 * count, pb + 4 * count, &qa, &qb, what)) {
        anim_joint(w, qa, qb, what);
        count++;
    }
    if (word(w, &w->a, pa + 4 * count) != 0 || word(w, &w->b, pb + 4 * count) != 0) {
        FAIL(w, what);
    }
    return count;
}

/* The arrays at the pointers at oa and ob: `size` bytes equal */
static void array(Walk* w, u32 oa, u32 ob, u32 size, const char* what)
{
    u32 pa, pb;
    if (struct_ptrs(w, oa, ob, &pa, &pb, what) && size != 0) {
        same(w, pa, pb, size, what);
    }
}

/* A joint with its animations (DynamicModelDesc); how many animations */
static u32 model(Walk* w, u32 pa, u32 pb, const char* what)
{
    u32 ja, jb;
    if (struct_ptrs(w, pa, pb, &ja, &jb, what)) {
        joint(w, ja, jb, what);
    }
    return anim_list(w, pa + 4, pb + 4, what);
}

static void collision(Walk* w, u32 pa, u32 pb)
{
    static const u32 COUNTS[] = { 4, 0xC, 0x28, 0x2C };
    static const u32 ARRAYS[][3] = { { 0, 4, 8 }, { 8, 0xC, 0x10 }, { 0x24, 0x28, 0x28 } };
    const char* what = "the collision";
    for (u32 i = 0; i < 4; i++) {
        if (word(w, &w->a, pa + COUNTS[i]) != word(w, &w->b, pb + COUNTS[i])) {
            FAIL(w, what);
        }
    }
    same(w, pa + 0x10, pb + 0x10, 0x14, what);
    for (u32 i = 0; i < 3; i++) {
        u32 count = word(w, &w->a, pa + ARRAYS[i][1]);
        if (count > 0x100000) {
            FAIL(w, what);
        }
        array(w, pa + ARRAYS[i][0], pb + ARRAYS[i][0], count * ARRAYS[i][2], what);
    }
}

static void parameters(Walk* w, u32 pa, u32 pb)
{
    const char* what = "the stage's parameters";
    u32 ra, rb, rows;
    same(w, pa, pb, 0xB0, what);
    same(w, pa + 0xB4, pb + 0xB4, 0x28, what);
    if (!struct_ptrs(w, pa + 0xB0, pb + 0xB0, &ra, &rb, what)) {
        return;
    }
    rows = word(w, &w->a, pa + 0xB4);
    if (rows > 64) {
        FAIL(w, what);
    }
    for (u32 i = 0; i < rows; i++) {
        u32 oa = ra + i * ROW, ob = rb + i * ROW;
        same(w, oa, ob, ROW_SONGS, what);
        same(w, oa + ROW_SONGS_END, ob + ROW_SONGS_END, ROW - ROW_SONGS_END, what);
        need(w, &w->a, oa, ROW);
        need(w, &w->b, ob, ROW);
        if (memcmp(w->a.d + oa + ROW_SONGS, w->b.d + ob + ROW_SONGS, ROW_SONGS_END - ROW_SONGS) != 0) {
            if (w->patch_count >= w->patch_max) {
                FAIL(w, what);
            }
            /* (in the file: its data starts 0x20 in) */
            w->patches[w->patch_count].offset = 0x20 + ob + ROW_SONGS;
            memcpy(w->patches[w->patch_count].bytes, w->a.d + oa + ROW_SONGS, ROW_SONGS_END - ROW_SONGS);
            w->patch_count++;
        }
    }
}

static void map_head(Walk* w, u32 pa, u32 pb)
{
    static const u32 COUNTS[] = { 4, 0xC, 0x14 };
    u32 ea, eb, qa, qb, count;
    for (u32 i = 0; i < 3; i++) {
        if (word(w, &w->a, pa + COUNTS[i]) != word(w, &w->b, pb + COUNTS[i])) {
            FAIL(w, "the map");
        }
    }
    /* the points: a joint each, and pairs of joint numbers */
    count = word(w, &w->a, pa + 4);
    if (struct_ptrs(w, pa, pb, &ea, &eb, "the map's points")) {
        if (count > 4096) {
            FAIL(w, "the map's points");
        }
        for (u32 i = 0; i < count; i++) {
            u32 oa = ea + i * 0xC, ob = eb + i * 0xC, pairs = word(w, &w->a, oa + 8);
            if (pairs != word(w, &w->b, ob + 8) || pairs > 0x10000) {
                FAIL(w, "the map's points");
            }
            if (struct_ptrs(w, oa, ob, &qa, &qb, "the map's points")) {
                joint(w, qa, qb, "the map's points");
            }
            array(w, oa + 4, ob + 4, pairs * 4, "the map's points");
        }
    }
    /* the models */
    count = word(w, &w->a, pa + 0xC);
    if (struct_ptrs(w, pa + 8, pb + 8, &ea, &eb, "the map's models")) {
        if (count > 4096) {
            FAIL(w, "the map's models");
        }
        for (u32 i = 0; i < count; i++) {
            const char* what = "one of the map's models";
            u32 oa = ea + i * 0x34, ob = eb + i * 0x34, anims = model(w, oa, ob, what);
            for (u32 off = 0x10; off <= 0x14; off += 4) { /* its camera and the camera's animation */
                if (struct_ptrs(w, oa + off, ob + off, &qa, &qb, what)) {
                    whole(w, qa, qb, "one of the map's cameras");
                }
            }
            if (word(w, &w->a, oa + 0x24) != word(w, &w->b, ob + 0x24) ||
                word(w, &w->a, oa + 0x30) != word(w, &w->b, ob + 0x30) || word(w, &w->a, oa + 0x24) > 0x10000 ||
                word(w, &w->a, oa + 0x30) > 0x10000)
            {
                FAIL(w, what);
            }
            array(w, oa + 0x20, ob + 0x20, word(w, &w->a, oa + 0x24) * 6, what);
            /* (how each animation loops: bytes, not words) */
            if (struct_ptrs(w, oa + 0x28, ob + 0x28, &qa, &qb, what)) {
                same_bytes(w, qa, qb, anims, what);
            }
            array(w, oa + 0x2C, ob + 0x2C, word(w, &w->a, oa + 0x30) * 2, what);
            /* (+8, +C: material and shape animations; +18, +1C: lights, fog) */
        }
    }
    /* the splines */
    count = word(w, &w->a, pa + 0x14);
    if (struct_ptrs(w, pa + 0x10, pb + 0x10, &ea, &eb, "the map's splines")) {
        if (count > 4096) {
            FAIL(w, "the map's splines");
        }
        for (u32 i = 0; i < count; i++) {
            if (struct_ptrs(w, ea + 4 * i, eb + 4 * i, &qa, &qb, "the map's splines")) {
                whole(w, qa, qb, "the map's splines");
            }
        }
    }
    /* (+18: lights; +20: shadows; +28: the materials shadows fall on) */
}

/* itemdata: a list of the stage's own items, each a kind and its data (an
 * Article) */
static void items(Walk* w, u32 pa, u32 pb)
{
    static const u32 WHOLE[] = { 0, 4, 8, 0x14 }; /* its attributes, its own, its hurtboxes, its physics */
    const char* what = "the stage's items";
    u32 n = block_end(&w->a, pa) - pa;
    if (n != block_end(&w->b, pb) - pb) {
        FAIL(w, what);
    }
    for (u32 i = 0; i + 4 <= n; i += 4) {
        u32 ea, eb, ra, rb, qa, qb, sa, sb;
        if (!struct_ptrs(w, pa + i, pb + i, &ea, &eb, what)) {
            if (word(w, &w->a, pa + i) != word(w, &w->b, pb + i)) {
                FAIL(w, what);
            }
            continue;
        }
        if (word(w, &w->a, ea) != word(w, &w->b, eb)) {
            FAIL(w, what);
        }
        if (!struct_ptrs(w, ea + 4, eb + 4, &ra, &rb, what) || !first(w, K_ARTICLE, ra, rb, what)) {
            continue;
        }
        for (u32 k = 0; k < 4; k++) {
            if (struct_ptrs(w, ra + WHOLE[k], rb + WHOLE[k], &qa, &qb, what)) {
                whole(w, qa, qb, what);
            }
        }
        /* its states: a joint animation, a material animation (looks),
         * parameters, a script */
        if (struct_ptrs(w, ra + 0xC, rb + 0xC, &sa, &sb, what) && first(w, K_STATES, sa, sb, what)) {
            u32 size = block_end(&w->a, sa) - sa;
            if (size != block_end(&w->b, sb) - sb || (size & 0xF) != 0) {
                FAIL(w, what);
            }
            for (u32 s = 0; s < size; s += 0x10) {
                for (u32 off = 0; off < 0x10; off += 4) {
                    if (!struct_ptrs(w, sa + s + off, sb + s + off, &qa, &qb, what)) {
                        if (word(w, &w->a, sa + s + off) != word(w, &w->b, sb + s + off)) {
                            FAIL(w, what);
                        }
                    } else if (off == 0) {
                        anim_joint(w, qa, qb, what);
                    } else if (off != 4) {
                        whole(w, qa, qb, what);
                    }
                }
            }
        }
        /* its model: a joint, and three words */
        if (struct_ptrs(w, ra + 0x10, rb + 0x10, &qa, &qb, what) && first(w, K_ITEMMODEL, qa, qb, what)) {
            u32 ja, jb;
            same(w, qa + 4, qb + 4, 0xC, what);
            if (struct_ptrs(w, qa, qb, &ja, &jb, what)) {
                joint(w, ja, jb, what);
            }
        }
    }
}

static int ends_with(const char* s, const char* end)
{
    size_t n = strlen(s), m = strlen(end);
    return n >= m && strcmp(s + n - m, end) == 0;
}

static int is_looks(const char* name)
{
    return ends_with(name, "_image") || ends_with(name, "_tlut") || ends_with(name, "_tlut_desc") ||
           ends_with(name, "_image_desc");
}

static void walk(Walk* w)
{
    for (u32 i = 0; i < w->a.root_count; i++) {
        const char* name = root_name(&w->a, i);
        u32 pa = be32(w->a.roots + 8 * i), pb;
        if (!find_root(&w->b, name, &pb)) {
            /* (the game finds its files' parts by these names) */
            FAIL(w, "a part is missing");
        }
        if (strcmp(name, "coll_data") == 0) {
            collision(w, pa, pb);
        } else if (strcmp(name, "grGroundParam") == 0) {
            parameters(w, pa, pb);
        } else if (strcmp(name, "map_head") == 0) {
            map_head(w, pa, pb);
        } else if (strcmp(name, "itemdata") == 0) {
            items(w, pa, pb);
        } else if (strcmp(name, "quake_model_set") == 0) {
            model(w, pa, pb, "the quake's model");
        } else if (ends_with(name, "_matanim_joint") || ends_with(name, "_shapeanim_joint")) {
            /* looks */
        } else if (ends_with(name, "_animjoint")) {
            anim_joint(w, pa, pb, "an animation");
        } else if (ends_with(name, "_joint")) {
            joint(w, pa, pb, "a model");
        } else if (strcmp(name, "map_texg") == 0 || strcmp(name, "map_plit") == 0 || is_looks(name)) {
            /* looks */
        } else {
            whole(w, pa, pb, "another part");
        }
    }
}

int stage_check(const unsigned char* original, unsigned int original_size, const unsigned char* modded,
                unsigned int modded_size, StagePatch* patches, int patch_max, int* patch_count, const char** why)
{
    /* (volatile: read after the longjmp) */
    Walk* volatile w = calloc(1, sizeof(Walk));
    volatile int result = -1;
    *patch_count = 0;
    *why = "no memory";
    if (w == NULL) {
        return -1;
    }
    if (dat_open(&w->a, original, original_size) == 0 && dat_open(&w->b, modded, modded_size) == 0) {
        w->seen = calloc(w->a.size / 4 + 1, sizeof(u32));
        w->seen_kind = calloc(w->a.size / 4 + 1, 1);
        w->stack = malloc(STACK * sizeof(w->stack[0]));
        w->patches = patches;
        w->patch_max = patch_max;
        if (w->seen != NULL && w->seen_kind != NULL && w->stack != NULL) {
            w->stack[0][0] = 0xFFFFFFFFu;
            if (setjmp(w->fail) == 0) {
                walk(w);
                result = 0;
                *why = "";
                *patch_count = w->patch_count;
            } else {
                *why = w->why;
            }
        }
    } else {
        *why = "not a stage file";
    }
    free(w->seen);
    free(w->seen_kind);
    free(w->stack);
    dat_close(&w->a);
    dat_close(&w->b);
    free(w);
    return result;
}
