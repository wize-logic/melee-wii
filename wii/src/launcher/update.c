/* The launcher's updates (see update.h).
 *
 * GitHub's API names the latest release, its zip and the zip's SHA-256
 * (https://api.github.com/repos/<repo>/releases/latest). Installing
 * downloads the zip over TLS (so_https.c), checks the SHA-256 and each
 * file's CRC-32, and writes the files where the zip puts them, as the
 * README's install step does by hand: only apps/melee/ and the zip's two
 * files beside it. boot.dol goes last, written beside and then renamed.
 */
#include "update.h"

#include <gccore.h>
#include <malloc.h>
#include <network.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "../launcher.h"
#include "../slippi/so_config.h"
#include "../slippi/so_entropy.h"
#include "../slippi/so_https.h"
#include "../slippi/so_json.h"
#include "../slippi/so_platform.h"
#include "../slippi/so_socket.h"
#include "bearssl.h"

#define API_URL "https://api.github.com/repos/" MELEE_WII_UPDATE_REPO "/releases/latest"
#define USER_AGENT "melee-wii/" MELEE_WII_VERSION
#define ZIP_MAX (64u << 20)
#define UPDATE_THREAD_PRIORITY 32
#ifndef SD_ROOT /* (the host tests write elsewhere) */
const char* melee_wii_drive_root(void);
#define SD_ROOT melee_wii_drive_root() /* the drive the port was started from */
#endif

static volatile int s_state = UPDATE_IDLE;
static char s_version[40], s_error[96];
static char s_zip_url[512];
static u8 s_sha256[32];
static int s_have_sha;

static void failed(const char* text)
{
    snprintf(s_error, sizeof(s_error), "%s", text);
    so_log("update: check failed: %s", text);
    s_state = UPDATE_FAILED;
}

static int hex_byte(const char* p)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}

/* The release's version and zip, from the API's answer */
static int read_release(const char* text, u32 len)
{
    SoJson doc, assets, a;
    char tag[32], name[96], digest[80];
    int n;
    if (so_json_parse(&doc, text, len) < 0 || so_json_type(&doc) != SO_JSON_OBJECT) {
        return -1;
    }
    so_json_get_string(&doc, "tag_name", tag, sizeof(tag), "");
    snprintf(s_version, sizeof(s_version), "%s", tag[0] == 'v' ? tag + 1 : tag);
    if (!s_version[0] || so_json_get(&doc, "assets", &assets) < 0) {
        return -1;
    }
    n = so_json_count(&assets);
    for (int i = 0; i < n; i++) {
        so_json_at(&assets, i, &a);
        so_json_get_string(&a, "name", name, sizeof(name), "");
        if (strncmp(name, "melee-wii-v", 11) != 0 || strstr(name, ".zip") == NULL) {
            continue;
        }
        so_json_get_string(&a, "browser_download_url", s_zip_url, sizeof(s_zip_url), "");
        so_json_get_string(&a, "digest", digest, sizeof(digest), "");
        s_have_sha = 0;
        if (strncmp(digest, "sha256:", 7) == 0 && strlen(digest) == 7 + 64) {
            s_have_sha = 1;
            for (int k = 0; k < 32; k++) {
                int v = hex_byte(digest + 7 + 2 * k);
                if (v < 0) {
                    s_have_sha = 0;
                    break;
                }
                s_sha256[k] = (u8) v;
            }
        }
        return s_zip_url[0] ? 0 : -1;
    }
    return -1;
}

static u32 CheckThread(void* unused)
{
    u8* body = NULL;
    u32 len = 0;
    char error[96];
    u32 start = so_time_ms(), net_ms;
    (void) unused;
    s_state = UPDATE_CHECKING;
    so_log("update: bringing the network up");
    for (int tries = 0;; tries++) {
        u32 t = so_time_ms();
        s32 res = net_init();
        so_log("update: net_init %d after %u ms", (int) res, (unsigned) (so_time_ms() - t));
        if (res >= 0) {
            break;
        }
        if (tries >= 2) {
            failed("No internet connection");
            return 0;
        }
        so_sleep_us(2000000);
    }
    net_ms = so_time_ms() - start;
    /* (so_resolve, under so_https_get, looks a name up only once the network
     * is noted as up: so_socket_ogc.c's own thread notes it for Slippi Online,
     * which this check runs before) */
    so_socket_note_net_up();
    so_log("update: asking GitHub");
    so_entropy_init();
    if (so_https_get(API_URL, "application/vnd.github+json", USER_AGENT, 1u << 20, &body, &len, NULL, NULL, error,
                     sizeof(error)) < 0)
    {
        failed(error);
        return 0;
    }
    if (read_release((const char*) body, len) < 0) {
        free(body);
        failed("GitHub's answer has no release zip");
        return 0;
    }
    free(body);
    so_log("update: network up in %u ms, GitHub's answer %u ms after: latest %s", (unsigned) net_ms,
           (unsigned) (so_time_ms() - start - net_ms), s_version);
    s_state = so_version_newer(s_version, MELEE_WII_VERSION) ? UPDATE_AVAILABLE : UPDATE_NONE;
    return 0;
}

