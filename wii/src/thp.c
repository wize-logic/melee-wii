/* The SDK's THP movie decoder, the parts of it that are MWCC assembly
 * (libs/dolphin/src/dolphin/thp/THPDec.c, built without them): the Huffman
 * decoding of a frame's blocks, the inverse DCT, and the rows of 16 lines it
 * writes out. The headers and the Huffman and quantization tables are the
 * SDK's own C; the start of the bit stream is its __THPPrepBitStream, which
 * is static there.
 *
 * The same results as the SDK's: its bit reader (32-bit words, no byte
 * stuffing; a restart only aligns to the next byte), its float inverse DCT
 * (libjpeg's AAN, the quantization tables scaled for it by the SDK), and its
 * output, the planes in GX's I8 tiles (8x4 pixels). The SDK decodes a row
 * into the locked cache and DMAs it out; this writes the planes directly. */

#include "melee_wii.h"

#include <stdint.h>
#include <string.h>

#include <dolphin/os.h>
#include <dolphin/thp/thp.h>


static const u8 s_natural_order[80] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

/* THPDec.c's __THPPrepBitStream: the reader at the scan's first word, and
 * each Huffman table's lookup of its codes of up to 5 bits */
static void prep_bit_stream(THPFileInfo* info)
{
    u32* ptr = (u32*) ((uintptr_t) info->file & ~3);
    u32 offset = (uintptr_t) info->file & 3;

    if (info->cnt != 33) {
        info->cnt -= (3 - offset) * 8;
    } else {
        info->cnt = (offset * 8) + 1;
    }
    info->file = (u8*) ptr;
    info->currByte = *ptr;

    for (u32 i = 0; i < 4; i++) {
        if (!(info->validHuffmanTabs & (1 << i))) {
            continue;
        }
        THPHuffmanTab* h = &info->huffmanTabs[i];
        for (u32 j = 0; j < 32; j++) {
            h->quick[j] = 0xFF;
            for (u32 k = 0; k < 5; k++) {
                s32 code = j >> (5 - k - 1);
                if (code <= h->maxCode[k + 1]) {
                    h->quick[j] = h->Vij[code + h->valPtr[k + 1]];
                    h->increment[j] = k + 1;
                    break;
                }
            }
        }
    }
}

/* The locked cache and the GQRs the SDK sets up: nothing to do here */
void THPInit(void) {}

/* `n` (0 to 16) bits of the stream, the SDK's way: info->file at the word
 * being read, info->currByte that word, info->cnt the next bit's number in it
 * (1 its top bit, 33 when it is used up) */
static inline u32 get_bits(THPFileInfo* info, u32 n)
{
    u32 cnt = info->cnt;
    if (n == 0) {
        return 0;
    }
    if (cnt + n <= 33) {
        u32 v = (info->currByte << (cnt - 1)) >> (32 - n);
        info->cnt = cnt + n;
        return v;
    }
    u32 have = 33 - cnt;
    u32 hi = have != 0 ? (info->currByte << (cnt - 1)) >> (32 - have) : 0;
    u32 need = n - have;
    info->file += 4;
    info->currByte = *(u32*) info->file;
    info->cnt = 1 + need;
    return (hi << need) | (info->currByte >> (32 - need));
}

static s32 decode_huffman(THPFileInfo* info, const THPHuffmanTab* h)
{
    u32 cnt = info->cnt;
    if (cnt <= 28) {
        u32 index = (info->currByte << (cnt - 1)) >> 27;
        u8 code = h->quick[index];
        if (code != 0xFF) {
            info->cnt = cnt + h->increment[index];
            return code;
        }
    }
    s32 code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | (s32) get_bits(info, 1);
        if (code <= h->maxCode[l]) {
            return h->Vij[code + h->valPtr[l]];
        }
    }
    return 0; /* a corrupt stream: the SDK's reads on; an end of block here */
}

static inline s32 extend(u32 v, u32 n)
{
    return v < (1u << (n - 1)) ? (s32) v + (s32) (0xFFFFFFFFu << n) + 1 : (s32) v;
}

