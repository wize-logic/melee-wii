/* 20XX's custom shield colours in the plain game and Slippi Online
 * (mod_values.c).
 *
 * A player's shield has the colour of its port: four colours (and a grey
 * one) in the fighters' common data, PlCo.dat, 0x2ED8 bytes after the
 * structure the game keeps a pointer to (p_ftCommonData). 20XX writes its
 * menu's colours over them as the game loads the file for a match; so does
 * the port, taking over that load's call (cmake/Wii.cmake's --redirect), and
 * it puts the game's own back for a player whose colour is off. Colours
 * only: nothing a match depends on reads them.
 *
 * In an online match "P1" is this console's player, whichever port Slippi
 * gives it, and P2 to P4 the others in the ports' order. */

#ifdef MELEE_ORIG_CODE

#include "melee_wii.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>

#include "mod_values.h"

void* lbArchive_LoadSymbols(const char* file, ...);

/* wii/src/slippi/so_netplay.c: how many opponents an online match has (0:
 * not one), and which player this console is (from 0) */
u8 so_np_remote_count(void);
u8 so_np_local_player_idx(void);

#define SHIELD_COLORS 0x2ED8u

/* The game's own: red, blue, yellow, green (and that these are there says
 * the data is the game's) */
static const u8 OWN[4][3] = { { 0xF2, 0x59, 0x59 }, { 0x66, 0x66, 0xFF }, { 0xFF, 0xBF, 0x40 }, { 0x4D, 0xE6, 0x4D } };

static BOOL s_ours; /* the table has colours of the menu's */

/* Fighter_LoadCommonData's call of lbArchive_LoadSymbols("PlCo.dat", &data,
 * "ftLoadCommonData", 0): `data` points at the file's structure, whose first
 * word is the common data's address */
void* melee_wii_fighters_loaded(const char* file, u8*** data, const char* symbol, void* end)
{
    void* archive = lbArchive_LoadSymbols(file, data, symbol, end);
    const u32* values = melee_wii_mod_value(MOD_SHIELDS); /* players 4 to 1: blue, green, red, on */
    u8* table;
    BOOL any = values[3] != 0 || values[7] != 0 || values[11] != 0 || values[15] != 0;
    if (data == NULL || (u32) *data < 0x80003100u || (u32) *data >= 0x817F0000u || (u32) **data < 0x80003100u ||
        (u32) **data >= 0x817F0000u || (!any && !s_ours))
    {
        return archive;
    }
    table = **data + SHIELD_COLORS;
    if (!s_ours && (memcmp(table, OWN[0], 3) != 0 || memcmp(table + 4, OWN[1], 3) != 0 ||
                    memcmp(table + 8, OWN[2], 3) != 0 || memcmp(table + 12, OWN[3], 3) != 0))
    {
        OSReport("20XX shields: the fighters' data is not the game's own: left as it is\n");
        return archive;
    }
    for (u32 player = 0; player < 4; player++) {
        /* Online the game says which port a player is, after the menu: there
         * the first colour is this console's player's, wherever it plays,
         * and the others' follow in the ports' order */
        u32 local = so_np_remote_count() != 0 ? so_np_local_player_idx() : 0;
        u32 whose = local > 3 ? player : player == local ? 0 : player < local ? player + 1 : player;
        const u32* v = &values[(3 - whose) * 4];
        u8* color = table + player * 4;
        if (v[3] != 0) {
            color[0] = (u8) v[2];
            color[1] = (u8) v[1];
            color[2] = (u8) v[0];
            OSReport("20XX shields: player %u's is %02x%02x%02x\n", (unsigned) (player + 1), color[0], color[1], color[2]);
        } else {
            memcpy(color, OWN[player], 3);
        }
    }
    s_ours = any;
    return archive;
}

/* ---- 20XX's costumes on the character select screen ----------------------
 * In 20XX, L and R on the character select screen give the player's
 * character its costume of 20XX's first or second set, and the same button
 * again the game's own back. The screen's cursor code calls the game's
 * costume change (X and Y) with the buttons a player pressed, each frame the
 * player may change costume; the port takes those calls over
 * (cmake/Wii.cmake's --redirect) and adds L and R. Which file is which
 * costume is the game's own table's; mod_stages.c gives the game the file. */

