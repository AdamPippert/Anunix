/*
 * shell.c — Kernel monitor shell.
 *
 * Interactive command loop for exercising all kernel subsystems
 * over the serial console. Supports line editing (backspace)
 * and dispatches to subsystem-specific command handlers.
 */

#include <anx/types.h>
#include <anx/shell.h>
#include <anx/arch.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/cell_plan.h>
#include <anx/cell_trace.h>
#include <anx/memplane.h>
#include <anx/engine.h>
#include <anx/route.h>
#include <anx/sched.h>
#include <anx/netplane.h>
#include <anx/capability.h>
#include <anx/page.h>
#include <anx/alloc.h>
#include <anx/pci.h>
#include <anx/net.h>
#include <anx/virtio_net.h>
#include <anx/http.h>
#include <anx/credential.h>
#include <anx/virtio_blk.h>
#include <anx/blk.h>
#include <anx/objstore_disk.h>
#include <anx/auth.h>
#include <anx/model_client.h>
#include <anx/gui.h>
#include <anx/interface_plane.h>
#include <anx/io.h>
#include <anx/perf.h>
#include <anx/tools.h>
#include <anx/installer.h>
#include <anx/acpi.h>
#include <anx/httpd.h>
#include <anx/sshd.h>
#include <anx/crypto.h>
#include <anx/browser.h>
#include <anx/base64.h>
#include <anx/e1000.h>
#include <anx/mt7925.h>
#include <anx/xdna.h>
#include <anx/browser_cell.h>
#include <anx/vm.h>
#include <anx/loop.h>
#include <anx/rlm.h>
#include <anx/jepa.h>
#include <anx/memory.h>
#include <anx/wm.h>
#include <anx/fb.h>

/* --- Line input with history --- */

#define MAX_LINE	256
#define MAX_ARGS	16
#define HISTORY_SIZE	32
#define PIPE_CAP_SZ	8192
#define MAX_PIPE_STAGES	8

/* Pipe stdin — set by execute_line when running right side of a pipe */
static const char *g_pipe_stdin;
static uint32_t    g_pipe_stdin_len;

static char history[HISTORY_SIZE][MAX_LINE];
static uint32_t history_count;
static uint32_t history_write;	/* next slot to write */

static void kputs(const char *s)
{
	kprintf("%s", s);
}

/* Shell history persistence — uses a fixed OID on the disk store */
#define HIST_DISK_OID_HI	0x0000000000000001ULL
#define HIST_DISK_OID_LO	0x0000000000000002ULL
#define HIST_DISK_TYPE		0x00005348	/* 'SH' */
#define HIST_MAGIC		0x48535448	/* 'HSTH' */

struct history_disk {
	uint32_t magic;
	uint32_t count;
	uint32_t write_idx;
	uint32_t _pad;
	char     entries[HISTORY_SIZE][MAX_LINE];
};

static void history_save_to_disk(void)
{
	static struct history_disk disk;
	anx_oid_t oid;

	if (!anx_blk_ready())
		return;

	disk.magic     = HIST_MAGIC;
	disk.count     = history_count;
	disk.write_idx = history_write;
	disk._pad      = 0;
	anx_memcpy(disk.entries, history, sizeof(history));

	oid.hi = HIST_DISK_OID_HI;
	oid.lo = HIST_DISK_OID_LO;
	anx_disk_delete_obj(&oid);
	anx_disk_write_obj(&oid, HIST_DISK_TYPE, &disk, sizeof(disk));
}

static void history_load_from_disk(void)
{
	static struct history_disk disk;
	anx_oid_t oid;
	uint32_t actual = 0, obj_type = 0;

	if (!anx_blk_ready())
		return;

	oid.hi = HIST_DISK_OID_HI;
	oid.lo = HIST_DISK_OID_LO;
	if (anx_disk_read_obj(&oid, &disk, sizeof(disk), &actual, &obj_type)
	    != ANX_OK)
		return;
	if (disk.magic != HIST_MAGIC || actual < sizeof(disk))
		return;

	anx_memcpy(history, disk.entries, sizeof(history));
	history_count = disk.count;
	history_write = disk.write_idx;

	/* Clamp to valid range */
	if (history_count > HISTORY_SIZE)
		history_count = HISTORY_SIZE;
	if (history_write >= HISTORY_SIZE)
		history_write = 0;
}

static void history_add(const char *line)
{
	if (line[0] == '\0')
		return;
	/* Don't duplicate the last entry */
	if (history_count > 0) {
		uint32_t prev = (history_write + HISTORY_SIZE - 1) %
				HISTORY_SIZE;
		if (anx_strcmp(history[prev], line) == 0)
			return;
	}
	anx_strlcpy(history[history_write], line, MAX_LINE);

	/* Scrub 'secret set' values from history */
	if (anx_strncmp(history[history_write], "secret set ", 11) == 0) {
		/* Keep "secret set <name>" but erase the value */
		char *p = history[history_write] + 11;

		/* Skip the name */
		while (*p && *p != ' ')
			p++;
		/* Zero everything after the name */
		if (*p)
			*p = '\0';
	}

	history_write = (history_write + 1) % HISTORY_SIZE;
	if (history_count < HISTORY_SIZE)
		history_count++;

	history_save_to_disk();
}

/* Clear the current line on the terminal and redraw with new content */
static void line_replace(char *buf, size_t *pos, size_t size,
			  const char *new_content)
{
	size_t i;
	size_t new_len;

	/* Erase current line: backspace over every character */
	for (i = 0; i < *pos; i++)
		kputs("\b \b");

	/* Copy new content */
	new_len = anx_strlen(new_content);
	if (new_len >= size)
		new_len = size - 1;
	anx_memcpy(buf, new_content, new_len);
	buf[new_len] = '\0';
	*pos = new_len;

	/* Display it */
	for (i = 0; i < new_len; i++)
		kputc(buf[i]);
}

static int kgetline(char *buf, size_t size)
{
	size_t pos = 0;
	uint32_t hist_idx = history_count;	/* past the end = current input */
	char saved[MAX_LINE];			/* save in-progress input */

	saved[0] = '\0';

	while (pos < size - 1) {
		int c;

		/* Poll for input, updating clock, repainting, and servicing HTTP */
		while (!arch_console_has_input()) {
			anx_gui_update_time();
			anx_iface_compositor_repaint();
			anx_net_poll();
			anx_httpd_poll();
			anx_sshd_poll();
			anx_e1000_poll();
			anx_mt7925_poll();
			anx_browser_cell_tick();
			anx_browser_poll();
		}

		c = arch_console_getc();
		if (c < 0)
			break;

		if (c == '\r' || c == '\n') {
			kputc('\r');
			kputc('\n');
			break;
		}

		if (c == 0x7F || c == '\b') {
			if (pos > 0) {
				pos--;
				kputs("\b \b");
			}
			continue;
		}

		if (c == 0x03) {
			kputs("^C\n");
			pos = 0;
			break;
		}

		/* Arrow key escape sequences: ESC [ A (up), ESC [ B (down) */
		if (c == 0x1B) {
			int c2 = arch_console_getc();

			if (c2 < 0)
				continue;
			if (c2 == '[') {
				int c3 = arch_console_getc();

				if (c3 < 0)
					continue;
				if (c3 == 'A') {
					/* Up arrow */
					if (history_count == 0)
						continue;
					if (hist_idx == history_count) {
						/* Save current input */
						buf[pos] = '\0';
						anx_strlcpy(saved, buf,
							     MAX_LINE);
					}
					if (hist_idx > 0)
						hist_idx--;
					/* Map hist_idx to ring buffer */
					{
						uint32_t ri;

						if (history_count < HISTORY_SIZE)
							ri = hist_idx;
						else
							ri = (history_write +
							      hist_idx) %
							     HISTORY_SIZE;
						line_replace(buf, &pos,
							     size,
							     history[ri]);
					}
				} else if (c3 == 'B') {
					/* Down arrow */
					if (hist_idx >= history_count)
						continue;
					hist_idx++;
					if (hist_idx == history_count) {
						/* Restore saved input */
						line_replace(buf, &pos,
							     size, saved);
					} else {
						uint32_t ri;

						if (history_count < HISTORY_SIZE)
							ri = hist_idx;
						else
							ri = (history_write +
							      hist_idx) %
							     HISTORY_SIZE;
						line_replace(buf, &pos,
							     size,
							     history[ri]);
					}
				}
				/* Ignore other escape sequences */
			}
			continue;
		}

		if (c >= 0x20 && c < 0x7F) {
			buf[pos++] = (char)c;
			kputc((char)c);
		}
	}

	buf[pos] = '\0';
	return (int)pos;
}

/* --- Argument parsing --- */

static int parse_args(char *line, char **argv, int max_args)
{
	int argc = 0;

	while (*line && argc < max_args) {
		/* Skip whitespace */
		while (*line == ' ' || *line == '\t')
			line++;
		if (*line == '\0')
			break;

		if (*line == '"' || *line == '\'') {
			/* Quoted argument: strip quotes, allow spaces inside */
			char q = *line++;
			argv[argc++] = line;
			while (*line && *line != q)
				line++;
			if (*line)
				*line++ = '\0';
		} else {
			argv[argc++] = line;
			while (*line && *line != ' ' && *line != '\t')
				line++;
			if (*line)
				*line++ = '\0';
		}
	}

	return argc;
}

/* --- Command handlers --- */

static void cmd_help(int argc, char **argv)
{
	const char *topic = (argc >= 2) ? argv[1] : NULL;

	if (!topic) {
		kputs("ansh — Anunix Shell.  Type 'help <topic>' for details.\n\n");
		kputs("Topics:\n");
		kputs("  help objects    State objects, namespaces\n");
		kputs("  help model      AI models, agents, tensors\n");
		kputs("  help network    Networking, HTTP, WiFi\n");
		kputs("  help system     System info, hardware, scheduler\n");
		kputs("  help workflow   Workflow engine\n");
		kputs("  help security   Credentials, auth\n");
		kputs("  help shell      Builtins, pipes, history\n");
		return;
	}

	if (anx_strcmp(topic, "objects") == 0) {
		kputs("State objects and namespaces:\n");
		kputs("  ls [ns:path]               List namespace entries\n");
		kputs("  cat <oid-or-path>          Read object payload\n");
		kputs("  write <ns:path> <content>  Create a State Object\n");
		kputs("  cp <src> <dst>             Copy object with provenance\n");
		kputs("  mv <src> <dst>             Move/rename namespace binding\n");
		kputs("  rm [-f] <ns:path>          Delete a State Object\n");
		kputs("  inspect <oid-or-path>      Full object inspection\n");
		kputs("  search [-i] <pattern>      Search object payloads\n");
		kputs("  fetch <host> <port> [path] [ns:name]  HTTP GET -> object\n");
		kputs("  state create|show|seal|delete  State object lifecycle\n");
		kputs("  meta show|set|get <path>   Object metadata editor\n");
		kputs("  store format|mount|stats   Object store management\n");
		kputs("  disk                       Show block device info\n");
		kputs("  cells                      List execution cells\n");
		return;
	}

	if (anx_strcmp(topic, "model") == 0) {
		kputs("AI models, agents, tensors:\n");
		kputs("  ask <message...>           Ask Claude a question\n");
		kputs("  agent <goal...>            Run AI agent loop\n");
		kputs("  model-init <cred> <host> <port>  Configure model endpoint\n");
		kputs("  model info|layers|diff|import  Model namespace\n");
		kputs("  tensor create|info|stats|fill  Tensor operations\n");
		kputs("  tensor slice|diff|quantize|search  Tensor ops (Phase 2)\n");
		kputs("  rlm run [prompt]           Run a rollout with current adapter\n");
		kputs("  rlm pal <i> <world> [s] [a]  Feed rollout score to PAL\n");
		kputs("  xdna [load]                AMD XDNA NPU info / load firmware\n");
		kputs("  loop status|run            IBAL training loop\n");
		kputs("  jepa                       JEPA world-model status\n");
		kputs("  api <cred> <host> <port> [path]  Authenticated API call\n");
		return;
	}

	if (anx_strcmp(topic, "network") == 0) {
		kputs("Networking, HTTP, WiFi:\n");
		kputs("  net status                 Show network plane status\n");
		kputs("  netinfo                    Network configuration\n");
		kputs("  ntp [server-ip]            Sync time from NTP server\n");
		kputs("  ping <ip>                  Send ICMP echo request\n");
		kputs("  dns <hostname>             Resolve hostname to IP\n");
		kputs("  wifi status|connect|disconnect|mac  WiFi management\n");
		kputs("  http-get <host> [port] [path]  HTTP GET request\n");
		kputs("  fetch <host> <port> [path] [ns:name]  HTTP GET -> object\n");
		return;
	}

	if (anx_strcmp(topic, "system") == 0) {
		kputs("System info, hardware, scheduler:\n");
		kputs("  sysinfo                    System overview\n");
		kputs("  mem stats                  Page allocator statistics\n");
		kputs("  sched status               Show scheduler queue depths\n");
		kputs("  engine register|list       Tool engine registry\n");
		kputs("  memplane admit|show        Memory control plane\n");
		kputs("  cap create|list|validate|install  Capability objects\n");
		kputs("  cell create|run|show|list  Execution cell runtime\n");
		kputs("  vm create|start|stop|list|info  Virtual machine control\n");
		kputs("  pci                        List PCI devices\n");
		kputs("  perf                       Show boot performance profile\n");
		kputs("  reboot                     Reboot the system\n");
		kputs("  halt                       Halt the system\n");
		kputs("  version                    Show kernel version\n");
		kputs("  hwd                        Hardware detection summary\n");
		kputs("  hw-inventory               Show hardware summary\n");
		kputs("  tz <offset>                Set UTC offset (e.g., -7 for PDT)\n");
		kputs("  theme                      Theme selector\n");
		kputs("  fb_info                    Framebuffer geometry (JSON)\n");
		kputs("  mode                       Display mode selection\n");
		return;
	}

	if (anx_strcmp(topic, "workflow") == 0) {
		kputs("Workflow engine:\n");
		kputs("  workflow run|list|show|create  Workflow management\n");
		kputs("  kickstart                  Kickstart provisioning agent\n");
		kputs("  install -i                 Interactive OS installer\n");
		return;
	}

	if (anx_strcmp(topic, "security") == 0) {
		kputs("Credentials and auth:\n");
		kputs("  secret set <name> <value>  Store a credential\n");
		kputs("  secret list                List credentials (no values)\n");
		kputs("  secret show <name>         Show credential metadata\n");
		kputs("  secret fetch <name> <host> <port> [path]  Fetch from HTTP\n");
		kputs("  secret revoke <name>       Revoke a credential\n");
		kputs("  login <user>               Login with password\n");
		kputs("  logout                     End session\n");
		kputs("  useradd <user> <pass>      Create user account\n");
		kputs("  ssh-keygen                 Generate Ed25519 keypair; print public key\n");
		kputs("  ssh-addkey <b64-blob>      Authorize an SSH public key\n");
		return;
	}

	if (anx_strcmp(topic, "shell") == 0) {
		kputs("Shell builtins, pipes, history:\n");
		kputs("  echo <text...>             Print text ($? for return code)\n");
		kputs("  grep [-v] [-i] <pattern>   Filter lines\n");
		kputs("  head [-n N]                First N lines (default 10)\n");
		kputs("  tail [-n N]                Last N lines (default 10)\n");
		kputs("  wc [-l] [-w] [-c]          Count lines, words, chars\n");
		kputs("  sort [-r]                  Sort piped lines (r=reverse)\n");
		kputs("  history                    Show command history\n");
		kputs("  date                       Show current date and time\n");
		kputs("  clear                      Clear terminal output\n");
		kputs("  edit <ns:path>             Open text editor\n");
		kputs("  help [topic]               This help\n");
		return;
	}

	kprintf("help: unknown topic '%s'  (objects|model|network|system|workflow|security|shell)\n",
		topic);
}

