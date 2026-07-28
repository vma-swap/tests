#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "test_helper.h"

#define DEFAULT_ITERS 8
/* VM has 130M; allocate more than RAM so later iters must cycle from disk. */
#define DEFAULT_BYTES (200ULL * 1024 * 1024)

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
	size_t bytes = DEFAULT_BYTES;
	int iters = DEFAULT_ITERS;
	unsigned char *addr;
	long t0, t1, prev;
	int i, page;

	if (argc > 1)
		bytes = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		iters = atoi(argv[2]);
	if (bytes < PAGE_SIZE || iters < 1) {
		fprintf(stderr, "usage: %s [bytes] [iters]\n", argv[0]);
		return 1;
	}

	addr = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NAMED_SWAP, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	printf("bytes=%zu pages=%zu iters=%d\n", bytes, bytes / PAGE_SIZE, iters);
	fflush(stdout);

	/* First pass: RMW like the microbench (read zero + write). */
	t0 = now_ms();
	for (page = 0; page < (int)(bytes / PAGE_SIZE); page++)
		addr[page * PAGE_SIZE]++;
	t1 = now_ms();
	printf("init_ms=%ld\n", t1 - t0);
	prev = t1;

	for (i = 0; i < iters; i++) {
		for (page = 0; page < (int)(bytes / PAGE_SIZE); page++)
			addr[page * PAGE_SIZE]++;
		t1 = now_ms();
		printf("iter_%d_ms=%ld\n", i, t1 - prev);
		prev = t1;
		fflush(stdout);
	}

	munmap(addr, bytes);
	printf("BENCH_DONE\n");
	return 0;
}
