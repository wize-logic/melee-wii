/* Slippi Dolphin's rounding of the fused multiply-adds, for online play.
 *
 * Online, the game runs in lockstep with Slippi Dolphin on PCs: each side
 * simulates every frame itself, and the two must compute the same bits.
 * Slippi Dolphin is Ishiiruka, a 2016 Dolphin, and it does the PowerPC's
 * single-precision fused multiply-adds (fmadds, fmsubs, fnmadds, fnmsubs and
 * the paired singles' ps_madd, ps_msub, ps_nmadd, ps_nmsub, ps_madds0,
 * ps_madds1) as x86's double-precision FMA, then rounds that to single: two
 * roundings, where the Wii's FPU rounds a*c+b to single once. Where the
 * double result lands exactly halfway between two singles the two differ in
 * the last bit (Dolphin's issue 13865, fixed in Dolphin in 2026, not in
 * Slippi's). Operands with short mantissas (50.0, 0.5...) make that common,
 * and one bit is enough for the two sides to drift apart: a desync.
 *
 * x86's FNMADD/FNMSUB also compute -(a*c) +/- b, where the PowerPC computes
 * -(a*c +/- b): the same but for the sign of an exact zero (+0 on the PC,
 * -0 on the Wii), which reaches atan2 and divisions. That holds for the
 * double-precision fnmadd and fnmsub too.
 *
 * So for online play each such instruction of the game's code (and of the
 * Slippi codeset's) goes through a few instructions that compute what
 * Ishiiruka does (melee_wii_fused_install):
 *   - fmadds d,a,c,b  ->  fmadd d,a,c,b; frsp d,d; ps_merge00 d,d,d (the
 *     double FMA is exact where Ishiiruka's is: a and c are singles, their
 *     product fits; the merge puts the result in d's second half too, as
 *     fmadds does)
 *   - fnmadds/fnmsubs ->  a factor negated, fmsub/fmadd, frsp, the factor
 *     back (or, where both factors share a register, a scratch register
 *     saved around it on the stack)
 *   - fnmadd/fnmsub   ->  the same without the frsp
 *   - paired singles  ->  the operands saved on the stack, each half
 *     computed as above, the pair loaded back
 * The plain fmadd/fmsub (double) are the same on both and stay.
 *
 * Where the link register is free (MWCC's non-leaf functions, between the
 * mflr r0 of the prologue and the mtlr of the epilogue) the site becomes a
 * bl to code all sites of that instruction share, ending in blr; elsewhere,
 * a branch to code of its own, which branches back. The code goes where
 * nothing runs or nothing else will go: the bodies of the SDK functions the
 * port replaces (their first 16 bytes jump to the port's), the island's
 * room, what is left of libogc's MEM1 arena. Never the game's arena: the
 * game uses all of it (half a megabyte less, and the character select
 * screen's files no longer fit).
 *
 * sd:/apps/melee/nofused turns it off. sd:/apps/melee/fusedcount (tests,
 * in Dolphin with a MEM1 over 24 MiB) also runs the original instruction at
 * each site and counts where the two results differ, logged after each game
 * (melee_wii_fused_report). */

#include "melee_wii.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <gccore.h>

#include <wl/base/base.h>
#include <wl/platform/platform.h>

/* main.c: the SDK functions the port replaces, and the island */
typedef struct MeleeTramp {
    u32 site;
    void* target;
    u32 size;
} MeleeTramp;
extern const MeleeTramp melee_tramp_table[];
extern u32 __melee_island_start[];
extern u32 __melee_island_end[];
extern void (*const __melee_ctors_start[])(void);
u32* melee_wii_island_next(void);

#define TEXT_LO 0x80005940u

/* ---- instructions ------------------------------------------------------- */

