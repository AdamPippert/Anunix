/*
 * iface_tools.c — Interface Plane CLI tools.
 *
 * surfctl  — manage surfaces (list, create, destroy, commit)
 * evctl    — inspect and inject events
 * compctl  — compositor control (repaint, focus)
 * envctl   — manage environments (define, activate, query)
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static const char *surf_state_str(enum anx_surface_state s)
{
	switch (s) {
	case ANX_SURF_CREATED:   return "created";
	case ANX_SURF_MAPPED:    return "mapped";
	case ANX_SURF_VISIBLE:   return "visible";
	case ANX_SURF_OCCLUDED:  return "occluded";
	case ANX_SURF_MINIMIZED: return "minimized";
	case ANX_SURF_DESTROYED: return "destroyed";
	default:                 return "?";
	}
}

static const char *renderer_str(int cls)
{
	switch (cls) {
	case ANX_ENGINE_RENDERER_GPU:        return "gpu";
	case ANX_ENGINE_RENDERER_VOICE:      return "voice";
	case ANX_ENGINE_RENDERER_HAPTIC:     return "haptic";
	case ANX_ENGINE_RENDERER_ROBOT:      return "robot";
	case ANX_ENGINE_RENDERER_HEADLESS:   return "headless";
	case ANX_ENGINE_RENDERER_COMPOSITOR: return "compositor";
	default:                             return "?";
	}
}

static void print_oid_short(anx_oid_t oid)
{
	/* Print last 8 hex digits of .lo — enough to distinguish surfaces */
	kprintf("%08x", (uint32_t)(oid.lo & 0xFFFFFFFF));
}

/* ------------------------------------------------------------------ */
/* surfctl                                                              */
/* ------------------------------------------------------------------ */

static void surfctl_usage(void)
{
	kprintf("%s", "usage: surfctl <subcommand>\n");
	kprintf("%s", "  list                     list all surfaces\n");
	kprintf("%s", "  show <oid-prefix>        show surface details\n");
	kprintf("%s", "  commit <oid-prefix>      force commit (repaint) a surface\n");
	kprintf("%s", "  destroy <oid-prefix>     destroy a surface\n");
	kprintf("%s", "  headless <w> <h>         create a headless test surface\n");
}

void
cmd_surfctl(int argc, char **argv)
{
	if (argc < 2) {
		surfctl_usage();
		return;
	}

	if (anx_strcmp(argv[1], "list") == 0) {
		anx_oid_t oids[ANX_SURF_MAX];
		uint32_t count, i;
		int rc;

		rc = anx_iface_surface_list(oids, ANX_SURF_MAX, &count);
		if (rc != ANX_OK) {
			kprintf("surfctl: list failed (%d)\n", rc);
			return;
		}
		if (count == 0) {
			kprintf("%s", "no surfaces\n");
			return;
		}
		kprintf("%-10s  %-10s  %-9s  %s\n",
		        "OID", "RENDERER", "STATE", "GEOMETRY");
		for (i = 0; i < count; i++) {
			struct anx_surface *s;
			if (anx_iface_surface_lookup(oids[i], &s) != ANX_OK)
				continue;
			print_oid_short(s->oid);
			kprintf("  %-10s  %-9s  %dx%d+%d+%d z=%u\n",
			        renderer_str(s->renderer_class),
			        surf_state_str(s->state),
			        (int)s->width, (int)s->height,
			        (int)s->x,     (int)s->y,
			        (unsigned)s->z_order);
		}
		return;
	}

	if (anx_strcmp(argv[1], "commit") == 0) {
		/* Trigger a full compositor repaint */
		int n = anx_iface_compositor_repaint();
		kprintf("committed %d surface(s)\n", n);
		return;
	}

	if (anx_strcmp(argv[1], "headless") == 0) {
		struct anx_surface *surf;
		struct anx_content_node node;
		uint32_t w = 320, h = 240;
		int rc;

		if (argc >= 4) {
			uint32_t v = 0;
			const char *p;
			for (p = argv[2]; *p >= '0' && *p <= '9'; p++)
				v = v * 10 + (uint32_t)(*p - '0');
			w = v;
			v = 0;
			for (p = argv[3]; *p >= '0' && *p <= '9'; p++)
				v = v * 10 + (uint32_t)(*p - '0');
			h = v;
		}

		anx_memset(&node, 0, sizeof(node));
		node.type = ANX_CONTENT_VOID;

		rc = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS,
		                               &node, 0, 0, w, h, &surf);
		if (rc != ANX_OK) {
			kprintf("surfctl: create failed (%d)\n", rc);
			return;
		}
		rc = anx_iface_surface_map(surf);
		if (rc != ANX_OK) {
			kprintf("surfctl: map failed (%d)\n", rc);
			return;
		}
		kprintf("%s", "created headless surface ");
		print_oid_short(surf->oid);
		kprintf("%s", "\n");
		return;
	}

	surfctl_usage();
}

/* ------------------------------------------------------------------ */
/* evctl                                                                */
/* ------------------------------------------------------------------ */

