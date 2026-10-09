#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int c, char **v) {
	unsigned long a = strtoul(v[1], 0, 0), n = strtoul(v[2], 0, 0), pa = a & ~4095UL;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	unsigned char *m = mmap(0, n + (a - pa), PROT_READ, MAP_SHARED, fd, pa);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	unsigned char *buf = malloc(n);
	for (unsigned long i = 0; i < n; i += 4) *(unsigned *)(buf + i) = *(volatile unsigned *)(m + (a - pa) + i);
	fwrite(buf, 1, n, stdout);
	return 0;
}
