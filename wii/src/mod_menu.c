/* 20XX's menu in the plain game and Slippi Online (mod_values.c): Y on the
 * main menu opens it.
 *
 * 20XX's menu is the game's own debug menu (if/textlib_1.c: lists of 32-byte
 * items, each a kind, a callback, a label, the choices' names, the value's
 * address and its bounds) with 20XX's lists, which are data in its
 * MnSlChr.0sd. The game's menu code and its debug menu's scene are still in
 * the game, so the port has them show a copy of 20XX's lists:
 *
 * - The copy is made as the game starts, from the player's image, into the
 *   port's memory (MEM2), with only what the port acts on: an item is kept
 *   when its variable is one mod_values.c has (the music's, the stages'
 *   looks), a submenu when something of it is kept. Its values are the
 *   port's words, not 20XX's addresses (in the plain game those are the
 *   game's own debug menu's data).
 * - Every item's callback is the port's (menu_event): opening a submenu,
 *   going back, a song played with A (never by going through the songs).
 *   None of 20XX's code runs.
 * - Y on the main menu's first screen (or Slippi's Online Play screen, where
 *   Slippi Online starts) leaves for the game's debug mode, as a
 *   menu entry leaves for its mode; that mode's scene opens its menu with a
 *   call the port takes over (cmake/Wii.cmake's --redirect) to give it this
 *   list in place of the game's. B at its top goes back to the main menu.
 *
 * It can't be reached during a match or from the character select, so no
 * value changes while a match's frames are played, or played again. */

#ifdef MELEE_ORIG_CODE

#include "melee_wii.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>

#include "mod_values.h"

/* dvd.c */
int melee_wii_disc2_file(const char* path, u32* offset, u32* size);
int melee_wii_disc2_read(u32 offset, void* buf, u32 len);

/* ---- the game's (1.02) --------------------------------------------------- */

typedef int (*MenuCallback)(int event);
enum { EVENT_B, EVENT_A, EVENT_LEFT, EVENT_RIGHT, EVENT_INIT, EVENT_CLOSE, EVENT_START };
enum { KIND_TEXT, KIND_ACTION, KIND_CHOICE, KIND_INT, KIND_INT_SIGNED, KIND_FLOAT = 8, KIND_END = 9 };

typedef struct MenuItem {
    s32 kind;
    MenuCallback callback;
    const char* label;
    void* names; /* a choice's names; here, an action's submenu */
    void* value;
    f32 min, max, step;
} MenuItem;

typedef struct DevText {
    s16 x, y;
    u8 w, h, cursor_x, cursor_y;
    f32 scale_x, scale_y;
    u32 bg_color;
    u32 text_colors[4];
} DevText;

typedef struct Menu {
    u8 cursor;
    u8 flags; /* 1 draw again, 0x10 under a submenu, 0x20 hidden */
    DevText* text;
    MenuItem* items;
    MenuCallback callback;
    void* gobj;
    void* proc;
    struct Menu* prev; /* its submenu */
    struct Menu* next; /* the menu it is the submenu of */
} Menu;

void* un_80304168(void* items, int arg1, int x, int y);            /* a scene's menu */
Menu* un_80304210(Menu* parent, void* items, int arg2, int dx, int dy); /* a submenu */
void un_80304334(Menu* menu);                                      /* closes it */
Menu* un_80302DF0(void);                                           /* the menu in use */
void lbAudioAx_80024030(int sound);                                /* 0 back, 1 forward, 2 move */
int lbAudioAx_800236DC(void);                                      /* stops the music */
int lbAudioAx_80023F28(int song);                                  /* plays a song */
void mn_80229860(s8 mode);                                         /* the main menu leaves for a mode */
int mn_8022F218(void);
int un_803039A4(u8 pad); /* the debug menu's directions, from a pad's stick */
int mn_80229938(int menu, int option); /* is this option of a main menu screen unlocked? */
void DCFlushRange(void* addr, u32 len);
void ICInvalidateRange(void* addr, u32 len);
u64 gm_GetButtonsTriggered(u8 pad);
void gm_ChangeGameModeAfterCurrentScene(int mode);
void gm_801A4B60(void);
void* DevText_GetGObj(void);
DevText* DevText_Create(char id, int x, int y, int w, int h, void* buf);
void DevText_Show(void* gobj, DevText* text);
void DevText_HideCursor(DevText* text);
void DevText_ShowBackground(DevText* text);
void DevText_HideBackground(DevText* text);
void DevText_ShowText(DevText* text);
void DevText_HideText(DevText* text);
void DevText_SetScale(DevText* text, f32 x, f32 y);
void DevText_Erase(DevText* text);
void DevText_Print(DevText* text, char* str);

