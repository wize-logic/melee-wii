/* 20XX's extras in Slippi Online (mod_values.c): the
 * addresses of 20XX's variables the port acts on (1.02), and the parts. */
#ifndef MELEE_WII_MOD_VALUES_H
#define MELEE_WII_MOD_VALUES_H

#include <dolphin/types.h>

/* The music playlists: 11 (the last is the global one), each a type and 20
 * songs ended by a 0, in 0x60 bytes with its name */
#define MOD_PLAYLISTS 11
#define MOD_GLOBAL_PLAYLIST (MOD_PLAYLISTS - 1)
#define MOD_SLOTS 20
#define MOD_LIST_WORDS 24
#define MOD_OWNERS 0x803FA8E0u /* whose song each replaces: a song name's address */
#define MOD_TYPES 0x803FA940u
#define MOD_LISTS 0x803FA9B0u
#define MOD_GLOBAL_ON 0x803FAE94u
#define MOD_AFFECTS_MENU 0x803FA17Cu
#define MOD_SONG_PREVIEW 0x803FA3CCu

/* The stages' looks: Pokemon Stadium, Dream Land, Fountain, Yoshi's Story,
 * Final Destination, Battlefield */
#define MOD_SKINS 6
#define MOD_SKINS_ADDR 0x803FA1D4u

/* Final Destination's custom colour: blue, green, red (0 to 255), on */
#define MOD_FD_COLOR 0x803FA398u

/* The music off, and the stages' second songs off */
#define MOD_MUSIC_OFF 0x803FA2F0u
#define MOD_ALT_MUSIC_OFF 0x803FA364u

/* The players' shield colours: for players 4 to 1, blue, green, red (0 to
 * 255), on */
#define MOD_SHIELDS 0x803FA28Cu
#define MOD_SHIELD_WORDS 16

/* 20XX's costumes on: L and R on the character select screen choose a
 * costume's other two. Not a variable of 20XX's (it has one for each
 * button): the port's own, under a number that is no address. */
#define MOD_COSTUMES 0xFFFF0000u

/* mod_values.c */
int melee_wii_mod_start(const char* image);
u32* melee_wii_mod_value(u32 addr); /* the port's word for a 20XX variable, or NULL */
int melee_wii_mod_dol_read(u32 addr, void* buf, u32 len);
int melee_wii_mod_save(void);

/* mod_songs.c */
int melee_wii_songs_start(void);
s32 melee_wii_songs_substitute(const char* path);
u32 melee_wii_songs_count(void);     /* 20XX's songs: the numbers below it */
void melee_wii_songs_preview(u32 song); /* the next song asked for is this one; 0: no more */

/* mod_stages.c */
void melee_wii_stages_start(void);
void melee_wii_stages_scene(int kind);
/* (the launcher's screen) called with each file as it is checked */
void melee_wii_stages_progress(void (*fn)(const char* name, int done, int total));
s32 melee_wii_stages_substitute(const char* path);
/* 1 when that look of that stage (MOD_SKINS_ADDR's order) can be played */
int melee_wii_stages_allowed(u32 stage, u32 skin);
int melee_wii_costumes_toggle(const char* file, u32 set);
void melee_wii_costumes_set(const char* file, u32 set);
int melee_wii_costumes_chosen(const char* file);
void melee_wii_costumes_mine(BOOL match, const char* file, const char* second);

/* mod_fighters.c */
void melee_wii_fighters_start(void);

/* mod_songs.c (more) */
void melee_wii_songs_scene(int kind);

/* mod_menu.c */
void melee_wii_menu_start(void);
void melee_wii_menu_scene(int kind);

#endif
