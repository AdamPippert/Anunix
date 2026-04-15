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

/* --- Line input --- */

#define MAX_LINE	256
#define MAX_ARGS	16

static void kputs(const char *s)
{
	kprintf("%s", s);
}

static int kgetline(char *buf, size_t size)
{
	size_t pos = 0;

	while (pos < size - 1) {
		int c = arch_console_getc();

		if (c < 0)
			break;

		if (c == '\r' || c == '\n') {
			arch_console_putc('\r');
			arch_console_putc('\n');
			break;
		}

		if (c == 0x7F || c == '\b') {
			/* Backspace */
			if (pos > 0) {
				pos--;
				kputs("\b \b");
			}
			continue;
		}

		if (c == 0x03) {
			/* Ctrl-C */
			kputs("^C\n");
			pos = 0;
			break;
		}

		if (c >= 0x20 && c < 0x7F) {
			buf[pos++] = (char)c;
			arch_console_putc((char)c);
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

		argv[argc++] = line;

		/* Find end of token */
		while (*line && *line != ' ' && *line != '\t')
			line++;
		if (*line)
			*line++ = '\0';
	}

	return argc;
}

/* --- Command handlers --- */

static void cmd_help(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	kputs("Anunix kernel monitor\n\n");
	kputs("  help                       Show this help\n");
	kputs("  version                    Show kernel version\n");
	kputs("  mem stats                  Page allocator statistics\n");
	kputs("  state create [type]        Create a state object\n");
	kputs("  state show <oid-prefix>    Show object details\n");
	kputs("  state seal <oid-prefix>    Seal an object\n");
	kputs("  state delete <oid-prefix>  Delete an object\n");
	kputs("  cell create <name>         Create an execution cell\n");
	kputs("  cell run <cid-prefix>      Run a cell through pipeline\n");
	kputs("  cell show <cid-prefix>     Show cell details\n");
	kputs("  memplane admit <oid-pfx>   Admit object to memory plane\n");
	kputs("  memplane show <oid-pfx>    Show memory entry\n");
	kputs("  engine register <name>     Register a local tool engine\n");
	kputs("  engine list                List registered engines\n");
	kputs("  cap create <name>          Create a capability (draft)\n");
	kputs("  cap list                   List capabilities\n");
	kputs("  sched status               Show scheduler queue depths\n");
	kputs("  net status                 Show network plane status\n");
	kputs("  halt                       Halt the system\n");
}

static void cmd_version(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	kprintf("Anunix 0.1.0 (kernel monitor)\n");
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
				kprintf("  %s  %s v%s  %s\n",
					buf, cap->name, cap->version,
					cap_status_name(cap->status));
			}
		}
	} else {
		kputs("usage: cap <create|list>\n");
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

/* --- Command dispatch --- */

static void dispatch(int argc, char **argv)
{
	if (argc == 0)
		return;

	if (anx_strcmp(argv[0], "help") == 0 ||
	    anx_strcmp(argv[0], "?") == 0) {
		cmd_help(argc, argv);
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
	} else if (anx_strcmp(argv[0], "halt") == 0) {
		kputs("halting system\n");
		arch_halt();
	} else {
		kprintf("unknown command: %s (type 'help')\n", argv[0]);
	}
}

/* --- Shell main loop --- */

void anx_shell_run(void)
{
	char line[MAX_LINE];
	char *argv[MAX_ARGS];
	int argc;

	kputs("\nAnunix kernel monitor ready. Type 'help' for commands.\n\n");

	for (;;) {
		kputs("anx> ");
		kgetline(line, sizeof(line));

		if (line[0] == '\0')
			continue;

		argc = parse_args(line, argv, MAX_ARGS);
		dispatch(argc, argv);
	}
}
