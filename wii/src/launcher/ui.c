/* The launcher's screen (launcher_ui, for launcher.c): Melee or Slippi
 * Online, the settings (the online client's and Slippi's codes for Melee),
 * the game's information with a check of the disc image, and what Slippi
 * Online needs on its first start (its menu files, made from the disc). */

#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../launcher.h"
#include "../slippi/so_wii.h"
#include "files.h"
#include "gfx.h"
#include "input.h"
#include "md5.h"
#include "mods.h"

/* wii/src/dvd.c, wii/src/launcher.c, wii/src/main.c, wii/src/slippi */
int melee_wii_disc_open(char* path, int path_size, char* id, int* revision);
int melee_wii_disc_read(unsigned int offset, void* buf, unsigned int len);
unsigned long long melee_wii_disc_size(void);
int launcher_slippi_folder(void);
void melee_wii_exit(void);
int so_link_detect(void);
const char* so_link_name(int link);
enum { SO_LINK_WIRED = 1, SO_LINK_WIFI = 2 }; /* (slippi/so_link.h's SoLink) */

/* The look */
#define BG_TOP 0x0D1424
#define BG_BOTTOM 0x1B2440
#define PANEL 0x222C47
#define PANEL_LIGHT 0x2C3858
#define GOLD 0xF2C230
#define GREEN 0x2ECC71
#define RED 0xE5534B
#define WHITE 0xF2F4F8
#define GREY 0x9AA5BE
#define DARK 0x141A2C

/* The NTSC 1.02 disc's MD5 (Nintendont's list, and Slippi's) */
#define GALE01_R2_MD5 "0e63d4223b01d9aba596259dc155a174"

static char s_disc_path[96], s_disc_id[8];
static int s_disc_revision, s_disc_state; /* melee_wii_disc_open's */
static int s_link = -1;
static char s_md5_result[64];

static int left(void)
{
    return (gfx_width() - 640) / 2;
}

/* ---- text that stays in its place ----------------------------------------
 * gfx_text draws a line as long as it is. What comes from the card (a
 * Slippi name, a file's path) or is too long for a line is cut or wrapped
 * here, so that nothing leaves its panel: the launcher's column is 640
 * pixels wide whatever the screen (centred on a 16:9 one). */

static const char* utf8_next(const char* p)
{
    p++;
    while (((unsigned char) *p & 0xC0) == 0x80) {
        p++;
    }
    return p;
}

/* `s` cut to max_w pixels with "...": at its end, or (keep_end: a path, whose
 * end says most) at its start */
static void fit_string(const Font* f, const char* s, int max_w, int keep_end, char* out, int size)
{
    int dots = gfx_text_width(f, "...");
    int len = (int) strlen(s);
    if (gfx_text_width(f, s) <= max_w || size < 8) {
        snprintf(out, size, "%s", s);
        return;
    }
    if (keep_end) {
        const char* start = s + len;
        while (start > s) {
            const char* prev = start - 1;
            while (prev > s && ((unsigned char) *prev & 0xC0) == 0x80) {
                prev--;
            }
            if (gfx_text_width(f, prev) + dots > max_w || (int) (s + len - prev) > size - 4) {
                break;
            }
            start = prev;
        }
        snprintf(out, size, "...%s", start);
    } else {
        int n = 0;
        while (s[n] != 0) {
            int next = (int) (utf8_next(s + n) - s);
            if (next > size - 4) {
                break;
            }
            memcpy(out, s, next);
            out[next] = 0;
            if (gfx_text_width(f, out) + dots > max_w) {
                break;
            }
            n = next;
        }
        memcpy(out, s, n);
        strcpy(out + n, "...");
    }
}

static void text_fit(const Font* f, int x, int y, unsigned int rgb, const char* s, int max_w, int keep_end)
{
    char buf[256];
    fit_string(f, s, max_w, keep_end, buf, sizeof(buf));
    gfx_text(f, x, y, rgb, buf);
}

/* Text wrapped at spaces (a word longer than a line is cut), each line at
 * most max_w pixels, `pitch` apart, at most max_lines: the last ends "..."
 * when there is more. Returns the lines drawn. */