void update_check_start(void)
{
    /* one check at a time; a release already found is kept (an install that
     * failed is tried again) */
    if (s_state == UPDATE_CHECKING || s_state == UPDATE_AVAILABLE) {
        return;
    }
    s32 thread;
    s_error[0] = '\0';
    s_state = UPDATE_CHECKING;
    thread = so_thread_start(CheckThread, NULL, NULL, 64 * 1024, 0);
    if (thread < 0) {
        failed("Could not start the check");
        return;
    }
    /* below the launcher's own thread (libogc's main, 64): the TLS handshake
     * is seconds of work, and above it the screen and the controllers would
     * wait for it */
    LWP_SetThreadPriority((lwp_t) thread, UPDATE_THREAD_PRIORITY);
}

int update_state(void)
{
    return s_state;
}

const char* update_version(void)
{
    return s_version;
}

const char* update_error(void)
{
    return s_error;
}

void update_wait(int ms)
{
    while (s_state == UPDATE_CHECKING && ms > 0) {
        so_sleep_us(50000);
        ms -= 50;
    }
}

/* ---- installing ---------------------------------------------------------- */

typedef struct {
    void (*progress)(const char* what, int done, int total);
} Ctx;

static void download_progress(u32 done, u32 total, void* ctx)
{
    static u32 last;
    Ctx* c = (Ctx*) ctx;
    char what[64];
    if (total == 0 || done - last < 64 * 1024) {
        if (done != total) {
            return;
        }
    }
    last = done;
    snprintf(what, sizeof(what), "Downloading  (%u of %u KB)", (unsigned) (done / 1024),
             (unsigned) (total / 1024));
    c->progress(what, (int) (done / 1024), (int) (total / 1024));
}

static u32 le16(const u8* p)
{
    return (u32) p[0] | ((u32) p[1] << 8);
}

static u32 le32(const u8* p)
{
    return le16(p) | (le16(p + 2) << 16);
}

/* Only the zip's own places: apps/melee/... and plain names at the top */
static int allowed(const char* name)
{
    if (name[0] == '/' || strstr(name, "..") != NULL || strchr(name, '\\') != NULL || strchr(name, ':') != NULL) {
        return 0;
    }
    return strncmp(name, "apps/melee/", 11) == 0 || strchr(name, '/') == NULL;
}

/* Makes the folders of `path` (below the root) */
static void make_dirs(const char* path)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char* p = dir + strlen(SD_ROOT); *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0777);
            *p = '/';
        }
    }
}

static int write_file(const char* path, const u8* data, u32 len)
{
    FILE* f;
    int ok;
    make_dirs(path);
    f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }
    ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok ? 0 : -1;
}

/* One entry's bytes (malloc'd), checked against its CRC-32 */
static u8* extract(const u8* zip, u32 zip_len, const u8* central, u32* out_len)
{
    u32 method = le16(central + 10), crc = le32(central + 16), csize = le32(central + 20);
    u32 usize = le32(central + 24), local = le32(central + 42);
    const u8* data;
    u8* out;
    if (local + 30 > zip_len || le32(zip + local) != 0x04034B50) {
        return NULL;
    }
    data = zip + local + 30 + le16(zip + local + 26) + le16(zip + local + 28);
    if ((u32) (data - zip) + csize > zip_len) {
        return NULL;
    }
    out = (u8*) memalign(32, usize + 1);
    if (out == NULL) {
        return NULL;
    }
    if (method == 0 && csize == usize) {
        memcpy(out, data, usize);
    } else if (method == 8) {
        z_stream z;
        memset(&z, 0, sizeof(z));
        z.next_in = (Bytef*) data;
        z.avail_in = csize;
        z.next_out = out;
        z.avail_out = usize;
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
            free(out);
            return NULL;
        }
        int res = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        if (res != Z_STREAM_END || z.total_out != usize) {
            free(out);
            return NULL;
        }
    } else {
        free(out);
        return NULL;
    }
    if (crc32(0, out, usize) != crc) {
        free(out);
        return NULL;
    }
    *out_len = usize;
    return out;
}

