/* Gecko codes, applied once before the game starts, as a code handler would
 * at the game's first frame: the codes Slippi's online mode needs installed
 * at boot (Slippi's bootloader, which fetches the rest of the codeset over
 * EXI and applies it itself; the delay and rumble buttons...).
 *
 * The types such a codeset uses: 04 (a word), 02/00 (halfwords and bytes,
 * repeated), 06 (bytes) and C2 (code inserted at an address: a branch there
 * to a copy of the code, which ends with a branch back). The C2 copies go in
 * MEM1 in the game's reach (melee_wii_island_alloc). A code that patches the
 * SDK's code the port replaces (the GameCube's OSReport, EXI...) is left out
 * (melee_replaced_table): it would patch code that never runs, or the port's
 * branch at its start. */

#include "melee_wii.h"

#include <gccore.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <wl/base/base.h>

extern const u32 melee_replaced_table[][2];

static int replaced(u32 address)
{
    for (int i = 0; melee_replaced_table[i][1] != 0; i++) {
        if (address >= melee_replaced_table[i][0] && address < melee_replaced_table[i][1]) {
            return 1;
        }
    }
    return 0;
}

/* The same for the Slippi codeset the bootloader applies (so_files.c) */
int melee_wii_code_replaced(unsigned int address)
{
    return replaced(address);
}

/* diag_watch.c (test builds): told of the port's writes to the game's code */
void melee_wii_watch_note(u32 addr, u32 len) __attribute__((weak));
#define NOTE(a, l)                                                                                   \
    do {                                                                                             \
        if (melee_wii_watch_note != NULL) {                                                          \
            melee_wii_watch_note((u32) (a), (u32) (l));                                              \
        }                                                                                            \
    } while (0)

static void write32(u32 address, u32 value)
{
    *(volatile u32*) address = value;
    NOTE(address, 4);
    DCFlushRange((void*) (address & ~31u), 32);
    ICInvalidateRange((void*) (address & ~31u), 32);
}

static u32 get32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

/* Applies the codes of a GCT (00D0C0DE 00D0C0DE, codes, F0000000 00000000,
 * or the codes alone); returns how many were applied, or < 0 for a malformed
 * or unsupported one */
static int gecko_apply(const u8* gct, u32 len);

int melee_wii_gecko_apply(const u8* gct, u32 len)
{
    /* from a copy whose calls to the game's memset and memcpy go where the
     * port runs them (main.c) */
    u8* copy = memalign(32, len + 4);
    int applied;
    if (copy == NULL) {
        return gecko_apply(gct, len);
    }
    memcpy(copy, gct, len);
    melee_wii_libc_fix_code((unsigned int*) copy, len / 4);
    applied = gecko_apply(copy, len);
    free(copy);
    return applied;
}

static int gecko_apply(const u8* gct, u32 len)
{
    u32 off = 0;
    int applied = 0, skipped = 0;
    if (len >= 8 && get32(gct) == 0x00D0C0DE && get32(gct + 4) == 0x00D0C0DE) {
        off = 8;
    }
    while (off + 8 <= len) {
        u32 a = get32(gct + off), b = get32(gct + off + 4);
        u32 type = (a >> 24) & 0xFE;
        u32 address = 0x80000000 | (a & 0x01FFFFFF);
        u32 size;
        if (a == 0xF0000000 || type == 0xF0) {
            break;
        }
        switch (type) {
        case 0x00: /* 8 bits, b's upper half + 1 times */
        case 0x02: /* 16 bits */
        case 0x04: /* 32 bits */
            size = 8;
            break;
        case 0x06: /* b bytes */
            size = 8 + ((b + 7) & ~7u);
            break;
        case 0xC2: /* b lines of code */
            size = 8 + b * 8;
            break;
        default:
            WL_LOG_E("gecko", "code type %02X at %u is not supported", (unsigned) type,
                     (unsigned) off);
            return -1;
        }
        if (off + size > len) {
            WL_LOG_E("gecko", "a code runs past the codes' end");
            return -1;
        }
        if (replaced(address)) {
            WL_LOG_I("gecko", "left out: %08X (the port's code is there)", (unsigned) a);
            skipped++;
            off += size;
            continue;
        }
        switch (type) {
        case 0x00: {
            u32 count = (b >> 16) + 1;
            for (u32 i = 0; i < count; i++) {
                *(volatile u8*) (address + i) = (u8) b;
            }
            NOTE(address, count);
            DCFlushRange((void*) address, count);
            ICInvalidateRange((void*) address, count);
            break;
        }
        case 0x02: {
            u32 count = (b >> 16) + 1;
            for (u32 i = 0; i < count; i++) {
                *(volatile u16*) (address + i * 2) = (u16) b;
            }
            NOTE(address, count * 2);
            DCFlushRange((void*) address, count * 2);
            ICInvalidateRange((void*) address, count * 2);
            break;
        }
        case 0x04:
            write32(address, b);
            break;
        case 0x06:
            memcpy((void*) address, gct + off + 8, b);
            NOTE(address, b);
            DCFlushRange((void*) address, b);
            ICInvalidateRange((void*) address, b);
            break;
        case 0xC2: {
            u32 bytes = b * 8;
            u32* block = melee_wii_island_alloc(bytes);
            if (block == NULL) {
                WL_LOG_E("gecko", "no room for the code at %08X", (unsigned) address);
                return -1;
            }
            memcpy(block, gct + off + 8, bytes);
            /* the last word: the branch back, after the replaced instruction */
            {
                u32 last = (u32) &block[b * 2 - 1];
                s32 disp = (s32) ((address + 4) - last);
                block[b * 2 - 1] = 0x48000000 | ((u32) disp & 0x03FFFFFC);
            }
            DCFlushRange(block, bytes);
            ICInvalidateRange(block, bytes);
            melee_wii_put_branch(address, block, 0);
            NOTE(address, 4);
            DCFlushRange((void*) (address & ~31u), 32);
            ICInvalidateRange((void*) (address & ~31u), 32);
            break;
        }
        }
        applied++;
        off += size;
    }
    WL_LOG_I("gecko", "%d codes applied, %d left out", applied, skipped);
    return applied;
}