#define CURRENT_ITEM (*(MenuItem* volatile*) 0x804D6E48)   /* un_804D6E48: the item whose callback runs */
#define MAIN_MENU_KIND (*(volatile u8*) 0x804A04F0)         /* mn_804A04F0.cur_menu */
/* The main menu's screens Y opens the menu from: its first, and the one
 * Slippi's codeset adds and starts on (Online Play: a screen the game has no
 * use for; Slippi's reads A, B and the directions) */
#define MENU_KIND_MAIN 0
#define MENU_KIND_ONLINE 8
#define MAIN_MENU_COOLDOWN (*(volatile u16*) 0x804D6BC8)    /* mn_804D6BC8.cooldown */
/* HSD_PadCopyStatus[pad]: its buttons pressed this frame, and held long enough to repeat */
#define PAD_TRIGGER(pad) (*(volatile u32*) (0x804C20BCu + (pad) * 0x44u + 8))
#define PAD_REPEAT(pad) (*(volatile u32*) (0x804C20BCu + (pad) * 0x44u + 0xC))
/* The main menu's mode, as it is entered, chooses its screen by the mode left
 * (gmmenumode.c's onEnter): a table of where to go on, a word a mode, in the
 * game's data. Slippi's codeset has its own entry there for its online mode
 * (8), which comes back to Online Play. */
#define MENU_RETURN(mode) (*(volatile u32*) (0x803DD8E8u + (mode) * 4u))
#define GM_ONLINE 8
#define MAIN_MENU_HOVERED (*(volatile u16*) 0x804A04F2) /* mn_804A04F0.hovered_selection */
#define EXTRA_OPTION 6u /* Slippi's Log-out's place on that screen: see "Extra" below */
/* Slippi's codeset's: the online mode chosen (0 Ranked, 1 Unranked...), which
 * is also the Online Play screen's option the main menu comes back on */
#define SLIPPI_ONLINE_MODE (*(volatile u8*) (0x804DB6A0u - 0x5060u))
#define GM_MENU 1
#define GM_DEBUG 6
#define GAME_SONG_MENU 0x34
#define PAD_Y 0x800u
#define PAD_A 0x100u
#define PAD_START 0x1000u

/* ---- 20XX's lists -------------------------------------------------------- */

#define FILE_BASE 0x80BEC720u    /* where 20XX loads MnSlChr.0sd */
#define PREVIEW_NAME 0x803BC308u /* a text item's label there: the played song's file name */
#define ARENA_SIZE (48u * 1024u)
#define MAX_ITEMS 64
#define MAX_DEPTH 4

static const u8* s_file; /* while the copy is made */
static u32 s_file_size;
static u8* s_arena;
static u32 s_arena_used;
static BOOL s_overflow;
static MenuItem* s_root;
static const char** s_song_names;
static char s_preview_name[16] = " ";

static BOOL s_open;      /* Y was pressed: the debug scene's menu is ours */
static BOOL s_in_menu;   /* it is on screen */
static BOOL s_extra;     /* Slippi's menu files have "Extra" where Ranked was */
static BOOL s_unlocked;  /* and that option is shown */
static BOOL s_on_extra;  /* the menu was opened from that option */
static BOOL s_restore_mode;
static u8 s_online_mode; /* Slippi's online mode, while its place holds Extra's number */
static u8 s_from;        /* the main menu's screen Y was pressed on */
static u32 s_debug_return; /* the game's own MENU_RETURN of its debug mode */
static BOOL s_changed;   /* a value: to be written as the menu is left */
static BOOL s_previewed; /* a song was played from the menu */

static int menu_event(int event);

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

static void* arena(u32 size)
{
    void* p = s_arena + s_arena_used;
    size = (size + 3) & ~3u;
    if (s_arena == NULL || size > ARENA_SIZE - s_arena_used) {
        s_overflow = TRUE;
        return NULL;
    }
    s_arena_used += size;
    memset(p, 0, size);
    return p;
}

static const u8* at(u32 addr, u32 len)
{
    if (addr < FILE_BASE || addr - FILE_BASE > s_file_size || len > s_file_size - (addr - FILE_BASE)) {
        return NULL;
    }
    return s_file + (addr - FILE_BASE);
}

/* The text at an address of the file: printable, of at most 79 characters */
static const char* text_at(u32 addr)
{
    const u8* p = at(addr, 1);
    u32 n = 0;
    if (p == NULL) {
        return NULL;
    }
    while (at(addr + n, 1) != NULL && p[n] != '\0') {
        if (p[n] < 0x20 || p[n] > 0x7E || n >= 79) {
            return NULL;
        }
        n++;
    }
    return at(addr + n, 1) != NULL ? (const char*) p : NULL;
}

