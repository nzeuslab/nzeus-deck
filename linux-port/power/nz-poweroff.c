/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nz-poweroff: ask the kernel to switch the console off (the RAM rescue
 * BusyBox has no poweroff applet). sync(), then reboot(RB_POWER_OFF); on
 * gma-native-power the nzeus-r36s-power driver then drives L14 low.
 * Usage: nz-poweroff --yes
 */
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "--yes")) {
		fputs("Usage: nz-poweroff --yes\n", stderr);
		return 1;
	}
	sync();
	puts("NZ_POWEROFF_REQUESTED");
	fflush(stdout);
	sleep(1);   /* let the line reach the PC over USB */
	reboot(RB_POWER_OFF);
	perror("reboot(RB_POWER_OFF)");
	return 1;
}
