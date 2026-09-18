/* Shared history recall, bounded ring, draft restoration, and credential exclusion. */
#include <anx/shell.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/objstore_disk.h>
#include <anx/mock_blk.h>

#define CHECK(c) do { if (!(c)) { kprintf("history failed at %u\n", __LINE__); return -1; } } while (0)

int test_shell_history(void)
{
	struct anx_shell_history_cursor a, b;
	char input[256], other[256], cmd[32];
	struct {
		uint32_t magic, count, write_idx, pad;
		char entries[32][256];
	} disk;
	anx_oid_t oid = {1, 2};
	uint32_t actual, type, i, j;

	test_mock_blk_init(16384);
	CHECK(anx_disk_format("shell-history") == ANX_OK);
	anx_shell_history_record("echo first");
	anx_shell_history_record("echo second");
	anx_shell_history_record("echo second");
	anx_shell_history_reset(&a);
	anx_shell_history_reset(&b);
	anx_strlcpy(input, "unfinished", sizeof(input));
	CHECK(anx_shell_history_move(&a, -1, input, sizeof(input)) == 11);
	CHECK(!anx_strcmp(input, "echo second"));
	CHECK(anx_shell_history_move(&a, -1, input, sizeof(input)) == 10);
	CHECK(!anx_strcmp(input, "echo first"));
	CHECK(anx_shell_history_move(&a, 1, input, sizeof(input)) == 11);
	CHECK(anx_shell_history_move(&a, 1, input, sizeof(input)) == 10);
	CHECK(!anx_strcmp(input, "unfinished"));
	CHECK(anx_shell_history_move(&a, 1, input, sizeof(input)) < 0);
	anx_strlcpy(other, "independent", sizeof(other));
	CHECK(anx_shell_history_move(&b, -1, other, sizeof(other)) == 11);
	CHECK(!anx_strcmp(input, "unfinished"));
	CHECK(anx_shell_history_move(&b, 1, other, sizeof(other)) == 11);
	CHECK(!anx_strcmp(other, "independent"));

	anx_shell_history_record("secret set dummy synthetic-value");
	anx_shell_history_record("  secret\trotate dummy synthetic-value");
	anx_shell_history_record("echo ok|'secret' set dummy synthetic-value");
	anx_shell_history_record("useradd dummy synthetic-value");
	anx_shell_history_record(" \t");
	anx_shell_history_record(NULL);
	CHECK(anx_shell_history_move(&a, -1, input, sizeof(input)) == 11);
	CHECK(!anx_strcmp(input, "echo second"));
	CHECK(anx_shell_history_move(&a, 0, input, sizeof(input)) < 0);
	CHECK(anx_shell_history_move(&a, -1, input, 0) < 0);
	CHECK(anx_shell_history_move(NULL, -1, input, sizeof(input)) < 0);

	/* Ring wrap has exactly 32 unique commands, oldest boundary is stable. */
	for (i = 0; i < 40; i++) {
		anx_snprintf(cmd, sizeof(cmd), "echo item-%u", i);
		anx_shell_history_record(cmd);
	}
	anx_shell_history_reset(&a);
	for (i = 0; i < 32; i++) {
		anx_snprintf(cmd, sizeof(cmd), "echo item-%u", 39 - i);
		CHECK(anx_shell_history_move(&a, -1, input, sizeof(input)) >= 0);
		CHECK(!anx_strcmp(input, cmd));
	}
	CHECK(anx_shell_history_move(&a, -1, input, sizeof(input)) < 0);
	CHECK(!anx_strcmp(input, "echo item-8"));
	anx_shell_history_reset(&a);
	CHECK(anx_shell_history_move(&a, -1, input, 5) == 4);
	CHECK(!anx_strcmp(input, "echo"));

	/* Raw persistence must have zero tails (not merely hidden strings). */
	CHECK(anx_disk_read_obj(&oid, &disk, sizeof(disk), &actual, &type) == ANX_OK);
	CHECK(actual == sizeof(disk) && type == 0x5348 && disk.count == 32);
	for (i = 0; i < 32; i++) {
		CHECK(!anx_strncmp(disk.entries[i], "echo item-", 10));
		for (j = (uint32_t)anx_strlen(disk.entries[i]); j < 256; j++)
			CHECK(disk.entries[i][j] == 0);
	}
	return 0;
}
