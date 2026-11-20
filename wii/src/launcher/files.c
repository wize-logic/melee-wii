/* Slippi's folder for the launcher (LAUNCHER_SLIPPI: sd:/apps/melee/slippi):
 *
 *   netplay.gct            the online codeset (Slippi Dolphin's, without its
 *                          music hooks)
 *   files/<name>           Slippi's menu files, served to the game over EXI
 *   patches/<name>.diff    Slippi's patches for the disc's menu files, and
 *   patches/manifest.txt   what each file must be: "copy" (shipped as is),
 *                          "patch" (made here from the disc file of its
 *                          name), or "link" (made from the disc files it
 *                          lists, one after the other: Slippi 3.7.0's files
 *                          that refer to the disc's art), size, CRC-32
 *   user.json              the account (Slippi Launcher's)
 *   rumble.txt, config.ini   the online client's settings (and delay.txt, which
 *                          the character select screen writes)
 *
 * Nothing from the disc is shipped: on the first start of Slippi Online the
 * launcher applies the patches to the disc's files, checks each result
 * against the manifest and keeps it. */

#include "files.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../launcher.h"
#include "vcdiff.h"

/* wii/src/dvd.c */
int melee_wii_disc_file(const char* name, unsigned int* offset, unsigned int* size);
int melee_wii_disc_read(unsigned int offset, void* buf, unsigned int len);

#define ROOT LAUNCHER_SLIPPI
#define MAX_ENTRIES 32

typedef struct Entry {
    int patch;      /* made from the disc */
    char path[48];  /* in ROOT */
    char sources[160]; /* "link": the disc files, '+' between; "": the target's own */
    unsigned long size;
    unsigned long crc;
} Entry;

static long size_of(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long) st.st_size : -1;
}