/* The same, of 20XX's executable (the game's own "OFF" and "ON" are there):
 * kept until the next one is asked for */
static const char* dol_text(u32 addr)
{
    static char text[80];
    u32 n;
    memset(text, 0, sizeof(text));
    /* (as much as there is: a text may be at a section's end) */
    for (n = sizeof(text) - 1; n >= 8 && melee_wii_mod_dol_read(addr, text, n) < 0; n /= 2) {
    }
    if (n < 8) {
        return NULL;
    }
    text[n] = '\0';
    for (n = 0; text[n] != '\0'; n++) {
        if ((u8) text[n] < 0x20 || (u8) text[n] > 0x7E) {
            return NULL;
        }
    }
    return n != 0 && n < sizeof(text) - 1 ? text : NULL;
}

/* The word at an address of the file, or of 20XX's executable; 1 when there */
static BOOL word_at(u32 addr, u32* out)
{
    const u8* p = at(addr, 4);
    u8 raw[4];
    if (p == NULL) {
        if (melee_wii_mod_dol_read(addr, raw, 4) < 0) {
            return FALSE;
        }
        p = raw;
    }
    *out = be32(p);
    return TRUE;
}

static char* copy_text(const char* text)
{
    char* p = arena((u32) strlen(text) + 1);
    if (p != NULL) {
        strcpy(p, text);
    }
    return p;
}

static BOOL is_song(const void* value)
{
    return value == melee_wii_mod_value(MOD_SONG_PREVIEW) ||
           ((const u32*) value >= melee_wii_mod_value(MOD_LISTS) &&
            (const u32*) value < melee_wii_mod_value(MOD_LISTS) + MOD_PLAYLISTS * MOD_LIST_WORDS);
}

/* The stage whose look this value is, or -1 */
static int skin_of(const void* value)
{
    const u32* skins = melee_wii_mod_value(MOD_SKINS_ADDR);
    return (const u32*) value >= skins && (const u32*) value < skins + MOD_SKINS ? (int) ((const u32*) value - skins)
                                                                               : -1;
}

/* The songs' names, by number: 20XX's table's (it names its custom songs
 * too), or the file's name */
static const char** song_names(u32 table)
{
    u32 count = melee_wii_songs_count();
    if (s_song_names != NULL) {
        return s_song_names;
    }
    s_song_names = arena(count * 4);
    for (u32 i = 0; s_song_names != NULL && i < count; i++) {
        const u8* entry = at(table + i * 4, 4);
        const char* name = entry != NULL ? text_at(be32(entry)) : NULL;
        char file[24];
        if (name == NULL || name[0] == '\0') {
            snprintf(file, sizeof(file), "CUSTOM SONG %02X.hps", (unsigned) i);
            name = file;
        }
        s_song_names[i] = copy_text(name);
    }
    return s_song_names;
}

/* The lists made, by 20XX's address (a list is reached from several, and one
 * being made is not gone into again) */
static struct {
    u32 addr;
    MenuItem* list;
} s_made[48];
static u32 s_made_count;

