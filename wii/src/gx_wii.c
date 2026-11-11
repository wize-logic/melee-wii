/* The SDK's GX is the GameCube's (MELEE_ORIG_CODE keeps it as it is) but for
 * what it leaves to bits the GameCube's GPU ignores and the Wii's reads: a
 * function here replaces the SDK's own (cmake/Wii.cmake's --override). */

#include <dolphin/gx.h>

#include "melee_wii.h"

typedef struct TlutObj {
    u32 tlut;
    u32 loadTlut0;
    u16 numEntries;
} TlutObj;

/* GXTexture.c's: it ORs the table's address into loadTlut0 without clearing
 * its bits 21-23, which the GameCube ignores and the Wii takes for the
 * address's upper bits (Dolphin: "Invalid range in CopyFromEmu"; the palette
 * would load from elsewhere). The same register, with those bits clear. */
void GXInitTlutObj(GXTlutObj* tlut_obj, void* lut, GXTlutFmt fmt, u16 n_entries)
{
    TlutObj* t = (TlutObj*) tlut_obj;
    t->tlut = ((u32) fmt & 3) << 10;
    t->loadTlut0 = ((((u32) lut & 0x3FFFFFFF) >> 5) & 0x1FFFFF) | (0x64u << 24);
    t->numEntries = n_entries;
}

#if defined(MELEE_ORIG_CODE)
/* The game's texture copies (its characters' shadows, the refraction and
 * HSD_ImageDescCopyFromEFB: cmake/Wii.cmake's --redirect): the GPU writes the
 * texture in memory past the CPU; noted for rollback's log (wii/src/mmu_track.c),
 * whose savestates leave them out. The size at most:
 * the copy's stride (32-byte units) for each 4 rows (the shortest tiles) of
 * its source, from GX's state (gx at 0x804D5BA8: cpTexSize 0x1F4, cpTexStride
 * 0x1F8) as GXSetTexCopySrc and Dst left it. Then the game's own GXCopyTex. */
void melee_wii_copy_tex(void* dest, GXBool clear)
{
    const u8* state = *(u8* const*) 0x804D5BA8;
    u32 size = *(const u32*) (state + 0x1F4), stride = *(const u32*) (state + 0x1F8);
    u32 rows = ((size >> 10) & 0x3FF) + 1;
    melee_wii_mmu_touch_gpu((u32) dest, (stride & 0x3FF) * 32 * ((rows + 3) / 4));
    ((void (*)(void*, GXBool)) 0x8033DCBC)(dest, clear);
}
#endif