static void decode_block(THPFileInfo* info, THPCoeff* block, THPComponent* comp,
                         const THPHuffmanTab* dc, const THPHuffmanTab* ac)
{
    memset(block, 0, 64 * sizeof(THPCoeff));
    s32 t = decode_huffman(info, dc);
    s32 diff = t != 0 ? extend(get_bits(info, t), t) : 0;
    s16 value = (s16) (comp->predDC + diff);
    comp->predDC = value;
    block[0] = value;
    for (s32 k = 1; k < 64; k++) {
        s32 rs = decode_huffman(info, ac);
        s32 r = rs >> 4;
        s32 s = rs & 15;
        if (s != 0) {
            k += r;
            block[s_natural_order[k]] = (s16) extend(get_bits(info, s), s);
        } else if (r == 15) {
            k += 15;
        } else {
            break;
        }
    }
}

static inline u8 clamp_sample(f32 v)
{
    /* (v + 1024) / 8, as the SDK's quantized store of it as u8 */
    s32 i = (s32) ((v + 1024.0f) * 0.125f);
    return i < 0 ? 0 : i > 255 ? 255 : (u8) i;
}

/* libjpeg's jidctflt: the block into 8x8 samples at line `y0` (0 or 8) of a
 * row of 16 lines, column `x` (a multiple of 8), in I8 tiles `width` wide */
static void inverse_dct(const THPCoeff* in, const f32* q, u8* base, u32 width,
                        u32 x, u32 y0)
{
    f32 ws[64];
    for (int c = 0; c < 8; c++) {
        const THPCoeff* col = in + c;
        const f32* qc = q + c;
        f32* w = ws + c;
        if (col[8] == 0 && col[16] == 0 && col[24] == 0 && col[32] == 0 &&
            col[40] == 0 && col[48] == 0 && col[56] == 0)
        {
            f32 dc = col[0] * qc[0];
            for (int r = 0; r < 8; r++) {
                w[r * 8] = dc;
            }
            continue;
        }
        f32 tmp0 = col[0] * qc[0];
        f32 tmp1 = col[16] * qc[16];
        f32 tmp2 = col[32] * qc[32];
        f32 tmp3 = col[48] * qc[48];
        f32 tmp10 = tmp0 + tmp2;
        f32 tmp11 = tmp0 - tmp2;
        f32 tmp13 = tmp1 + tmp3;
        f32 tmp12 = (tmp1 - tmp3) * 1.414213562f - tmp13;
        tmp0 = tmp10 + tmp13;
        tmp3 = tmp10 - tmp13;
        tmp1 = tmp11 + tmp12;
        tmp2 = tmp11 - tmp12;
        f32 tmp4 = col[8] * qc[8];
        f32 tmp5 = col[24] * qc[24];
        f32 tmp6 = col[40] * qc[40];
        f32 tmp7 = col[56] * qc[56];
        f32 z13 = tmp6 + tmp5;
        f32 z10 = tmp6 - tmp5;
        f32 z11 = tmp4 + tmp7;
        f32 z12 = tmp4 - tmp7;
        tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        f32 z5 = (z10 + z12) * 1.847759065f;
        tmp10 = z12 * 1.082392200f - z5;
        tmp12 = z10 * -2.613125930f + z5;
        tmp6 = tmp12 - tmp7;
        tmp5 = tmp11 - tmp6;
        tmp4 = tmp10 + tmp5;
        w[0] = tmp0 + tmp7;
        w[56] = tmp0 - tmp7;
        w[8] = tmp1 + tmp6;
        w[48] = tmp1 - tmp6;
        w[16] = tmp2 + tmp5;
        w[40] = tmp2 - tmp5;
        w[32] = tmp3 + tmp4;
        w[24] = tmp3 - tmp4;
    }
    for (int r = 0; r < 8; r++) {
        const f32* w = ws + r * 8;
        f32 tmp10 = w[0] + w[4];
        f32 tmp11 = w[0] - w[4];
        f32 tmp13 = w[2] + w[6];
        f32 tmp12 = (w[2] - w[6]) * 1.414213562f - tmp13;
        f32 tmp0 = tmp10 + tmp13;
        f32 tmp3 = tmp10 - tmp13;
        f32 tmp1 = tmp11 + tmp12;
        f32 tmp2 = tmp11 - tmp12;
        f32 z13 = w[5] + w[3];
        f32 z10 = w[5] - w[3];
        f32 z11 = w[1] + w[7];
        f32 z12 = w[1] - w[7];
        f32 tmp7 = z11 + z13;
        tmp11 = (z11 - z13) * 1.414213562f;
        f32 z5 = (z10 + z12) * 1.847759065f;
        tmp10 = z12 * 1.082392200f - z5;
        tmp12 = z10 * -2.613125930f + z5;
        f32 tmp6 = tmp12 - tmp7;
        f32 tmp5 = tmp11 - tmp6;
        f32 tmp4 = tmp10 + tmp5;
        u32 y = y0 + r;
        u8* out = base + x * 4 + (y >> 2) * width * 4 + (y & 3) * 8;
        out[0] = clamp_sample(tmp0 + tmp7);
        out[7] = clamp_sample(tmp0 - tmp7);
        out[1] = clamp_sample(tmp1 + tmp6);
        out[6] = clamp_sample(tmp1 - tmp6);
        out[2] = clamp_sample(tmp2 + tmp5);
        out[5] = clamp_sample(tmp2 - tmp5);
        out[4] = clamp_sample(tmp3 + tmp4);
        out[3] = clamp_sample(tmp3 - tmp4);
    }
}