static MenuItem* copy_list(u32 addr, int depth)
{
    MenuItem items[MAX_ITEMS];
    MenuItem* list;
    u32 n = 0, kept = 0, made;
    BOOL dropped = FALSE;
    for (u32 i = 0; i < s_made_count; i++) {
        if (s_made[i].addr == addr) {
            return s_made[i].list;
        }
    }
    if (depth > MAX_DEPTH || s_made_count >= sizeof(s_made) / sizeof(s_made[0])) {
        return NULL;
    }
    made = s_made_count++;
    s_made[made].addr = addr;
    s_made[made].list = NULL;
    for (u32 i = 0; i < MAX_ITEMS; i++) {
        const u8* raw = at(addr + i * 0x20, 0x20);
        MenuItem item;
        u32 kind, label, names, variable;
        const char* text;
        if (raw == NULL) {
            return NULL;
        }
        kind = be32(raw);
        label = be32(raw + 8);
        names = be32(raw + 0xC);
        variable = be32(raw + 0x10);
        if (kind == KIND_END) {
            break;
        }
        if (kind > KIND_END || n >= MAX_ITEMS - 1) {
            return NULL;
        }
        memset(&item, 0, sizeof(item));
        item.kind = (s32) kind;
        item.callback = menu_event;
        memcpy(&item.min, raw + 0x14, 12); /* (both big-endian) */
        text = label == 0 ? "" : label == PREVIEW_NAME ? NULL : text_at(label);
        if (kind == KIND_TEXT) {
            /* a line of text; not one that explains an item left out, nor a
             * second blank line */
            BOOL blank = label == 0 || (text != NULL && text[strspn(text, " -")] == '\0');
            if (label == PREVIEW_NAME) {
                item.label = s_preview_name;
            } else if (text == NULL || (dropped && !blank) || (blank && n != 0 && items[n - 1].label[0] == '\0')) {
                dropped = dropped && !blank;
                continue;
            } else {
                item.label = blank ? "" : copy_text(text);
            }
            dropped = FALSE;
        } else if (kind == KIND_ACTION) {
            /* a submenu (20XX keeps its list's address where a value's
             * minimum is): kept when something of it is */
            MenuItem* sub = text != NULL ? copy_list(be32(raw + 0x14), depth + 1) : NULL;
            dropped = sub == NULL;
            if (dropped) {
                continue;
            }
            item.label = copy_text(text);
            item.names = sub;
            item.min = item.max = item.step = 0;
            kept++;
        } else {
            u32* value = melee_wii_mod_value(variable);
            int count = (int) item.max;
            dropped = value == NULL || text == NULL || (kind != KIND_CHOICE && kind != KIND_INT);
            if (dropped) {
                continue;
            }
            item.label = copy_text(text);
            item.value = value;
            if (kind == KIND_CHOICE && is_song(value)) {
                item.names = song_names(names);
                item.max = (f32) melee_wii_songs_count();
                if (*value < (u32) item.min) {
                    *value = (u32) item.min;
                }
            } else if (kind == KIND_CHOICE) {
                const char** table;
                u32 first;
                /* (its names: in the file, or the executable's) */
                if (count < 1 || count > 64 || !word_at(names, &first) || !word_at(names + (u32) (count - 1) * 4, &first)) {
                    dropped = TRUE;
                    continue;
                }
                table = arena((u32) count * 4);
                for (int k = 0; table != NULL && k < count; k++) {
                    const char* name = NULL;
                    u32 where;
                    if (word_at(names + (u32) k * 4, &where)) {
                        name = text_at(where);
                        if (name == NULL) {
                            name = dol_text(where);
                        }
                    }
                    table[k] = name != NULL ? copy_text(name) : "?";
                }
                item.names = table;
                if (*value >= (u32) count) {
                    *value = 0;
                }
            }
            kept++;
        }
        items[n++] = item;
    }
    while (n != 0 && items[n - 1].kind == KIND_TEXT && items[n - 1].label[0] == '\0') {
        n--;
    }
    if (kept == 0 || s_overflow) {
        return NULL;
    }
    list = arena((n + 1) * sizeof(MenuItem));
    if (list == NULL) {
        return NULL;
    }
    memcpy(list, items, n * sizeof(MenuItem));
    list[n].kind = KIND_END;
    list[n].callback = menu_event;
    s_made[made].list = list;
    return list;
}

/* 20XX's list of this title (its first item's label), or 0 */
static u32 find_list(const char* title)
{
    size_t len = strlen(title) + 1;
    for (u32 i = 0; i + len <= s_file_size; i++) {
        u32 text;
        if (s_file[i] != (u8) title[0] || memcmp(s_file + i, title, len) != 0) {
            continue;
        }
        text = FILE_BASE + i;
        for (u32 k = 0; k + 0x20 <= s_file_size; k += 4) {
            if (be32(s_file + k + 8) == text && be32(s_file + k) == KIND_TEXT && be32(s_file + k + 4) == 0) {
                return FILE_BASE + k;
            }
        }
    }
    return 0;
}

/* mod_values.c, as the game starts: the copy. Without it (not the 20XX this
 * knows) there is no menu, and the values stay as the file has them. */