static void cmd_version(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	kprintf("Anunix 2026.4.29 (kernel monitor)\n");
}

/* --- Memory commands --- */

static void cmd_mem_stats(void)
{
	uint64_t total, free_pages;
	anx_page_stats(&total, &free_pages);
	kprintf("pages: %u total, %u free, %u used (%u KiB free)\n",
		(unsigned)total, (unsigned)free_pages,
		(unsigned)(total - free_pages),
		(unsigned)(free_pages * 4));
}

/* --- State Object commands --- */

/* Track created objects for the shell */
#define MAX_SHELL_OBJECTS 32
static anx_oid_t shell_oids[MAX_SHELL_OBJECTS];
static uint32_t shell_oid_count;

static const char *obj_type_name(enum anx_object_type t)
{
	switch (t) {
	case ANX_OBJ_BYTE_DATA:		return "byte_data";
	case ANX_OBJ_STRUCTURED_DATA:	return "structured_data";
	case ANX_OBJ_EMBEDDING:		return "embedding";
	case ANX_OBJ_GRAPH_NODE:	return "graph_node";
	case ANX_OBJ_MODEL_OUTPUT:	return "model_output";
	case ANX_OBJ_EXECUTION_TRACE:	return "execution_trace";
	case ANX_OBJ_CAPABILITY:	return "capability";
	case ANX_OBJ_TENSOR:		return "tensor";
	default:			return "unknown";
	}
}

static const char *obj_state_name(enum anx_object_state s)
{
	switch (s) {
	case ANX_OBJ_CREATING:		return "creating";
	case ANX_OBJ_ACTIVE:		return "active";
	case ANX_OBJ_SEALED:		return "sealed";
	case ANX_OBJ_EXPIRED:		return "expired";
	case ANX_OBJ_DELETED:		return "deleted";
	case ANX_OBJ_TOMBSTONE:		return "tombstone";
	default:			return "unknown";
	}
}

static struct anx_state_object *find_obj_by_prefix(const char *prefix)
{
	uint32_t i;
	char buf[37];

	for (i = 0; i < shell_oid_count; i++) {
		anx_uuid_to_string(&shell_oids[i], buf, sizeof(buf));
		if (anx_strncmp(buf, prefix, anx_strlen(prefix)) == 0) {
			return anx_objstore_lookup(&shell_oids[i]);
		}
	}
	return NULL;
}

static void cmd_state(int argc, char **argv)
{
	if (argc < 2) {
		kputs("usage: state <create|show|seal|delete> [args]\n");
		return;
	}

	if (anx_strcmp(argv[1], "create") == 0) {
		struct anx_state_object *obj;
		struct anx_so_create_params params;
		enum anx_object_type type = ANX_OBJ_BYTE_DATA;
		char oid_str[37];
		int ret;

		if (argc >= 3) {
			if (anx_strcmp(argv[2], "structured") == 0)
				type = ANX_OBJ_STRUCTURED_DATA;
			else if (anx_strcmp(argv[2], "embedding") == 0)
				type = ANX_OBJ_EMBEDDING;
			else if (anx_strcmp(argv[2], "graph") == 0)
				type = ANX_OBJ_GRAPH_NODE;
		}

		anx_memset(&params, 0, sizeof(params));
		params.object_type = type;

		ret = anx_so_create(&params, &obj);
		if (ret != ANX_OK) {
			kprintf("error: create failed (%d)\n", ret);
			return;
		}

		if (shell_oid_count < MAX_SHELL_OBJECTS)
			shell_oids[shell_oid_count++] = obj->oid;

		anx_uuid_to_string(&obj->oid, oid_str, sizeof(oid_str));
		kprintf("created %s object: %s\n", obj_type_name(type), oid_str);
		anx_objstore_release(obj);

	} else if (anx_strcmp(argv[1], "show") == 0) {
		struct anx_state_object *obj;

		if (argc < 3) {
			/* List all known objects */
			uint32_t i;
			char buf[37];
			kprintf("known objects (%u):\n", shell_oid_count);
			for (i = 0; i < shell_oid_count; i++) {
				obj = anx_objstore_lookup(&shell_oids[i]);
				if (obj) {
					anx_uuid_to_string(&obj->oid, buf,
							   sizeof(buf));
					kprintf("  %s  %s  %s  v%u  %u bytes\n",
						buf,
						obj_type_name(obj->object_type),
						obj_state_name(obj->state),
						(unsigned)obj->version,
						(unsigned)obj->payload_size);
					anx_objstore_release(obj);
				}
			}
			return;
		}

		obj = find_obj_by_prefix(argv[2]);
		if (!obj) {
			kprintf("error: no object matching '%s'\n", argv[2]);
			return;
		}
		{
			char buf[37];
			anx_uuid_to_string(&obj->oid, buf, sizeof(buf));
			kprintf("oid:     %s\n", buf);
			kprintf("type:    %s\n", obj_type_name(obj->object_type));
			kprintf("state:   %s\n", obj_state_name(obj->state));
			kprintf("version: %u\n", (unsigned)obj->version);
			kprintf("payload: %u bytes\n", (unsigned)obj->payload_size);
			kprintf("refcount: %u\n", (unsigned)obj->refcount);
		}
		anx_objstore_release(obj);

	} else if (anx_strcmp(argv[1], "seal") == 0) {
		struct anx_state_object *obj;
		int ret;

		if (argc < 3) {
			kputs("usage: state seal <oid-prefix>\n");
			return;
		}
		obj = find_obj_by_prefix(argv[2]);
		if (!obj) {
			kprintf("error: no object matching '%s'\n", argv[2]);
			return;
		}
		ret = anx_so_seal(&obj->oid);
		anx_objstore_release(obj);
		if (ret == ANX_OK)
			kputs("sealed\n");
		else
			kprintf("error: seal failed (%d)\n", ret);

	} else if (anx_strcmp(argv[1], "delete") == 0) {
		struct anx_state_object *obj;
		int ret;

		if (argc < 3) {
			kputs("usage: state delete <oid-prefix>\n");
			return;
		}
		obj = find_obj_by_prefix(argv[2]);
		if (!obj) {
			kprintf("error: no object matching '%s'\n", argv[2]);
			return;
		}
		ret = anx_so_delete(&obj->oid, false);
		anx_objstore_release(obj);
		if (ret == ANX_OK)
			kputs("deleted\n");
		else
			kprintf("error: delete failed (%d)\n", ret);
	} else {
		kputs("usage: state <create|show|seal|delete>\n");
	}
}

/* --- Cell commands --- */

#define MAX_SHELL_CELLS 16
static anx_cid_t shell_cids[MAX_SHELL_CELLS];
static uint32_t shell_cid_count;

static const char *cell_status_name(enum anx_cell_status s)
{
	switch (s) {
	case ANX_CELL_CREATED:		return "created";
	case ANX_CELL_ADMITTED:		return "admitted";
	case ANX_CELL_PLANNING:		return "planning";
	case ANX_CELL_PLANNED:		return "planned";
	case ANX_CELL_QUEUED:		return "queued";
	case ANX_CELL_RUNNING:		return "running";
	case ANX_CELL_WAITING:		return "waiting";
	case ANX_CELL_VALIDATING:	return "validating";
	case ANX_CELL_COMMITTING:	return "committing";
	case ANX_CELL_COMPLETED:	return "completed";
	case ANX_CELL_FAILED:		return "failed";
	case ANX_CELL_CANCELLED:	return "cancelled";
	case ANX_CELL_COMPENSATING:	return "compensating";
	case ANX_CELL_COMPENSATED:	return "compensated";
	default:			return "unknown";
	}
}

static struct anx_cell *find_cell_by_prefix(const char *prefix)
{
	uint32_t i;
	char buf[37];

	for (i = 0; i < shell_cid_count; i++) {
		anx_uuid_to_string(&shell_cids[i], buf, sizeof(buf));
		if (anx_strncmp(buf, prefix, anx_strlen(prefix)) == 0)
			return anx_cell_store_lookup(&shell_cids[i]);
	}
	return NULL;
}

static void cmd_cell(int argc, char **argv)
{
	if (argc < 2) {
		kputs("usage: cell <create|run|show> [args]\n");
		return;
	}

	if (anx_strcmp(argv[1], "create") == 0) {
		struct anx_cell *cell;
		struct anx_cell_intent intent;
		char cid_str[37];
		int ret;

		anx_memset(&intent, 0, sizeof(intent));
		if (argc >= 3)
			anx_strlcpy(intent.name, argv[2],
				     sizeof(intent.name));
		else
			anx_strlcpy(intent.name, "shell_task",
				     sizeof(intent.name));

		ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell);
		if (ret != ANX_OK) {
			kprintf("error: create failed (%d)\n", ret);
			return;
		}

		if (shell_cid_count < MAX_SHELL_CELLS)
			shell_cids[shell_cid_count++] = cell->cid;

		anx_uuid_to_string(&cell->cid, cid_str, sizeof(cid_str));
		kprintf("created cell: %s (%s)\n", cid_str, intent.name);
		anx_cell_store_release(cell);

	} else if (anx_strcmp(argv[1], "run") == 0) {
		struct anx_cell *cell;
		int ret;

		if (argc < 3) {
			kputs("usage: cell run <cid-prefix>\n");
			return;
		}
		cell = find_cell_by_prefix(argv[2]);
		if (!cell) {
			kprintf("error: no cell matching '%s'\n", argv[2]);
			return;
		}
		kputs("running cell...\n");
		ret = anx_cell_run(cell);
		if (ret == ANX_OK)
			kprintf("completed (status: %s)\n",
				cell_status_name(cell->status));
		else
			kprintf("failed: %s (error %d)\n",
				cell->error_msg[0] ? cell->error_msg : "unknown",
				ret);
		anx_cell_store_release(cell);

	} else if (anx_strcmp(argv[1], "show") == 0) {
		if (argc < 3) {
			uint32_t i;
			char buf[37];
			kprintf("known cells (%u):\n", shell_cid_count);
			for (i = 0; i < shell_cid_count; i++) {
				struct anx_cell *c;
				c = anx_cell_store_lookup(&shell_cids[i]);
				if (c) {
					anx_uuid_to_string(&c->cid, buf,
							   sizeof(buf));
					kprintf("  %s  %s  %s\n",
						buf,
						c->intent.name,
						cell_status_name(c->status));
					anx_cell_store_release(c);
				}
			}
			return;
		}
		{
			struct anx_cell *cell = find_cell_by_prefix(argv[2]);
			char buf[37];
			if (!cell) {
				kprintf("error: no cell matching '%s'\n",
					argv[2]);
				return;
			}
			anx_uuid_to_string(&cell->cid, buf, sizeof(buf));
			kprintf("cid:      %s\n", buf);
			kprintf("intent:   %s\n", cell->intent.name);
			kprintf("status:   %s\n",
				cell_status_name(cell->status));
			kprintf("attempts: %u\n", cell->attempt_count);
			kprintf("children: %u\n", cell->child_count);
			kprintf("outputs:  %u\n", cell->output_count);
			if (cell->error_code)
				kprintf("error:    %d (%s)\n",
					cell->error_code, cell->error_msg);
			anx_cell_store_release(cell);
		}
	} else {
		kputs("usage: cell <create|run|show>\n");
	}
}