/* __THPDecompressiMCURow640x480 and __THPDecompressiMCURowNxN: a row of 16
 * lines, `width` wide, its Y, U and V written at the tiles' pointers */
static void decompress_row(THPFileInfo* info, u32 width)
{
    THPComponent* y = &info->components[0];
    THPComponent* u = &info->components[1];
    THPComponent* v = &info->components[2];
    const THPHuffmanTab* tabs = info->huffmanTabs;
    const THPHuffmanTab* ydc = &tabs[y->DCTableSelector << 1];
    const THPHuffmanTab* yac = &tabs[(y->ACTableSelector << 1) + 1];
    const THPHuffmanTab* udc = &tabs[u->DCTableSelector << 1];
    const THPHuffmanTab* uac = &tabs[(u->ACTableSelector << 1) + 1];
    const THPHuffmanTab* vdc = &tabs[v->DCTableSelector << 1];
    const THPHuffmanTab* vac = &tabs[(v->ACTableSelector << 1) + 1];
    const f32* yq = info->quantTabs[y->quantizationTableSelector];
    const f32* uq = info->quantTabs[u->quantizationTableSelector];
    const f32* vq = info->quantTabs[v->quantizationTableSelector];

    for (u32 mcu = 0; mcu < info->MCUsPerRow; mcu++) {
        decode_block(info, info->mcuBuffer[0], y, ydc, yac);
        decode_block(info, info->mcuBuffer[1], y, ydc, yac);
        decode_block(info, info->mcuBuffer[2], y, ydc, yac);
        decode_block(info, info->mcuBuffer[3], y, ydc, yac);
        decode_block(info, info->mcuBuffer[4], u, udc, uac);
        decode_block(info, info->mcuBuffer[5], v, vdc, vac);

        u32 x = mcu * 16;
        inverse_dct(info->mcuBuffer[0], yq, info->tileY, width, x, 0);
        inverse_dct(info->mcuBuffer[1], yq, info->tileY, width, x + 8, 0);
        inverse_dct(info->mcuBuffer[2], yq, info->tileY, width, x, 8);
        inverse_dct(info->mcuBuffer[3], yq, info->tileY, width, x + 8, 8);
        inverse_dct(info->mcuBuffer[4], uq, info->tileU, width / 2, x / 2, 0);
        inverse_dct(info->mcuBuffer[5], vq, info->tileV, width / 2, x / 2, 0);

        if (info->RST != 0) {
            info->currMCU--;
            if (info->currMCU == 0) {
                info->currMCU = info->nMCU;
                info->cnt = 1 + ((info->cnt + 6) & 0xFFFFFFF8);
                if (info->cnt > 32) {
                    info->cnt = 33;
                }
                y->predDC = 0;
                u->predDC = 0;
                v->predDC = 0;
            }
        }
    }

    info->tileY += width * 16;
    info->tileU += width * 4;
    info->tileV += width * 4;
}

static void decompress(THPFileInfo* info, void* tileY, void* tileU,
                       void* tileV, u32 width)
{
    info->tileY = tileY;
    info->tileU = tileU;
    info->tileV = tileV;
    prep_bit_stream(info);
    while (info->x8EE < info->x8EA + info->decompressedY) {
        decompress_row(info, width);
        info->x8EE += info->x8D4;
    }
}

void THPDec_80331340(THPFileInfo* info, void* tileY, void* tileU, void* tileV)
{
    decompress(info, tileY, tileU, tileV, 640);
}

void THPDec_803313D0(THPFileInfo* info, void* tileY, void* tileU, void* tileV,
                     u32 x)
{
    decompress(info, tileY, tileU, tileV, x);
}