#define OP(o) ((u32) (o) << 26)
/* A-form, written as the assembler takes it: op d,a,c,b */
static u32 af(u32 op, u32 d, u32 a, u32 c, u32 b, u32 xo)
{
    return OP(op) | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1;
}
#define FRSP(d, b) (OP(63) | (u32) (d) << 21 | (u32) (b) << 11 | 12u << 1)
#define FNEG(d, b) (OP(63) | (u32) (d) << 21 | (u32) (b) << 11 | 40u << 1)
/* loads and stores at r1 + disp */
#define AT_R1(op, r, disp) (OP(op) | (u32) (r) << 21 | 1u << 16 | ((u32) (disp) & 0xFFFF))
#define LWZ 32
#define STW 36
#define LFS 48
#define LFD 50
#define STFS 52
#define STFD 54
/* psq_l/psq_st r, disp(r1), 0, 0: a pair of singles (GQR0: floats) */
#define PSQ_L(r, disp) (OP(56) | (u32) (r) << 21 | 1u << 16 | ((u32) (disp) & 0xFFF))
#define PSQ_ST(r, disp) (OP(60) | (u32) (r) << 21 | 1u << 16 | ((u32) (disp) & 0xFFF))
#define STWU_R1(n) (OP(37) | 1u << 21 | 1u << 16 | ((u32) - (n) & 0xFFFF))
#define ADDI_R1(n) (OP(14) | 1u << 21 | 1u << 16 | (u32) (n))
#define MFCR(r) (OP(31) | (u32) (r) << 21 | 19u << 1)
#define MTCR(r) (OP(31) | (u32) (r) << 21 | 0xFFu << 12 | 144u << 1)
#define CMPW(a, b) (OP(31) | (u32) (a) << 16 | (u32) (b) << 11)
#define BNE(disp) (OP(16) | 4u << 21 | 2u << 16 | ((u32) (disp) & 0xFFFC))
#define BEQ(disp) (OP(16) | 12u << 21 | 2u << 16 | ((u32) (disp) & 0xFFFC))
#define LIS(r, v) (OP(15) | (u32) (r) << 21 | ((u32) (v) & 0xFFFF))
#define ADDI(d, a, v) (OP(14) | (u32) (d) << 21 | (u32) (a) << 16 | ((u32) (v) & 0xFFFF))
#define LWZ_AT(d, a, v) (OP(32) | (u32) (d) << 21 | (u32) (a) << 16 | ((u32) (v) & 0xFFFF))
#define STW_AT(s, a, v) (OP(36) | (u32) (s) << 21 | (u32) (a) << 16 | ((u32) (v) & 0xFFFF))
#define BLR 0x4E800020u
/* ps_merge00 d,d,d: ps1 = ps0 */
#define PS_MERGE00(d) (OP(4) | (u32) (d) << 21 | (u32) (d) << 16 | (u32) (d) << 11 | 528u << 1)

static u32 branch(u32 from, u32 to)
{
    return 0x48000000u | ((to - from) & 0x03FFFFFCu);
}

static int in_reach(u32 from, u32 to)
{
    s32 disp = (s32) (to - from);
    return disp >= -0x2000000 && disp < 0x2000000;
}

enum { KIND_NONE, KIND_SINGLE, KIND_DOUBLE, KIND_PAIRED };

/* Which of the instructions above (a record form, with Rc set, is none of
 * the game's, and is how data in the codeset's codes reads: 0xFFFFFFFF) */
static int kind_of(u32 w)
{
    u32 op = w >> 26, xo = (w >> 1) & 31;
    if (w & 1) {
        return KIND_NONE;
    }
    if (op == 59 && xo >= 28) {
        return KIND_SINGLE;
    }
    if (op == 63 && (xo == 30 || xo == 31)) {
        return KIND_DOUBLE;
    }
    if (op == 4 && (xo >= 28 || xo == 14 || xo == 15)) {
        return KIND_PAIRED;
    }
    return KIND_NONE;
}

/* ---- the code at a site ------------------------------------------------- */

#define E(x)                                                                                         \
    do {                                                                                             \
        if (o != NULL) {                                                                             \
            o[n] = (x);                                                                              \
        }                                                                                            \
        n++;                                                                                         \
    } while (0)

/* d = -(a*c) - b (xo 28) or -(a*c) + b (xo 29), rounded once to double, as
 * x86's FNMSUB/FNMADD: a factor negated in its register around an
 * fmsub/fmadd (put back unless d took the register), or a scratch register
 * where the factors share one */
static int emit_neg(u32* o, int n, u32 xo, u32 d, u32 a, u32 c, u32 b)
{
    if (a != c && a != b) {
        E(FNEG(a, a));
        E(af(63, d, a, c, b, xo));
        if (d != a) {
            E(FNEG(a, a));
        }
    } else if (c != a && c != b) {
        E(FNEG(c, c));
        E(af(63, d, a, c, b, xo));
        if (d != c) {
            E(FNEG(c, c));
        }
    } else {
        u32 s = 0;
        while (s == a || s == b || s == c || s == d) {
            s++;
        }
        E(STWU_R1(32));
        E(AT_R1(STFD, s, 8));
        E(PSQ_ST(s, 16));
        E(FNEG(s, a));
        E(af(63, d, s, c, b, xo));
        E(PSQ_L(s, 16));
        E(AT_R1(LFD, s, 8));
        E(ADDI_R1(32));
    }
    return n;
}

/* Ishiiruka's result of w into its d (all else as it was); the number of
 * words, or -1 for a site left as it is */