void mnCharSel_CostumeChange(int door, u32 input);
void mnCharSel_8025DB34(u8 door);     /* a door's character and costume, shown and loaded */
void lbAudioAx_80024030(int sound); /* 0 back, 1 forward, 3 no */
int HSD_AudioSFXStartParam(int id, u8 vol, u8 pan, int track, int channel);
#define SOUND_COSTUME 0xB5 /* the sound 20XX plays as L or R gives a costume */

#define DOORS ((volatile u8*) 0x803F0DFCu)  /* mnCharSel_803F0DFC: 0x24 a player: costume +0xD, icon +0xE */
#define ICONS ((const u8*) 0x803F0B24u)     /* the screen's icons, 0x1C each: the character +1 */
#define ICON_COUNT 0x19u
#define FIGHTERS ((const s8*) 0x803BCDE0u)  /* ftMapping_list, 3 bytes a character: its fighter, a second or -1 */
#define COSTUME_FILES ((const u32*) 0x803C2360u) /* ftData_803C2360, by fighter: 12 bytes a costume, its file's name first */
#define PAD_R 0x20u
#define PAD_L 0x40u

/* The screen loads the players' characters ahead, each frame asking for the
 * (character, costume) pairs it wants (mnCharSel_Scene_OnFrame); what is
 * loaded already stays. A costume whose file has just changed is the same
 * pair: its player is left out for one frame, so that the old file is let go
 * and the pair looked up again the next. */
void lbDvd_80018254(void);                  /* loads what is asked for */
void* lbDvd_GetPreloadCacheScene(void);     /* what is asked for: the players' pairs from +0x10, 8 bytes each */
#define NO_CHARACTER 0x21
static u8 s_again; /* the players to leave out, a bit each */

static const char* costume_file(int fighter, u32 costume)
{
    u32 list, name;
    if (fighter < 0 || fighter >= 33 || costume >= 8) {
        return NULL;
    }
    list = COSTUME_FILES[fighter];
    if (list < 0x80003100u || list >= 0x817F0000u) {
        return NULL;
    }
    name = *(const u32*) (list + costume * 12);
    return name >= 0x80003100u && name < 0x817F0000u ? (const char*) name : NULL;
}

/* so_exi.c, each time Slippi's character select asks how its match stands:
 * this console's player's character and costume in the match the game is
 * being given (both players have chosen), or a character of -1 while there
 * is none. From then until the next character select a costume of 20XX's is
 * that player's only (mod_stages.c). */
void melee_wii_costumes_match(int character, int costume)
{
    if (character < 0) {
        melee_wii_costumes_mine(FALSE, NULL, NULL);
    } else if (character >= 33 || costume < 0) {
        melee_wii_costumes_mine(TRUE, NULL, NULL);
    } else {
        melee_wii_costumes_mine(TRUE, costume_file(FIGHTERS[character * 3], (u32) costume),
                                costume_file(FIGHTERS[character * 3 + 1], (u32) costume));
    }
}

/* What a player's costume is, shown on the player's window as 20XX shows
 * it: the window's two colours (the port's red, blue, yellow or green, a
 * light and a dark one) become two of the costume's own. 20XX's are in its
 * MnSlChr.0sd, which it loads at FILE_BASE: by character, the address of
 * the L costumes' colours and of the R ones', 16 bytes a costume there (L's
 * two words, then R's two; a first word of 0xFF: none). They are read from
 * the image as the game starts.
 *
 * The window's background is a texture drawn in two colours, which the game
 * sets as it shows a player's port or team (mnCharSel_8025DB34) and then
 * leaves alone: so the port writes 20XX's over them each frame an L or R
 * costume is chosen, and puts back what the game had set when it no longer
 * is. */
int melee_wii_disc2_file(const char* path, u32* offset, u32* size);
int melee_wii_disc2_read(u32 offset, void* buf, u32 len);
int lb_80011E24(void* root, void** result, ...); /* a joint of a model, by number (the list ends with -1) */

#define FILE_BASE 0x80BEC720u
#define TRIMS 0x80F90D30u
#define TRIM_CHARACTERS 26u
#define TRIM_COSTUMES 6u
#define TRIM_SPAN 0x1000u /* the colours are just before their addresses' table */
#define NO_TRIM 0xFFu
#define SCREEN_MODEL (*(void* volatile*) 0x804D6CC0u) /* mnCharSel_804D6CC0 */
#define SINGLE_PLAYER (*(volatile u8*) 0x804D6CF5u)    /* mnCharSel_804D6CF5: 1 in the one-player modes */
#define SINGLE_JOINT 0x29                              /* the one window's background there */

