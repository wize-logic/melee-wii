/* The launcher's settings (wii/src/launcher.c): sd:/apps/melee/settings.ini,
 * "key=value" lines. */

#include "launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wl/base/base.h>

static const struct {
    const char* key;
    size_t offset;
} KEYS[] = {
#define K(name) { #name, offsetof(LauncherSettings, name) }
    K(mode),     K(skip),     K(controller_fix), K(pal),    K(mods),
    K(stages),   K(gameplay), K(screen),         K(safety), K(cheats),
    K(replays),  K(console),  K(xx_menu),       K(diagnostics),
#undef K
};

void launcher_settings_default(LauncherSettings* s)
{
    memset(s, 0, sizeof(*s));
    s->mode = LAUNCH_MELEE;
    s->replays = 1;
    s->console = 1;
    s->screen = LAUNCH_SCREEN_AUTO;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int launcher_settings_load(LauncherSettings* s, const char* path)
{
    char line[128];
    FILE* f = fopen(path, "r");
    launcher_settings_default(s);
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char* eq = strchr(line, '=');
        if (line[0] == '#' || line[0] == ';' || eq == NULL) {
            continue;
        }
        *eq = '\0';
        if (strcmp(line, "mod_path") == 0) {
            char* end = eq + 1 + strcspn(eq + 1, "\r\n");
            *end = '\0';
            snprintf(s->mod_path, sizeof(s->mod_path), "%s", eq + 1);
            continue;
        }
        for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
            if (strcmp(line, KEYS[i].key) == 0) {
                *(int*) ((char*) s + KEYS[i].offset) = atoi(eq + 1);
            }
        }
    }
    fclose(f);
    s->mode = clamp(s->mode, LAUNCH_MELEE, LAUNCH_MOD);
    s->controller_fix = clamp(s->controller_fix, 0, 2);
    s->pal = s->pal != 0;
    s->mods = clamp(s->mods, 0, 3);
    s->stages = clamp(s->stages, 0, 2);
    s->gameplay = clamp(s->gameplay, 0, 3);
    s->screen = clamp(s->screen, 0, LAUNCH_SCREEN_AUTO);
    s->safety = s->safety != 0;
    s->skip = s->skip != 0;
    s->cheats = s->cheats != 0;
    s->replays = s->replays != 0;
    s->console = s->console != 0;
    s->xx_menu = s->xx_menu != 0;
    s->diagnostics = s->diagnostics != 0;
    return 0;
}

int launcher_settings_save(const LauncherSettings* s, const char* path)
{
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "# Super Smash Bros. Melee for the Wii: the launcher's settings\n");
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        fprintf(f, "%s=%d\n", KEYS[i].key, *(const int*) ((const char*) s + KEYS[i].offset));
    }
    if (s->mod_path[0] != '\0') {
        fprintf(f, "mod_path=%s\n", s->mod_path);
    }
    fclose(f);
    return 0;
}