static int emit_fix(u32* o, u32 w)
{
    u32 op = w >> 26, xo = (w >> 1) & 31;
    u32 d = (w >> 21) & 31, a = (w >> 16) & 31, b = (w >> 11) & 31, c = (w >> 6) & 31;
    int n = 0;
    if (op == 59 || op == 63) {
        if (xo == 28 || xo == 29) {
            E(af(63, d, a, c, b, xo));
        } else {
            n = emit_neg(o, n, xo == 31 ? 28 : 29, d, a, c, b);
        }
        if (op == 59) {
            /* a single-precision instruction leaves its result in both
             * halves of d; on the Wii the double-precision one before the
             * frsp puts its result, cut to a single, in the second, and the
             * frsp rounds only the first */
            E(FRSP(d, d));
            E(PS_MERGE00(d));
        }
        return n;
    }
    {
        /* paired: a, c and b saved, each half loaded (lfs: into both of a
         * register's), computed, stored; the operands loaded back, then d.
         * Shared registers load the same value, but for ps_madds0/1, whose
         * c is one half for both. */
        u32 fused = (xo == 28 || xo == 31) ? 28 : 29, neg = xo == 30 || xo == 31, f = 0;
        u32 ci[2] = { xo == 15 ? 1u : 0u, xo == 14 ? 0u : 1u };
        int k;
        if ((xo == 14 || xo == 15) && (a == c || c == b)) {
            return -1;
        }
        if (neg) {
            if (a != c && a != b) {
                f = a;
            } else if (c != a && c != b) {
                f = c;
            } else {
                return -1;
            }
        }
        E(STWU_R1(48));
        E(PSQ_ST(a, 8));
        E(PSQ_ST(c, 16));
        E(PSQ_ST(b, 24));
        for (k = 0; k < 2; k++) {
            E(AT_R1(LFS, a, 8 + 4 * k));
            E(AT_R1(LFS, c, 16 + 4 * ci[k]));
            E(AT_R1(LFS, b, 24 + 4 * k));
            if (neg) {
                E(FNEG(f, f));
            }
            E(af(63, d, a, c, b, fused));
            E(FRSP(d, d));
            E(AT_R1(STFS, d, 32 + 4 * k));
        }
        E(PSQ_L(a, 8));
        E(PSQ_L(c, 16));
        E(PSQ_L(b, 24));
        E(PSQ_L(d, 32));
        E(ADDI_R1(48));
    }
    return n;
}

/* Tests (fusedcount): the original instruction, then the fix, from the same
 * operands; *counter + 1 where the two differ. The fix's result stays. */
static int emit_count(u32* o, u32 w, u32 counter, u32 c_counter)
{
    u32 d = (w >> 21) & 31, a = (w >> 16) & 31, b = (w >> 11) & 31, c = (w >> 6) & 31;
    int paired = (w >> 26) == 4, n = 0, fix, bne, beq, diff;
    E(STWU_R1(96));
    E(AT_R1(STW, 3, 8));
    E(AT_R1(STW, 4, 12));
    E(MFCR(3));
    E(AT_R1(STW, 3, 16));
    E(paired ? PSQ_ST(a, 24) : AT_R1(STFD, a, 24));
    E(paired ? PSQ_ST(c, 32) : AT_R1(STFD, c, 32));
    E(paired ? PSQ_ST(b, 40) : AT_R1(STFD, b, 40));
    if ((w >> 26) == 59) {
        /* and where c isn't a single (its low 29 bits): Ishiiruka rounds
         * it to 25 bits first */
        E(AT_R1(LWZ, 3, 36));
        E(OP(21) | 3u << 21 | 3u << 16 | 3u << 6 | 31u << 1 | 1u); /* rlwinm. r3,r3,0,3,31 */
        E(BEQ(5 * 4));
        E(LIS(3, (c_counter + 0x8000) >> 16));
        E(LWZ_AT(4, 3, c_counter & 0xFFFF));
        E(ADDI(4, 4, 1));
        E(STW_AT(4, 3, c_counter & 0xFFFF));
    }
    E(w);
    E(paired ? PSQ_ST(d, 48) : AT_R1(STFD, d, 48));
    E(paired ? PSQ_L(a, 24) : AT_R1(LFD, a, 24));
    E(paired ? PSQ_L(c, 32) : AT_R1(LFD, c, 32));
    E(paired ? PSQ_L(b, 40) : AT_R1(LFD, b, 40));
    fix = emit_fix(o != NULL ? o + n : NULL, w);
    if (fix < 0) {
        return -1;
    }
    n += fix;
    E(paired ? PSQ_ST(d, 56) : AT_R1(STFD, d, 56));
    E(AT_R1(LWZ, 3, 48));
    E(AT_R1(LWZ, 4, 56));
    E(CMPW(3, 4));
    bne = n;
    E(0);
    E(AT_R1(LWZ, 3, 52));
    E(AT_R1(LWZ, 4, 60));
    E(CMPW(3, 4));
    beq = n;
    E(0);
    diff = n;
    E(LIS(3, (counter + 0x8000) >> 16));
    E(LWZ_AT(4, 3, counter & 0xFFFF));
    E(ADDI(4, 4, 1));
    E(STW_AT(4, 3, counter & 0xFFFF));
    if (o != NULL) {
        o[bne] = BNE((diff - bne) * 4);
        o[beq] = BEQ((n - beq) * 4);
    }
    E(AT_R1(LWZ, 3, 16));
    E(MTCR(3));
    E(AT_R1(LWZ, 4, 12));
    E(AT_R1(LWZ, 3, 8));
    E(ADDI_R1(96));
    return n;
}