static int text_wrapped(const Font* f, int x, int y, int pitch, unsigned int rgb, const char* text, int max_w,
                        int max_lines)
{
    char buf[256];
    const char* p = text;
    int lines = 0;
    while (*p != 0 && lines < max_lines) {
        int n = 0, last_space = -1, cut = 0;
        while (p[n] != 0 && p[n] != '\n') {
            int next = (int) (utf8_next(p + n) - p);
            if (next >= (int) sizeof(buf)) {
                cut = 1;
                break;
            }
            memcpy(buf, p, next);
            buf[next] = 0;
            if (gfx_text_width(f, buf) > max_w && n > 0) {
                if (last_space > 0) {
                    n = last_space;
                }
                cut = 1;
                break;
            }
            if (p[n] == ' ') {
                last_space = n;
            }
            n = next;
        }
        if (lines == max_lines - 1 && cut) {
            /* the last line there is room for: the rest of it, cut */
            const char* end = strchr(p, '\n');
            int rest = end != NULL ? (int) (end - p) : (int) strlen(p);
            if (rest >= (int) sizeof(buf)) {
                rest = sizeof(buf) - 1;
            }
            memcpy(buf, p, rest);
            buf[rest] = 0;
            text_fit(f, x, y, rgb, buf, max_w, 0);
            return lines + 1;
        }
        memcpy(buf, p, n);
        buf[n] = 0;
        gfx_text(f, x, y, rgb, buf);
        lines++;
        y += pitch;
        p += n;
        while (*p == ' ') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }
    return lines;
}

static void background(const char* title_right)
{
    int x = left();
    gfx_gradient_v(0, 0, gfx_width(), gfx_height(), BG_TOP, BG_BOTTOM);
    gfx_text(&font_title, x + 40, 28, WHITE, "Super Smash Bros. Melee");
    gfx_text(&font_small, x + 42, 70, GREY, "for the Wii  \xC2\xB7  Slippi Online  \xC2\xB7  " MELEE_WII_VERSION);
    if (title_right != NULL) {
        int w = gfx_text_width(&font_body_bold, title_right);
        gfx_text(&font_body_bold, x + 600 - w, 40, GOLD, title_right);
    }
    gfx_rect(x + 40, 96, 560, 2, GOLD, 200);
}

static void footer(const char* hints)
{
    int x = left();
    gfx_rect(0, 444, gfx_width(), 36, DARK, 180);
    gfx_text(&font_small, x + 40, 452, GREY, hints);
}

static void status_line(int y, const char* label, int ok, const char* text)
{
    int x = left();
    gfx_round_rect(x + 40, y + 6, 10, 10, 5, ok > 0 ? GREEN : ok == 0 ? RED : GOLD, 255);
    gfx_text(&font_small, x + 60, y, GREY, label);
    text_fit(&font_small, x + 190, y, WHITE, text, 410, 0);
}

/* The Wii Remote's pointer, over everything; then the frame */
static void present(void)
{
    int px, py;
    if (input_pointer(&px, &py)) {
        int x = px * gfx_width() / 640, y = py;
        gfx_round_rect(x - 9, y - 9, 18, 18, 9, DARK, 200);
        gfx_round_rect(x - 7, y - 7, 14, 14, 7, WHITE, 255);
        gfx_round_rect(x - 4, y - 4, 8, 8, 4, GOLD, 255);
    }
    gfx_present();
}

/* The row of a list at (x0 + 40.., top + i * pitch, height h) the pointer is
 * on, or -1 */
static int pointed_row(int top, int pitch, int h, int count, int x_from, int x_to)
{
    int px, py;
    if (!input_pointer(&px, &py)) {
        return -1;
    }
    px = px * gfx_width() / 640;
    if (px < left() + x_from || px >= left() + x_to || py < top) {
        return -1;
    }
    if ((py - top) % pitch >= h || (py - top) / pitch >= count) {
        return -1;
    }
    return (py - top) / pitch;
}

/* ---- the main screen ----------------------------------------------------- */

enum { ITEM_MELEE, ITEM_ONLINE, ITEM_MOD, ITEM_SETTINGS, ITEM_INFO, ITEM_EXIT, ITEM_COUNT };

/* The modded images found (20XX), the one picked, and the main screen's
 * items (the mod's only when there is one) */
#define MAX_MODS 4
static ModDisc s_mods[MAX_MODS];
static int s_mod_count, s_mod_pick;
static int s_items[ITEM_COUNT], s_item_count;

static int item_pitch(void)
{
    return s_item_count > 5 ? 44 : 52;
}

