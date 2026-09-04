/*
 * config.c — ansh frontend for RLM-driven configuration (RFC-0027).
 *
 *   config get <area> <key>
 *   config set <area> <key> <value...>          gated write
 *   config list [area]
 *   config declare <area> <key> <type> [spec]   type: string|int|bool|enum
 *   config propose <goal...>                     RLM proposes; the gate decides
 *
 * Every write is a world-graph patch through the commit gate, so `config set`
 * and `config propose` share one governed, versioned, provenance-tracked path.
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/config.h>
#include <anx/worldgraph.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Join argv[from..argc) into buf, space-separated. */
static void join_args(int argc, char **argv, int from, char *buf, size_t len)
{
	int i;
	size_t pos = 0;

	buf[0] = '\0';
	for (i = from; i < argc; i++) {
		size_t n = anx_strlen(argv[i]);

		if (pos && pos + 1 < len)
			buf[pos++] = ' ';
		if (pos + n >= len)
			n = len - pos - 1;
		anx_memcpy(buf + pos, argv[i], n);
		pos += n;
		buf[pos] = '\0';
	}
}

static void report_result(int rc, const struct anx_world_commit_report *rep)
{
	if (rc == ANX_OK)
		kprintf("committed (config graph v%u)\n",
			(unsigned)anx_world_graph_version(anx_config_graph()));
	else if (rep && rep->reason[0])
		kprintf("rejected (%d): %s\n", rc, rep->reason);
	else
		kprintf("failed (%d)\n", rc);
}

static int list_cb(const char *area, const char *key, const char *value,
		   void *arg)
{
	(void)arg;
	kprintf("  %s.%s = %s\n", area, key, value);
	return 0;
}

static enum anx_config_type parse_type(const char *s, bool *ok)
{
	*ok = true;
	if (anx_strcmp(s, "int") == 0)
		return ANX_CFG_INT;
	if (anx_strcmp(s, "bool") == 0)
		return ANX_CFG_BOOL;
	if (anx_strcmp(s, "enum") == 0)
		return ANX_CFG_ENUM;
	if (anx_strcmp(s, "string") == 0)
		return ANX_CFG_STRING;
	*ok = false;
	return ANX_CFG_STRING;
}

static void usage(void)
{
	kprintf("usage: config <command>\n");
	kprintf("  get <area> <key>\n");
	kprintf("  set <area> <key> <value...>\n");
	kprintf("  list [area]\n");
	kprintf("  declare <area> <key> <type> [spec]   type: string|int|bool|enum\n");
	kprintf("  propose <goal...>                    RLM proposes a change\n");
}

void cmd_config(int argc, char **argv)
{
	anx_config_init();

	if (argc < 2 || anx_strcmp(argv[1], "help") == 0) {
		usage();
		return;
	}

	if (anx_strcmp(argv[1], "get") == 0) {
		char val[ANX_CONFIG_VAL_MAX];

		if (argc < 4) {
			kprintf("usage: config get <area> <key>\n");
			return;
		}
		if (anx_config_get(argv[2], argv[3], val, sizeof(val)) == ANX_OK)
			kprintf("%s.%s = %s\n", argv[2], argv[3], val);
		else
			kprintf("config: %s.%s not set\n", argv[2], argv[3]);
	} else if (anx_strcmp(argv[1], "set") == 0) {
		char value[ANX_CONFIG_VAL_MAX];
		struct anx_world_commit_report rep;
		int rc;

		if (argc < 5) {
			kprintf("usage: config set <area> <key> <value...>\n");
			return;
		}
		join_args(argc, argv, 4, value, sizeof(value));
		rc = anx_config_try(argv[2], argv[3], value, &rep);
		report_result(rc, &rep);
	} else if (anx_strcmp(argv[1], "list") == 0) {
		kprintf("config:\n");
		anx_config_list(argc >= 3 ? argv[2] : NULL, list_cb, NULL);
	} else if (anx_strcmp(argv[1], "declare") == 0) {
		enum anx_config_type type;
		bool ok;

		if (argc < 5) {
			kprintf("usage: config declare <area> <key> <type> [spec]\n");
			return;
		}
		type = parse_type(argv[4], &ok);
		if (!ok) {
			kprintf("config: unknown type '%s'\n", argv[4]);
			return;
		}
		if (anx_config_declare(argv[2], argv[3], type,
				       argc >= 6 ? argv[5] : NULL) == ANX_OK)
			kprintf("declared %s.%s : %s\n", argv[2], argv[3],
				argv[4]);
		else
			kprintf("config: declare failed\n");
	} else if (anx_strcmp(argv[1], "propose") == 0) {
		struct anx_world_commit_report rep;
		char goal[256];
		int rc;

		if (argc < 3) {
			kprintf("usage: config propose <goal...>\n");
			return;
		}
		join_args(argc, argv, 2, goal, sizeof(goal));
		rc = anx_configurator_run(goal, &rep);
		if (rc == ANX_OK) {
			report_result(rc, &rep);
		} else {
			kprintf("propose failed (%d)", rc);
			if (rc == ANX_ENOSYS || rc == ANX_EIO)
				kprintf(" — no model endpoint configured?");
			kprintf("\n");
		}
	} else {
		kprintf("config: unknown subcommand '%s' (try 'config help')\n",
			argv[1]);
	}
}