#undef E

/* ---- where the code goes ------------------------------------------------ */

typedef struct Span {
    u32 lo, hi;
} Span;

#define MAX_SPANS 320
static Span s_spans[MAX_SPANS];
static int s_span_count;

static void span_add(u32 lo, u32 hi)
{
    lo = (lo + 3) & ~3u;
    hi &= ~3u;
    if (hi > lo + 8 && s_span_count < MAX_SPANS) {
        s_spans[s_span_count].lo = lo;
        s_spans[s_span_count].hi = hi;
        s_span_count++;
    }
}

static u32 spans_free(void)
{
    u32 total = 0;
    for (int i = 0; i < s_span_count; i++) {
        total += s_spans[i].hi - s_spans[i].lo;
    }
    return total;
}

static u32* take(u32 words, u32 site)
{
    for (int i = 0; i < s_span_count; i++) {
        Span* s = &s_spans[i];
        if (s->hi - s->lo >= words * 4 && in_reach(site, s->lo) && in_reach(s->lo + words * 4 - 4, site + 4)) {
            u32* p = (u32*) s->lo;
            s->lo += words * 4;
            return p;
        }
    }
    return NULL;
}

/* The replaced functions' bodies (past the jump at their start); none that
 * code outside them branches into */
static int replaced_at(u32 address)
{
    for (const MeleeTramp* p = melee_tramp_table; p->site != 0; p++) {
        if (address >= p->site && address < p->site + p->size) {
            return 1;
        }
    }
    return 0;
}

static u32 add_dead_code(void)
{
    static u8 entered[512];
    int count = 0, i;
    u32 total = 0, first = 0xFFFFFFFFu, last = 0;
    for (const MeleeTramp* p = melee_tramp_table; p->site != 0; p++) {
        count++;
        first = p->site < first ? p->site : first;
        last = p->site + p->size > last ? p->site + p->size : last;
    }
    if (count > (int) sizeof(entered)) {
        count = sizeof(entered);
    }
    memset(entered, 0, sizeof(entered));
    for (u32 pc = TEXT_LO; pc < (u32) __melee_ctors_start; pc += 4) {
        u32 w = *(const u32*) pc, op = w >> 26, target;
        if (op == 18 && !(w & 2)) {
            s32 disp = (s32) ((w & 0x03FFFFFCu) << 6) >> 6;
            target = pc + (u32) disp;
        } else if (op == 16 && !(w & 2)) {
            target = pc + (u32) (s32) (s16) (w & 0xFFFC);
        } else {
            continue;
        }
        if (target < first || target >= last) {
            continue;
        }
        for (i = 0; i < count; i++) {
            const MeleeTramp* p = &melee_tramp_table[i];
            if (target >= p->site + 16 && target < p->site + p->size &&
                !(pc >= p->site && pc < p->site + p->size)) {
                entered[i] = 1;
            }
        }
    }
    for (i = 0; i < count; i++) {
        const MeleeTramp* p = &melee_tramp_table[i];
        if (p->size > 16 + 32 && !entered[i]) {
            span_add(p->site + 16, p->site + p->size);
            total += p->size - 16;
        }
    }
    return total;
}

/* ---- the sites ---------------------------------------------------------- */

static int s_installed;
static int s_counting;
static u32* s_counts;
static u32* s_counts_c; /* (where c wasn't a single) */
static u32* s_count_sites;
static int s_count_max, s_count_used;

/* Code shared by the sites of one instruction (each calls it with a bl):
 * where the link register is free */
#define MAX_SHARED 2048
static struct {
    u32 word;
    u32* code;
} s_shared[MAX_SHARED];
static int s_shared_count, s_shared_sites;

#define MFLR_R0 0x7C0802A6u
#define IS_MTLR(w) (((w) & 0xFC1FFFFFu) == 0x7C0803A6u)
#define IS_BCLR(w) (((w) & 0xFC0007FEu) == 0x4C000020u) /* blr, blrl and the conditional ones */

/* Is the link register free at pc: past its function's mflr r0 (which keeps
 * the return address in r0, and on the stack) and before the mtlr that puts
 * it back, with no return between? (MWCC's prologue and epilogue; a leaf
 * function has neither) */