/* --- Memory plane commands --- */

static void cmd_memplane(int argc, char **argv)
{
	if (argc < 2) {
		kputs("usage: memplane <admit|show> [args]\n");
		return;
	}

	if (anx_strcmp(argv[1], "admit") == 0) {
		struct anx_state_object *obj;
		struct anx_mem_entry *entry;
		int ret;

		if (argc < 3) {
			kputs("usage: memplane admit <oid-prefix>\n");
			return;
		}
		obj = find_obj_by_prefix(argv[2]);
		if (!obj) {
			kprintf("error: no object matching '%s'\n", argv[2]);
			return;
		}
		ret = anx_memplane_admit(&obj->oid,
					 ANX_ADMIT_RETRIEVAL_CANDIDATE,
					 &entry);
		anx_objstore_release(obj);
		if (ret == ANX_OK)
			kprintf("admitted to memory plane (tiers: L2+L3)\n");
		else
			kprintf("error: admit failed (%d)\n", ret);

	} else if (anx_strcmp(argv[1], "show") == 0) {
		struct anx_state_object *obj;
		struct anx_mem_entry *entry;

		if (argc < 3) {
			kputs("usage: memplane show <oid-prefix>\n");
			return;
		}
		obj = find_obj_by_prefix(argv[2]);
		if (!obj) {
			kprintf("error: no object matching '%s'\n", argv[2]);
			return;
		}
		entry = anx_memplane_lookup(&obj->oid);
		anx_objstore_release(obj);
		if (!entry) {
			kputs("not in memory plane\n");
			return;
		}
		kprintf("tiers:         L0=%d L1=%d L2=%d L3=%d L4=%d L5=%d\n",
			anx_mem_in_tier(entry, ANX_MEM_L0),
			anx_mem_in_tier(entry, ANX_MEM_L1),
			anx_mem_in_tier(entry, ANX_MEM_L2),
			anx_mem_in_tier(entry, ANX_MEM_L3),
			anx_mem_in_tier(entry, ANX_MEM_L4),
			anx_mem_in_tier(entry, ANX_MEM_L5));
		kprintf("confidence:    %u%%\n", entry->confidence_pct);
		kprintf("contradictions: %u\n", entry->contradiction_count);
		kprintf("access count:  %u\n", entry->access_count);
		kprintf("decay score:   %u\n", entry->decay_score);
		anx_memplane_release(entry);
	} else {
		kputs("usage: memplane <admit|show>\n");
	}
}

/* --- Engine commands --- */

#define MAX_SHELL_ENGINES 16
static anx_eid_t shell_eids[MAX_SHELL_ENGINES];
static uint32_t shell_eid_count;

static const char *engine_class_name(enum anx_engine_class c)
{
	switch (c) {
	case ANX_ENGINE_DETERMINISTIC_TOOL:	return "deterministic_tool";
	case ANX_ENGINE_LOCAL_MODEL:		return "local_model";
	case ANX_ENGINE_REMOTE_MODEL:		return "remote_model";
	case ANX_ENGINE_RETRIEVAL_SERVICE:	return "retrieval_service";
	case ANX_ENGINE_GRAPH_SERVICE:		return "graph_service";
	case ANX_ENGINE_VALIDATION_SERVICE:	return "validation_service";
	case ANX_ENGINE_EXECUTION_SERVICE:	return "execution_service";
	case ANX_ENGINE_DEVICE_SERVICE:		return "device_service";
	case ANX_ENGINE_INSTALLED_CAPABILITY:	return "installed_capability";
	default:				return "unknown";
	}
}

static void cmd_engine(int argc, char **argv)
{
	if (argc < 2) {
		kputs("usage: engine <register|list> [args]\n");
		return;
	}

	if (anx_strcmp(argv[1], "register") == 0) {
		struct anx_engine *eng;
		char eid_str[37];
		const char *name = argc >= 3 ? argv[2] : "shell_engine";
		uint32_t caps = ANX_CAP_TOOL_EXECUTION;
		int ret;

		ret = anx_engine_register(name,
					  ANX_ENGINE_DETERMINISTIC_TOOL,
					  caps, &eng);
		if (ret != ANX_OK) {
			kprintf("error: register failed (%d)\n", ret);
			return;
		}

		if (shell_eid_count < MAX_SHELL_ENGINES)
			shell_eids[shell_eid_count++] = eng->eid;

		anx_uuid_to_string(&eng->eid, eid_str, sizeof(eid_str));
		kprintf("registered engine: %s (%s)\n", name, eid_str);

	} else if (anx_strcmp(argv[1], "list") == 0) {
		uint32_t i;
		char buf[37];
		kprintf("registered engines (%u):\n", shell_eid_count);
		for (i = 0; i < shell_eid_count; i++) {
			struct anx_engine *eng;
			eng = anx_engine_lookup(&shell_eids[i]);
			if (eng) {
				anx_uuid_to_string(&eng->eid, buf,
						   sizeof(buf));
				kprintf("  %s  %s  %s\n",
					buf, eng->name,
					engine_class_name(eng->engine_class));
			}
		}
	} else {
		kputs("usage: engine <register|list>\n");
	}
}

/* --- Capability commands --- */

#define MAX_SHELL_CAPS 8
static anx_oid_t shell_cap_oids[MAX_SHELL_CAPS];
static uint32_t shell_cap_count;

static const char *cap_status_name(enum anx_cap_status s)
{
	switch (s) {
	case ANX_CAP_DRAFT:		return "draft";
	case ANX_CAP_VALIDATING:	return "validating";
	case ANX_CAP_VALIDATED:		return "validated";
	case ANX_CAP_INSTALLED:		return "installed";
	case ANX_CAP_SUSPENDED:		return "suspended";
	case ANX_CAP_SUPERSEDED:	return "superseded";
	case ANX_CAP_RETIRED:		return "retired";
	default:			return "unknown";
	}
}

static void cmd_cap(int argc, char **argv)
{
	if (argc < 2) {
		kputs("usage: cap <create|list> [args]\n");
		return;
	}

	if (anx_strcmp(argv[1], "create") == 0) {
		struct anx_capability *cap;
		char oid_str[37];
		const char *name = argc >= 3 ? argv[2] : "shell_cap";
		int ret;

		ret = anx_cap_create(name, "1.0", &cap);
		if (ret != ANX_OK) {
			kprintf("error: create failed (%d)\n", ret);
			return;
		}

		if (shell_cap_count < MAX_SHELL_CAPS)
			shell_cap_oids[shell_cap_count++] = cap->cap_oid;

		anx_uuid_to_string(&cap->cap_oid, oid_str, sizeof(oid_str));
		kprintf("created capability: %s v%s (%s)\n",
			cap->name, cap->version, oid_str);

	} else if (anx_strcmp(argv[1], "list") == 0) {
		uint32_t i;
		char buf[37];
		kprintf("capabilities (%u):\n", shell_cap_count);
		for (i = 0; i < shell_cap_count; i++) {
			struct anx_capability *cap;
			cap = anx_cap_lookup(&shell_cap_oids[i]);
			if (cap) {
				anx_uuid_to_string(&cap->cap_oid, buf,
						   sizeof(buf));
				kprintf("  [%u] %s  %s v%s  %s  score=%u\n",
					i, buf, cap->name, cap->version,
					cap_status_name(cap->status),
					cap->validation_score);
			}
		}
	} else if (anx_strcmp(argv[1], "validate") == 0) {
		struct anx_capability *cap;
		uint32_t idx;
		int ret;

		if (argc < 3) {
			kputs("usage: cap validate <index>\n");
			return;
		}
		idx = (uint32_t)anx_strtoul(argv[2], NULL, 10);
		if (idx >= shell_cap_count) {
			kprintf("error: index %u out of range\n", idx);
			return;
		}
		cap = anx_cap_lookup(&shell_cap_oids[idx]);
		if (!cap) {
			kputs("error: capability not found\n");
			return;
		}
		ret = anx_cap_validate(cap);
		if (ret != ANX_OK)
			kprintf("validate failed (%d): score=%u\n",
				ret, cap->validation_score);

	} else if (anx_strcmp(argv[1], "install") == 0) {
		struct anx_capability *cap;
		uint32_t idx;
		int ret;

		if (argc < 3) {
			kputs("usage: cap install <index>\n");
			return;
		}
		idx = (uint32_t)anx_strtoul(argv[2], NULL, 10);
		if (idx >= shell_cap_count) {
			kprintf("error: index %u out of range\n", idx);
			return;
		}
		cap = anx_cap_lookup(&shell_cap_oids[idx]);
		if (!cap) {
			kputs("error: capability not found\n");
			return;
		}
		ret = anx_cap_install(cap);
		if (ret != ANX_OK)
			kprintf("install failed (%d)\n", ret);
		else
			kprintf("installed: %s v%s\n", cap->name, cap->version);

	} else {
		kputs("usage: cap <create|list|validate|install> [args]\n");
	}
}

/* --- JEPA commands --- */

static void cmd_jepa(int argc, char **argv)
{
	if (argc < 2) {
		kputs("usage: jepa <status|world|traj> [args]\n");
		return;
	}

	/* jepa status */
	if (anx_strcmp(argv[1], "status") == 0) {
		static const char *const status_names[] = {
			"uninitialized", "initializing", "ready",
			"training", "degraded", "unavailable",
		};
		enum anx_jepa_status st = anx_jepa_status_get();
		const char *st_name = ((unsigned)st < 6) ? status_names[st] : "?";

		kprintf("jepa: status=%s available=%s train_steps=%u\n",
			st_name,
			anx_jepa_available() ? "yes" : "no",
			anx_jepa_get_train_step_count());

		if (anx_jepa_available()) {
			const struct anx_jepa_world_profile *w =
				anx_jepa_world_get_active();
			if (w)
				kprintf("jepa: world=%s obs_dim=%u "
					"latent_dim=%u actions=%u\n",
					w->uri, w->arch.obs_dim,
					w->arch.latent_dim, w->action_count);
		}
		return;
	}

	/* jepa world [list|active|set <uri>] */
	if (anx_strcmp(argv[1], "world") == 0) {
		if (argc < 3 || anx_strcmp(argv[2], "list") == 0) {
			const char *uris[ANX_JEPA_MAX_WORLDS];
			uint32_t found = 0, i;

			anx_jepa_world_list(uris, ANX_JEPA_MAX_WORLDS, &found);
			kprintf("jepa: %u registered world(s):\n", found);
			for (i = 0; i < found; i++)
				kprintf("  %s\n", uris[i]);
			return;
		}

		if (anx_strcmp(argv[2], "active") == 0) {
			const struct anx_jepa_world_profile *w =
				anx_jepa_world_get_active();
			if (!w) {
				kputs("jepa: no active world\n");
				return;
			}
			kprintf("jepa: active world=%s\n", w->uri);
			kprintf("  display_name=%s\n", w->display_name);
			kprintf("  obs_dim=%u latent_dim=%u "
				"action_count=%u\n",
				w->arch.obs_dim, w->arch.latent_dim,
				w->action_count);
			kprintf("  collect_obs=%s\n",
				w->collect_obs ? "registered" : "(stub)");
			return;
		}

		if (anx_strcmp(argv[2], "set") == 0) {
			int ret;

			if (argc < 4) {
				kputs("usage: jepa world set <uri>\n");
				return;
			}
			ret = anx_jepa_world_set_active(argv[3]);
			if (ret != ANX_OK)
				kprintf("jepa: world set failed (%d)\n", ret);
			else
				kprintf("jepa: active world → %s\n", argv[3]);
			return;
		}

		kputs("usage: jepa world [list|active|set <uri>]\n");
		return;
	}

	/* jepa traj [count|reset|dump] */
	if (anx_strcmp(argv[1], "traj") == 0) {
		const char *sub = (argc >= 3) ? argv[2] : "count";

		if (anx_strcmp(sub, "reset") == 0) {
			anx_jepa_traj_reset();
			kputs("jepa: trajectory ring buffer cleared\n");
			return;
		}

		if (anx_strcmp(sub, "count") == 0 ||
		    anx_strcmp(sub, "dump") == 0) {
			uint8_t  *buf;
			uint32_t  written = 0;
			uint32_t  buf_size = 32768;
			int ret;

			buf = (uint8_t *)anx_alloc(buf_size);
			if (!buf) {
				kputs("jepa: out of memory\n");
				return;
			}

			ret = anx_jepa_export_trajectory(buf, buf_size,
							  &written);

			if (ret == ANX_ENOENT) {
				kputs("jepa: trajectory ring buffer is empty\n");
				anx_free(buf);
				return;
			}
			if (ret != ANX_OK) {
				kprintf("jepa: export failed (%d)\n", ret);
				anx_free(buf);
				return;
			}

			if (anx_strcmp(sub, "count") == 0) {
				const struct anx_jepa_traj_header *h =
					(const struct anx_jepa_traj_header *)buf;
				kprintf("jepa: trajectory entries=%u "
					"(%u bytes)\n",
					h->entry_count, written);
				anx_free(buf);
				return;
			}

			/* dump: store as state object, print OID */
			{
				struct anx_so_create_params params;
				struct anx_state_object     *obj;

				anx_memset(&params, 0, sizeof(params));
				params.object_type  = ANX_OBJ_BYTE_DATA;
				params.schema_uri   = "anx:schema/jepa-trajectory/v1";
				params.payload      = buf;
				params.payload_size = written;

				ret = anx_so_create(&params, &obj);
				anx_free(buf);
				if (ret != ANX_OK) {
					kprintf("jepa: store failed (%d)\n",
						ret);
					return;
				}
				kprintf("jepa: trajectory stored: "
					"%016llx%016llx (%u bytes)\n",
					(unsigned long long)obj->oid.hi,
					(unsigned long long)obj->oid.lo,
					written);
				anx_objstore_release(obj);
			}
			return;
		}

		kputs("usage: jepa traj [count|reset|dump]\n");
		return;
	}

	kputs("usage: jepa <status|world|traj> [args]\n");
}

