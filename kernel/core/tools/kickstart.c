/*
 * tools/kickstart.c — ansh builtin for kickstart provisioning.
 *
 * Allows in-shell testing of kickstart configs and prints example
 * config text for reference.
 *
 * USAGE
 *   kickstart apply <inline-config>   Apply a mini config string
 *   kickstart example                 Print a sample kickstart file
 *   kickstart help                    Show usage
 */

#include <anx/types.h>
#include <anx/kickstart.h>
#include <anx/string.h>
#include <anx/kprintf.h>

static void
print_usage(void)
{
	kprintf("usage: kickstart <subcommand> [args]\n");
	kprintf("  apply <config>   Apply an inline kickstart config string\n");
	kprintf("  example          Print a sample kickstart file\n");
	kprintf("  help             Show this help\n");
}

static void
print_example(void)
{
	kprintf("# Anunix Kickstart - example\n");
	kprintf("[system]\n");
	kprintf("hostname=myhost\n");
	kprintf("timezone=-7\n");
	kprintf("\n");
	kprintf("[ui]\n");
	kprintf("theme=pretty\n");
	kprintf("font_scale=2\n");
	kprintf("\n");
	kprintf("[network]\n");
	kprintf("mode=dhcp\n");
	kprintf("\n");
	kprintf("[credentials]\n");
	kprintf("key:openai-key=sk-your-key-here\n");
	kprintf("\n");
	kprintf("[drivers]\n");
	kprintf("load=mt7925\n");
}

int
cmd_kickstart(int argc, char **argv)
{
	int ret;

	if (argc < 2) {
		print_usage();
		return ANX_EINVAL;
	}

	if (anx_strcmp(argv[1], "apply") == 0) {
		if (argc < 3) {
			kprintf("kickstart apply: missing config argument\n");
			return ANX_EINVAL;
		}
		ret = anx_ks_apply(argv[2], (uint32_t)anx_strlen(argv[2]));
		if (ret != ANX_OK)
			kprintf("kickstart apply: failed (%d): %s\n",
				ret, anx_ks_last_error());
		return ret;
	}

	if (anx_strcmp(argv[1], "example") == 0) {
		print_example();
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "help") == 0) {
		print_usage();
		return ANX_OK;
	}

	kprintf("kickstart: unknown subcommand '%s'\n", argv[1]);
	print_usage();
	return ANX_EINVAL;
}