static int lr_free(u32 pc)
{
    u32 a;
    for (a = pc - 4; a >= TEXT_LO; a -= 4) {
        u32 w = *(const u32*) a;
        if (w == MFLR_R0) {
            break;
        }
        if (IS_MTLR(w) || IS_BCLR(w)) {
            return 0;
        }
    }
    if (a < TEXT_LO) {
        return 0;
    }
    for (a = pc + 4; a < (u32) __melee_ctors_start; a += 4) {
        u32 w = *(const u32*) a;
        if (IS_MTLR(w)) {
            return 1;
        }
        if (IS_BCLR(w) || w == MFLR_R0) {
            return 0;
        }
    }
    return 0;
}

/* Has the instruction at `site` (its final address; `word` points to where
 * it is now) go through Ishiiruka's version: a branch to code of its own
 * that branches back, or (`shared`, the link register free there) a bl to
 * the code its instruction's sites share; 0, or -1 where it stays */
static int patch_site(u32* word, u32 site, int shared)
{
    u32 w = *word, counter = 0;
    u32* t = NULL;
    int n;
    if (s_counting) {
        shared = 0; /* (a counter each) */
    }
    if (shared) {
        for (int i = 0; i < s_shared_count && t == NULL; i++) {
            if (s_shared[i].word == w) {
                t = s_shared[i].code;
            }
        }
    }
    if (t == NULL) {
        if (s_counting) {
            if (s_count_used >= s_count_max) {
                return -1;
            }
            counter = (u32) &s_counts[s_count_used];
            n = emit_count(NULL, w, counter, (u32) &s_counts_c[s_count_used]);
        } else {
            n = emit_fix(NULL, w);
        }
        if (n < 0) {
            return -1;
        }
        t = take(n + 1, site);
        if (t == NULL) {
            return -1;
        }
        if (s_counting) {
            emit_count(t, w, counter, (u32) &s_counts_c[s_count_used]);
            s_count_sites[s_count_used++] = site;
        } else {
            emit_fix(t, w);
        }
        t[n] = shared ? BLR : branch((u32) &t[n], site + 4);
        DCFlushRange(t, (n + 1) * 4);
        ICInvalidateRange(t, (n + 1) * 4);
        if (shared && s_shared_count < MAX_SHARED) {
            s_shared[s_shared_count].word = w;
            s_shared[s_shared_count].code = t;
            s_shared_count++;
        }
    }
    if (shared) {
        s_shared_sites++;
    }
    *word = branch(site, (u32) t) | (shared ? 1u : 0u);
    return 0;
}

/* ---- the self-test ------------------------------------------------------ */

/* Each form of the rewrite (the register sharing the game's code has, and
 * more) on 4096 sets of operands chosen to land on halfway cases and exact
 * zeros: the hashes of all of f0-f6 afterwards, both halves (but a double
 * result's second), by the instruction itself and by its rewrite. To match
 * an exact model of the hardware's and of Ishiiruka's rounding. */

static u32 s_rng;

