/* dmlinear <name> <blockdev> <extents>: create /dev/mapper-less dm-linear device
 * <name> that concatenates the byte ranges listed in <extents> (lines of
 * "logical physical length flags", as fiemap prints them) of <blockdev>.
 * Used to reach a file inside Android's /data without mounting /data.
 * Prints the dm device's "major:minor". */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <linux/dm-ioctl.h>

#define BUF (1 << 20)

static void init(struct dm_ioctl *io, const char *name, size_t size)
{
	memset(io, 0, sizeof(*io));
	io->version[0] = DM_VERSION_MAJOR;
	io->version[1] = DM_VERSION_MINOR;
	io->version[2] = DM_VERSION_PATCHLEVEL;
	io->data_size = size;
	io->data_start = sizeof(*io);
	strncpy(io->name, name, sizeof(io->name) - 1);
}

int main(int argc, char **argv)
{
	char *buf = calloc(1, BUF), *p;
	struct dm_ioctl *io = (struct dm_ioctl *)buf;
	unsigned long long log, phys, len, next = 0;
	unsigned flags, n = 0;
	FILE *f;
	int fd;

	if (argc != 4 || !(f = fopen(argv[3], "r"))) {
		fprintf(stderr, "usage: dmlinear name blockdev extents\n");
		return 2;
	}
	if ((fd = open("/dev/mapper/control", O_RDWR)) < 0 &&
	    (fd = open("/dev/device-mapper", O_RDWR)) < 0) {
		perror("dm control");
		return 1;
	}
	init(io, argv[1], sizeof(*io));
	if (ioctl(fd, DM_DEV_CREATE, io) < 0) {
		perror("DM_DEV_CREATE");
		return 1;
	}

	init(io, argv[1], BUF);
	p = buf + sizeof(*io);
	while (fscanf(f, "%llu %llu %llu %x", &log, &phys, &len, &flags) == 4) {
		struct dm_target_spec *t = (struct dm_target_spec *)p;
		char *params = p + sizeof(*t);
		size_t plen;

		if (log != next || (phys | len) & 511) {
			fprintf(stderr, "extent %u: hole or unaligned at %llu\n", n, log);
			return 1;
		}
		memset(t, 0, sizeof(*t));
		t->sector_start = log >> 9;
		t->length = len >> 9;
		strcpy(t->target_type, "linear");
		plen = sprintf(params, "%s %llu", argv[2], phys >> 9) + 1;
		t->next = (sizeof(*t) + plen + 7) & ~7;
		p += t->next;
		next = log + len;
		if (p - buf > BUF - 512) {
			fprintf(stderr, "too many extents\n");
			return 1;
		}
		n++;
	}
	io->target_count = n;
	io->data_size = p - buf;
	if (ioctl(fd, DM_TABLE_LOAD, io) < 0) {
		perror("DM_TABLE_LOAD");
		return 1;
	}
	init(io, argv[1], sizeof(*io));
	if (ioctl(fd, DM_DEV_SUSPEND, io) < 0) { /* without DM_SUSPEND_FLAG = resume */
		perror("DM_DEV_SUSPEND");
		return 1;
	}
	printf("%u:%u\n", major(io->dev), minor(io->dev));
	fprintf(stderr, "dmlinear: %s = %u extents, %llu MB\n", argv[1], n, next >> 20);
	return 0;
}
