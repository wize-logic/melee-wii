/* Slippi's folder for the launcher (files.c) */
#ifndef LAUNCHER_FILES_H
#define LAUNCHER_FILES_H

typedef struct SlippiStatus {
    int have_codeset;     /* netplay.gct */
    int files_ready;      /* every file of the manifest, at its size */
    int need_prepare;     /* the files made from the disc are not all there */
    int missing_shipped;  /* a file shipped as is is missing */
    int have_account;
    char name[32], code[16];
} SlippiStatus;

void slippi_status(SlippiStatus* st);
int slippi_account(char* name, int name_size, char* code, int code_size);
/* Makes the files the manifest says are patched from the disc; 0, or -1 and
 * error. progress is called as each file starts and is done. */
int slippi_prepare(void (*progress)(const char* name, int done, int total, void* ctx), void* ctx,
                   char* error, int error_size);

int slippi_get_rumble(void);
void slippi_set_rumble(int on);
int slippi_get_connection(void); /* 0 auto, 1 wired, 2 Wi-Fi */
void slippi_set_connection(int value);
/* a config.ini setting: one of `names` (0, the first, without a line) */
int slippi_get_choice(const char* key, const char* const* names, int count);
void slippi_set_choice(const char* key, const char* const* names, int value);

#endif