static u32 next(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

typedef union {
    u32 u;
    float f;
} Bits;

static u32 make(u32 sign, u32 exp, u32 mant)
{
    return sign << 31 | exp << 23 | (mant & 0x7FFFFFu);
}

static u32 mant_bits(u32 k)
{
    u32 r = next();
    return (r & ((1u << k) - 1)) << (23 - k);
}

/* a, c, b for one half */
static void gen_slot(float* a, float* c, float* b)
{
    u32 kind = next() % 4, ea = 120 + next() % 15, ec = 115 + next() % 16;
    u32 sa = next() & 1, sc = next() & 1, sb = next() & 1, eb, m;
    Bits A, C, B;
    if (kind == 0 || kind == 3) {
        A.u = make(sa, ea, mant_bits(3));
        m = next();
        C.u = make(sc, ec, m);
        eb = ea + ec - 127 - (kind == 0 ? 55 + next() % 30 : 24 + next() % 8);
        m = next();
        B.u = make(sb, eb, m);
    } else if (kind == 1) {
        A.u = make(sa, ea, mant_bits(4));
        C.u = make(sc, ec, mant_bits(16));
        B.f = A.f * C.f;
        if (sb) {
            B.f = -B.f;
        }
    } else {
        m = next();
        A.u = make(sa, ea, m);
        m = next();
        C.u = make(sc, ec, m);
        eb = ea + ec - 127 + next() % 5 - 2;
        m = next();
        B.u = make(sb, eb, m);
    }
    *a = A.f;
    *c = C.f;
    *b = B.f;
}

static float junk(void)
{
    Bits j;
    u32 s = next() & 1, e = 100 + next() % 50, m = next();
    j.u = make(s, e, m);
    return j.f;
}

#define TEST_CASES 4096
#define TEST_WORDS 128 /* the harness's room, then the rewrite's */

static u32 mix(u32 h, u32 v)
{
    h = (h ^ v) * 0x01000193u;
    return h ^ (h >> 13);
}

static u32 run_kernel(u32* code, int is_double, u32 kernel)
{
    float in[14] __attribute__((aligned(32)));
    u32 out[28] __attribute__((aligned(32)));
    u32 h = 0x811C9DC5u, d = (kernel >> 21) & 31;
    void (*fn)(void*, void*) = (void (*)(void*, void*)) code;
    (void) is_double;
    (void) d;
    s_rng = 0x1234567u;
    for (int i = 0; i < TEST_CASES; i++) {
        gen_slot(&in[4], &in[6], &in[8]);
        gen_slot(&in[5], &in[7], &in[9]);
        in[0] = junk();
        in[1] = junk();
        in[2] = junk();
        in[3] = junk();
        in[10] = junk();
        in[11] = junk();
        in[12] = junk();
        in[13] = junk();
        memset(out, 0, sizeof(out));
        fn(in, out);
        for (int k = 0; k < 28; k++) {
            h = mix(h, out[k]);
        }
    }
    return h;
}

static const u32 s_kernels[] = {
    /* single */
    0, 1, 2, 3, 4, 29, 0, 4, 2, 3, 4, 29, 0, 2, 2, 3, 4, 28, 0, 1, 2, 3, 4, 31, 0, 1, 2, 3, 4, 30,
    0, 3, 2, 3, 4, 30, 0, 1, 2, 2, 4, 30, 0, 1, 2, 3, 2, 30, 0, 2, 2, 3, 4, 31,
    /* double */
    1, 1, 2, 3, 4, 30, 1, 4, 2, 3, 4, 30, 1, 2, 2, 2, 4, 30, 1, 1, 2, 3, 4, 31,
    /* paired */
    2, 1, 2, 3, 4, 29, 2, 4, 2, 3, 4, 29, 2, 2, 2, 3, 4, 29, 2, 1, 2, 2, 4, 29, 2, 1, 2, 3, 2, 28,
    2, 3, 2, 3, 4, 30, 2, 4, 2, 3, 4, 31, 2, 4, 2, 3, 4, 14, 2, 1, 2, 3, 4, 15, 2, 2, 2, 3, 4, 28,
};

static void self_test(void)
{
    static const u32 ops[3] = { 59, 63, 4 };
    int count = sizeof(s_kernels) / sizeof(s_kernels[0]) / 6;
    u32* code = NULL;
    static Span saved[MAX_SPANS];
    int saved_count = s_span_count, counting = s_counting, shared_bad = 0;
    char raw[300], fixed[300];
    int rl = 0, fl = 0;
    /* in the room the rewrite gets (given back after) */
    for (int i = 0; i < s_span_count && code == NULL; i++) {
        if (s_spans[i].hi - s_spans[i].lo >= TEST_WORDS * 8) {
            code = (u32*) s_spans[i].lo;
        }
    }
    if (code == NULL) {
        WL_LOG_W("melee", "fused: no room for the self-test");
        return;
    }
    memcpy(saved, s_spans, sizeof(saved));
    s_counting = 0;
    for (int k = 0; k < count; k++) {
        const u32* p = &s_kernels[k * 6];
        u32 w = af(ops[p[0]], p[1], p[2], p[3], p[4], p[5]);
        int n = 0, site, dbl = p[0] == 1;
        u32 hr, hf;
        /* the harness: f0-f6 from the pairs at r3; the instruction; each
         * register's double, then its pair (a double result's left out),
         * at r4. Its return address in r0, as a non-leaf function's: the
         * rewrite is tried both ways, its own code and the shared */
        code[n++] = MFLR_R0;
        for (u32 r = 0; r <= 6; r++) {
            code[n++] = OP(56) | r << 21 | 3u << 16 | (r * 8);
        }
        site = n;
        code[n++] = w;
        for (u32 r = 0; r <= 6; r++) {
            code[n++] = OP(54) | r << 21 | 4u << 16 | (r * 8);
        }
        for (u32 r = 0; r <= 6; r++) {
            if (!(dbl && r == p[1])) {
                code[n++] = OP(60) | r << 21 | 4u << 16 | (56 + r * 8);
            }
        }
        code[n++] = 0x7C0803A6u; /* mtlr r0 */
        code[n++] = BLR;
        DCFlushRange(code, n * 4);
        ICInvalidateRange(code, n * 4);
        hr = run_kernel(code, dbl, w);
        hf = 0;
        for (int shared = 0; shared < 2; shared++) {
            u32 h;
            int shared_count = s_shared_count, shared_sites = s_shared_sites;
            code[site] = w;
            s_span_count = 0;
            span_add((u32) &code[TEST_WORDS], (u32) &code[TEST_WORDS * 2]);
            s_shared_count = 0;
            if (patch_site(&code[site], (u32) &code[site], shared) < 0) {
                h = 0;
            } else {
                DCFlushRange(code, n * 4);
                ICInvalidateRange(code, n * 4);
                h = run_kernel(code, dbl, w);
            }
            s_shared_count = shared_count;
            s_shared_sites = shared_sites;
            if (!shared) {
                hf = h;
            } else if (h != hf) {
                shared_bad++;
            }
        }
        rl += snprintf(raw + rl, sizeof(raw) - rl, " %08x", (unsigned) hr);
        fl += snprintf(fixed + fl, sizeof(fixed) - fl, " %08x", (unsigned) hf);
    }
    memcpy(s_spans, saved, sizeof(saved));
    s_span_count = saved_count;
    s_counting = counting;
    WL_LOG_I("melee", "fused: self-test, the instructions:%s", raw);
    WL_LOG_I("melee", "fused: self-test, rewritten:%s%s", fixed,
             shared_bad ? " (and differently shared)" : "");
}

/* ---- the entry points --------------------------------------------------- */

void melee_wii_fused_install(void)
{
    u32 dead, island = 0, arena1 = 0, high = 0, lo, hi, island_next;
    int sites[4] = { 0 }, left = 0;
    if (wl_platform_fs_exists("sd:/apps/melee/nofused")) {
        WL_LOG_I("melee", "fused: off (nofused)");
        return;
    }
    s_counting = wl_platform_fs_exists("sd:/apps/melee/fusedcount");
    s_span_count = 0;
    if (s_counting) {
        /* tests in Dolphin, whose MEM1 can be made bigger than the Wii's:
         * the counting code past 24 MiB (in reach of the game's code up to
         * 0x82000000) */
        u32 mem1 = *(volatile u32*) 0x80000028u;
        if (*(volatile u32*) 0x80003100u > mem1) {
            mem1 = *(volatile u32*) 0x80003100u;
        }
        if (mem1 >= 0x01900000u) {
            high = (mem1 > 0x02000000u ? 0x02000000u : mem1) - 0x01800000u;
            span_add(0x81800000u, 0x81800000u + high);
        }
        s_count_max = 4096 + 64;
        s_counts = calloc(s_count_max, 4);
        s_counts_c = calloc(s_count_max, 4);
        s_count_sites = calloc(s_count_max, 4);
        if (high == 0 || s_counts == NULL || s_counts_c == NULL || s_count_sites == NULL) {
            WL_LOG_W("melee", "fused: fusedcount needs a MEM1 over 24 MiB (Dolphin's override): not counting");
            s_counting = 0;
        }
    }
    dead = add_dead_code();
    /* the island's room (MetroTRK's code), but for some kept for later */
    island_next = (u32) melee_wii_island_next();
    if ((u32) __melee_island_end > island_next + 2048 + 256) {
        u32 size = ((u32) __melee_island_end - island_next - 2048) & ~31u;
        u8* p = melee_wii_island_alloc(size);
        if (p != NULL) {
            island = size;
            span_add((u32) p, (u32) p + size);
        }
    }
    /* libogc's MEM1 arena: its kernel's workspace came from it at start;
     * nothing takes from it later (newlib's malloc is in MEM2) */
    lo = ((u32) SYS_GetArena1Lo() + 31) & ~31u;
    hi = (u32) SYS_GetArena1Hi() & ~31u;
    if (hi > lo + 4096) {
        arena1 = hi - lo - 4096;
        SYS_SetArena1Lo((void*) (lo + arena1));
        span_add(lo, lo + arena1);
    }
    self_test();
    for (u32 pc = TEXT_LO; pc < (u32) __melee_ctors_start; pc += 4) {
        int kind;
        if (pc == (u32) __melee_island_start) {
            pc = (u32) __melee_island_end; /* (below) */
        }
        kind = kind_of(*(u32*) pc);
        if (kind == KIND_NONE || replaced_at(pc)) {
            continue;
        }
        if (patch_site((u32*) pc, pc, lr_free(pc)) < 0) {
            left++;
        } else {
            sites[kind]++;
        }
    }
    /* the island's code: the boot codes' blocks (their displaced
     * instructions) */
    for (u32 pc = (u32) __melee_island_start; pc < island_next; pc += 4) {
        int kind = kind_of(*(u32*) pc);
        if (kind != KIND_NONE) {
            if (patch_site((u32*) pc, pc, 0) < 0) {
                left++;
            } else {
                sites[kind]++;
            }
        }
    }
    DCFlushRange((void*) TEXT_LO, (u32) __melee_ctors_start - TEXT_LO);
    ICInvalidateRange((void*) TEXT_LO, (u32) __melee_ctors_start - TEXT_LO);
    s_installed = 1;
    WL_LOG_I("melee",
             "fused: %d single, %d double and %d paired-single multiply-adds round as Slippi "
             "Dolphin's (%d left as they are; %d through %d shared blocks)%s; room: %u KB of replaced "
             "SDK code, %u KB of the island, %u KB of libogc's arena%s, %u KB of it left",
             sites[KIND_SINGLE], sites[KIND_DOUBLE], sites[KIND_PAIRED], left, s_shared_sites,
             s_shared_count, s_counting ? ", each counted" : "", (unsigned) (dead / 1024),
             (unsigned) (island / 1024), (unsigned) (arena1 / 1024),
             s_counting ? ", the memory past 24 MiB" : "", (unsigned) (spans_free() / 1024));
}

/* The Slippi codeset's codes (so_exi.c: before the bootloader copies it to
 * `load_address` and applies it): the instructions in its C2 codes' blocks,
 * at their final addresses */
int melee_wii_fused_patch_gct(unsigned char* gct, unsigned int len, unsigned int load_address)
{
    u32 off = 8;
    int patched = 0;
    if (!s_installed) {
        return 0;
    }
    while (off + 8 <= len) {
        u32 word = ((u32) gct[off] << 24) | ((u32) gct[off + 1] << 16) | ((u32) gct[off + 2] << 8) |
                   gct[off + 3];
        u32 value = ((u32) gct[off + 4] << 24) | ((u32) gct[off + 5] << 16) |
                    ((u32) gct[off + 6] << 8) | gct[off + 7];
        u32 type = (word >> 24) & 0xFE, next = 8;
        if (type == 0xC0 || type == 0xC2) {
            next = 8 + value * 8;
        } else if (type == 0x06) {
            next = 8 + ((value + 7) & ~7u);
        } else if (type == 0x08) {
            next = 16;
        } else if ((word & 0xF0FFFFFF) == 0xF0000000 && value == 0) {
            break;
        }
        if (next > len - off) {
            break;
        }
        if (type == 0xC2 && value > 0 && ((load_address + off) & 3) == 0) {
            /* (the last word: the branch back) */
            for (u32 i = off + 8; i + 4 < off + next; i += 4) {
                u32 w = ((u32) gct[i] << 24) | ((u32) gct[i + 1] << 16) | ((u32) gct[i + 2] << 8) | gct[i + 3];
                u32 site = load_address + i, patched_word = w;
                if (kind_of(w) == KIND_NONE) {
                    continue;
                }
                if (patch_site(&patched_word, site, 0) == 0) {
                    gct[i] = patched_word >> 24;
                    gct[i + 1] = patched_word >> 16;
                    gct[i + 2] = patched_word >> 8;
                    gct[i + 3] = patched_word;
                    patched++;
                }
            }
        }
        off += next;
    }
    WL_LOG_I("melee", "fused: %d in the codeset", patched);
    return patched;
}

/* After each game (tests: fusedcount): where the hardware's result and
 * Ishiiruka's differed */
void melee_wii_fused_report(void)
{
    u32 total = 0;
    int sites = 0, len = 0;
    char line[160];
    if (s_counts == NULL) {
        return;
    }
    u32 c_total = 0;
    int c_sites = 0;
    for (int i = 0; i < s_count_used; i++) {
        total += s_counts[i];
        sites += s_counts[i] != 0;
        c_total += s_counts_c[i];
        c_sites += s_counts_c[i] != 0;
    }
    WL_LOG_I("melee", "fused: %u results differed from the hardware's at %d sites", (unsigned) total, sites);
    WL_LOG_I("melee", "fused: c not a single %u times at %d sites", (unsigned) c_total, c_sites);
    for (int i = 0; i < s_count_used; i++) {
        if (s_counts_c[i] != 0) {
            len += snprintf(line + len, sizeof(line) - len, " %08x:%u", (unsigned) s_count_sites[i],
                            (unsigned) s_counts_c[i]);
            if (len > 120) {
                WL_LOG_I("melee", "fused: c:%s", line);
                len = 0;
            }
        }
    }
    if (len > 0) {
        WL_LOG_I("melee", "fused: c:%s", line);
        len = 0;
    }
    for (int i = 0; i < s_count_used; i++) {
        if (s_counts[i] == 0) {
            continue;
        }
        len += snprintf(line + len, sizeof(line) - len, " %08x:%u", (unsigned) s_count_sites[i],
                        (unsigned) s_counts[i]);
        if (len > 120) {
            WL_LOG_I("melee", "fused:%s", line);
            len = 0;
        }
    }
    if (len > 0) {
        WL_LOG_I("melee", "fused:%s", line);
    }
    memset(s_counts, 0, s_count_used * 4);
    memset(s_counts_c, 0, s_count_used * 4);
}
