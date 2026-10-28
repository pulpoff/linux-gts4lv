/* fiemap <file>: print the file's extents as "logical physical length flags"
 * in bytes, so the blocks can be mapped (dm-linear / loop offset) from another
 * kernel without mounting the filesystem that holds them. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#define N 512

int main(int argc, char **argv)
{
	struct fiemap *fm;
	unsigned long long start = 0;
	int fd, last = 0;

	if (argc != 2 || (fd = open(argv[1], O_RDONLY)) < 0) {
		perror("open");
		return 1;
	}
	fm = calloc(1, sizeof(*fm) + N * sizeof(struct fiemap_extent));
	while (!last) {
		fm->fm_start = start;
		fm->fm_length = ~0ULL;
		fm->fm_flags = FIEMAP_FLAG_SYNC;
		fm->fm_extent_count = N;
		if (ioctl(fd, FS_IOC_FIEMAP, fm) < 0) {
			perror("FS_IOC_FIEMAP");
			return 1;
		}
		if (!fm->fm_mapped_extents)
			break;
		for (unsigned i = 0; i < fm->fm_mapped_extents; i++) {
			struct fiemap_extent *e = &fm->fm_extents[i];
			printf("%llu %llu %llu 0x%x\n", e->fe_logical, e->fe_physical,
			       e->fe_length, e->fe_flags);
			start = e->fe_logical + e->fe_length;
			last = e->fe_flags & FIEMAP_EXTENT_LAST;
		}
	}
	return 0;
}