static u32 s_trim[TRIM_CHARACTERS][TRIM_COSTUMES][4];
static BOOL s_trims;

static u32 word(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

/* mod_values.c, once the image is open */
void melee_wii_fighters_start(void)
{
    static u8 table[(TRIM_CHARACTERS * 8 + 31) & ~31u] ATTRIBUTE_ALIGN(32);
    u32 file, size, low = 0xFFFFFFFFu, high = 0, count = 0;
    u8* colors;
    s_trims = FALSE;
    memset(s_trim, 0, sizeof(s_trim));
    if (melee_wii_disc2_file("/MnSlChr.0sd", &file, &size) < 0 || size < TRIMS - FILE_BASE + sizeof(table) ||
        melee_wii_disc2_read(file + (TRIMS - FILE_BASE), table, sizeof(table)) < 0)
    {
        return;
    }
    for (u32 c = 0; c < TRIM_CHARACTERS; c++) {
        u32 l = word(table + c * 8), r = word(table + c * 8 + 4);
        if (l == 0 && r == 0) {
            continue; /* (a character with no costumes of 20XX's) */
        }
        if (r != l + 8 || l < FILE_BASE || l >= TRIMS || TRIMS - l > TRIM_SPAN) {
            OSReport("20XX costumes: the windows' colours are not where they are known to be: the game's own\n");
            return;
        }
        low = l < low ? l : low;
        high = l > high ? l : high;
    }
    if (high == 0) {
        return;
    }
    high += TRIM_COSTUMES * 16;
    high = high > TRIMS ? TRIMS : high;
    colors = memalign(32, (high - low + 31) & ~31u);
    if (colors == NULL || melee_wii_disc2_read(file + (low - FILE_BASE), colors, high - low) < 0) {
        free(colors);
        return;
    }
    for (u32 c = 0; c < TRIM_CHARACTERS; c++) {
        u32 l = word(table + c * 8);
        for (u32 n = 0; l != 0 && n < TRIM_COSTUMES && l + (n + 1) * 16 <= high; n++) {
            const u8* at = colors + (l - low) + n * 16;
            for (u32 i = 0; i < 4; i++) {
                s_trim[c][n][i] = word(at + i * 4);
            }
            count += (s_trim[c][n][0] != NO_TRIM) + (s_trim[c][n][2] != NO_TRIM);
        }
    }
    free(colors);
    s_trims = TRUE;
    OSReport("20XX costumes: the windows' colours for its costumes read (%u places)\n", (unsigned) count);
}

/* The two colours of a window's background: the first texture of the
 * joint's that is drawn in colours of its own (HSD_TObj's tev: its konst and
 * tev0, one after the other) */
static volatile u32* window_colors(u32 door)
{
    void* joint = NULL;
    u32 dobj, mobj, tobj, tev;
    void* model = SCREEN_MODEL;
    int number;
    if (SINGLE_PLAYER == 1) {
        if (door != 0) {
            return NULL;
        }
        number = SINGLE_JOINT;
    } else {
        number = DOORS[door * 0x24 + 4];
    }
    if (model == NULL || lb_80011E24(model, &joint, number, -1) == 0 || joint == NULL) {
        return NULL;
    }
#define PTR(a) ((a) >= 0x80003100u && (a) < 0x817F0000u && ((a) & 3) == 0)
    for (dobj = *(u32*) ((u8*) joint + 0x18); PTR(dobj); dobj = *(u32*) (dobj + 4)) {
        mobj = *(u32*) (dobj + 8);
        for (tobj = PTR(mobj) ? *(u32*) (mobj + 8) : 0; PTR(tobj); tobj = *(u32*) (tobj + 8)) {
            tev = *(u32*) (tobj + 0xA8);
            if (PTR(tev)) {
                return (volatile u32*) (tev + 0x10);
            }
        }
    }
#undef PTR
    return NULL;
}

/* The set (1: L's, 2: R's) of the costume a player's window shows, and the
 * colours 20XX gives the window for it */
static u32 chosen(u32 door, const u32** colors)
{
    const u32* on = melee_wii_mod_value(MOD_COSTUMES);
    u32 icon = DOORS[door * 0x24 + 0xE], costume = DOORS[door * 0x24 + 0xD], character, set;
    *colors = NULL;
    if (on == NULL || *on == 0 || icon >= ICON_COUNT || DOORS[door * 0x24 + 0xF] >= ICON_COUNT) {
        return 0;
    }
    character = ICONS[icon * 0x1C + 1];
    if (character >= 33) {
        return 0;
    }
    set = (u32) melee_wii_costumes_chosen(costume_file(FIGHTERS[character * 3], costume));
    if (set != 0 && s_trims && character < TRIM_CHARACTERS && costume < TRIM_COSTUMES &&
        s_trim[character][costume][(set - 1) * 2] != NO_TRIM)
    {
        *colors = &s_trim[character][costume][(set - 1) * 2];
    }
    return set;
}

static void windows(void)
{
    static u32 s_wrote[4][2], s_games[4][2];
    static u8 s_ours[4];
    for (u32 i = 0; i < 4; i++) {
        const u32* want;
        volatile u32* now;
        chosen(i, &want);
        if (want == NULL && !s_ours[i]) {
            continue;
        }
        now = window_colors(i);
        if (now == NULL) {
            s_ours[i] = 0;
            continue;
        }
        if (want != NULL) {
            /* (colours that are not the ones written: the game has set its
             * own since, or this is another screen) */
            if (!s_ours[i] || now[0] != s_wrote[i][0] || now[1] != s_wrote[i][1]) {
                s_games[i][0] = now[0];
                s_games[i][1] = now[1];
            }
            now[0] = s_wrote[i][0] = want[0];
            now[1] = s_wrote[i][1] = want[1];
            s_ours[i] = 1;
        } else {
            if (now[0] == s_wrote[i][0] && now[1] == s_wrote[i][1]) {
                now[0] = s_games[i][0];
                now[1] = s_games[i][1];
            }
            s_ours[i] = 0;
        }
    }
}

void melee_wii_css_preload(void)
{
    windows();
    if (s_again != 0) {
        u8* asked = lbDvd_GetPreloadCacheScene();
        for (u32 i = 0; asked != NULL && i < 4; i++) {
            if ((s_again >> i) & 1) {
                *(volatile s32*) (asked + 0x10 + i * 8) = NO_CHARACTER;
            }
        }
        s_again = 0;
    }
    lbDvd_80018254();
}

void melee_wii_css_costume(int door, u32 input)
{
    const u32* on = melee_wii_mod_value(MOD_COSTUMES);
    if (on != NULL && *on != 0 && (input & (PAD_L | PAD_R)) != 0 && door >= 0 && door < 4) {
        u32 icon = DOORS[door * 0x24 + 0xE], costume = DOORS[door * 0x24 + 0xD];
        /* (a character chosen, as the game's own costume change wants) */
        if (icon < ICON_COUNT && DOORS[door * 0x24 + 0xF] < ICON_COUNT) {
            u32 character = ICONS[icon * 0x1C + 1];
            u32 set = (input & PAD_L) != 0 ? 1 : 2;
            int now = character < 33 ? melee_wii_costumes_toggle(costume_file(FIGHTERS[character * 3], costume), set) : -1;
            if (now >= 0) {
                melee_wii_costumes_set(costume_file(FIGHTERS[character * 3 + 1], costume), (u32) now);
                OSReport("20XX costumes: player %d's costume is %s\n", door + 1,
                         now == 0 ? "the game's own" : now == 1 ? "20XX's first" : "20XX's second");
                mnCharSel_8025DB34((u8) door);
                s_again |= (u8) (1u << door);
                /* (after the screen's own work on the window: a sound before
                 * it was not heard) 20XX's sound for one of its costumes,
                 * played as the menus' "forward" is; the menus' "back" for
                 * the game's own */
                if (now != 0) {
                    HSD_AudioSFXStartParam(SOUND_COSTUME, 254, 128, 0x13, 3);
                } else {
                    lbAudioAx_80024030(0);
                }
            } else {
                lbAudioAx_80024030(3);
            }
        }
    }
    mnCharSel_CostumeChange(door, input);
}

#endif