static unsigned char* load(const char* path, unsigned long* size)
{
    FILE* f = fopen(path, "rb");
    long n;
    unsigned char* buf;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = n > 0 ? memalign(32, (size_t) n + 1) : NULL;
    if (buf == NULL || fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = 0;
    fclose(f);
    *size = (unsigned long) n;
    return buf;
}

static int save(const char* path, const void* data, unsigned long size)
{
    FILE* f = fopen(path, "wb");
    int ok;
    if (f == NULL) {
        return -1;
    }
    ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    return ok ? 0 : -1;
}

static int read_manifest(Entry* e, int max)
{
    char path[96];
    unsigned long size;
    char* text;
    char* line;
    int n = 0;
    snprintf(path, sizeof(path), "%s/patches/manifest.txt", ROOT);
    text = (char*) load(path, &size);
    if (text == NULL) {
        return -1;
    }
    for (line = strtok(text, "\r\n"); line != NULL && n < max; line = strtok(NULL, "\r\n")) {
        char kind[8];
        if (line[0] == '#') {
            continue;
        }
        e[n].sources[0] = '\0';
        if (sscanf(line, "%7s %47s %lu %lx %159s", kind, e[n].path, &e[n].size, &e[n].crc, e[n].sources) >= 4) {
            int link = strcmp(kind, "link") == 0;
            e[n].patch = link || strcmp(kind, "patch") == 0;
            if (!link) {
                e[n].sources[0] = '\0';
            }
            n++;
        }
    }
    free(text);
    return n;
}

static const char* base_name(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

void slippi_status(SlippiStatus* st)
{
    char path[96];
    Entry e[MAX_ENTRIES];
    int n;
    memset(st, 0, sizeof(*st));
    snprintf(path, sizeof(path), "%s/netplay.gct", ROOT);
    st->have_codeset = size_of(path) > 0;
    n = read_manifest(e, MAX_ENTRIES);
    if (n < 0) {
        /* no manifest: the files were made elsewhere (a PC), as they are */
        snprintf(path, sizeof(path), "%s/files", ROOT);
        st->files_ready = size_of(path) >= 0;
    } else {
        st->files_ready = 1;
        for (int i = 0; i < n; i++) {
            snprintf(path, sizeof(path), "%s/%s", ROOT, e[i].path);
            if (size_of(path) != (long) e[i].size) {
                st->files_ready = 0;
                if (e[i].patch) {
                    st->need_prepare = 1;
                } else {
                    st->missing_shipped = 1;
                }
            }
        }
    }
    slippi_account(st->name, sizeof(st->name), st->code, sizeof(st->code));
    st->have_account = st->code[0] != '\0';
}

/* "key": "value" in user.json (a flat object of strings) */
static int json_string(const char* json, const char* key, char* out, int size)
{
    char pattern[40];
    const char* p;
    int n = 0;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (p == NULL) {
        return -1;
    }
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) {
        return -1;
    }
    p = strchr(p, '"');
    if (p == NULL) {
        return -1;
    }
    for (p++; *p && *p != '"' && n < size - 1; p++) {
        out[n++] = *p;
    }
    out[n] = '\0';
    return 0;
}

int slippi_account(char* name, int name_size, char* code, int code_size)
{
    char path[96];
    unsigned long size;
    char* json;
    name[0] = code[0] = '\0';
    snprintf(path, sizeof(path), "%s/user.json", ROOT);
    json = (char*) load(path, &size);
    if (json == NULL) {
        return -1;
    }
    json_string(json, "displayName", name, name_size);
    json_string(json, "connectCode", code, code_size);
    free(json);
    return code[0] != '\0' ? 0 : -1;
}

/* The disc files `list` names ('+' between), one after the other: their
 * total size, and with `out` their bytes */
static int disc_sources(const char* list, unsigned int* total, unsigned char* out, char* error, int error_size)
{
    char names[160];
    unsigned int at = 0;
    snprintf(names, sizeof(names), "%s", list);
    for (char* name = strtok(names, "+"); name != NULL; name = strtok(NULL, "+")) {
        unsigned int offset, size;
        if (melee_wii_disc_file(name, &offset, &size) < 0) {
            snprintf(error, error_size, "The disc has no %s: is it Melee 1.02 (GALE01, revision 2)?", name);
            return -1;
        }
        if (out != NULL && melee_wii_disc_read(offset, out + at, size) < 0) {
            snprintf(error, error_size, "Could not read %s from the disc", name);
            return -1;
        }
        at += size;
    }
    *total = at;
    return 0;
}

int slippi_prepare(void (*progress)(const char* name, int done, int total, void* ctx), void* ctx,
                   char* error, int error_size)
{
    Entry e[MAX_ENTRIES];
    char path[96];
    int n = read_manifest(e, MAX_ENTRIES), total = 0, done = 0;
    if (n < 0) {
        snprintf(error, error_size, "Slippi's patches are missing (%s/patches)", ROOT);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/files", ROOT);
    mkdir(path, 0777);
    for (int i = 0; i < n; i++) {
        total += e[i].patch;
    }
    for (int i = 0; i < n; i++) {
        const char* name = base_name(e[i].path);
        unsigned int disc_size;
        unsigned long diff_size;
        unsigned char *source, *diff, *out;
        const char* why = NULL;
        long target;
        if (!e[i].patch) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", ROOT, e[i].path);
        if (size_of(path) == (long) e[i].size) {
            progress(name, ++done, total, ctx);
            continue;
        }
        progress(name, done, total, ctx);
        if (disc_sources(e[i].sources[0] ? e[i].sources : name, &disc_size, NULL, error, error_size) < 0) {
            return -1;
        }
        snprintf(path, sizeof(path), "%s/patches/%s.diff", ROOT, name);
        diff = load(path, &diff_size);
        if (diff == NULL) {
            snprintf(error, error_size, "Slippi's patch for %s is missing", name);
            return -1;
        }
        source = memalign(32, disc_size ? disc_size : 32);
        target = vcdiff_target_size(diff, diff_size, &why);
        out = target > 0 ? memalign(32, (size_t) target) : NULL;
        if (source == NULL || out == NULL) {
            free(source);
            free(out);
            free(diff);
            snprintf(error, error_size, "No memory to patch %s", name);
            return -1;
        }
        if (disc_sources(e[i].sources[0] ? e[i].sources : name, &disc_size, source, error, error_size) < 0 ||
            vcdiff_decode(source, disc_size, diff, diff_size, out, (size_t) target, &why) < 0 ||
            (unsigned long) target != e[i].size ||
            vcdiff_crc32(0, out, (size_t) target) != e[i].crc)
        {
            free(source);
            free(out);
            free(diff);
            snprintf(error, error_size,
                     "%s did not come out as Slippi's (%s): the disc must be an unchanged "
                     "Melee 1.02 (GALE01, revision 2)",
                     name, why != NULL ? why : "a different file");
            return -1;
        }
        snprintf(path, sizeof(path), "%s/%s", ROOT, e[i].path);
        if (save(path, out, (unsigned long) target) < 0) {
            free(source);
            free(out);
            free(diff);
            snprintf(error, error_size, "Could not write %s", path);
            return -1;
        }
        free(source);
        free(out);
        free(diff);
        progress(name, ++done, total, ctx);
    }
    return 0;
}

/* ---- the online client's settings ---------------------------------------- */

int slippi_get_rumble(void)
{
    char path[96];
    unsigned long size;
    char* text;
    int rumble = 1;
    snprintf(path, sizeof(path), "%s/rumble.txt", ROOT);
    text = (char*) load(path, &size);
    if (text != NULL) {
        rumble = text[0] != '0';
        free(text);
    }
    return rumble;
}

void slippi_set_rumble(int on)
{
    char path[96], text[3] = { on ? '1' : '0', '\n', 0 };
    snprintf(path, sizeof(path), "%s/rumble.txt", ROOT);
    save(path, text, 2);
}

/* A config.ini setting of `names` (index = value; the first, 0, is the
 * default and has no line); its other lines stay */
int slippi_get_choice(const char* key, const char* const* names, int count)
{
    char path[96], prefix[40];
    unsigned long size;
    char* text;
    int value = 0;
    snprintf(path, sizeof(path), "%s/config.ini", ROOT);
    snprintf(prefix, sizeof(prefix), "%s=", key);
    text = (char*) load(path, &size);
    if (text != NULL) {
        for (char* line = strtok(text, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
            if (strncmp(line, prefix, strlen(prefix)) == 0) {
                for (int i = 0; i < count; i++) {
                    if (strcmp(line + strlen(prefix), names[i]) == 0) {
                        value = i;
                    }
                }
            }
        }
        free(text);
    }
    return value;
}

void slippi_set_choice(const char* key, const char* const* names, int value)
{
    char path[96], prefix[40];
    unsigned long size = 0;
    char* text;
    FILE* f;
    snprintf(path, sizeof(path), "%s/config.ini", ROOT);
    snprintf(prefix, sizeof(prefix), "%s=", key);
    text = (char*) load(path, &size);
    f = fopen(path, "w");
    if (f == NULL) {
        free(text);
        return;
    }
    if (text != NULL) {
        for (char* line = strtok(text, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
            if (strncmp(line, prefix, strlen(prefix)) != 0) {
                fprintf(f, "%s\n", line);
            }
        }
        free(text);
    }
    if (value > 0) {
        fprintf(f, "%s%s\n", prefix, names[value]);
    }
    fclose(f);
}

/* config.ini's connection= (auto, wired, wifi) */
static const char* const CONNECTIONS[] = { "auto", "wired", "wifi" };

int slippi_get_connection(void)
{
    return slippi_get_choice("connection", CONNECTIONS, 3);
}

void slippi_set_connection(int value)
{
    slippi_set_choice("connection", CONNECTIONS, value);
}