void melee_wii_menu_start(void)
{
    static const struct {
        const char* label;
        const char* title;
    } TOP[] = {
        { "MUSIC >", "<MUSIC CODES>" },
        { "STAGE TEXTURES >", "<TEXTURE HACKS>" },
        { "STAGE COLORS >", "<STAGE CODES>" },
        { "SHIELD COLORS >", "<CUSTOM SHIELD COLORS>" },
    };
    MenuItem items[20];
    u32 offset, size, n = 0, kept = 0;
    u8* file;
    if (s_root != NULL || melee_wii_disc2_file("/MnSlChr.0sd", &offset, &size) < 0 || size < 0x1000 ||
        size > 16u * 1024 * 1024)
    {
        return;
    }
    file = memalign(32, (size + 31) & ~31u);
    s_arena = malloc(ARENA_SIZE);
    if (file == NULL || s_arena == NULL || melee_wii_disc2_read(offset, file, size) < 0) {
        OSReport("20XX menu: its lists could not be read\n");
        free(file);
        return;
    }
    s_file = file;
    s_file_size = size;
    s_arena_used = 0;
    s_overflow = FALSE;
    memset(items, 0, sizeof(items));
    items[n++].label = "<20XX MENU>";
    items[n++].label = "";
    for (u32 i = 0; i < sizeof(TOP) / sizeof(TOP[0]); i++) {
        u32 addr = find_list(TOP[i].title);
        MenuItem* sub = addr != 0 ? copy_list(addr, 0) : NULL;
        if (sub != NULL) {
            items[n].kind = KIND_ACTION;
            items[n].label = TOP[i].label;
            items[n].names = sub;
            n++;
            kept++;
        }
    }
    {
        static const char* OFF_ON[2] = { "OFF", "ON" };
        items[n].kind = KIND_CHOICE;
        items[n].label = "20XX COSTUMES :";
        items[n].names = OFF_ON;
        items[n].value = melee_wii_mod_value(MOD_COSTUMES);
        items[n].min = 0.0f;
        items[n].max = 2.0f;
        items[n].step = 1.0f;
        n++;
        kept++;
        items[n++].label = "L AND R ON THE CHARACTER SELECT SCREEN";
    }
    items[n++].label = "";
    items[n++].label = "A : OPEN OR PLAY     B : BACK";
    for (u32 i = 0; i < n; i++) {
        items[i].callback = menu_event;
    }
    if (kept != 0 && !s_overflow) {
        s_root = arena((n + 1) * sizeof(MenuItem));
    }
    if (s_root != NULL) {
        memcpy(s_root, items, n * sizeof(MenuItem));
        s_root[n].kind = KIND_END;
        s_root[n].callback = menu_event;
    }
    s_file = NULL;
    free(file);
    OSReport("20XX menu: %s (%u lists, %u bytes)\n",
             s_root != NULL ? "Y on the main menu opens it" : "its lists were not found: no menu",
             (unsigned) s_made_count, (unsigned) s_arena_used);
}

/* ---- the menu at work ---------------------------------------------------- */

/* Plays a song of 20XX's, by number (0: silence): the menu's song is asked
 * for, and mod_songs.c gives this one */
static void preview(u32 song)
{
    lbAudioAx_800236DC();
    s_previewed = TRUE;
    if (song == 0 || song >= melee_wii_songs_count()) {
        snprintf(s_preview_name, sizeof(s_preview_name), " ");
        return;
    }
    snprintf(s_preview_name, sizeof(s_preview_name), "%02X.hps", (unsigned) song);
    melee_wii_songs_preview(song);
    lbAudioAx_80023F28(GAME_SONG_MENU);
    melee_wii_songs_preview(0);
}

static void leave(void)
{
    if (s_previewed) {
        /* (the main menu asks for its song again) */
        lbAudioAx_800236DC();
        s_previewed = FALSE;
    }
    if (s_changed) {
        melee_wii_mod_save();
        s_changed = FALSE;
    }
    OSReport("20XX menu: left\n");
    s_in_menu = FALSE;
    /* back to the screen it was opened from: Online Play as Slippi's own mode
     * comes back to it, the first screen as the game's debug mode does */
    if (s_debug_return == 0) {
        s_debug_return = MENU_RETURN(GM_DEBUG);
    }
    MENU_RETURN(GM_DEBUG) = s_from == MENU_KIND_ONLINE ? MENU_RETURN(GM_ONLINE) : s_debug_return;
    if (s_from == MENU_KIND_ONLINE && s_on_extra) {
        SLIPPI_ONLINE_MODE = EXTRA_OPTION;
        s_restore_mode = TRUE;
    }
    gm_ChangeGameModeAfterCurrentScene(GM_MENU);
    gm_801A4B60();
}

/* Every item's callback. Not 0: the scene's own callback (the game's debug
 * menu's) is not to run. */