/* ---- cheats: the player's own codes ----------------------------------------
 *
 * A code handler runs its codes every frame: a cheat that writes the game's
 * data (a character's damage, a timer) writes it again each frame, and
 * conditional codes (if this halfword is that...) decide each frame. The
 * player's .gct is run here at each retrace (wii/src/vi.c), as Gecko's
 * handler runs at the game's VI interrupt. Its C2 codes (inserted code) are
 * put in once, now. The types run: 00/02/04 writes (and their pointer forms,
 * 10/12/14), 06 (bytes), 08 (serial writes), 20-2E (32- and 16-bit
 * conditionals, with their endif bit), 42/4A (the base address and pointer),
 * E0 (full terminator) and E2 (endif/else). A code of another type ends the
 * list where it is (its length is unknown), with a message in the log. */

static u8* s_cheats;
static u32 s_cheats_len;

static void poke(u32 address, const void* data, u32 len)
{
    if (address < 0x80000000u || address + len > 0x81800000u) {
        return;
    }
    memcpy((void*) address, data, len);
    NOTE(address, len);
    DCFlushRange((void*) (address & ~31u), ((address & 31u) + len + 31u) & ~31u);
    ICInvalidateRange((void*) (address & ~31u), ((address & 31u) + len + 31u) & ~31u);
}

static int readable(u32 address, u32 len)
{
    return address >= 0x80000000u && address + len <= 0x81800000u;
}

/* The length of the code at `off`, or 0 for a type not run here */
static u32 code_size(const u8* gct, u32 off, u32 len)
{
    u32 a = get32(gct + off), b = get32(gct + off + 4);
    u32 type = (a >> 24) & 0xEE; /* without the pointer bit (0x10) and address bit */
    switch ((a >> 24) & 0xFE) {
    case 0xC2:
    case 0xC0:
        return 8 + b * 8;
    case 0x42:
    case 0x4A:
    case 0xE0:
    case 0xE2:
        return 8;
    }
    switch (type) {
    case 0x00:
    case 0x02:
    case 0x04:
    case 0x20:
    case 0x22:
    case 0x24:
    case 0x26:
    case 0x28:
    case 0x2A:
    case 0x2C:
    case 0x2E:
        return 8;
    case 0x06:
        return 8 + ((b + 7) & ~7u);
    case 0x08:
        return 16;
    }
    (void) len;
    return 0;
}

