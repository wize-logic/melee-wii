/* The ping in an online game's corner, in the text Slippi's codeset showed
 * the input delay in ("Delay: 2f", which now goes with the connection:
 * so_config_apply_link): the same place, font, size and colour.
 *
 * The codeset's HUD code (its hook at 0x802f6694) makes, as a game starts,
 * a text for its messages (Desync Risk, DESYNC DETECTED...), whose pointer
 * it keeps in its data (at 0x804D6CBC) + 0xAD2, and right after it the
 * delay's: the next in Melee's list of texts (HSD_SisLib_803A5ACC appends),
 * right-aligned. The device blanks the delay's format (so_exi.c,
 * hide_delay_text); this sets the text's one line, with Melee's own call,
 * from the device's 0xB0 in the game's frame (so_exi.c). The text and its
 * buffer are the game's: a rollback's load puts back an older number until
 * the next one. */

#include <gccore.h>
#include <string.h>

#define SLIPPI_DATA 0x804D6CBCu /* (r13 - 18916) */
#define MESSAGE_TEXT 0xAD2u     /* in it: the messages' text */
#define TEXT_NEXT 0x50u         /* HSD_Text: next */
#define TEXT_ALIGNMENT 0x4Au    /* HSD_Text: default_alignment (2: right) */
#define TEXT_BUFFER 0x5Cu       /* HSD_Text: sis_buffer */

/* HSD_SisLib_803A70A0: a text's line `index` set from a format */
typedef void (*SetText)(void* text, s32 index, const char* format, ...);
#define SET_TEXT ((SetText) 0x803A70A0u)

static int in_mem1(u32 a)
{
    return a >= 0x80000000u && a < 0x81800000u;
}

static u32 word_at(u32 a)
{
    u32 v;
    memcpy(&v, (const void*) a, 4); /* (the codeset's data: not aligned) */
    return v;
}

void melee_wii_ping_text(int ms)
{
    u32 data = word_at(SLIPPI_DATA), message, text;
    if (!in_mem1(data)) {
        return;
    }
    message = word_at(data + MESSAGE_TEXT);
    if (!in_mem1(message)) {
        return;
    }
    text = word_at(message + TEXT_NEXT);
    if (!in_mem1(text) || *(const u8*) (text + TEXT_ALIGNMENT) != 2 || !in_mem1(word_at(text + TEXT_BUFFER))) {
        return;
    }
    SET_TEXT((void*) text, 0, "Ping: %dms", ms > 999 ? 999 : ms);
}