static int menu_event(int event)
{
    MenuItem* item = CURRENT_ITEM;
    Menu* menu = un_80302DF0();
    int stage;
    if (item == NULL || menu == NULL) {
        return 1;
    }
    switch (event) {
    case EVENT_B:
    case EVENT_START:
        lbAudioAx_80024030(0);
        if (menu->next != NULL && event == EVENT_B) {
            menu->next->flags &= (u8) ~0x20;
            un_80304334(menu);
        } else {
            leave();
        }
        break;
    case EVENT_A:
        if (item->kind == KIND_ACTION && item->names != NULL) {
            /* the submenu where this one is, which hides */
            DevText* text = menu->text;
            lbAudioAx_80024030(1);
            if (un_80304210(menu, item->names, 0, -(int) (text->scale_x * (f32) text->w),
                            -(int) (text->scale_y * (f32) menu->cursor)) != NULL)
            {
                menu->flags |= 0x20;
            }
        } else if (item->kind == KIND_CHOICE && is_song(item->value)) {
            preview(*(u32*) item->value);
            menu->flags |= 1;
        }
        break;
    case EVENT_LEFT:
    case EVENT_RIGHT:
        s_changed = TRUE;
        stage = skin_of(item->value);
        if (stage >= 0) {
            /* past the looks that can't be played (mod_stages.c) */
            u32* value = item->value;
            u32 v = *value, max = (u32) item->max;
            if (event == EVENT_RIGHT) {
                while (v < max && !melee_wii_stages_allowed((u32) stage, v)) {
                    v++;
                }
                if (v >= max) {
                    v = *value; /* none further: back to where it was */
                }
            }
            while (v != 0 && !melee_wii_stages_allowed((u32) stage, v)) {
                v--;
            }
            *value = v;
        } else if (item->value == melee_wii_mod_value(MOD_MUSIC_OFF)) {
            /* off: silence now; on again: the main menu asks for its song */
            lbAudioAx_800236DC();
            s_previewed = TRUE;
        }
        break;
    default:
        break;
    }
    return 1;
}

/* ---- the game's calls the port takes over -------------------------------- */

/* mnMain_Scene_OnFrame's call of mn_8022F218 (L+R+Start), each frame of the
 * main menu: Y opens the menu from the screens above, where a hint says so */
int melee_wii_menu_frame(void)
{
    if (s_root != NULL) {
        BOOL here = MAIN_MENU_KIND == MENU_KIND_MAIN || MAIN_MENU_KIND == MENU_KIND_ONLINE;
        /* No on-screen "Y 20XX MENU" hint: the Extra button is the entry on the
         * Online Play screen, and Y still opens it from the menus below. */
        if (s_restore_mode) {
            /* (it was on Extra's number for the screen to come back on it) */
            SLIPPI_ONLINE_MODE = s_online_mode;
            s_restore_mode = FALSE;
        }
        if (here && MAIN_MENU_COOLDOWN == 0 &&
            ((gm_GetButtonsTriggered(4) & PAD_Y) != 0 ||
             (s_unlocked && MAIN_MENU_KIND == MENU_KIND_ONLINE && MAIN_MENU_HOVERED == EXTRA_OPTION &&
              (gm_GetButtonsTriggered(4) & (PAD_A | PAD_START)) != 0 && mn_80229938(MENU_KIND_ONLINE, EXTRA_OPTION))))
        {
            if ((gm_GetButtonsTriggered(4) & PAD_Y) == 0) {
                /* the Extra option: the screen's own code is not to see the
                 * button (the cooldown), and comes back on this option */
                MAIN_MENU_COOLDOWN = 2;
                s_online_mode = SLIPPI_ONLINE_MODE;
                s_on_extra = TRUE;
            } else {
                s_on_extra = FALSE;
            }
            OSReport("20XX menu: opened from the main menu's screen %d\n", (int) MAIN_MENU_KIND);
            s_open = TRUE;
            s_from = MAIN_MENU_KIND;
            lbAudioAx_80024030(1);
            mn_80229860(GM_DEBUG);
        }
    }
    return mn_8022F218();
}

/* gm_Scene_DebugMenu_OnEnter's call of un_80304168: the debug scene opens
 * its menu */
void* melee_wii_menu_open(void* items, int arg1, int x, int y)
{
    if (s_open && s_root != NULL) {
        s_open = FALSE;
        s_in_menu = TRUE;
        s_changed = FALSE;
        s_previewed = FALSE;
        return un_80304168(s_root, 0, 28, 22);
    }
    return un_80304168(items, arg1, x, y);
}

/* un_80303AC4's call of un_803039A4: the debug menu reads its directions
 * from the stick (and X, Y, L, R). In this menu the D-pad moves too. */
int melee_wii_menu_stick(u8 pad)
{
    u32 moves = (u32) un_803039A4(pad);
    if (s_in_menu && pad < 4) {
        u32 buttons = PAD_TRIGGER(pad) | PAD_REPEAT(pad);
        moves |= (buttons & 8) != 0 ? 0x10000000u : 0; /* up */
        moves |= (buttons & 4) != 0 ? 0x20000000u : 0; /* down */
        moves |= (buttons & 1) != 0 ? 0x40000000u : 0; /* left */
        moves |= (buttons & 2) != 0 ? 0x80000000u : 0; /* right */
    }
    return (int) moves;
}