static void draw_main(int focus_index, const SlippiStatus* st)
{
    static const char* const titles[ITEM_COUNT] = {
        "Melee", "Slippi Online", "20XX", "Settings", "Game info", "Exit to the Homebrew Channel",
    };
    char sub[ITEM_COUNT][120];
    char line[128];
    int x = left();
    int pitch = item_pitch(), h = pitch - 6;
    snprintf(sub[ITEM_MELEE], sizeof(sub[0]), "Play Melee, with the codes chosen in Settings");
    if (s_mod_count > 0) {
        const ModDisc* m = &s_mods[s_mod_pick];
        snprintf(sub[ITEM_MOD], sizeof(sub[0]), "%s%s  (%s)", s_mod_count > 1 ? "\xE2\x86\x90\xE2\x86\x92 " : "",
                 m->title, m->path);
    }
    /* (Wi-Fi plays again, 2026-11-10: no "Needs a wired (LAN) connection" here) */
    if (st->have_account) {
        snprintf(sub[ITEM_ONLINE], sizeof(sub[0]), "Unranked, Direct and Teams as %s (%s)", st->name,
                 st->code);
    } else {
        snprintf(sub[ITEM_ONLINE], sizeof(sub[0]), "Needs your Slippi account: see Game info");
    }
    snprintf(sub[ITEM_SETTINGS], sizeof(sub[0]), "Online rumble and delay, Slippi's codes, cheats, replays...");
    snprintf(sub[ITEM_INFO], sizeof(sub[0]), "The disc image, its check, and Slippi's files");
    snprintf(sub[ITEM_EXIT], sizeof(sub[0]), " ");

    background(NULL);
    for (int n = 0; n < s_item_count; n++) {
        int i = s_items[n];
        int y = 118 + n * pitch;
        const char* title = i == ITEM_MOD && !s_mods[s_mod_pick].is_20xx ? "Modded Melee" : titles[i];
        if (n == focus_index) {
            gfx_round_rect(x + 40, y, 560, h, 12, GOLD, 255);
            text_fit(&font_body_bold, x + 62, y + 3, DARK, title, 526, 0);
            text_fit(&font_small, x + 62, y + h - 20, 0x3A3018, sub[i], 526, 0);
        } else {
            gfx_round_rect(x + 40, y, 560, h, 12, PANEL, 235);
            gfx_text(&font_body_bold, x + 62, y + 3, i == ITEM_ONLINE ? GREEN : i == ITEM_MOD ? GOLD : WHITE,
                     title);
            text_fit(&font_small, x + 62, y + h - 20, GREY, sub[i], 526, 0);
        }
    }
    {
        char path[128];
        snprintf(line, sizeof(line), "%s  revision %d  \xC2\xB7  ", s_disc_id, s_disc_revision);
        fit_string(&font_small, s_disc_path, 410 - gfx_text_width(&font_small, line), 1, path, sizeof(path));
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s", path);
    }
    status_line(384, "Disc", s_disc_state == 0 && s_disc_revision == 2 ? 1 : s_disc_state == 0 ? -1 : 0,
                s_disc_state == 0 ? line : s_disc_state == -1 ? "none found" : "not Melee (GALE01)");
    if (st->have_account) {
        snprintf(line, sizeof(line), "%s (%s)", st->name, st->code);
    }
    status_line(404, "Slippi account", st->have_account,
                st->have_account ? line : "none: copy user.json to /apps/melee/slippi");
    status_line(424, "Connection", s_link > 0 ? 1 : -1, s_link >= 0 ? so_link_name(s_link) : "...");
    footer("A  choose      + / X  settings      - / Z  game info      Y  help");
}

/* ---- messages and progress ----------------------------------------------- */

static void message(const char* title, const char* text, unsigned int color)
{
    int x = left();
    for (;;) {
        unsigned int in = input_poll();
        background(NULL);
        gfx_round_rect(x + 60, 140, 520, 220, 16, PANEL, 245);
        text_fit(&font_body_bold, x + 84, 160, color, title, 472, 0);
        text_wrapped(&font_small, x + 84, 200, 24, WHITE, text, 470, 6);
        footer("A / B  back");
        present();
        if (in & (IN_A | IN_B | IN_START)) {
            return;
        }
    }
}

typedef struct Progress {
    const char* title;
} Progress;

static void draw_progress(const char* title, const char* what, int done, int total)
{
    int x = left();
    background(NULL);
    gfx_round_rect(x + 60, 170, 520, 150, 16, PANEL, 245);
    text_fit(&font_body_bold, x + 84, 190, WHITE, title, 472, 0);
    text_fit(&font_small, x + 84, 228, GREY, what, 472, 0);
    gfx_round_rect(x + 84, 266, 472, 20, 10, DARK, 255);
    if (total > 0 && done > 0) {
        gfx_round_rect(x + 84, 266, 472 * done / total < 20 ? 20 : 472 * done / total, 20, 10, GREEN, 255);
    }
    footer("Please wait");
    present();
}

static void prepare_progress(const char* name, int done, int total, void* ctx)
{
    char what[96];
    (void) ctx;
    snprintf(what, sizeof(what), "%s  (%d of %d)", name, done < total ? done + 1 : done, total);
    draw_progress("Making Slippi's menus from your disc", what, done, total);
}

/* (launcher_mod_start) 20XX's stage files, checked the first time */
static void mod_progress(const char* name, int done, int total)
{
    char what[96];
    snprintf(what, sizeof(what), "%s  (%d of %d)", name, done + 1, total);
    draw_progress("Checking your 20XX image's stages", what, done, total);
}