/* --- RLM commands --- */

#define MAX_SHELL_ROLLOUTS 8
static struct anx_rlm_rollout *shell_rollouts[MAX_SHELL_ROLLOUTS];
static uint32_t shell_rollout_count;

static void cmd_rlm(int argc, char **argv)
{
	if (argc < 2) {
		kputs("usage: rlm <run|pal> [args]\n");
		return;
	}

	if (anx_strcmp(argv[1], "run") == 0) {
		const char *text = argc >= 3 ? argv[2] : "hello";
		struct anx_so_create_params params;
		struct anx_state_object *obj;
		struct anx_rlm_config cfg;
		struct anx_rlm_rollout *r;
		anx_oid_t prompt_oid;
		int ret;

		anx_memset(&params, 0, sizeof(params));
		params.object_type  = ANX_OBJ_BYTE_DATA;
		params.payload      = text;
		params.payload_size = (uint32_t)anx_strlen(text);
		ret = anx_so_create(&params, &obj);
		if (ret != ANX_OK) {
			kprintf("error: prompt create failed (%d)\n", ret);
			return;
		}
		prompt_oid = obj->oid;
		anx_objstore_release(obj);

		anx_rlm_config_default(&cfg);
		cfg.max_steps = 4;
		cfg.admit_responses = false;

		ret = anx_rlm_rollout_create(&prompt_oid, &cfg, &r);
		if (ret != ANX_OK) {
			kprintf("error: rollout create failed (%d)\n", ret);
			return;
		}

		ret = anx_rlm_rollout_run(r);
		kprintf("rollout [%u]: status=%d steps=%u in=%d out=%d\n",
			shell_rollout_count, r->status, r->step_count,
			r->total_input_tokens, r->total_output_tokens);

		if (shell_rollout_count < MAX_SHELL_ROLLOUTS)
			shell_rollouts[shell_rollout_count++] = r;

	} else if (anx_strcmp(argv[1], "pal") == 0) {
		struct anx_rlm_rollout *r;
		const char *world;
		uint32_t idx, action_id;
		int32_t score;
		int ret;

		if (argc < 4) {
			kputs("usage: rlm pal <index> <world-uri> [score] [action-id]\n");
			return;
		}
		idx = (uint32_t)anx_strtoul(argv[2], NULL, 10);
		if (idx >= shell_rollout_count) {
			kprintf("error: index %u out of range\n", idx);
			return;
		}
		r = shell_rollouts[idx];
		world = argv[3];
		score = argc >= 5 ? (int32_t)anx_strtoul(argv[4], NULL, 10) : 50;
		action_id = argc >= 6 ? (uint32_t)anx_strtoul(argv[5], NULL, 10) : 0;

		anx_rlm_rollout_set_score(r, score);
		ret = anx_rlm_pal_feedback(r, world, action_id);
		if (ret != ANX_OK)
			kprintf("pal feedback failed (%d)\n", ret);
		else
			kprintf("pal updated: world=%s action=%u score=%d\n",
				world, action_id, (int)score);

	} else {
		kputs("usage: rlm <run|pal> [args]\n");
	}
}

/* --- Scheduler and network commands --- */

static void cmd_sched_status(void)
{
	kprintf("scheduler queues:\n");
	kprintf("  interactive:      %u\n",
		anx_sched_queue_depth(ANX_QUEUE_INTERACTIVE));
	kprintf("  background:       %u\n",
		anx_sched_queue_depth(ANX_QUEUE_BACKGROUND));
	kprintf("  latency_sensitive: %u\n",
		anx_sched_queue_depth(ANX_QUEUE_LATENCY_SENSITIVE));
	kprintf("  batch:            %u\n",
		anx_sched_queue_depth(ANX_QUEUE_BATCH));
	kprintf("  validation:       %u\n",
		anx_sched_queue_depth(ANX_QUEUE_VALIDATION));
	kprintf("  replication:      %u\n",
		anx_sched_queue_depth(ANX_QUEUE_REPLICATION));
}

static void cmd_net_status(void)
{
	struct anx_net_node *local = anx_netplane_local_node();
	char nid_str[37];

	if (!local) {
		kputs("network plane not initialized\n");
		return;
	}
	anx_uuid_to_string(&local->nid, nid_str, sizeof(nid_str));
	kprintf("local node: %s\n", local->name);
	kprintf("  nid:    %s\n", nid_str);
	kprintf("  type:   personal\n");
	kprintf("  trust:  local\n");
	kprintf("  status: online\n");
}

/* --- Network helpers --- */

static uint32_t parse_ip(const char *s)
{
	uint32_t a = 0, b = 0, c = 0, d = 0;
	int field = 0;
	uint32_t val = 0;

	while (*s) {
		if (*s >= '0' && *s <= '9') {
			val = val * 10 + (uint32_t)(*s - '0');
		} else if (*s == '.') {
			if (field == 0) a = val;
			else if (field == 1) b = val;
			else if (field == 2) c = val;
			field++;
			val = 0;
		}
		s++;
	}
	d = val;
	return ANX_IP4(a, b, c, d);
}

static uint16_t parse_port(const char *s)
{
	uint16_t val = 0;

	while (*s >= '0' && *s <= '9')
		val = val * 10 + (uint16_t)(*s++ - '0');
	return val;
}

static void cmd_api(int argc, char **argv)
{
	char key_buf[256];
	uint32_t key_len;
	char headers[512];
	uint32_t hdr_off = 0;
	struct anx_http_response resp;
	const char *host;
	uint16_t port;
	const char *path;
	int ret;

	if (argc < 4) {
		kputs("usage: api <cred-name> <host> <port> [path]\n");
		kputs("  reads credential, injects as x-api-key header\n");
		return;
	}

	/* Read the credential */
	ret = anx_credential_read(argv[1], key_buf, sizeof(key_buf) - 1,
				   &key_len);
	if (ret != ANX_OK) {
		kprintf("api: credential '%s' not found (%d)\n", argv[1], ret);
		return;
	}
	key_buf[key_len] = '\0';

	/* Build auth headers (pre-formatted with \r\n) */
	hdr_off = 0;
	anx_memcpy(headers + hdr_off, "x-api-key: ", 11);
	hdr_off += 11;
	anx_memcpy(headers + hdr_off, key_buf, key_len);
	hdr_off += key_len;
	anx_memcpy(headers + hdr_off, "\r\nanthropic-version: 2023-06-01\r\n", 33);
	hdr_off += 33;
	headers[hdr_off] = '\0';

	/* Zero the key from our local buffer immediately */
	anx_memset(key_buf, 0, sizeof(key_buf));

	host = argv[2];
	port = parse_port(argv[3]);
	path = argc >= 5 ? argv[4] : "/";

	kprintf("API %s:%u%s (credential: %s)\n",
		host, (uint32_t)port, path, argv[1]);

	ret = anx_http_get_authed(host, port, path, headers, &resp);

	/* Zero headers containing the key */
	anx_memset(headers, 0, sizeof(headers));

	if (ret != ANX_OK) {
		kprintf("api: request failed (%d)\n", ret);
		return;
	}

	kprintf("HTTP %d, %u bytes\n", resp.status_code, resp.body_len);
	if (resp.body && resp.body_len > 0) {
		uint32_t show = resp.body_len;

		if (show > 1024)
			show = 1024;
		resp.body[show] = '\0';
		kprintf("%s\n", resp.body);
		if (resp.body_len > 1024)
			kputs("... (truncated)\n");
	}
	anx_http_response_free(&resp);
}

static void cmd_http_get(int argc, char **argv)
{
	struct anx_http_response resp;
	const char *host;
	uint16_t port = 80;
	const char *path = "/";
	int ret;

	if (argc < 2) {
		kputs("usage: http-get <host> [port] [path]\n");
		return;
	}

	host = argv[1];
	if (argc >= 3)
		port = parse_port(argv[2]);
	if (argc >= 4)
		path = argv[3];

	kprintf("GET http://%s:%u%s\n", host, (uint32_t)port, path);

	ret = anx_http_get(host, port, path, &resp);
	if (ret != ANX_OK) {
		kprintf("http-get: failed (%d)\n", ret);
		return;
	}

	kprintf("HTTP %d, %u bytes\n", resp.status_code, resp.body_len);
	if (resp.body && resp.body_len > 0) {
		uint32_t show = resp.body_len;

		if (show > 512)
			show = 512;
		resp.body[show] = '\0';
		kprintf("%s\n", resp.body);
		if (resp.body_len > 512)
			kputs("... (truncated)\n");
	}
	anx_http_response_free(&resp);
}

