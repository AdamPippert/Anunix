/* Shared history recall, bounded ring, draft restoration, and credential exclusion. */
#include <anx/shell.h>
#include <anx/input.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/objstore_disk.h>
#include <anx/mock_blk.h>

#define CHECK(c) do { if (!(c)) { kprintf("history failed at %u\n", __LINE__); return -1; } } while (0)

extern void test_mock_console_input(const char *input);
extern int test_shell_readline(char *buf, size_t size);

static int test_console_editing(void)
{
	char input[256], overflow[270];
	uint32_t i;

	/* CSI, SS3, tilde Home/End and Delete arrive one byte at a time. */
	test_mock_console_input("echo AXC\033[D\033[D\033[3~B\033[H\b\033[F\r");
	CHECK(test_shell_readline(input, sizeof(input)) == 8);
	CHECK(!anx_strcmp(input, "echo ABC"));
	test_mock_console_input("AXC\033OH\033[C\033[3~B\033OF\bD\033[1~\033[4~\r");
	CHECK(test_shell_readline(input, sizeof(input)) == 3);
	CHECK(!anx_strcmp(input, "ABD"));
	/* Unsupported CSI must not leak its suffix into a command. */
	test_mock_console_input("ok\033[1;5D!\r");
	CHECK(test_shell_readline(input, sizeof(input)) == 3);
	CHECK(!anx_strcmp(input, "ok!"));
	for (i = 0; i < 260; i++) overflow[i] = 'x';
	anx_strlcpy(overflow + 260, "\bZ\r", sizeof(overflow) - 260);
	test_mock_console_input(overflow);
	CHECK(test_shell_readline(input, sizeof(input)) == 255);
	CHECK(input[254] == 'Z' && !input[255]);
	test_mock_console_input("ab\003");
	CHECK(test_shell_readline(input, sizeof(input)) == 0 && !input[0]);
	test_mock_console_input(NULL);
	return 0;
}

static int test_input_editing(void)
{
	struct anx_shell_history_cursor recall;
	struct { char input[8]; char guard[4]; } draft = {"AC", "XYZ"};
	uint32_t len = 2, pos = 2;

#define EDIT(k, u) anx_shell_input_key(draft.input, sizeof(draft.input), \
					&len, &pos, &recall, (k), (u))
	anx_shell_history_reset(&recall);
	CHECK(EDIT(ANX_KEY_LEFT, 0) && pos == 1);
	CHECK(EDIT(ANX_KEY_NONE, 'B') && !anx_strcmp(draft.input, "ABC"));
	CHECK(len == 3 && pos == 2);
	CHECK(EDIT(ANX_KEY_HOME, 0) && pos == 0);
	CHECK(EDIT(ANX_KEY_BACKSPACE, 0) && pos == 0 && len == 3);
	CHECK(EDIT(ANX_KEY_LEFT, 0) && pos == 0);
	CHECK(EDIT(ANX_KEY_DELETE, 0) && !anx_strcmp(draft.input, "BC"));
	CHECK(EDIT(ANX_KEY_RIGHT, 0) && pos == 1);
	CHECK(EDIT(ANX_KEY_BACKSPACE, 0) && !anx_strcmp(draft.input, "C"));
	CHECK(pos == 0 && len == 1);
	CHECK(EDIT(ANX_KEY_END, 0) && pos == 1);
	CHECK(EDIT(ANX_KEY_DELETE, 0) && len == 1);
	CHECK(EDIT(ANX_KEY_RIGHT, 0) && pos == 1);
	while (len < 7) CHECK(EDIT(ANX_KEY_NONE, 'x'));
	CHECK(EDIT(ANX_KEY_NONE, 'y') && len == 7 && pos == 7);
	CHECK(EDIT(ANX_KEY_HOME, 0));
	CHECK(EDIT(ANX_KEY_NONE, 'z') && len == 7 && pos == 0);
	CHECK(EDIT(ANX_KEY_DELETE, 0) && len == 6 && pos == 0);
	CHECK(EDIT(ANX_KEY_NONE, 'z') && !anx_strcmp(draft.input, "zxxxxxx"));
	CHECK(!anx_strcmp(draft.guard, "XYZ"));
	CHECK(!EDIT(ANX_KEY_NONE, 0x100));
	CHECK(!EDIT(ANX_KEY_ENTER, 0));
	CHECK(EDIT(ANX_KEY_UP, 0) && pos == len);
	CHECK(EDIT(ANX_KEY_LEFT, 0));
	CHECK(EDIT(ANX_KEY_DOWN, 0) && !anx_strcmp(draft.input, "zxxxxxx"));
	CHECK(pos == len);
	CHECK(EDIT(ANX_KEY_UP, 0));
	CHECK(EDIT(ANX_KEY_BACKSPACE, 0) && recall.offset == -1);
	CHECK(EDIT(ANX_KEY_UP, 0));
	CHECK(EDIT(ANX_KEY_DOWN, 0) && !anx_strcmp(draft.input, "echo i"));
	CHECK(!anx_strcmp(draft.guard, "XYZ"));
#undef EDIT
	return 0;
}

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
	CHECK(test_input_editing() == 0);
	return test_console_editing();
}
