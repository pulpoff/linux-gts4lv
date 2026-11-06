/* extents <file> <size>: the extent list init's dmlinear takes, for the sideload
 * installer: "GTS4LV-EXTENTS 1", one "logical physical length flags" line per
 * extent (bytes), "END". Fails unless the extents are contiguous, cover exactly
 * <size> bytes, and are all plain blocks on the disk (dm-linear reads them raw). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <linux/fiemap.h>

#define N 512
#define RAW_ONLY (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED | \
		  FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED | \
		  FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_DATA_TAIL)

int main(int argc, char **argv)
{
	struct fiemap *fm;
	unsigned long long next = 0, size;
	int fd, last = 0;

	if (argc != 3 || (fd = open(argv[1], O_RDONLY)) < 0) {
		fprintf(stderr, "usage: extents file size\n");
		return 2;
	}
	size = strtoull(argv[2], NULL, 0);
	fm = calloc(1, sizeof(*fm) + N * sizeof(struct fiemap_extent));
	printf("GTS4LV-EXTENTS 1\n");
	while (!last) {
		fm->fm_start = next;
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
			if (e->fe_logical != next || (e->fe_physical | e->fe_length) % 512 ||
			    (e->fe_flags & RAW_ONLY)) {
				fprintf(stderr, "unusable extent at %llu (flags 0x%x)\n", e->fe_logical, e->fe_flags);
				return 1;
			}
			printf("%llu %llu %llu 0x%x\n", e->fe_logical, e->fe_physical, e->fe_length, e->fe_flags);
			next = e->fe_logical + e->fe_length;
			last = e->fe_flags & FIEMAP_EXTENT_LAST;
		}
	}
	if (next != size) {
		fprintf(stderr, "extents cover %llu bytes, expected %llu\n", next, size);
		return 1;
	}
	printf("END\n");
	return 0;
}