static void cmd_dns(const char *hostname)
{
	uint32_t ip;
	int ret;

	kprintf("resolving %s...\n", hostname);
	ret = anx_dns_resolve(hostname, &ip);
	if (ret == ANX_OK) {
		kprintf("%s -> %u.%u.%u.%u\n", hostname,
			(ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
			(ip >> 8) & 0xFF, ip & 0xFF);
	} else {
		kprintf("dns: failed (%d)\n", ret);
	}
}

static void cmd_ping(const char *target)
{
	uint32_t ip = parse_ip(target);

	kprintf("ping %u.%u.%u.%u...\n",
		(ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
		(ip >> 8) & 0xFF, ip & 0xFF);

	if (anx_icmp_ping(ip, 1) == ANX_OK)
		kputs("ping sent\n");
	else
		kputs("ping failed\n");
}

/* --- Secret commands (RFC-0008) --- */

static const char *cred_type_name(enum anx_credential_type t)
{
	switch (t) {
	case ANX_CRED_API_KEY:		return "api_key";
	case ANX_CRED_TOKEN:		return "token";
	case ANX_CRED_CERTIFICATE:	return "certificate";
	case ANX_CRED_PRIVATE_KEY:	return "private_key";
	case ANX_CRED_PASSWORD:		return "password";
	case ANX_CRED_OPAQUE:		return "opaque";
	default:			return "unknown";
	}
}

static void cmd_secret(int argc, char **argv)
{
	if (argc < 2) {
		kputs("usage: secret <set|list|show|revoke> [args]\n");
		return;
	}

	if (anx_strcmp(argv[1], "set") == 0) {
		int ret;

		if (argc < 4) {
			kputs("usage: secret set <name> <value>\n");
			return;
		}
		ret = anx_credential_create(argv[2], ANX_CRED_API_KEY,
					     argv[3],
					     (uint32_t)anx_strlen(argv[3]));
		if (ret == ANX_EEXIST)
			kprintf("secret: '%s' already exists (use rotate)\n",
				argv[2]);
		else if (ret != ANX_OK)
			kprintf("secret: create failed (%d)\n", ret);

		/* Zero the value in the command buffer */
		anx_memset(argv[3], 0, anx_strlen(argv[3]));

	} else if (anx_strcmp(argv[1], "list") == 0) {
		struct anx_credential_info entries[16];
		uint32_t count = 0;
		uint32_t i;

		anx_credential_list(entries, 16, &count);
		if (count == 0) {
			kputs("(no credentials stored)\n");
			return;
		}
		for (i = 0; i < count; i++) {
			kprintf("  %s  %s  %u bytes  %u accesses\n",
				entries[i].name,
				cred_type_name(entries[i].cred_type),
				entries[i].secret_len,
				entries[i].access_count);
		}

	} else if (anx_strcmp(argv[1], "show") == 0) {
		struct anx_credential_info info;
		int ret;

		if (argc < 3) {
			kputs("usage: secret show <name>\n");
			return;
		}
		ret = anx_credential_info(argv[2], &info);
		if (ret != ANX_OK) {
			kprintf("secret: '%s' not found\n", argv[2]);
			return;
		}
		kprintf("  name:     %s\n", info.name);
		kprintf("  type:     %s\n", cred_type_name(info.cred_type));
		kprintf("  size:     %u bytes\n", info.secret_len);
		kprintf("  accesses: %u\n", info.access_count);
		kputs("  payload:  [REDACTED]\n");

	} else if (anx_strcmp(argv[1], "rotate") == 0) {
		int ret;

		if (argc < 4) {
			kputs("usage: secret rotate <name> <new-value>\n");
			return;
		}
		ret = anx_credential_rotate(argv[2], argv[3],
					     (uint32_t)anx_strlen(argv[3]));
		if (ret != ANX_OK)
			kprintf("secret: rotate failed (%d)\n", ret);
		anx_memset(argv[3], 0, anx_strlen(argv[3]));

	} else if (anx_strcmp(argv[1], "fetch") == 0) {
		struct anx_http_response resp;
		int ret;

		if (argc < 5) {
			kputs("usage: secret fetch <name> <host> <port> [path]\n");
			return;
		}
		{
			const char *host = argv[3];
			uint16_t port = parse_port(argv[4]);
			const char *path = argc >= 6 ? argv[5] : "/";

			kprintf("fetching credential '%s' from %s:%u%s...\n",
				argv[2], host, (uint32_t)port, path);
			ret = anx_http_get(host, port, path, &resp);
		}
		if (ret != ANX_OK) {
			kprintf("secret fetch: request failed (%d)\n", ret);
			return;
		}
		if (resp.status_code != 200) {
			kprintf("secret fetch: HTTP %d\n", resp.status_code);
			anx_http_response_free(&resp);
			return;
		}
		if (resp.body && resp.body_len > 0) {
			/* Trim trailing whitespace/newlines */
			while (resp.body_len > 0 &&
			       (resp.body[resp.body_len - 1] == '\n' ||
				resp.body[resp.body_len - 1] == '\r' ||
				resp.body[resp.body_len - 1] == ' '))
				resp.body_len--;

			ret = anx_credential_create(argv[2],
						     ANX_CRED_API_KEY,
						     resp.body,
						     resp.body_len);
			if (ret == ANX_OK)
				kprintf("credential: %s fetched (%u bytes)\n",
					argv[2], resp.body_len);
			else
				kprintf("secret fetch: store failed (%d)\n",
					ret);
		} else {
			kputs("secret fetch: empty response\n");
		}
		anx_http_response_free(&resp);

	} else if (anx_strcmp(argv[1], "revoke") == 0) {
		int ret;

		if (argc < 3) {
			kputs("usage: secret revoke <name>\n");
			return;
		}
		ret = anx_credential_revoke(argv[2]);
		if (ret != ANX_OK)
			kprintf("secret: revoke failed (%d)\n", ret);

	} else {
		kputs("usage: secret <set|list|show|rotate|revoke>\n");
	}
}

/* --- Model commands --- */

static void execute_line(const char *input);	/* forward */

static void cmd_model_init(int argc, char **argv)
{
	struct anx_model_endpoint ep;

	if (argc < 4) {
		kputs("usage: model-init <cred-name> <host> <port>\n");
		return;
	}
	ep.cred_name = argv[1];
	ep.host = argv[2];
	ep.port = parse_port(argv[3]);
	anx_model_client_init(&ep);
}

static void cmd_ask(int argc, char **argv)
{
	struct anx_model_request req;
	struct anx_model_response resp;
	char message[1024];
	uint32_t off = 0;
	int i, ret;

	if (argc < 2) {
		kputs("usage: ask <message...>\n");
		return;
	}

	if (!anx_model_client_ready()) {
		kputs("model not configured (use 'model-init' first)\n");
		return;
	}

	/* Reconstruct message from args */
	for (i = 1; i < argc && off < sizeof(message) - 2; i++) {
		uint32_t len = (uint32_t)anx_strlen(argv[i]);

		if (i > 1 && off < sizeof(message) - 1)
			message[off++] = ' ';
		if (off + len >= sizeof(message) - 1)
			len = (uint32_t)(sizeof(message) - 1 - off);
		anx_memcpy(message + off, argv[i], len);
		off += len;
	}
	message[off] = '\0';

	req.model = "claude-sonnet-4-6";

	/* Allow model override: 'ask -m model-id message...' */
	if (argc >= 4 && anx_strcmp(argv[1], "-m") == 0) {
		req.model = argv[2];
		/* Rebuild message from argv[3+] */
		off = 0;
		for (i = 3; i < argc && off < sizeof(message) - 2; i++) {
			uint32_t len = (uint32_t)anx_strlen(argv[i]);

			if (i > 3 && off < sizeof(message) - 1)
				message[off++] = ' ';
			if (off + len >= sizeof(message) - 1)
				len = (uint32_t)(sizeof(message) - 1 - off);
			anx_memcpy(message + off, argv[i], len);
			off += len;
		}
		message[off] = '\0';
	}
	req.system = "You are an AI assistant running inside Anunix, "
		     "an AI-native operating system. Be concise.";
	req.user_message = message;
	req.max_tokens = 1024;

	kputs("thinking...\n");
	ret = anx_model_call(&req, &resp);

	if (ret != ANX_OK) {
		kprintf("ask: failed (%d)\n", ret);
		return;
	}

	if (resp.content) {
		kprintf("\n%s\n\n", resp.content);
		kprintf("[%d in / %d out tokens]\n",
			resp.input_tokens, resp.output_tokens);
	}
	anx_model_response_free(&resp);
}

/* --- Agent: iterative LLM + shell execution loop --- */

#define AGENT_MAX_ITERS	8
#define AGENT_HIST_SZ	3072
#define AGENT_CAP_SZ	2048

static const char *const AGENT_SYS =
	"You are a shell agent inside Anunix OS. "
	"CRITICAL: Your ENTIRE response must be ONE LINE ONLY — no newlines, no extra text. "
	"Output EITHER 'CMD: <shell-command>' (runs the command and shows you the output) "
	"OR 'DONE: <brief summary>' (terminates the session). "
	"Anunix commands: ls [ns:path], cat <path>, write <path> <data>, "
	"echo <text>, grep <pattern>, head [-N], tail [-N], wc, sort [-r], "
	"sysinfo, netinfo, date, history, tensor, cells, loop, state, disk. "
	"Wait to see real output before drawing conclusions. Do not fabricate output.";

static void cmd_agent(int argc, char **argv)
{
	char   goal[512];
	char  *hist;
	char  *capture;
	uint32_t hist_off = 0;
	int    iter, i;

	if (!anx_model_client_ready()) {
		kputs("model not configured (use 'model-init' first)\n");
		return;
	}
	if (argc < 2) {
		kputs("usage: agent <goal text...>\n");
		return;
	}

	/* Build goal string from args */
	{
		uint32_t off = 0;
		for (i = 1; i < argc && off < sizeof(goal) - 2; i++) {
			uint32_t l = (uint32_t)anx_strlen(argv[i]);
			if (i > 1 && off < sizeof(goal) - 1)
				goal[off++] = ' ';
			if (off + l >= sizeof(goal) - 1)
				l = (uint32_t)(sizeof(goal) - 1 - off);
			anx_memcpy(goal + off, argv[i], l);
			off += l;
		}
		goal[off] = '\0';
	}

	hist = anx_alloc(AGENT_HIST_SZ);
	capture = anx_alloc(AGENT_CAP_SZ);
	if (!hist || !capture) {
		anx_free(hist);
		anx_free(capture);
		kprintf("agent: out of memory\n");
		return;
	}

	kprintf("agent: goal=\"%s\"\n", goal);

	/* Seed history with goal */
	hist_off = (uint32_t)anx_strlen(goal);
	if (hist_off >= AGENT_HIST_SZ - 1)
		hist_off = AGENT_HIST_SZ - 2;
	anx_memcpy(hist, goal, hist_off);
	hist[hist_off++] = '\n';
	hist[hist_off]   = '\0';

	for (iter = 0; iter < AGENT_MAX_ITERS; iter++) {
		struct anx_model_request  req;
		struct anx_model_response resp;
		int ret;

		anx_memset(&req, 0, sizeof(req));
		req.model       = "claude-haiku-4-5-20251001";
		req.system      = AGENT_SYS;
		req.user_message = hist;
		req.max_tokens  = 128;

		ret = anx_model_call(&req, &resp);
		if (ret != ANX_OK) {
			kprintf("agent: model error (%d)\n", ret);
			break;
		}
		if (!resp.content) {
			kprintf("agent: empty response\n");
			anx_model_response_free(&resp);
			break;
		}

		/* Truncate to first line — model must respond with one line */
		{
			char *nl = resp.content;
			while (*nl && *nl != '\n' && *nl != '\r')
				nl++;
			*nl = '\0';
		}

		kprintf("agent[%d]: %s\n", iter + 1, resp.content);

		if (anx_strncmp(resp.content, "DONE:", 5) == 0) {
			anx_model_response_free(&resp);
			break;
		}

		if (anx_strncmp(resp.content, "CMD:", 4) == 0) {
			const char *cmd = resp.content + 4;
			uint32_t cap;
			struct anx_capture_state outer;

			while (*cmd == ' ')
				cmd++;

			/* Capture command output without disrupting caller's capture */
			anx_kprintf_capture_save(&outer);
			anx_kprintf_capture_start(capture, AGENT_CAP_SZ);
			execute_line(cmd);
			cap = anx_kprintf_capture_stop();
			anx_kprintf_capture_restore(&outer);

			if (cap >= AGENT_CAP_SZ)
				cap = AGENT_CAP_SZ - 1;
			capture[cap] = '\0';

			/* Show output to user too */
			kprintf("  -> %s", capture[0] ? capture : "(empty)\n");

			/* Append cmd + truncated output to history */
			{
				uint32_t rem = AGENT_HIST_SZ - hist_off - 1;
				uint32_t n;

				n = (uint32_t)anx_strlen(cmd);
				if (n + 6 < rem) {
					anx_memcpy(hist + hist_off, "CMD: ", 5);
					hist_off += 5;
					anx_memcpy(hist + hist_off, cmd, n);
					hist_off += n;
					hist[hist_off++] = '\n';
				}
				rem = AGENT_HIST_SZ - hist_off - 1;
				n = cap < rem ? cap : rem;
				if (n > 512)
					n = 512;	/* cap per-step output in history */
				if (n > 0) {
					anx_memcpy(hist + hist_off, "OUTPUT: ", 8);
					hist_off += 8;
					anx_memcpy(hist + hist_off, capture, n);
					hist_off += n;
					if (hist[hist_off - 1] != '\n')
						hist[hist_off++] = '\n';
				}
				hist[hist_off] = '\0';
			}
		}

		anx_model_response_free(&resp);
	}

	anx_free(hist);
	anx_free(capture);
}

/* --- HW Inventory --- */

static void cmd_hw_inventory(void)
{
	const struct anx_acpi_info *acpi = anx_acpi_get_info();
	struct anx_list_head *pos;
	struct anx_list_head *pci_list = anx_pci_device_list();
	uint32_t net_count = 0, storage_count = 0, display_count = 0;

	kputs("=== Hardware Inventory ===\n\n");

	/* CPU info from ACPI */
	if (acpi) {
		kprintf("CPUs:    %u\n", acpi->cpu_count);
		kprintf("LAPIC:   0x%x\n", acpi->lapic_addr);
		kprintf("IOAPICs: %u\n", acpi->ioapic_count);
	} else {
		kputs("CPUs:    (ACPI unavailable)\n");
	}

	/* Memory */
	{
		uint64_t total, free_pages;

		anx_page_stats(&total, &free_pages);
		kprintf("Memory:  %u KiB free / %u KiB total\n",
			(uint32_t)(free_pages * 4),
			(uint32_t)(total * 4));
	}

	/* Categorize PCI devices */
	ANX_LIST_FOR_EACH(pos, pci_list) {
		struct anx_pci_device *dev;

		dev = ANX_LIST_ENTRY(pos, struct anx_pci_device, link);
		if (dev->class_code == 0x02)
			net_count++;
		else if (dev->class_code == 0x01)
			storage_count++;
		else if (dev->class_code == 0x03)
			display_count++;
	}

	kprintf("\nDevices:\n");
	kprintf("  Network:  %u\n", net_count);
	kprintf("  Storage:  %u\n", storage_count);
	kprintf("  Display:  %u\n", display_count);

	/* Block device */
	if (anx_blk_ready())
		kprintf("\nBlock:   %u MiB (virtio-blk)\n",
			(uint32_t)(anx_blk_capacity() * 512 / (1024 * 1024)));

	/* Network */
	if (anx_virtio_net_ready()) {
		uint8_t mac[6];

		anx_virtio_net_mac(mac);
		kprintf("Network: %x:%x:%x:%x:%x:%x (virtio-net)\n",
			(uint32_t)mac[0], (uint32_t)mac[1],
			(uint32_t)mac[2], (uint32_t)mac[3],
			(uint32_t)mac[4], (uint32_t)mac[5]);
	}

	kputs("\n");
}

/* --- Auth commands --- */

static void cmd_login(int argc, char **argv)
{
	struct anx_session session;
	char password[128];
	const char *username;
	int ret;

	if (anx_auth_current_session()) {
		kprintf("already logged in as %s (use 'logout' first)\n",
			anx_auth_current_session()->username);
		return;
	}

	if (argc < 2) {
		kputs("usage: login <username>\n");
		return;
	}
	username = argv[1];

	kputs("password: ");
	/* Read password without echo */
	{
		size_t pos = 0;

		while (pos < sizeof(password) - 1) {
			int c = arch_console_getc();

			if (c < 0)
				break;
			if (c == '\r' || c == '\n') {
				kputc('\n');
				break;
			}
			if (c == 0x7F || c == '\b') {
				if (pos > 0)
					pos--;
				continue;
			}
			if (c >= 0x20 && c < 0x7F)
				password[pos++] = (char)c;
		}
		password[pos] = '\0';
	}

	ret = anx_auth_login_password(username, password, &session);

	/* Zero password immediately */
	anx_memset(password, 0, sizeof(password));

	if (ret == ANX_OK)
		kprintf("logged in as %s\n", session.username);
	else
		kputs("login failed\n");
}

static void cmd_useradd(int argc, char **argv)
{
	int ret;

	if (argc < 3) {
		kputs("usage: useradd <username> <password>\n");
		return;
	}

	ret = anx_auth_create_user(argv[1]);
	if (ret != ANX_OK && ret != ANX_EEXIST) {
		kprintf("useradd: failed (%d)\n", ret);
		return;
	}

	ret = anx_auth_add_password(argv[1], argv[2], ANX_SCOPE_ADMIN);
	if (ret != ANX_OK) {
		kprintf("useradd: password set failed (%d)\n", ret);
		return;
	}

	/* Zero the password from the command buffer */
	anx_memset(argv[2], 0, anx_strlen(argv[2]));

	kprintf("user '%s' created with admin scope\n", argv[1]);
}

/* --- SSH authorized-key management --- */

static void cmd_ssh_addkey(int argc, char **argv)
{
	/*
	 * ssh-addkey <base64-blob>
	 *
	 * Accepts the blob portion of an OpenSSH public key line:
	 *   ssh-ed25519 AAAA...base64... optional-comment
	 *
	 * The blob decodes to: string "ssh-ed25519" + string pubkey(32).
	 * We extract the 32-byte raw pubkey and append it to the
	 * ssh-authorized-keys credential.
	 */
	const char *b64;
	uint8_t decoded[256];
	uint32_t dec_len = 0;
	uint8_t current[ANX_AUTHORIZED_KEYS_MAX * 32];
	uint32_t cur_len = 0;
	uint32_t pbo = 0;
	uint32_t key_type_len, pk_len;
	const uint8_t *pk;
	uint32_t new_len;
	uint32_t i;
	int ret;

	if (argc < 2) {
		kputs("usage: ssh-addkey <base64-blob>\n");
		kputs("  Get blob: ssh-keygen -e -m pkcs8 ... or copy from authorized_keys\n");
		return;
	}

	/* Accept either full "ssh-ed25519 BLOB comment" or just "BLOB" */
	b64 = argv[1];
	if (anx_strncmp(b64, "ssh-ed25519", 11) == 0 && argc >= 3)
		b64 = argv[2];

	dec_len = (uint32_t)anx_base64_decode(b64, anx_strlen(b64),
					      decoded, sizeof(decoded));
	if (dec_len < 4) {
		kputs("ssh-addkey: base64 decode failed\n");
		return;
	}

	/* Blob: uint32_be key_type_len; key_type; uint32_be pk_len; pk */
	key_type_len = ((uint32_t)decoded[pbo] << 24) |
	               ((uint32_t)decoded[pbo+1] << 16) |
	               ((uint32_t)decoded[pbo+2] << 8) |
	               (uint32_t)decoded[pbo+3];
	pbo += 4;
	if (key_type_len > dec_len - pbo || key_type_len != 11 ||
	    anx_strncmp((const char *)(decoded + pbo), "ssh-ed25519", 11) != 0) {
		kputs("ssh-addkey: not an ssh-ed25519 key\n");
		return;
	}
	pbo += key_type_len;

	if (pbo + 4 > dec_len) { kputs("ssh-addkey: truncated blob\n"); return; }
	pk_len = ((uint32_t)decoded[pbo] << 24) |
	         ((uint32_t)decoded[pbo+1] << 16) |
	         ((uint32_t)decoded[pbo+2] << 8) |
	         (uint32_t)decoded[pbo+3];
	pbo += 4;
	if (pk_len != 32 || pbo + 32 > dec_len) {
		kputs("ssh-addkey: unexpected pubkey length\n");
		return;
	}
	pk = decoded + pbo;

	/* Read existing keys */
	anx_credential_read("ssh-authorized-keys", current,
			    sizeof(current), &cur_len);
	if (cur_len % 32 != 0) cur_len = 0;	/* corrupt — reset */

	/* Check for duplicate */
	for (i = 0; i + 32 <= cur_len; i += 32) {
		if (anx_memcmp(current + i, pk, 32) == 0) {
			kputs("ssh-addkey: key already authorized\n");
			return;
		}
	}

	if (cur_len + 32 > (uint32_t)(ANX_AUTHORIZED_KEYS_MAX * 32)) {
		kprintf("ssh-addkey: max %d keys reached\n",
			ANX_AUTHORIZED_KEYS_MAX);
		return;
	}

	anx_memcpy(current + cur_len, pk, 32);
	new_len = cur_len + 32;

	if (anx_credential_exists("ssh-authorized-keys")) {
		ret = anx_credential_rotate("ssh-authorized-keys",
					    current, new_len);
	} else {
		ret = anx_credential_create("ssh-authorized-keys",
					    ANX_CRED_OPAQUE,
					    current, new_len);
	}

	if (ret != ANX_OK) {
		kprintf("ssh-addkey: store failed (%d)\n", ret);
		return;
	}

	kprintf("ssh-addkey: authorized key added (%u total)\n",
		new_len / 32);
}

static void cmd_ssh_keygen(void)
{
	/*
	 * Generate a fresh Ed25519 identity keypair, persist the private key
	 * as the "ssh-identity" credential, and print the public key in
	 * OpenSSH authorized_keys format.
	 *
	 * SSH wire blob: uint32_be(11) "ssh-ed25519" uint32_be(32) pubkey[32]
	 * Total: 4 + 11 + 4 + 32 = 51 bytes → 68 base64 chars.
	 */
	uint8_t seed[32];
	uint8_t pub[32];
	uint8_t priv[64];
	uint8_t wire[51];
	char b64[ANX_BASE64_ENC_LEN(51) + 1];
	uint8_t existing_keys[ANX_AUTHORIZED_KEYS_MAX * 32];
	uint32_t key_len = 0;
	uint32_t new_len;
	size_t b64_len;
	int ret;

	anx_random_bytes(seed, sizeof(seed));
	anx_ed25519_keypair(pub, priv, seed);

	/* Build SSH wire encoding */
	wire[0] = 0; wire[1] = 0; wire[2] = 0; wire[3] = 11;
	anx_memcpy(wire + 4, "ssh-ed25519", 11);
	wire[15] = 0; wire[16] = 0; wire[17] = 0; wire[18] = 32;
	anx_memcpy(wire + 19, pub, 32);

	b64_len = anx_base64_encode(wire, 51, b64, sizeof(b64) - 1);
	b64[b64_len] = '\0';

	/* Store or replace private key */
	if (anx_credential_exists("ssh-identity"))
		ret = anx_credential_rotate("ssh-identity", priv, 64);
	else
		ret = anx_credential_create("ssh-identity",
					    ANX_CRED_PRIVATE_KEY, priv, 64);
	if (ret != ANX_OK) {
		kprintf("ssh-keygen: failed to store key (%d)\n", ret);
		return;
	}

	/* Self-authorize: append pubkey to ssh-authorized-keys */
	anx_credential_read("ssh-authorized-keys", existing_keys,
			    sizeof(existing_keys), &key_len);
	if (key_len % 32 != 0) key_len = 0;
	if (key_len + 32 <= (uint32_t)(ANX_AUTHORIZED_KEYS_MAX * 32)) {
		anx_memcpy(existing_keys + key_len, pub, 32);
		new_len = key_len + 32;
		if (anx_credential_exists("ssh-authorized-keys"))
			anx_credential_rotate("ssh-authorized-keys",
					      existing_keys, new_len);
		else
			anx_credential_create("ssh-authorized-keys",
					      ANX_CRED_OPAQUE,
					      existing_keys, new_len);
	}

	kprintf("ssh-ed25519 %s anunix-local\n", b64);
}

/* --- Store commands --- */

static void cmd_store(int argc, char **argv)
{
	if (argc < 2) {
		kputs("usage: store <format|mount|stats> [label]\n");
		return;
	}

	if (anx_strcmp(argv[1], "format") == 0) {
		const char *label = argc >= 3 ? argv[2] : "anunix";
		int ret = anx_disk_format(label);

		if (ret != ANX_OK)
			kprintf("store format: failed (%d)\n", ret);
	} else if (anx_strcmp(argv[1], "mount") == 0) {
		int ret = anx_disk_store_init();

		if (ret != ANX_OK)
			kprintf("store mount: failed (%d)\n", ret);
	} else if (anx_strcmp(argv[1], "stats") == 0) {
		uint64_t obj_count, used, total;
		int ret = anx_disk_stats(&obj_count, &used, &total);

		if (ret != ANX_OK) {
			kputs("store not mounted\n");
			return;
		}
		kprintf("objects: %u  used: %u sectors  total: %u sectors\n",
			(uint32_t)obj_count, (uint32_t)used, (uint32_t)total);
	} else {
		kputs("usage: store <format|mount|stats>\n");
	}
}

/* --- Disk commands --- */

static void cmd_disk(void)
{
	uint64_t cap;
	uint32_t mb;

	if (!anx_blk_ready()) {
		kputs("no block device detected\n");
		return;
	}
	cap = anx_blk_capacity();
	mb = (uint32_t)(cap * 512 / (1024 * 1024));
	kprintf("virtio-blk: %u MiB (%u sectors)\n", mb, (uint32_t)cap);
}

/* --- PCI commands --- */

static void cmd_pci(int argc, char **argv)
{
	struct anx_list_head *pos;
	struct anx_list_head *list = anx_pci_device_list();
	bool detail = (argc >= 2 && anx_strcmp(argv[1], "detail") == 0);

	kputs("PCI devices:\n");
	ANX_LIST_FOR_EACH(pos, list) {
		struct anx_pci_device *dev;

		dev = ANX_LIST_ENTRY(pos, struct anx_pci_device, link);
		kprintf("  %x:%x.%x  %x:%x  %s",
			(uint32_t)dev->bus, (uint32_t)dev->slot,
			(uint32_t)dev->func,
			(uint32_t)dev->vendor_id, (uint32_t)dev->device_id,
			anx_pci_class_name(dev->class_code, dev->subclass));
		if (detail) {
			int i;

			kprintf("\n    class: %x:%x  rev: %x  irq: %u\n",
				(uint32_t)dev->class_code,
				(uint32_t)dev->subclass,
				(uint32_t)dev->revision,
				(uint32_t)dev->irq_line);
			for (i = 0; i < 6; i++) {
				if (dev->bar[i] == 0)
					continue;
				kprintf("    bar%d: 0x%x (%s)\n", i,
					dev->bar[i],
					(dev->bar[i] & 1) ? "I/O" : "MMIO");
			}
		} else {
			if (dev->irq_line)
				kprintf("  irq %u",
					(uint32_t)dev->irq_line);
			kprintf("\n");
		}
	}
}

/* --- Shell variables --- */

static int last_return_code;	/* $? */

/* --- Pipe-aware filter commands --- */

static char to_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Substring search within a length-bounded string, optionally case-insensitive */
static bool line_contains(const char *line, uint32_t len, const char *pat,
			   bool ignore_case)
{
	uint32_t plen = (uint32_t)anx_strlen(pat);
	uint32_t i, j;

	if (plen == 0 || plen > len)
		return plen == 0;

	for (i = 0; i + plen <= len; i++) {
		bool match = true;
		for (j = 0; j < plen; j++) {
			char a = ignore_case ? to_lower(line[i + j]) : line[i + j];
			char b = ignore_case ? to_lower(pat[j]) : pat[j];
			if (a != b) {
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

/* Print a length-bounded line (kprintf doesn't support %.*s) */
static void print_line(const char *s, uint32_t len)
{
	char buf[256];
	uint32_t copy = len < sizeof(buf) - 2 ? len : sizeof(buf) - 2;

	anx_memcpy(buf, s, copy);
	buf[copy]     = '\n';
	buf[copy + 1] = '\0';
	kprintf("%s", buf);
}

static void cmd_grep(int argc, char **argv)
{
	const char *pattern = NULL;
	const char *p;
	bool invert = false;
	bool ignore_case = false;
	int i;

	/* Parse flags: -v (invert), -i (case-insensitive) */
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] != '\0') {
			const char *f = argv[i] + 1;
			while (*f) {
				if (*f == 'v') invert = true;
				else if (*f == 'i') ignore_case = true;
				f++;
			}
		} else if (!pattern) {
			pattern = argv[i];
		}
	}

	if (!pattern) {
		kprintf("usage: grep [-v] [-i] <pattern>\n");
		return;
	}

	if (!g_pipe_stdin) {
		kprintf("grep: no piped input\n");
		return;
	}

	p = g_pipe_stdin;
	while (*p) {
		const char *start = p;
		uint32_t len = 0;

		while (*p && *p != '\n') { p++; len++; }
		{
			bool found = line_contains(start, len, pattern,
						   ignore_case);
			if (found != invert)
				print_line(start, len);
		}
		if (*p == '\n')
			p++;
	}
}

static void cmd_head(int argc, char **argv)
{
	int n = 10;
	const char *p;
	int count = 0;

	if (argc >= 3 && anx_strcmp(argv[1], "-n") == 0)
		n = (int)anx_strtoul(argv[2], NULL, 10);
	else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] >= '1')
		n = (int)anx_strtoul(argv[1] + 1, NULL, 10);

	if (!g_pipe_stdin) {
		kprintf("head: no piped input\n");
		return;
	}

	p = g_pipe_stdin;
	while (*p && count < n) {
		const char *start = p;
		uint32_t len = 0;

		while (*p && *p != '\n') { p++; len++; }
		print_line(start, len);
		if (*p == '\n')
			p++;
		count++;
	}
}

static void cmd_tail(int argc, char **argv)
{
	int n = 10;
	const char *p;
	const char *lines[64];
	uint32_t lens[64];
	int total = 0, i;

	if (argc >= 3 && anx_strcmp(argv[1], "-n") == 0)
		n = (int)anx_strtoul(argv[2], NULL, 10);
	else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] >= '1')
		n = (int)anx_strtoul(argv[1] + 1, NULL, 10);
	if (n > 64) n = 64;

	if (!g_pipe_stdin) {
		kprintf("tail: no piped input\n");
		return;
	}

	/* Collect all lines (ring buffer of last n) */
	p = g_pipe_stdin;
	while (*p) {
		const char *start = p;
		uint32_t len = 0;

		while (*p && *p != '\n') { p++; len++; }
		lines[total % 64] = start;
		lens[total % 64]  = len;
		total++;
		if (*p == '\n')
			p++;
	}

	{
		int start_idx = total > n ? total - n : 0;
		for (i = start_idx; i < total; i++) {
			int slot = i % 64;
			print_line(lines[slot], lens[slot]);
		}
	}
}