/* One frame of the cheats (wii/src/vi.c, at the retrace) */
void melee_wii_gecko_frame(void)
{
    u32 off = 0, ba = 0x80000000u, po = 0x80000000u;
    int skip = 0; /* conditional levels that are false, around here */
    const u8* gct = s_cheats;
    u32 len = s_cheats_len;
    if (gct == NULL) {
        return;
    }
    while (off + 8 <= len) {
        u32 a = get32(gct + off), b = get32(gct + off + 4);
        u32 code = (a >> 24) & 0xFE;
        u32 size = code_size(gct, off, len);
        u32 base = (code & 0x10) && code < 0x40 ? po : ba;
        u32 address = base + (a & 0x01FFFFFFu);
        if (a == 0xF0000000u || size == 0 || off + size > len) {
            break;
        }
        if (code >= 0x20 && code <= 0x3E) {
            u32 cond = code & 0xEE;
            int ok;
            if (a & 1) { /* an endif first */
                address &= ~1u;
                if (skip > 0) {
                    skip--;
                }
            }
            if (skip > 0) {
                skip++;
            } else if (cond <= 0x26) {
                u32 have = readable(address, 4) ? *(volatile u32*) address : 0;
                ok = cond == 0x20 ? have == b : cond == 0x22 ? have != b : cond == 0x24 ? have > b : have < b;
                skip = ok ? 0 : 1;
            } else {
                u16 have = readable(address, 2) ? *(volatile u16*) address : 0;
                u16 want = (u16) b, mask = (u16) (b >> 16);
                have &= (u16) ~mask;
                ok = cond == 0x28 ? have == want : cond == 0x2A ? have != want : cond == 0x2C ? have > want : have < want;
                skip = ok ? 0 : 1;
            }
            off += size;
            continue;
        }
        if (code == 0xE0) {
            skip = 0;
            if ((b >> 16) != 0) {
                ba = b & 0xFFFF0000u;
            }
            if ((b & 0xFFFF) != 0) {
                po = b << 16;
            }
            off += size;
            continue;
        }
        if (code == 0xE2) {
            u32 n = a & 0xFF;
            for (u32 i = 0; i < n && skip > 0; i++) {
                skip--;
            }
            if ((a >> 20) & 1) { /* else */
                skip = skip == 0 ? 1 : skip == 1 ? 0 : skip;
            }
            off += size;
            continue;
        }
        if (skip > 0) {
            off += size;
            continue;
        }
        switch (code & 0xEE) {
        case 0x00: {
            u32 count = (b >> 16) + 1;
            u8 v = (u8) b;
            for (u32 i = 0; i < count; i++) {
                poke(address + i, &v, 1);
            }
            break;
        }
        case 0x02: {
            u32 count = (b >> 16) + 1;
            u16 v = (u16) b;
            for (u32 i = 0; i < count; i++) {
                poke(address + i * 2, &v, 2);
            }
            break;
        }
        case 0x04:
            poke(address, &b, 4);
            break;
        case 0x06:
            poke(address, gct + off + 8, b);
            break;
        case 0x08: {
            u32 c = get32(gct + off + 8), step = get32(gct + off + 12);
            u32 width = c >> 28, count = ((c >> 16) & 0xFFF) + 1, inc = c & 0xFFFF;
            u32 v = b;
            for (u32 i = 0; i < count; i++) {
                if (width == 0) {
                    u8 x = (u8) v;
                    poke(address, &x, 1);
                } else if (width == 1) {
                    u16 x = (u16) v;
                    poke(address, &x, 2);
                } else {
                    poke(address, &v, 4);
                }
                address += inc;
                v += step;
            }
            break;
        }
        default:
            if (code == 0x42 && (a & 0x00FFFFFFu) == 0) {
                ba = b;
            } else if (code == 0x4A && (a & 0x00FFFFFFu) == 0) {
                po = b;
            }
            /* C0/C2: put in once (melee_wii_gecko_cheats) */
            break;
        }
        off += size;
    }
}

/* The player's cheats: their C2 codes put in now, the rest run each frame
 * from now on. Returns how many codes there are, or < 0. */
int melee_wii_gecko_cheats(const u8* gct, u32 len)
{
    u32 off = 0, count = 0;
    if (len >= 8 && get32(gct) == 0x00D0C0DE && get32(gct + 4) == 0x00D0C0DE) {
        gct += 8;
        len -= 8;
    }
    while (off + 8 <= len) {
        u32 a = get32(gct + off), b = get32(gct + off + 4);
        u32 size = code_size(gct, off, len);
        (void) b;
        if (a == 0xF0000000u) {
            break;
        }
        if (size == 0 || off + size > len) {
            WL_LOG_E("gecko", "cheats: code type %02X is not supported; the codes after it are left out",
                     (unsigned) (a >> 24));
            break;
        }
        if (((a >> 24) & 0xFE) == 0xC2) {
            if (melee_wii_gecko_apply(gct + off, size) < 0) {
                return -1;
            }
        } else if (((a >> 24) & 0xFE) == 0xC0) {
            WL_LOG_E("gecko", "cheats: C0 codes (code run each frame) are left out");
        }
        off += size;
        count++;
    }
    s_cheats_len = off;
    s_cheats = memalign(32, off + 8);
    if (s_cheats == NULL) {
        return -1;
    }
    memcpy(s_cheats, gct, off);
    melee_wii_libc_fix_code((unsigned int*) s_cheats, off / 4);
    WL_LOG_I("gecko", "cheats: %u codes", (unsigned) count);
    return (int) count;
}