/* What Slippi Online needs before it starts; 0 when it can */
static int online_ready(void)
{
    SlippiStatus st;
    char error[256];
    if (s_disc_state != 0) {
        message("No Melee disc image",
                "Put your own dump of Super Smash Bros. Melee (NTSC 1.02, GALE01) on the SD card "
                "or a USB drive as /apps/melee/game.iso or /games/melee.iso.",
                RED);
        return -1;
    }
    if (launcher_slippi_folder() < 0) {
        message("No Slippi account",
                "Slippi Online plays as your Slippi account. Install Slippi Launcher on a computer, "
                "log in once, then copy its user.json (on Windows: %APPDATA%\\Slippi Launcher\\netplay\\"
                "User\\Slippi\\user.json) to /apps/melee/slippi on the SD card. Treat it like a "
                "password: anyone with it can play as you.",
                RED);
        return -1;
    }
#if 0 /* Wi-Fi plays again (2026-11-10) */
    if (s_link != SO_LINK_WIRED) {
        message("Slippi Online needs a wired connection",
                "On Wi-Fi the Wii's connection stops for a moment every 10 seconds, which makes online "
                "games stutter, so Slippi Online plays only over a LAN cable. Plug in a USB LAN adapter "
                "(one with the ASIX AX88772 chip, such as Nintendo's) and choose a wired connection in "
                "Wii Settings, Internet.",
                RED);
        return -1;
    }
#endif
    slippi_status(&st);
    if (!st.have_codeset || st.missing_shipped) {
        message("Slippi's files are missing",
                "Slippi's online code and files go in /apps/melee/slippi on the SD card (netplay.gct, "
                "files/ and patches/): copy them from this release's zip again.",
                RED);
        return -1;
    }
    if (st.need_prepare) {
        if (slippi_prepare(prepare_progress, NULL, error, sizeof(error)) < 0) {
            message("Could not make Slippi's menus", error, RED);
            return -1;
        }
    }
    return 0;
}

/* ---- settings ------------------------------------------------------------ */

enum { NEED_NONE, NEED_20XX }; /* a settings row's prerequisite */

typedef struct Row {
    const char* section;
    const char* label;
    const char* const* values;
    int count;
    int* value;
    const char* help;
    int need; /* NEED_*: the row shows only when its prerequisite is met */
} Row;

/* Whether a settings row's prerequisite is met, re-checked every frame */
static int row_shown(const Row* r, int have_20xx)
{
    switch (r->need) {
    case NEED_20XX:
        return have_20xx;
    default:
        return 1;
    }
}