static void cmd_wc(int argc, char **argv)
{
	const char *p;
	uint32_t lines = 0, words = 0, chars = 0;
	bool in_word = false;
	bool count_lines = false, count_words = false, count_chars = false;
	int i;

	for (i = 1; i < argc; i++) {
		if (anx_strcmp(argv[i], "-l") == 0) count_lines = true;
		else if (anx_strcmp(argv[i], "-w") == 0) count_words = true;
		else if (anx_strcmp(argv[i], "-c") == 0) count_chars = true;
	}
	if (!count_lines && !count_words && !count_chars)
		count_lines = count_words = count_chars = true;

	if (!g_pipe_stdin) {
		kprintf("wc: no piped input\n");
		return;
	}

	p = g_pipe_stdin;
	while (*p) {
		chars++;
		if (*p == '\n') {
			lines++;
			in_word = false;
		} else if (*p == ' ' || *p == '\t') {
			in_word = false;
		} else {
			if (!in_word) { words++; in_word = true; }
		}
		p++;
	}

	if (count_lines) kprintf("%u", lines);
	if (count_words) kprintf(count_lines ? " %u" : "%u", words);
	if (count_chars) kprintf((count_lines || count_words) ? " %u" : "%u", chars);
	kprintf("\n");
}

#define SORT_MAX_LINES	256
#define SORT_LINE_BUF	(SORT_MAX_LINES * 128)

