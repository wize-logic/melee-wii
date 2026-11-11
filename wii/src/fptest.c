/* A fingerprint of the floating point unit (logged once at boot): each of
 * the instructions Melee's code uses, run on the same 20000 pseudo-random
 * operand sets everywhere, its results hashed. A Wii and an emulator give
 * the same hashes only if they compute those instructions bit for bit the
 * same: online play runs the game in lockstep with Slippi Dolphin on PCs,
 * and an instruction one of them rounds differently in some case is a
 * desync where the game hits that case. Operands of all sizes, addends
 * near zero beside their products (Dolphin's fmadds differed from the
 * hardware there: its issue 13865), and the odd denormal. */

#include <gccore.h>

#include <wl/base/base.h>

#define CASES 20000

static u32 s_rng = 0x2545F491u;

static u32 next(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* A single's bits: exponent around `mid` (spread +-`spread`), sign and
 * mantissa random; now and then a denormal */
static u32 single(int mid, int spread)
{
    u32 r = next();
    int e = mid + (int) (next() % (2 * spread + 1)) - spread;
    if ((r & 0xFF) == 0) {
        return (r & 0x807FFFFFu); /* denormal (or zero) */
    }
    if (e < 1) {
        e = 1;
    }
    if (e > 254) {
        e = 254;
    }
    return (r & 0x807FFFFFu) | ((u32) e << 23);
}

static u32 mix(u32 h, u32 v)
{
    return (h ^ v) * 0x01000193u;
}

typedef union {
    u32 u;
    float f;
} Bits;

void melee_wii_fp_fingerprint(void)
{
    u32 h[16] = { 0 };
    Bits known;
    u32 hid2;
    /* paired singles on, GQR0 plain floats (as the game's __OSPSInit) */
    __asm__ volatile("mfspr %0, 920" : "=r"(hid2));
    hid2 |= 0xA0000000u;
    __asm__ volatile("mtspr 920, %0\n\tisync" ::"r"(hid2));
    __asm__ volatile("mtspr 912, %0\n\tisync" ::"r"(0));
    s_rng = 0x2545F491u;
    for (int i = 0; i < CASES; i++) {
        Bits a, b, c, r;
        double d;
        float pa[2] __attribute__((aligned(8))), pc[2] __attribute__((aligned(8))),
            pb[2] __attribute__((aligned(8)));
        u32 pr[2] __attribute__((aligned(8)));
        int mid = 100 + (int) (next() % 56);
        a.u = single(mid, 8);
        c.u = single(127, 10);
        /* the addend: as big as the product, or much smaller */
        b.u = (next() & 1) ? single(mid, 8) : single(mid - 20 - (int) (next() % 10), 4);
        __asm__ volatile("fmadds %0,%1,%2,%3" : "=f"(r.f) : "f"(a.f), "f"(c.f), "f"(b.f));
        h[0] = mix(h[0], r.u);
        __asm__ volatile("fmsubs %0,%1,%2,%3" : "=f"(r.f) : "f"(a.f), "f"(c.f), "f"(b.f));
        h[1] = mix(h[1], r.u);
        __asm__ volatile("fnmadds %0,%1,%2,%3" : "=f"(r.f) : "f"(a.f), "f"(c.f), "f"(b.f));
        h[2] = mix(h[2], r.u);
        __asm__ volatile("fnmsubs %0,%1,%2,%3" : "=f"(r.f) : "f"(a.f), "f"(c.f), "f"(b.f));
        h[3] = mix(h[3], r.u);
        __asm__ volatile("fmuls %0,%1,%2" : "=f"(r.f) : "f"(a.f), "f"(c.f));
        h[4] = mix(h[4], r.u);
        __asm__ volatile("fadds %0,%1,%2" : "=f"(r.f) : "f"(a.f), "f"(b.f));
        h[5] = mix(h[5], r.u);
        __asm__ volatile("fdivs %0,%1,%2" : "=f"(r.f) : "f"(a.f), "f"(c.f));
        h[6] = mix(h[6], r.u);
        __asm__ volatile("fres %0,%1" : "=f"(r.f) : "f"(c.f));
        h[7] = mix(h[7], r.u);
        {
            Bits pos;
            pos.u = c.u & 0x7FFFFFFFu;
            __asm__ volatile("frsqrte %0,%1" : "=f"(d) : "f"(pos.f));
            r.f = (float) d;
            h[8] = mix(h[8], r.u);
        }
        __asm__ volatile("fmadd %0,%1,%2,%3\n\tfrsp %0,%0" : "=&f"(r.f) : "f"(a.f), "f"(c.f), "f"(b.f));
        h[9] = mix(h[9], r.u);
        {
            int iv[2] __attribute__((aligned(8)));
            __asm__ volatile("fctiwz %0,%1" : "=f"(d) : "f"(a.f));
            __builtin_memcpy(iv, &d, 8);
            h[10] = mix(h[10], (u32) iv[1]);
        }
        /* paired singles: both halves */
        pa[0] = a.f;
        pc[0] = c.f;
        pb[0] = b.f;
        {
            Bits x, y, z;
            x.u = single(mid, 8);
            y.u = single(127, 10);
            z.u = single(mid - 24, 4);
            pa[1] = x.f;
            pc[1] = y.f;
            pb[1] = z.f;
        }
        __asm__ volatile("psq_l 1,0(%1),0,0\n\tpsq_l 2,0(%2),0,0\n\tpsq_l 3,0(%3),0,0\n\t"
                         "ps_madd 4,1,2,3\n\tpsq_st 4,0(%0),0,0"
                         :
                         : "b"(pr), "b"(pa), "b"(pc), "b"(pb)
                         : "fr1", "fr2", "fr3", "fr4", "memory");
        h[11] = mix(mix(h[11], pr[0]), pr[1]);
        __asm__ volatile("psq_l 1,0(%1),0,0\n\tpsq_l 2,0(%2),0,0\n\t"
                         "ps_mul 4,1,2\n\tpsq_st 4,0(%0),0,0"
                         :
                         : "b"(pr), "b"(pa), "b"(pc)
                         : "fr1", "fr2", "fr4", "memory");
        h[12] = mix(mix(h[12], pr[0]), pr[1]);
        __asm__ volatile("psq_l 1,0(%1),0,0\n\tpsq_l 2,0(%2),0,0\n\tpsq_l 3,0(%3),0,0\n\t"
                         "ps_sum0 4,1,3,2\n\tpsq_st 4,0(%0),0,0"
                         :
                         : "b"(pr), "b"(pa), "b"(pc), "b"(pb)
                         : "fr1", "fr2", "fr3", "fr4", "memory");
        h[13] = mix(mix(h[13], pr[0]), pr[1]);
        __asm__ volatile("psq_l 1,0(%1),0,0\n\tpsq_l 2,0(%2),0,0\n\tpsq_l 3,0(%3),0,0\n\t"
                         "ps_madds0 4,1,2,3\n\tpsq_st 4,0(%0),0,0"
                         :
                         : "b"(pr), "b"(pa), "b"(pc), "b"(pb)
                         : "fr1", "fr2", "fr3", "fr4", "memory");
        h[14] = mix(mix(h[14], pr[0]), pr[1]);
        __asm__ volatile("psq_l 1,0(%1),0,0\n\tps_res 4,1\n\tpsq_st 4,0(%0),0,0"
                         :
                         : "b"(pr), "b"(pc)
                         : "fr1", "fr4", "memory");
        h[15] = mix(mix(h[15], pr[0]), pr[1]);
    }
    {
        Bits a, c, b;
        a.u = 0x42480000u;
        c.u = 0xBC88CC38u;
        b.u = 0x1B1C72A0u;
        __asm__ volatile("fmadds %0,%1,%2,%3" : "=f"(known.f) : "f"(a.f), "f"(c.f), "f"(b.f));
    }
    WL_LOG_I("melee", "fp: fmadds %08x fmsubs %08x fnmadds %08x fnmsubs %08x fmuls %08x fadds %08x fdivs %08x "
             "fres %08x", h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    WL_LOG_I("melee", "fp: frsqrte %08x fmadd+frsp %08x fctiwz %08x ps_madd %08x ps_mul %08x ps_sum0 %08x "
             "ps_madds0 %08x ps_res %08x; Dolphin's issue 13865 case %08x (hardware: bf55bf17)",
             h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15], known.u);
}
