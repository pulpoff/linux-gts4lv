/* rebootmode <reason>: reboot(2) with a restart command, which busybox's reboot
 * cannot pass. The PON reboot-mode driver turns "normal" into Samsung's
 * NORMALBOOT (back to Android) and "download" into download mode. */
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: rebootmode normal|download|recovery|bootloader\n");
		return 2;
	}
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_RESTART2, argv[1]);
	perror("reboot");
	return 1;
}