static void cmd_sort(int argc, char **argv)
{
	static char   sort_buf[SORT_LINE_BUF];
	static const char *lines[SORT_MAX_LINES];
	static uint32_t    lens[SORT_MAX_LINES];
	uint32_t nlines = 0;
	uint32_t buf_used = 0;
	const char *p;
	bool reverse = false;
	int i;

	for (i = 1; i < argc; i++) {
		if (anx_strcmp(argv[i], "-r") == 0)
			reverse = true;
	}

	if (!g_pipe_stdin) {
		kprintf("sort: no piped input\n");
		return;
	}

	/* Copy lines into sort_buf, record pointers */
	p = g_pipe_stdin;
	while (*p && nlines < SORT_MAX_LINES) {
		const char *start = p;
		uint32_t len = 0;

		while (*p && *p != '\n') { p++; len++; }
		if (*p == '\n') p++;

		if (buf_used + len + 1 >= SORT_LINE_BUF)
			break;

		anx_memcpy(sort_buf + buf_used, start, len);
		sort_buf[buf_used + len] = '\0';
		lines[nlines] = sort_buf + buf_used;
		lens[nlines]  = len;
		buf_used += len + 1;
		nlines++;
	}

	/* Insertion sort (small N) */
	for (i = 1; i < (int)nlines; i++) {
		const char *key  = lines[i];
		uint32_t    klen = lens[i];
		int j = i - 1;

		while (j >= 0) {
			int cmp = anx_strcmp(lines[j], key);
			/* ascending: stop when lines[j] <= key; reverse: when >= key */
			if (reverse ? cmp >= 0 : cmp <= 0)
				break;
			lines[j + 1] = lines[j];
			lens[j + 1]  = lens[j];
			j--;
		}
		lines[j + 1] = key;
		lens[j + 1]  = klen;
	}

	for (i = 0; i < (int)nlines; i++)
		print_line(lines[i], lens[i]);
}

/* --- Command dispatch --- */