/* ---- "Extra" on Slippi's Online Play screen -------------------------------
 * Slippi's Online Play screen has eight options, of which it shows those its
 * "is this option unlocked?" code allows: logged in, Unranked, Direct, Teams
 * and Party (Ranked the port locks: wii/src/slippi/so_wii.c). The one after
 * them is Log-out, which Slippi locks always ("use the launcher instead").
 * With the menu on, that place, under Party, is "Extra", and opens it:
 *
 * - its label is one of the textures of Slippi's main menu file, and its
 *   description a text of Slippi's menu texts; both files are kept in RAM
 *   (so_files.c) and changed there before the game loads them. The label is
 *   put together from letters of the game's own labels in the same file
 *   ("Erase Data", and the x of "Fixed-Camera Mode", which is narrower), so
 *   it is the game's font and nothing is drawn by the port.
 * - Slippi's code (at 0x802299F0) compares the option with 6 where it locks
 *   Log-out: those tests are made ones that never hold;
 * - A or Start on it opens the menu as Y does. The screen's own code would
 *   ask "log out?": it reads the buttons through the menu's cooldown, which
 *   is set for that frame. */

u8* so_files_data(const char* name, u32* size); /* wii/src/slippi/so_files.c */

#define LABEL_W 176u
#define LABEL_H 32u /* (30 rows, stored as 32) */
#define LABEL_BYTES (LABEL_W * LABEL_H)
/* In Slippi's MnMaAll.usd as the port makes it: the labels' pixels (IA4:
 * a byte a pixel, alpha then intensity, in blocks of 8 by 4), and what they
 * hash to */
#define LABEL_ERASE 0xE6040u
#define LABEL_FIXED 0x1018A0u
#define LABEL_LOGOUT 0x42C0u
#define HASH_ERASE 0x24A09B0Du
#define HASH_FIXED 0xDA03AAF9u
#define HASH_LOGOUT 0x2AA44EB1u
/* In Slippi's SdMenu.usd: Log-out's description, 96 bytes: 17 of layout,
 * the text, and its end */
#define TEXT_LOGOUT 0xE0u
#define TEXT_BYTES 96u
#define TEXT_HEAD 17u
#define EXTRA_TEXT "20XX music and stage looks."

static u32 hash(const u8* p, u32 n)
{
    u32 h = 0x811C9DC5u;
    while (n-- != 0) {
        h = (h ^ *p++) * 0x01000193u;
    }
    return h;
}

