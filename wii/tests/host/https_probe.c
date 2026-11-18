/* Host test: so_https_get against GitHub (the release API and a download) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "so_entropy.h"
#include "so_https.h"
#include "bearssl.h"

static void prog(u32 done, u32 total, void *ctx) { (void)ctx; if (done == total) printf("  %u of %u\n", done, total); }

int main(int argc, char **argv)
{
	u8 *body; u32 len; char err[128];
	const char *url = argc > 1 ? argv[1] : "https://api.github.com/repos/wize-logic/melee-wii/releases/latest";
	so_entropy_init();
	if (so_https_get(url, "application/vnd.github+json", "melee-wii-updater", 32u << 20, &body, &len, prog, NULL, err, sizeof(err)) < 0) {
		printf("failed: %s\n", err);
		return 1;
	}
	printf("got %u bytes\n", len);
	if (argc > 2) { FILE *f = fopen(argv[2], "wb"); fwrite(body, 1, len, f); fclose(f); }
	else fwrite(body, 1, len > 600 ? 600 : len, stdout);
	printf("\n");
	free(body);
	return 0;
}
