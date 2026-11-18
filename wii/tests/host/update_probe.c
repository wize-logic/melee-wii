/* Host test: launcher/update.c's check and install, against the real
 * releases, into a scratch folder (SD_ROOT) */
#include <stdio.h>
#include "../../src/launcher/update.h"
void so_entropy_init(void);
void so_sleep_us(unsigned us);
static void progress(const char *what, int done, int total) { if (done == total || done % 20 == 0) printf("  %s %d/%d\n", what, done, total); }
int main(void)
{
	char error[128];
	update_check_start();
	while (update_state() == UPDATE_CHECKING) so_sleep_us(100000);
	printf("state %d version %s error %s\n", update_state(), update_version(), update_error());
	if (update_state() != UPDATE_AVAILABLE) return 1;
	if (update_install(progress, error, sizeof(error)) < 0) { printf("install failed: %s\n", error); return 1; }
	printf("installed\n");
	return 0;
}
