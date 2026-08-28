/*
 * exec.c — Run a binary via the real ELF exec path (anx_posix_exec).
 *
 * USAGE
 *   exec <path>
 *   exec /bin/hello
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/posix.h>
#include <anx/kprintf.h>

void cmd_exec(int argc, char **argv)
{
	struct anx_posix_exec_result result;
	int rc;
	size_t i;

	if (argc < 2) {
		kprintf("usage: exec <path>\n");
		return;
	}

	rc = anx_posix_exec(argv[1]);
	if (rc != ANX_OK) {
		kprintf("exec: failed (%d)\n", rc);
		return;
	}
	rc = anx_posix_exec_last_result(&result);
	if (rc != ANX_OK) {
		kprintf("exec: no result available\n");
		return;
	}

	kprintf("exec: exit_status=%d, stdout (%u bytes):\n",
		result.exit_status, (uint32_t)result.stdout_len);
	for (i = 0; i < result.stdout_len; i++)
		kputc(result.stdout_text[i]);
	if (result.stdout_len > 0)
		kputc('\n');
}
