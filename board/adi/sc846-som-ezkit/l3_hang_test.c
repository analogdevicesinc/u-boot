// SPDX-License-Identifier: GPL-2.0-or-later
/* Parametric bare-metal reproducer for the SC846 A55 memory freeze. */

#include <command.h>
#include <console.h>
#include <mapmem.h>
#include <string.h>
#include <vsprintf.h>

#define CHUNK_BYTES (32 * 1024)
#define DEFAULT_START 0x88000000UL
#define DEFAULT_SIZE (256UL * 1024 * 1024)

enum operation { OP_MIXED, OP_DESC, OP_READ, OP_WRITE, OP_REVWRITE };
enum pattern { PAT_ADDRESS, PAT_ZERO, PAT_ONES, PAT_AA55, PAT_WALKING };

static u64 value_for(enum pattern pattern, u64 *address)
{
	switch (pattern) {
	case PAT_ZERO: return 0;
	case PAT_ONES: return ~0ULL;
	case PAT_AA55: return ((uintptr_t)address >> 3) & 1 ?
		0xaaaaaaaaaaaaaaaaULL : 0x5555555555555555ULL;
	case PAT_WALKING: return 1ULL << (((uintptr_t)address >> 3) & 63);
	default: return (u64)(uintptr_t)address;
	}
}

static int parse_operation(const char *name, enum operation *operation)
{
	if (!strcmp(name, "mixed")) *operation = OP_MIXED;
	else if (!strcmp(name, "desc")) *operation = OP_DESC;
	else if (!strcmp(name, "read")) *operation = OP_READ;
	else if (!strcmp(name, "write")) *operation = OP_WRITE;
	else if (!strcmp(name, "revwrite")) *operation = OP_REVWRITE;
	else return -1;
	return 0;
}

static int parse_pattern(const char *name, enum pattern *pattern)
{
	if (!strcmp(name, "address")) *pattern = PAT_ADDRESS;
	else if (!strcmp(name, "zero")) *pattern = PAT_ZERO;
	else if (!strcmp(name, "ones")) *pattern = PAT_ONES;
	else if (!strcmp(name, "aa55")) *pattern = PAT_AA55;
	else if (!strcmp(name, "walking")) *pattern = PAT_WALKING;
	else return -1;
	return 0;
}

static int do_l3hang(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	ulong start = DEFAULT_START, size = DEFAULT_SIZE, limit = 0;
	ulong iteration, chunk, i, chunks, words;
	enum operation operation = OP_MIXED;
	enum pattern pattern = PAT_ADDRESS;
	volatile u64 sink = 0;
	u64 *buf, *p;

	if (argc > 1 && strict_strtoul(argv[1], 16, &start) < 0) return CMD_RET_USAGE;
	if (argc > 2 && strict_strtoul(argv[2], 16, &size) < 0) return CMD_RET_USAGE;
	if (argc > 3 && strict_strtoul(argv[3], 16, &limit) < 0) return CMD_RET_USAGE;
	if (argc > 4 && parse_operation(argv[4], &operation)) return CMD_RET_USAGE;
	if (argc > 5 && parse_pattern(argv[5], &pattern)) return CMD_RET_USAGE;
	if (!size || size % CHUNK_BYTES || start % sizeof(u64)) return CMD_RET_USAGE;

	printf("l3hang: start=0x%08lx size=0x%08lx chunk=0x%x iterations=%lu mode=%s pattern=%s\n",
	       start, size, CHUNK_BYTES, limit, argc > 4 ? argv[4] : "mixed",
	       argc > 5 ? argv[5] : "address");
	buf = map_sysmem(start, size);
	chunks = size / CHUNK_BYTES;
	words = size / sizeof(u64);
	for (iteration = 0; !limit || iteration < limit; iteration++) {
		if (ctrlc()) break;
		printf("l3hang: iteration %lu\r", iteration + 1);
		if (operation == OP_READ) {
			for (i = 0; i < words; i++) sink ^= buf[i];
			continue;
		}
		if (operation == OP_WRITE) {
			for (i = 0; i < words; i++) buf[i] = value_for(pattern, &buf[i]);
			continue;
		}
		if (operation == OP_DESC || operation == OP_REVWRITE) {
			if (operation == OP_DESC)
				for (i = 0; i < words; i++) buf[i] = value_for(pattern, &buf[i]);
			else
				for (i = words; i-- > 0;) buf[i] = value_for(pattern, &buf[i]);

			if (operation == OP_DESC) {
				for (i = words; i-- > 0;)
					if (buf[i] != value_for(pattern, &buf[i])) { p = &buf[i]; goto mismatch; }
			} else {
				for (i = 0; i < words; i++)
					if (buf[i] != value_for(pattern, &buf[i])) { p = &buf[i]; goto mismatch; }
			}
			continue;
		}
		for (chunk = 0; chunk < chunks; chunk++) {
			p = buf + chunk * (CHUNK_BYTES / sizeof(u64));

			for (i = 0; i < CHUNK_BYTES / sizeof(u64); i++)
				p[i] = value_for(pattern, &p[i]);
			for (i = 0; i < CHUNK_BYTES / sizeof(u64); i++)
				if (p[i] != value_for(pattern, &p[i])) { p = &p[i]; goto mismatch; }
		}
	}
	printf("\nl3hang: PASS iterations=%lu sink=0x%llx\n", iteration, sink);
	unmap_sysmem(buf);
	return CMD_RET_SUCCESS;

mismatch:
	printf("\nl3hang: FAIL iteration=%lu address=%p got=0x%llx expected=0x%llx\n",
	       iteration + 1, p, *p, value_for(pattern, p));
	unmap_sysmem(buf);
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(l3hang, 6, 1, do_l3hang,
	   "SC846 A55 memory-path test",
	   "[start] [size] [iterations] [mixed|desc|read|write|revwrite] "
	   "[address|zero|ones|aa55|walking]");