static void dispatch(int argc, char **argv)
{
	if (argc == 0)
		return;

	if (anx_strcmp(argv[0], "help") == 0 ||
	    anx_strcmp(argv[0], "?") == 0) {
		cmd_help(argc, argv);
	} else if (anx_strcmp(argv[0], "ls") == 0) {
		cmd_ls(argc, argv);
	} else if (anx_strcmp(argv[0], "cat") == 0) {
		cmd_cat(argc, argv);
	} else if (anx_strcmp(argv[0], "write") == 0) {
		cmd_write_obj(argc, argv);
	} else if (anx_strcmp(argv[0], "edit") == 0) {
		/* edit [ns:]<path>  — open text editor for a state object */
		if (argc < 2) {
			kprintf("usage: edit [ns:]<path>\n");
		} else if (!anx_fb_available()) {
			kprintf("edit: requires framebuffer (GUI mode)\n");
		} else {
			const char *arg = argv[1];
			const char *colon = arg;
			static char ns_buf[32];
			static char path_buf[96];

			while (*colon && *colon != ':')
				colon++;

			if (*colon == ':') {
				uint32_t nlen = (uint32_t)(colon - arg);
				if (nlen < sizeof(ns_buf)) {
					anx_memcpy(ns_buf, arg, nlen);
					ns_buf[nlen] = '\0';
				} else {
					anx_strlcpy(ns_buf, "default",
						    sizeof(ns_buf));
				}
				anx_strlcpy(path_buf, colon + 1,
					    sizeof(path_buf));
			} else {
				anx_strlcpy(ns_buf, "default",
					    sizeof(ns_buf));
				anx_strlcpy(path_buf, arg, sizeof(path_buf));
			}
			anx_wm_terminal_edit(ns_buf, path_buf);
		}
	} else if (anx_strcmp(argv[0], "cp") == 0) {
		cmd_cp(argc, argv);
	} else if (anx_strcmp(argv[0], "mv") == 0) {
		cmd_mv(argc, argv);
	} else if (anx_strcmp(argv[0], "rm") == 0) {
		cmd_rm_obj(argc, argv);
	} else if (anx_strcmp(argv[0], "inspect") == 0) {
		cmd_inspect(argc, argv);
	} else if (anx_strcmp(argv[0], "sysinfo") == 0) {
		cmd_sysinfo(argc, argv);
	} else if (anx_strcmp(argv[0], "fb_info") == 0) {
		cmd_fb_info(argc, argv);
	} else if (anx_strcmp(argv[0], "gop_list") == 0) {
		cmd_gop_list(argc, argv);
	} else if (anx_strcmp(argv[0], "fb_test") == 0) {
		cmd_fb_test(argc, argv);
	} else if (anx_strcmp(argv[0], "wifi") == 0) {
		cmd_wifi(argc, argv);
	} else if (anx_strcmp(argv[0], "netinfo") == 0) {
		cmd_netinfo(argc, argv);
	} else if (anx_strcmp(argv[0], "search") == 0) {
		cmd_search(argc, argv);
	} else if (anx_strcmp(argv[0], "fetch") == 0) {
		cmd_fetch(argc, argv);
	} else if (anx_strcmp(argv[0], "cells") == 0) {
		cmd_cells(argc, argv);
	} else if (anx_strcmp(argv[0], "version") == 0) {
		cmd_version(argc, argv);
	} else if (anx_strcmp(argv[0], "mem") == 0) {
		if (argc >= 2 && anx_strcmp(argv[1], "stats") == 0)
			cmd_mem_stats();
		else
			kputs("usage: mem stats\n");
	} else if (anx_strcmp(argv[0], "state") == 0) {
		cmd_state(argc, argv);
	} else if (anx_strcmp(argv[0], "cell") == 0) {
		cmd_cell(argc, argv);
	} else if (anx_strcmp(argv[0], "memplane") == 0) {
		cmd_memplane(argc, argv);
	} else if (anx_strcmp(argv[0], "engine") == 0) {
		cmd_engine(argc, argv);
	} else if (anx_strcmp(argv[0], "cap") == 0) {
		cmd_cap(argc, argv);
	} else if (anx_strcmp(argv[0], "sched") == 0) {
		if (argc >= 2 && anx_strcmp(argv[1], "status") == 0)
			cmd_sched_status();
		else
			kputs("usage: sched status\n");
	} else if (anx_strcmp(argv[0], "net") == 0) {
		if (argc >= 2 && anx_strcmp(argv[1], "status") == 0)
			cmd_net_status();
		else
			kputs("usage: net status\n");
	} else if (anx_strcmp(argv[0], "api") == 0) {
		cmd_api(argc, argv);
	} else if (anx_strcmp(argv[0], "secret") == 0) {
		cmd_secret(argc, argv);
	} else if (anx_strcmp(argv[0], "http-get") == 0) {
		cmd_http_get(argc, argv);
	} else if (anx_strcmp(argv[0], "dns") == 0) {
		if (argc >= 2)
			cmd_dns(argv[1]);
		else
			kputs("usage: dns <hostname>\n");
	} else if (anx_strcmp(argv[0], "ping") == 0) {
		if (argc >= 2)
			cmd_ping(argv[1]);
		else
			kputs("usage: ping <ip>\n");
	} else if (anx_strcmp(argv[0], "ask") == 0) {
		cmd_ask(argc, argv);
	} else if (anx_strcmp(argv[0], "agent") == 0) {
		cmd_agent(argc, argv);
	} else if (anx_strcmp(argv[0], "model-init") == 0) {
		cmd_model_init(argc, argv);
	} else if (anx_strcmp(argv[0], "hw-inventory") == 0) {
		cmd_hw_inventory();
	} else if (anx_strcmp(argv[0], "hwd") == 0) {
		cmd_hwd(argc, argv);
	} else if (anx_strcmp(argv[0], "login") == 0) {
		cmd_login(argc, argv);
	} else if (anx_strcmp(argv[0], "logout") == 0) {
		anx_auth_logout();
		kputs("logged out\n");
	} else if (anx_strcmp(argv[0], "useradd") == 0) {
		cmd_useradd(argc, argv);
	} else if (anx_strcmp(argv[0], "ssh-keygen") == 0) {
		cmd_ssh_keygen();
	} else if (anx_strcmp(argv[0], "ssh-addkey") == 0) {
		cmd_ssh_addkey(argc, argv);
	} else if (anx_strcmp(argv[0], "store") == 0) {
		cmd_store(argc, argv);
	} else if (anx_strcmp(argv[0], "disk") == 0) {
		cmd_disk();
	} else if (anx_strcmp(argv[0], "pci") == 0) {
		cmd_pci(argc, argv);
	} else if (anx_strcmp(argv[0], "surfctl") == 0) {
		cmd_surfctl(argc, argv);
	} else if (anx_strcmp(argv[0], "evctl") == 0) {
		cmd_evctl(argc, argv);
	} else if (anx_strcmp(argv[0], "compctl") == 0) {
		cmd_compctl(argc, argv);
	} else if (anx_strcmp(argv[0], "envctl") == 0) {
		cmd_envctl(argc, argv);
	} else if (anx_strcmp(argv[0], "halt") == 0) {
		kputs("halting system\n");
		arch_halt();
	} else if (anx_strcmp(argv[0], "perf") == 0) {
		anx_perf_report();
	} else if (anx_strcmp(argv[0], "tz") == 0) {
		if (argc >= 2) {
			const char *v = argv[1];
			int32_t offset = 0;
			bool neg = false;

			if (*v == '-') { neg = true; v++; }
			else if (*v == '+') { v++; }
			while (*v >= '0' && *v <= '9')
				offset = offset * 10 + (*v++ - '0');
			if (neg) offset = -offset;
			anx_gui_set_tz_offset(offset);
			kprintf("timezone set to UTC%s%d\n",
				offset >= 0 ? "+" : "", (int)offset);
		} else {
			kputs("usage: tz <offset> (e.g., tz -7 for PDT)\n");
		}
	} else if (anx_strcmp(argv[0], "reboot") == 0) {
		kputs("rebooting...\n");
#ifdef __x86_64__
		/* Keyboard controller reset (PS/2 port 0x64) */
		anx_outb(0xFE, 0x64);
		/* If that didn't work, triple-fault */
		{
			struct { uint16_t limit; uint64_t base; }
				__attribute__((packed)) null_idt = {0, 0};
			__asm__ volatile("lidt %0" : : "m"(null_idt));
			__asm__ volatile("int3");
		}
#else
		/* ARM64: PSCI system reset or spin */
		arch_halt();
#endif
	} else if (anx_strcmp(argv[0], "ntp") == 0) {
		if (argc >= 2) {
			uint32_t ntp_ip = parse_ip(argv[1]);
			int ntp_ret = anx_ntp_sync(ntp_ip);

			if (ntp_ret != ANX_OK)
				kprintf("ntp: sync failed (%d)\n", ntp_ret);
		} else {
			uint32_t ntp_ip;
			int ntp_ret = anx_dns_resolve("time.nist.gov", &ntp_ip);

			if (ntp_ret == ANX_OK) {
				ntp_ret = anx_ntp_sync(ntp_ip);
				if (ntp_ret != ANX_OK)
					kprintf("ntp: sync failed (%d)\n",
						ntp_ret);
			} else {
				kprintf("ntp: dns failed (%d)\n", ntp_ret);
			}
		}
	} else if (anx_strcmp(argv[0], "install") == 0) {
		if (argc >= 2 && anx_strcmp(argv[1], "-i") == 0) {
			anx_installer_interactive();
		} else {
			kprintf("usage: install -i (interactive)\n");
			kprintf("  Automated install requires provision.json\n");
		}
	} else if (anx_strcmp(argv[0], "meta") == 0) {
		cmd_meta(argc, argv);
	} else if (anx_strcmp(argv[0], "tensor") == 0) {
		cmd_tensor(argc, argv);
	} else if (anx_strcmp(argv[0], "model") == 0) {
		cmd_model(argc, argv);
	} else if (anx_strcmp(argv[0], "xdna") == 0) {
		if (argc >= 2 && anx_strcmp(argv[1], "load") == 0) {
			int xr = anx_xdna_load_firmware();

			if (xr != ANX_OK)
				kprintf("xdna: load failed (%d)\n", xr);
		} else {
			anx_xdna_info();
		}
	} else if (anx_strcmp(argv[0], "echo") == 0) {
		int ei;

		for (ei = 1; ei < argc; ei++) {
			if (anx_strcmp(argv[ei], "$?") == 0)
				kprintf("%d", last_return_code);
			else
				kprintf("%s", argv[ei]);
			if (ei < argc - 1)
				kprintf(" ");
		}
		kprintf("\n");
	} else if (anx_strcmp(argv[0], "fb_info") == 0) {
		cmd_fb_info(argc, argv);
	} else if (anx_strcmp(argv[0], "gop_list") == 0) {
		cmd_gop_list(argc, argv);
	} else if (anx_strcmp(argv[0], "fb_test") == 0) {
		cmd_fb_test(argc, argv);
	} else if (anx_strcmp(argv[0], "browser_init") == 0) {
		cmd_browser_init(argc, argv);
	} else if (anx_strcmp(argv[0], "browser") == 0) {
		cmd_browser(argc, argv);
	} else if (anx_strcmp(argv[0], "browser_stop") == 0) {
		cmd_browser_stop(argc, argv);
	} else if (anx_strcmp(argv[0], "vm") == 0) {
		cmd_vm(argc, argv);
	} else if (anx_strcmp(argv[0], "workflow") == 0) {
		cmd_workflow(argc, argv);
	} else if (anx_strcmp(argv[0], "rlm") == 0) {
		cmd_rlm(argc, argv);
	} else if (anx_strcmp(argv[0], "jepa") == 0) {
		cmd_jepa(argc, argv);
	} else if (anx_strcmp(argv[0], "loop") == 0) {
		anx_loop_shell_dispatch(argc, (const char *const *)argv);
	} else if (anx_strcmp(argv[0], "theme") == 0) {
		cmd_theme(argc, argv);
	} else if (anx_strcmp(argv[0], "clear") == 0) {
		cmd_clear(argc, argv);
	} else if (anx_strcmp(argv[0], "mode") == 0) {
		last_return_code = cmd_mode(argc, argv);
	} else if (anx_strcmp(argv[0], "kickstart") == 0) {
		cmd_kickstart(argc, argv);
	} else if (anx_strcmp(argv[0], "grep") == 0) {
		cmd_grep(argc, argv);
	} else if (anx_strcmp(argv[0], "head") == 0) {
		cmd_head(argc, argv);
	} else if (anx_strcmp(argv[0], "tail") == 0) {
		cmd_tail(argc, argv);
	} else if (anx_strcmp(argv[0], "wc") == 0) {
		cmd_wc(argc, argv);
	} else if (anx_strcmp(argv[0], "sort") == 0) {
		cmd_sort(argc, argv);
	} else if (anx_strcmp(argv[0], "date") == 0) {
		char time_buf[16];
		char date_buf[16];
		uint32_t unix_ts = anx_ntp_unix_time();

		anx_gui_get_time(time_buf, sizeof(time_buf));
		anx_gui_get_date(date_buf, sizeof(date_buf));
		if (unix_ts)
			kprintf("%s %s  (unix %u)\n", date_buf, time_buf, unix_ts);
		else
			kprintf("%s %s\n", date_buf, time_buf);
	} else if (anx_strcmp(argv[0], "history") == 0) {
		uint32_t i;
		uint32_t start = history_write >= history_count
			? history_write - history_count
			: HISTORY_SIZE + history_write - history_count;
		for (i = 0; i < history_count; i++) {
			uint32_t idx = (start + i) % HISTORY_SIZE;
			kprintf("%3u  %s\n", (unsigned)(i + 1), history[idx]);
		}
	} else {
		kprintf("unknown command: %s (type 'help')\n", argv[0]);
		last_return_code = -1;
	}
}

/* --- Shell main loop --- */

/* Execute a single command line (may be called recursively for if/for) */
static void execute_line(const char *input)
{
	char line_copy[MAX_LINE];
	char *argv[MAX_ARGS];
	int argc;
	char *pipe_pos;

	anx_strlcpy(line_copy, input, MAX_LINE);

	/* Detect pipe operator */
	pipe_pos = line_copy;
	while (*pipe_pos && *pipe_pos != '|')
		pipe_pos++;

	if (*pipe_pos == '|') {
		char *segs[MAX_PIPE_STAGES];
		int nseg = 0;
		char *p = line_copy;
		char *bufa, *bufb;
		char *cur_out, *cur_in;
		uint32_t cur_in_len;
		struct anx_capture_state outer;
		int i;

		/* Split line by '|' into segments */
		segs[nseg++] = p;
		while (*p && nseg < MAX_PIPE_STAGES) {
			if (*p == '|') {
				*p = '\0';
				segs[nseg++] = p + 1;
			}
			p++;
		}
		for (i = 0; i < nseg; i++) {
			while (*segs[i] == ' ')
				segs[i]++;
		}

		/* Two ping-pong buffers for intermediate captures */
		bufa = anx_alloc(PIPE_CAP_SZ);
		bufb = anx_alloc(PIPE_CAP_SZ);
		if (!bufa || !bufb) {
			anx_free(bufa);
			anx_free(bufb);
			kprintf("pipe: out of memory\n");
			return;
		}

		anx_kprintf_capture_save(&outer);

		cur_out    = bufa;
		cur_in     = NULL;
		cur_in_len = 0;

		for (i = 0; i < nseg; i++) {
			bool last = (i == nseg - 1);

			g_pipe_stdin     = cur_in;
			g_pipe_stdin_len = cur_in_len;

			if (!last) {
				anx_kprintf_capture_start(cur_out, PIPE_CAP_SZ);
			} else {
				/* Last stage: output goes back to caller */
				anx_kprintf_capture_restore(&outer);
			}

			argc = parse_args(segs[i], argv, MAX_ARGS);
			if (argc > 0)
				dispatch(argc, argv);

			if (!last) {
				uint32_t cap = anx_kprintf_capture_stop();
				if (cap >= PIPE_CAP_SZ)
					cap = PIPE_CAP_SZ - 1;
				cur_out[cap] = '\0';
				cur_in     = cur_out;
				cur_in_len = cap;
				cur_out    = (cur_out == bufa) ? bufb : bufa;
			}
		}

		g_pipe_stdin     = NULL;
		g_pipe_stdin_len = 0;
		anx_free(bufa);
		anx_free(bufb);
		return;
	}

	argc = parse_args(line_copy, argv, MAX_ARGS);
	if (argc > 0)
		dispatch(argc, argv);
}

void anx_shell_execute(const char *command)
{
	static bool history_loaded;

	if (!history_loaded) {
		history_loaded = true;
		history_load_from_disk();
	}
	if (command && command[0])
		history_add(command);
	execute_line(command);
}

void anx_shell_run(void)
{
	char line[MAX_LINE];
	char *argv[MAX_ARGS];
	int argc;

	history_load_from_disk();
	kputs("\nansh ready. Type 'help' for commands.\n\n");

	for (;;) {
		kputs("anx> ");
		kgetline(line, sizeof(line));

		if (line[0] == '\0')
			continue;

		history_add(line);

		/* Check for if/then/end construct */
		if (anx_strncmp(line, "if ", 3) == 0) {
			/* Simple: if $? == 0 then <cmd> end */
			/* Or multi-line: if <cond> then ... end */
			char *then_ptr = NULL;
			char *p = line + 3;

			/* Find 'then' keyword */
			while (*p) {
				if (anx_strncmp(p, "then ", 5) == 0 ||
				    anx_strncmp(p, "then\0", 5) == 0) {
					then_ptr = p + 5;
					break;
				}
				p++;
			}

			if (then_ptr) {
				/* Evaluate condition: "$? == 0" */
				bool condition = false;
				char *cond = line + 3;

				/* Trim spaces */
				while (*cond == ' ') cond++;

				if (anx_strncmp(cond, "$? == 0", 7) == 0)
					condition = (last_return_code == 0);
				else if (anx_strncmp(cond, "$? != 0", 7) == 0)
					condition = (last_return_code != 0);
				else
					condition = (last_return_code == 0);

				if (condition) {
					/* Strip trailing 'end' if present */
					char cmd[MAX_LINE];
					uint32_t cmd_len;

					anx_strlcpy(cmd, then_ptr, MAX_LINE);
					cmd_len = (uint32_t)anx_strlen(cmd);
					while (cmd_len > 0 &&
					       cmd[cmd_len-1] == ' ')
						cmd[--cmd_len] = '\0';
					if (cmd_len >= 3 &&
					    anx_strcmp(cmd + cmd_len - 3,
						      "end") == 0)
						cmd[cmd_len - 3] = '\0';

					/* Trim trailing spaces */
					cmd_len = (uint32_t)anx_strlen(cmd);
					while (cmd_len > 0 &&
					       cmd[cmd_len-1] == ' ')
						cmd[--cmd_len] = '\0';

					if (cmd[0])
						execute_line(cmd);
				}
			}
			continue;
		}

		argc = parse_args(line, argv, MAX_ARGS);
		dispatch(argc, argv);
	}
}