static u8* texel(u8* image, u32 x, u32 y)
{
    return &image[((y / 4) * (LABEL_W / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8];
}

/* launcher.c, once Slippi Online has started (its files are in RAM): makes
 * Log-out's label and description Extra's. Nothing is changed unless both
 * files are the ones this knows. */
void melee_wii_menu_extra(void)
{
    /* the letters: whose label, its first and last column, and the width
     * to stretch it to (0: as it is) */
    static const struct {
        u32 label, first, last, width;
    } LETTERS[5] = {
        { LABEL_ERASE, 9, 25, 0 },    /* E */
        { LABEL_FIXED, 17, 26, 15 },  /* x */
        { LABEL_ERASE, 136, 148, 0 }, /* t */
        { LABEL_ERASE, 26, 38, 0 },   /* r */
        { LABEL_ERASE, 149, 166, 0 }, /* a */
    };
    static const u8 TEXT_HEAD_BYTES[TEXT_HEAD] = { 0x10, 0x0C, 0xAA, 0xAA, 0xAA, 0x0E, 0x00, 0xB3, 0x00,
                                                   0xB3, 0x16, 0x06, 0x00, 0x00, 0x00, 0x00, 0x18 };
    static const u8 TEXT_END[4] = { 0x19, 0x0F, 0x0D, 0x00 };
    static u8 label[LABEL_BYTES];
    u8 text[TEXT_BYTES];
    u32 menu_size = 0, text_size = 0, total = 0, x, n = TEXT_HEAD;
    u8* menu = so_files_data("MnMaAll.usd", &menu_size);
    u8* texts = so_files_data("SdMenu.usd", &text_size);
    if (s_root == NULL || s_extra) {
        return;
    }
    if (menu == NULL || texts == NULL || menu_size < LABEL_FIXED + LABEL_BYTES || text_size < TEXT_LOGOUT + TEXT_BYTES ||
        hash(menu + LABEL_ERASE, LABEL_BYTES) != HASH_ERASE || hash(menu + LABEL_FIXED, LABEL_BYTES) != HASH_FIXED ||
        hash(menu + LABEL_LOGOUT, LABEL_BYTES) != HASH_LOGOUT ||
        memcmp(texts + TEXT_LOGOUT, TEXT_HEAD_BYTES, TEXT_HEAD) != 0 || texts[TEXT_LOGOUT + TEXT_HEAD] != 0x20)
    {
        OSReport("20XX menu: Slippi's menu files are not the ones known: no Extra option (Y opens the menu)\n");
        return;
    }
    /* the label: the letters side by side, a column over each other (their
     * edges' shade), centred, a row lower (as Slippi's own labels are) */
    for (u32 i = 0; i < 5; i++) {
        total += LETTERS[i].width != 0 ? LETTERS[i].width : LETTERS[i].last - LETTERS[i].first + 1;
    }
    total -= 4;
    x = (LABEL_W - total) / 2;
    memset(label, 0, sizeof(label));
    for (u32 i = 0; i < 5; i++) {
        u8* from = menu + LETTERS[i].label;
        u32 columns = LETTERS[i].last - LETTERS[i].first + 1;
        u32 width = LETTERS[i].width != 0 ? LETTERS[i].width : columns;
        for (u32 k = 0; k < width; k++) {
            /* (a stretched letter: between two of its columns, in 256ths) */
            u32 at = LETTERS[i].width != 0 ? k * (columns - 1) * 256 / (width - 1) : k * 256;
            u32 a = at / 256, b = a + 1 < columns ? a + 1 : a, t = at % 256;
            for (u32 y = 0; y + 1 < LABEL_H; y++) {
                u8 va = *texel(from, LETTERS[i].first + a, y), vb = *texel(from, LETTERS[i].first + b, y);
                u32 alpha = ((va >> 4) * (256 - t) + (vb >> 4) * t + 128) / 256;
                u32 shade = ((va & 15) * (256 - t) + (vb & 15) * t + 128) / 256;
                u8* to = texel(label, x + k, y + 1);
                if (alpha > (u32) (*to >> 4)) {
                    *to = (u8) (alpha << 4 | shade);
                }
            }
        }
        x += width - 1;
    }
    /* the description: Slippi's layout, the text in the game's own code (a
     * letter is 0x20 and its number, a space 0x1A), Slippi's end */
    memset(text, 0, sizeof(text));
    memcpy(text, TEXT_HEAD_BYTES, TEXT_HEAD);
    for (const char* c = EXTRA_TEXT; *c != '\0' && n + 2 + sizeof(TEXT_END) <= TEXT_BYTES; c++) {
        if (*c == ' ') {
            text[n++] = 0x1A;
            continue;
        }
        text[n++] = 0x20;
        text[n++] = *c >= '0' && *c <= '9'   ? (u8) (*c - '0')
                    : *c >= 'A' && *c <= 'Z' ? (u8) (0x0A + *c - 'A')
                    : *c >= 'a' && *c <= 'z' ? (u8) (0x24 + *c - 'a')
                                             : 0xE7; /* a full stop */
    }
    memcpy(text + n, TEXT_END, sizeof(TEXT_END));
    memcpy(menu + LABEL_LOGOUT, label, LABEL_BYTES);
    memcpy(texts + TEXT_LOGOUT, text, TEXT_BYTES);
    s_extra = TRUE;
    OSReport("20XX menu: Extra is on Slippi's Online Play screen, under Party\n");
}

/* mod_values.c's scene hook: before the main menu's scene, the option is
 * unlocked. Slippi's code at 0x802299F0 starts "cmpwi r3, 8" and has
 * "cmpwi r4, 6; beq locked" once for each of its three states: each becomes
 * a test with -1. */
void melee_wii_menu_scene(int kind)
{
    u32 insn = *(volatile u32*) 0x802299F0;
    if (kind != 1 || !s_extra || s_unlocked || (insn & 0xFC000003u) != 0x48000000u) {
        return;
    }
    {
        s32 offset = (s32) (insn & 0x03FFFFFCu);
        u32* code;
        u32 done = 0;
        if (offset & 0x02000000) {
            offset -= 0x04000000;
        }
        code = (u32*) (0x802299F0u + (u32) offset);
        if (code[0] != 0x2C030008u) {
            return;
        }
        for (u32 i = 1; i < 64 && done < 3; i++) {
            if (code[i] == (0x2C040000u | EXTRA_OPTION) && (code[i + 1] & 0xFFFF0003u) == 0x41820000u) {
                code[i] = 0x2C04FFFFu;
                DCFlushRange(&code[i], 4);
                ICInvalidateRange(&code[i], 4);
                done++;
            }
        }
        s_unlocked = done != 0;
        OSReport("20XX menu: the Extra option is %s\n", s_unlocked ? "unlocked" : "not there: Slippi's code is not the one known");
    }
}

#endif