int update_install(void (*progress)(const char* what, int done, int total), char* error, int error_size)
{
    Ctx ctx = { progress };
    u8 *zip = NULL, digest[32];
    u32 zip_len = 0, eocd = 0, count, dir, at;
    int written = 0, total;
    char path[256];
    if (s_state != UPDATE_AVAILABLE) {
        snprintf(error, error_size, "No update to install");
        return -1;
    }
    progress("Downloading", 0, 0);
    if (so_https_get(s_zip_url, "application/octet-stream", USER_AGENT, ZIP_MAX, &zip, &zip_len, download_progress,
                     &ctx, error, error_size) < 0)
    {
        return -1;
    }
    if (s_have_sha) {
        br_sha256_context sha;
        br_sha256_init(&sha);
        br_sha256_update(&sha, zip, zip_len);
        br_sha256_out(&sha, digest);
        if (memcmp(digest, s_sha256, 32) != 0) {
            free(zip);
            snprintf(error, error_size, "The download is not the release's file (SHA-256)");
            return -1;
        }
    }
    /* the end of central directory, then each entry */
    for (u32 i = zip_len >= 22 ? zip_len - 22 : 0; i > 0 && zip_len - i < 65558; i--) {
        if (le32(zip + i) == 0x06054B50) {
            eocd = i;
            break;
        }
    }
    if (eocd == 0) {
        free(zip);
        snprintf(error, error_size, "The download is not a zip");
        return -1;
    }
    count = le16(zip + eocd + 10);
    dir = le32(zip + eocd + 16);
    total = (int) count;
    /* everything first into memory and checked, then written: a bad zip
     * changes nothing */
    for (int pass = 0; pass < 3; pass++) {
        at = dir;
        for (u32 i = 0; i < count; i++) {
            char name[200];
            u32 nlen, len;
            u8* data;
            int is_dol;
            if (at + 46 > zip_len || le32(zip + at) != 0x02014B50) {
                free(zip);
                snprintf(error, error_size, "The zip's directory is damaged");
                return -1;
            }
            nlen = le16(zip + at + 28);
            if (nlen >= sizeof(name) || at + 46 + nlen > zip_len) {
                free(zip);
                snprintf(error, error_size, "The zip's directory is damaged");
                return -1;
            }
            memcpy(name, zip + at + 46, nlen);
            name[nlen] = '\0';
            {
                u32 next = at + 46 + nlen + le16(zip + at + 30) + le16(zip + at + 32);
                const u8* central = zip + at;
                at = next;
                if (nlen == 0 || name[nlen - 1] == '/') {
                    continue; /* a folder */
                }
                if (!allowed(name)) {
                    if (pass == 0) {
                        free(zip);
                        snprintf(error, error_size, "The zip has a file outside apps/melee: %.40s", name);
                        return -1;
                    }
                    continue;
                }
                is_dol = strcmp(name, "apps/melee/boot.dol") == 0;
                /* pass 0: check all; 1: write all but boot.dol; 2: boot.dol */
                if ((pass == 1 && is_dol) || (pass == 2 && !is_dol)) {
                    continue;
                }
                data = extract(zip, zip_len, central, &len);
                if (data == NULL) {
                    free(zip);
                    snprintf(error, error_size, "%.60s in the zip is damaged", name);
                    return -1;
                }
                if (pass == 0) {
                    free(data);
                    continue;
                }
                if (is_dol) {
                    char tmp[256];
                    snprintf(path, sizeof(path), "%s%s", SD_ROOT, name);
                    snprintf(tmp, sizeof(tmp), "%s%s.new", SD_ROOT, name);
                    if (write_file(tmp, data, len) < 0 || (unlink(path), rename(tmp, path)) != 0) {
                        free(data);
                        free(zip);
                        snprintf(error, error_size, "Could not write boot.dol");
                        return -1;
                    }
                } else {
                    snprintf(path, sizeof(path), "%s%s", SD_ROOT, name);
                    if (write_file(path, data, len) < 0) {
                        free(data);
                        free(zip);
                        snprintf(error, error_size, "Could not write %.60s", name);
                        return -1;
                    }
                }
                free(data);
                written++;
                progress("Installing", written, total);
            }
        }
    }
    free(zip);
    return 0;
}