static const char* const ON_OFF[] = { "Off", "On" };
/* config.ini's input_delay= (slippi/so_config.c), and how it shows */
static const char* const DELAY_KEYS[] = { "auto", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
static const char* const DELAY[] = { "Auto",     "1 frame",  "2 frames", "3 frames", "4 frames",
                                     "5 frames", "6 frames", "7 frames", "8 frames", "9 frames" };
static const char* const FIX[] = { "Off", "UCF 0.84", "UCF 0.84 (stealth)" };
static const char* const MODS[] = { "Off", "Stealth", "Tournament", "Friendlies" };
static const char* const STAGES[] = { "Off", "Pok\xC3\xA9mon Stadium", "All" };
static const char* const GAMEPLAY[] = { "Off", "Ledge grab limit", "Wobbling limit", "Both" };
static const char* const SCREEN[] = { "4:3", "Widescreen 16:9", "16:9, 4:3 inside", "Auto" };

static void settings(LauncherSettings* s)
{
    int rumble = slippi_get_rumble(), delay = slippi_get_choice("input_delay", DELAY_KEYS, 10);
    const int saved_delay = delay;
    int focus = 0, top = 0, focus_row = 0;
    Row rows[] = {
        { "Slippi Online", "Rumble", ON_OFF, 2, &rumble,
          "Your controller's rumble in online games (Melee's own setting can't reach it online)." },
        { NULL, "Delay frames", DELAY, 10, &delay,
          "Online input delay. Auto: by the opponent's ping, 3 to 5 frames on Wi-Fi, 2 to 4 wired. "
          "A number: always that." },
        /* (no row for config.ini's delay_adjust=1, the delay following the ping during a
         * game too: slippi/so_exi.c has it, off and untested; a line in config.ini turns it on) */
        { "Melee (offline)", "Controller fix", FIX, 3, &s->controller_fix,
          "UCF 0.84: dashback, shield drop, 1.0 cardinals and more, as on Slippi. Online play has it "
          "always." },
        { NULL, "PAL balance", ON_OFF, 2, &s->pal, "The PAL version's character balance changes." },
        { NULL, "Convenience", MODS, 4, &s->mods,
          "Stealth: neutral spawns, tags... Tournament: also D-pad rumble, stage striking. "
          "Friendlies: also skipped results, salty runback (A+B)." },
        { NULL, "Frozen stages", STAGES, 3, &s->stages, "Stages without their transformations or hazards." },
        { NULL, "Gameplay", GAMEPLAY, 4, &s->gameplay, "Tournament rules: limits on ledge grabs and wobbling." },
        { NULL, "Screen", SCREEN, 4, &s->screen,
          "Slippi's widescreen code (online too). Auto: on when Wii Settings say 16:9." },
        { NULL, "Safety", ON_OFF, 2, &s->safety, "Slippi's safety codes for offline play." },
        { NULL, "Cheats", ON_OFF, 2, &s->cheats,
          "Your Gecko codes: /apps/melee/codes/GALE01.gct (or /codes/GALE01.gct). Offline only." },
        { "Slippi", "Replays", ON_OFF, 2, &s->replays,
          "Each game saved as a .slp in /apps/melee/slippi/replays, as Slippi records it." },
        { NULL, "Console mirroring", ON_OFF, 2, &s->console,
          "Slippi Launcher can connect to this Wii (Console Mirroring) to watch, record or relay "
          "games." },
        { "Launcher", "Skip this screen", ON_OFF, 2, &s->skip,
          "Start the last choice at once. Hold B as the game starts for this screen (on a Wii "
          "Remote, 1 or B, when no GameCube controller is plugged in)." },
        { NULL, "Diagnostics", ON_OFF, 2, &s->diagnostics,
          "For finding what makes online games run badly: each one's timing, saved in "
          "/apps/melee/logs/diag (the newest 40 kept)." },
        /* only with a 20XX image */
        { "20XX", "20XX menu", ON_OFF, 2, &s->xx_menu,
          "20XX's music and looks in Slippi Online, under Extra.", NEED_20XX },
    };
    const int nrows = (int) (sizeof(rows) / sizeof(rows[0]));
    int vis[sizeof(rows) / sizeof(rows[0])];
    int have_20xx = 0;
    for (int i = 0; i < s_mod_count; i++) {
        have_20xx |= s_mods[i].is_20xx;
    }
    const int visible = 8;
    for (;;) {
        unsigned int in = input_poll();
        int x = left();
        /* rebuild the visible rows from their prerequisites each frame, keeping
         * the cursor on the same row -- rows appear and disappear live */
        int count = 0;
        focus = 0;
        for (int i = 0; i < nrows; i++) {
            if (row_shown(&rows[i], have_20xx)) {
                if (i == focus_row) {
                    focus = count;
                }
                vis[count++] = i;
            }
        }
        int pointed = pointed_row(112, 36, 32, count - top < visible ? count - top : visible, 196, 600);
        if (in & (IN_B | IN_START)) {
            break;
        }
        if (pointed >= 0) {
            focus = top + pointed;
        }
        if (in & IN_UP) {
            focus = (focus + count - 1) % count;
        }
        if (in & IN_DOWN) {
            focus = (focus + 1) % count;
        }
        focus_row = vis[focus];
        if (in & (IN_LEFT | IN_RIGHT | IN_A)) {
            Row* r = &rows[vis[focus]];
            int step = (in & IN_LEFT) ? r->count - 1 : 1;
            *r->value = (*r->value + step) % r->count;
        }
        if (focus < top) {
            top = focus;
        }
        if (focus >= top + visible) {
            top = focus - visible + 1;
        }
        background("Settings");
        for (int i = top; i < count && i < top + visible; i++) {
            int y = 112 + (i - top) * 36;
            const Row* r = &rows[vis[i]];
            if (r->section != NULL) {
                gfx_text(&font_small, x + 44, y + 7, GOLD, r->section);
            }
            if (i == focus) {
                gfx_round_rect(x + 196, y, 404, 32, 10, GOLD, 255);
            } else {
                gfx_round_rect(x + 196, y, 404, 32, 10, PANEL, 235);
            }
            gfx_text(&font_body, x + 210, y + 3, i == focus ? DARK : WHITE, r->label);
            {
                char value[48];
                snprintf(value, sizeof(value), "\xE2\x86\x90 %s \xE2\x86\x92", r->values[*r->value]);
                int w = gfx_text_width(&font_body_bold, value);
                gfx_text(&font_body_bold, x + 588 - w, y + 3, i == focus ? DARK : GREEN, value);
            }
        }
        gfx_round_rect(x + 40, 400, 560, 42, 10, PANEL_LIGHT, 230);
        if (gfx_text_width(&font_small, rows[vis[focus]].help) <= 536) {
            gfx_text(&font_small, x + 52, 412, WHITE, rows[vis[focus]].help);
        } else {
            text_wrapped(&font_small, x + 52, 403, 18, WHITE, rows[vis[focus]].help, 536, 2);
        }
        footer("\xE2\x86\x91\xE2\x86\x93  setting      \xE2\x86\x90\xE2\x86\x92 / A  change      B  done");
        present();
    }
    slippi_set_rumble(rumble);
    if (delay != saved_delay) {
        slippi_set_choice("input_delay", DELAY_KEYS, delay);
    }
    if (slippi_get_connection() != 0) {
        slippi_set_connection(0); /* (the old Wi-Fi/Wired choice: the connection is Wii Settings' now) */
    }
}

/* ---- game info ----------------------------------------------------------- */

static void verify_disc(void)
{
    static unsigned char buf[256 * 1024] __attribute__((aligned(32)));
    unsigned long long size = melee_wii_disc_size(), done = 0;
    md5_state_t md5;
    md5_byte_t digest[16];
    char hex[33], what[96];
    md5_init(&md5);
    while (done < size) {
        unsigned int n = size - done < sizeof(buf) ? (unsigned int) (size - done) : sizeof(buf);
        if (melee_wii_disc_read((unsigned int) done, buf, n) < 0) {
            snprintf(s_md5_result, sizeof(s_md5_result), "could not read the image");
            return;
        }
        md5_append(&md5, buf, (int) n);
        done += n;
        if ((done / n) % 16 == 0 || done == size) {
            snprintf(what, sizeof(what), "%llu of %llu MB", done >> 20, size >> 20);
            draw_progress("Checking the disc image (MD5)", what, (int) (done >> 20), (int) (size >> 20));
            if (input_poll() & IN_B) {
                s_md5_result[0] = 0;
                return;
            }
        }
    }
    md5_finish(&md5, digest);
    for (int i = 0; i < 16; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    snprintf(s_md5_result, sizeof(s_md5_result), "%s %s", hex,
             strcmp(hex, GALE01_R2_MD5) == 0 ? "(Melee 1.02: good)" : "(not a 1:1 Melee 1.02)");
}

static void game_info(void)
{
    for (;;) {
        unsigned int in = input_poll();
        int x = left();
        char line[160];
        SlippiStatus st;
        if (in & (IN_B | IN_START)) {
            return;
        }
        if ((in & IN_A) && s_disc_state == 0) {
            verify_disc();
            continue;
        }
        slippi_status(&st);
        background("Game info");
        gfx_round_rect(x + 40, 112, 560, 324, 14, PANEL, 235);
        snprintf(line, sizeof(line), "%s", s_disc_state == -1 ? "none found" : s_disc_path);
        gfx_text(&font_small, x + 60, 126, GREY, "Disc image");
        text_fit(&font_small, x + 200, 126, WHITE, line, 390, 1);
        snprintf(line, sizeof(line), "%s, revision %d (%s)", s_disc_id, s_disc_revision,
                 s_disc_revision == 2 ? "1.02, as Slippi needs" : "Slippi needs 1.02");
        gfx_text(&font_small, x + 60, 150, GREY, "Game");
        gfx_text(&font_small, x + 200, 150, s_disc_state == 0 ? WHITE : RED,
                 s_disc_state == 0 ? line : "not Melee");
        snprintf(line, sizeof(line), "%llu MB", melee_wii_disc_size() >> 20);
        gfx_text(&font_small, x + 60, 174, GREY, "Size");
        gfx_text(&font_small, x + 200, 174, WHITE, line);
        gfx_text(&font_small, x + 60, 198, GREY, "MD5");
        {
            /* the digest, and under it what it says of the image */
            char md5[sizeof(s_md5_result)];
            char* verdict;
            snprintf(md5, sizeof(md5), "%s", s_md5_result[0] ? s_md5_result : "A to check (about a minute)");
            verdict = strlen(md5) > 33 && md5[32] == ' ' ? md5 + 32 : NULL; /* (after the 32 digits) */
            if (verdict != NULL) {
                *verdict++ = 0;
                text_fit(&font_small, x + 200, 216, GREY, verdict, 390, 0);
            }
            text_fit(&font_small, x + 200, 198, s_md5_result[0] ? WHITE : GREY, md5, 390, 0);
        }

        gfx_text(&font_small, x + 60, 238, GREY, "Slippi account");
        if (st.have_account) {
            snprintf(line, sizeof(line), "%s (%s)", st.name, st.code);
        }
        text_fit(&font_small, x + 200, 238, st.have_account ? WHITE : RED,
                 st.have_account ? line : "none: user.json in /apps/melee/slippi", 390, 0);
        gfx_text(&font_small, x + 60, 262, GREY, "Online code");
        gfx_text(&font_small, x + 200, 262, st.have_codeset ? WHITE : RED,
                 st.have_codeset ? "netplay.gct" : "missing");
        gfx_text(&font_small, x + 60, 286, GREY, "Slippi's menus");
        gfx_text(&font_small, x + 200, 286, st.files_ready ? WHITE : GOLD,
                 st.files_ready ? "ready" : st.need_prepare ? "made at the first online start"
                                                          : "missing");
        gfx_text(&font_small, x + 60, 310, GREY, "Connection");
        gfx_text(&font_small, x + 200, 310, WHITE, s_link >= 0 ? so_link_name(s_link) : "...");
        gfx_text(&font_small, x + 60, 350, GREY,
                 "Slippi Online: Slippi's online modes (not Ranked), against Slippi on PC.");
        gfx_text(&font_small, x + 60, 372, GREY,
                 "Unofficial: not made or supported by Project Slippi or Nintendo.");
        footer("A  check the disc image      B  back");
        present();
    }
}

/* ---- the launcher -------------------------------------------------------- */

/* With "skip this screen": B held on a GameCube controller (looked at for a
 * moment, no screen yet) shows it all the same. With no GameCube controller
 * plugged in (a Wii without the ports), a Wii Remote's or its Classic
 * Controller's B does (input_remote_b_held). */
static int b_held(void)
{
    u32 connected = 0;
    PAD_Init();
    for (int i = 0; i < 20; i++) {
        connected |= PAD_ScanPads();
        for (int c = 0; c < 4; c++) {
            if (PAD_ButtonsHeld(c) & PAD_BUTTON_B) {
                return 1;
            }
        }
        usleep(16000);
    }
    return connected == 0 && input_remote_b_held();
}

/* One prerequisite on the Help page: its name, whether it is met, and how to
 * meet it. Advances *y to the next row. */
static void help_item(int x, int* y, const char* label, int met, const char* how)
{
    gfx_text(&font_body, x + 60, *y, GOLD, label);
    gfx_text(&font_small, x + 470, *y + 4, met ? GREEN : GOLD, met ? "have it" : "need it");
    text_wrapped(&font_small, x + 60, *y + 24, 18, GREY, how, 500, 2);
    *y += 76;
}

/* The prerequisites and how to meet them -- Y on the main menu, at any time,
 * whether or not they are met, so a hidden item can still be understood. */
static void help_page(void)
{
    for (;;) {
        unsigned int in = input_poll();
        int x = left();
        SlippiStatus st;
        int have_20xx = 0, y = 124;
        if (in & (IN_A | IN_B | IN_Y | IN_START)) {
            return;
        }
        slippi_status(&st);
        for (int i = 0; i < s_mod_count; i++) {
            have_20xx |= s_mods[i].is_20xx;
        }
        background("Help  \xC2\xB7  what each thing needs");
        gfx_round_rect(x + 40, 112, 560, 324, 14, PANEL, 235);
        help_item(x, &y, "Melee disc image", s_disc_state == 0,
                  "Needed for everything. Your own NTSC 1.02 dump (GALE01) at "
                  "/apps/melee/game.iso or /games/melee.iso.");
        help_item(x, &y, "Slippi account", st.have_account,
                  "For Slippi Online. Slippi Launcher on a PC, log in once, then copy its user.json "
                  "to /apps/melee/slippi. Treat it like a password.");
        help_item(x, &y, "Slippi's files", st.have_codeset && !st.missing_shipped,
                  "For Slippi Online. From this release's zip into /apps/melee/slippi "
                  "(netplay.gct, files/, patches/).");
        help_item(x, &y, "20XX image", have_20xx,
                  "For the 20XX option and its in-game menu. A 20XX HACK PACK image on the SD card "
                  "or a USB drive.");
        footer("B / Y  back");
        present();
    }
}

/* Whether a main-menu item's prerequisites are met, so it is shown and can be
 * chosen. Re-checked every frame, so items appear and disappear live. */
static int item_shown(int i, const SlippiStatus* st)
{
    switch (i) {
    case ITEM_ONLINE:
        return s_disc_state == 0 && st->have_account && st->have_codeset && !st->missing_shipped;
    case ITEM_MOD:
        return s_mod_count > 0; /* a mod (20XX) image */
    default:
        /* Melee stays visible with no disc so its "add a disc" message can
         * guide first-time setup; Settings, Game info, Exit: always */
        return 1;
    }
}

int launcher_ui(LauncherSettings* s)
{
    SlippiStatus st;
    s_disc_state = melee_wii_disc_open(s_disc_path, sizeof(s_disc_path), s_disc_id, &s_disc_revision);
    s_mod_count = mods_scan(s_mods, MAX_MODS);
    s_mod_pick = 0;
    for (int i = 0; i < s_mod_count; i++) {
        if (strcmp(s_mods[i].path, s->mod_path) == 0) {
            s_mod_pick = i;
        }
    }
    /* the item the cursor starts on (the last choice); the visible item list is
     * rebuilt from the prerequisites every frame in the loop below */
    int focus_item = s->mode == LAUNCH_ONLINE ? ITEM_ONLINE : s->mode == LAUNCH_MOD ? ITEM_MOD : ITEM_MELEE;
    s_item_count = 0;
    int skip = s->skip && s_disc_state == 0 && !b_held();
    if (skip && s->mode == LAUNCH_MOD) {
        if (s_mod_count > 0 && strcmp(s_mods[s_mod_pick].path, s->mod_path) == 0) {
            return -1; /* the same mod, as last time */
        }
        skip = 0;
    }
    if (skip && s->mode != LAUNCH_ONLINE) {
        return -1; /* nothing changed */
    }
    if (skip) {
        /* straight to Slippi Online when it has all it needs; when its menus
         * are still to be made (or something is missing), on screen */
        slippi_status(&st);
        if (launcher_slippi_folder() == 0 && st.have_codeset && !st.missing_shipped &&
            !st.need_prepare /* (wired-only, until 2026-11-10: && so_link_detect() == SO_LINK_WIRED) */) {
            return -1;
        }
    }
    s_link = so_link_detect();
    gfx_init();
    input_init();
    if (skip && online_ready() == 0) {
        draw_progress("Starting Slippi Online", " ", 0, 0);
        gfx_shutdown();
        input_hand_over();
        return -1;
    }
    slippi_status(&st);
    for (;;) {
        unsigned int in = input_poll();
        int pointed, item, focus = 0;
        /* rebuild the visible items from their prerequisites each frame, keeping
         * the cursor on the same item -- items appear and disappear live */
        s_item_count = 0;
        for (int i = 0; i < ITEM_COUNT; i++) {
            if (item_shown(i, &st)) {
                if (i == focus_item) {
                    focus = s_item_count;
                }
                s_items[s_item_count++] = i;
            }
        }
        pointed = pointed_row(118, item_pitch(), item_pitch() - 6, s_item_count, 40, 600);
        if (pointed >= 0) {
            focus = pointed;
        }
        if (in & IN_UP) {
            focus = (focus + s_item_count - 1) % s_item_count;
        }
        if (in & IN_DOWN) {
            focus = (focus + 1) % s_item_count;
        }
        item = s_items[focus];
        focus_item = item;
        if (item == ITEM_MOD && s_mod_count > 1 && (in & (IN_LEFT | IN_RIGHT))) {
            s_mod_pick = (s_mod_pick + ((in & IN_LEFT) ? s_mod_count - 1 : 1)) % s_mod_count;
        }
        if (in & IN_X) {
            settings(s);
            continue;
        }
        if (in & IN_Z) {
            game_info();
            slippi_status(&st);
            continue;
        }
        if (in & IN_Y) {
            help_page();
            continue;
        }
        if (in & (IN_A | IN_START)) {
            if (item == ITEM_MELEE) {
                if (s_disc_state != 0) {
                    message("No Melee disc image",
                            "Put your own dump of Super Smash Bros. Melee (NTSC 1.02, GALE01) on the SD "
                            "card or a USB drive as /apps/melee/game.iso or /games/melee.iso.",
                            RED);
                    continue;
                }
                s->mode = LAUNCH_MELEE;
                break;
            }
            if (item == ITEM_ONLINE) {
                if (online_ready() == 0) {
                    s->mode = LAUNCH_ONLINE;
                    break;
                }
                slippi_status(&st);
                continue;
            }
            if (item == ITEM_MOD) {
                s->mode = LAUNCH_MOD;
                snprintf(s->mod_path, sizeof(s->mod_path), "%s", s_mods[s_mod_pick].path);
                break;
            }
            if (item == ITEM_SETTINGS) {
                settings(s);
                continue;
            }
            if (item == ITEM_INFO) {
                game_info();
                slippi_status(&st);
                continue;
            }
            if (item == ITEM_EXIT) {
                gfx_shutdown();
                input_shutdown();
                melee_wii_exit();
            }
        }
        if (in & IN_HOME) {
            gfx_shutdown();
            input_shutdown();
            melee_wii_exit();
        }
        draw_main(focus, &st);
        present();
    }
    draw_progress(s->mode == LAUNCH_ONLINE ? "Starting Slippi Online"
                  : s->mode == LAUNCH_MOD ? "Starting 20XX" : "Starting Melee", " ", 0, 0);
    launcher_mod_start(s, mod_progress);
    gfx_shutdown();
    input_hand_over();
    return 0;
}