static void evctl_usage(void)
{
	kprintf("%s", "usage: evctl <subcommand>\n");
	kprintf("%s", "  focus                    show focused surface\n");
	kprintf("%s", "  inject-key <keycode>     inject a key-down+up event\n");
}

void
cmd_evctl(int argc, char **argv)
{
	if (argc < 2) {
		evctl_usage();
		return;
	}

	if (anx_strcmp(argv[1], "focus") == 0) {
		anx_oid_t f = anx_input_focus_get();
		if (f.hi == 0 && f.lo == 0) {
			kprintf("%s", "no focused surface\n");
		} else {
			kprintf("%s", "focused: ");
			print_oid_short(f);
			kprintf("%s", "\n");
		}
		return;
	}

	if (anx_strcmp(argv[1], "inject-key") == 0 && argc >= 3) {
		uint32_t keycode = 0;
		const char *p = argv[2];
		while (*p >= '0' && *p <= '9')
			keycode = keycode * 10 + (uint32_t)(*p++ - '0');
		anx_input_key_down(keycode, 0, 0);
		anx_input_key_up(keycode, 0);
		kprintf("injected key 0x%02x\n", (unsigned)keycode);
		return;
	}

	evctl_usage();
}

/* ------------------------------------------------------------------ */
/* compctl                                                              */
/* ------------------------------------------------------------------ */

static void compctl_usage(void)
{
	kprintf("%s", "usage: compctl <subcommand>\n");
	kprintf("%s", "  repaint                  force full compositor repaint\n");
	kprintf("%s", "  focus <oid-prefix>       (not yet implemented)\n");
}

void
cmd_compctl(int argc, char **argv)
{
	if (argc < 2) {
		compctl_usage();
		return;
	}

	if (anx_strcmp(argv[1], "repaint") == 0) {
		int n = anx_iface_compositor_repaint();
		kprintf("repainted %d surface(s)\n", n);
		return;
	}

	compctl_usage();
}

/* ------------------------------------------------------------------ */
/* envctl                                                               */
/* ------------------------------------------------------------------ */

static void envctl_usage(void)
{
	kprintf("%s", "usage: envctl <subcommand>\n");
	kprintf("%s", "  list                     list defined environments\n");
	kprintf("%s", "  activate <name>          activate an environment\n");
	kprintf("%s", "  deactivate <name>        deactivate an environment\n");
	kprintf("%s", "  define <name> <schema> <renderer>  define a new environment\n");
}

void
cmd_envctl(int argc, char **argv)
{
	if (argc < 2) {
		envctl_usage();
		return;
	}

	if (anx_strcmp(argv[1], "list") == 0) {
		/* Query known environments by trying each name in the env table.
		 * The API only exposes query-by-name, so enumerate via known list. */
		const char *names[] = {
			"visual-desktop", "headless-agent",
			"voice", "haptic", "robot", NULL
		};
		int i;
		kprintf("%-24s  %-40s  %-10s  %s\n",
		        "NAME", "SCHEMA", "RENDERER", "ACTIVE");
		for (i = 0; names[i]; i++) {
			struct anx_environment env;
			if (anx_iface_env_query(names[i], &env) != ANX_OK)
				continue;
			kprintf("%-24s  %-40s  %-10s  %s\n",
			        env.name, env.schema,
			        renderer_str(env.renderer_class),
			        env.active ? "yes" : "no");
		}
		return;
	}

	if (anx_strcmp(argv[1], "activate") == 0 && argc >= 3) {
		int rc = anx_iface_env_activate(argv[2]);
		if (rc != ANX_OK)
			kprintf("envctl: activate failed (%d)\n", rc);
		else
			kprintf("activated: %s\n", argv[2]);
		return;
	}

	if (anx_strcmp(argv[1], "deactivate") == 0 && argc >= 3) {
		int rc = anx_iface_env_deactivate(argv[2]);
		if (rc != ANX_OK)
			kprintf("envctl: deactivate failed (%d)\n", rc);
		else
			kprintf("deactivated: %s\n", argv[2]);
		return;
	}

	if (anx_strcmp(argv[1], "define") == 0 && argc >= 5) {
		int renderer_cls = ANX_ENGINE_RENDERER_HEADLESS;
		if (anx_strcmp(argv[4], "gpu") == 0)
			renderer_cls = ANX_ENGINE_RENDERER_GPU;
		else if (anx_strcmp(argv[4], "voice") == 0)
			renderer_cls = ANX_ENGINE_RENDERER_VOICE;
		else if (anx_strcmp(argv[4], "haptic") == 0)
			renderer_cls = ANX_ENGINE_RENDERER_HAPTIC;
		else if (anx_strcmp(argv[4], "robot") == 0)
			renderer_cls = ANX_ENGINE_RENDERER_ROBOT;

		int rc = anx_iface_env_define(argv[2], argv[3], renderer_cls);
		if (rc != ANX_OK)
			kprintf("envctl: define failed (%d)\n", rc);
		else
			kprintf("defined: %s\n", argv[2]);
		return;
	}

	envctl_usage();
}
