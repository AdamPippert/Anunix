/*
 * anx/interface_plane.h — Interface Plane (RFC-0012).
 *
 * Kernel-level abstraction for all interactive environments.
 * Surfaces are State Objects. Events flow through the Routing Plane.
 * Renderers are registered Engine classes. The compositor is a Cell.
 *
 * Medium-agnostic: the same surface model covers visual windowing,
 * voice interaction, robotics, tactile smartphone, and headless agents.
 */

#ifndef ANX_INTERFACE_PLANE_H
#define ANX_INTERFACE_PLANE_H

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/engine.h>
#include <anx/route.h>

/* ------------------------------------------------------------------ */
/* Surface lifecycle states                                             */
/* ------------------------------------------------------------------ */

enum anx_surface_state {
	ANX_SURF_CREATED,       /* allocated, not yet mapped to a renderer */
	ANX_SURF_MAPPED,        /* assigned to a renderer, not yet visible */
	ANX_SURF_VISIBLE,       /* actively rendered */
	ANX_SURF_OCCLUDED,      /* rendered but obscured by another surface */
	ANX_SURF_MINIMIZED,     /* withdrawn from renderer, state preserved */
	ANX_SURF_DESTROYED,     /* lifecycle complete */
};

/* ------------------------------------------------------------------ */
/* Semantic content node types (surface content tree)                  */
/* ------------------------------------------------------------------ */

enum anx_content_type {
	ANX_CONTENT_CANVAS,         /* raw pixel/GPU buffer (Wayland compat) */
	ANX_CONTENT_TEXT,           /* unicode text with optional markup */
	ANX_CONTENT_BUTTON,         /* activatable control */
	ANX_CONTENT_FORM,           /* structured input collection */
	ANX_CONTENT_VIEWPORT,       /* sub-surface region */
	ANX_CONTENT_AUDIO_STREAM,   /* PCM/compressed audio */
	ANX_CONTENT_SENSOR_STREAM,  /* live sensor data (camera, lidar, etc.) */
	ANX_CONTENT_ACTUATOR_TARGET, /* robot actuator control point */
	ANX_CONTENT_HAPTIC_PATTERN, /* vibration/force-feedback spec */
	ANX_CONTENT_VOID,           /* headless / agent-only surface */
};

/* ------------------------------------------------------------------ */
/* Renderer engine classes (registered in the Engine Registry)         */
/* ------------------------------------------------------------------ */

/*
 * Renderer engine classes extend enum anx_engine_class (engine.h).
 * Values start at ANX_ENGINE_RENDERER_BASE to avoid collision.
 */
#define ANX_ENGINE_RENDERER_BASE     0x100

#define ANX_ENGINE_RENDERER_GPU      (ANX_ENGINE_RENDERER_BASE + 0)
#define ANX_ENGINE_RENDERER_VOICE    (ANX_ENGINE_RENDERER_BASE + 1)
#define ANX_ENGINE_RENDERER_HAPTIC   (ANX_ENGINE_RENDERER_BASE + 2)
#define ANX_ENGINE_RENDERER_ROBOT    (ANX_ENGINE_RENDERER_BASE + 3)
#define ANX_ENGINE_RENDERER_HEADLESS (ANX_ENGINE_RENDERER_BASE + 4)
#define ANX_ENGINE_RENDERER_COMPOSITOR (ANX_ENGINE_RENDERER_BASE + 5)

/* ------------------------------------------------------------------ */
/* Capability flags for Interface Plane operations                     */
/* ------------------------------------------------------------------ */

#define ANX_CAP_SURF_CREATE    (1 << 20)  /* create new surfaces */
#define ANX_CAP_SURF_MAP       (1 << 21)  /* map surface to renderer */
#define ANX_CAP_SURF_DESTROY   (1 << 22)  /* destroy any surface */
#define ANX_CAP_INPUT_RECEIVE  (1 << 23)  /* receive input events */
#define ANX_CAP_INPUT_INJECT   (1 << 24)  /* inject synthetic events */
#define ANX_CAP_COMPOSITOR     (1 << 25)  /* register as compositor */
#define ANX_CAP_ENV_SWITCH     (1 << 26)  /* switch active environment */
#define ANX_CAP_RENDERER_REG   (1 << 27)  /* register a renderer engine */

/* ------------------------------------------------------------------ */
/* Surface Object                                                       */
/* ------------------------------------------------------------------ */

struct anx_content_node {
	enum anx_content_type   type;
	char                    label[128];    /* accessible name */
	void                   *data;          /* type-specific payload */
	uint32_t                data_len;
	struct anx_content_node *children;
	uint32_t                child_count;
};

struct anx_surface {
	anx_oid_t               oid;           /* State Object OID */
	enum anx_surface_state  state;
	anx_cid_t               owner_cid;     /* cell that created this surface */
	anx_cid_t               compositor_cid;/* compositor cell managing it */

	/* Semantic content tree */
	struct anx_content_node *content_root;

	/* Renderer assignment */
	int                     renderer_class; /* ANX_ENGINE_RENDERER_* */
	anx_eid_t               renderer_eid;  /* assigned renderer engine ID */

	/* Geometry (renderer-specific interpretation) */
	int32_t                 x, y;
	uint32_t                width, height;
	uint32_t                z_order;

	/* Hierarchy */
	anx_oid_t               parent_oid;    /* parent surface, or zero */
	struct anx_list         children;

	/* Synchronization */
	struct anx_spinlock     lock;
};

/* ------------------------------------------------------------------ */
/* Event Objects (ANX_OBJ_EVENT, routed via Routing Plane)             */
/* ------------------------------------------------------------------ */

enum anx_event_type {
	ANX_EVENT_POINTER_MOVE,
	ANX_EVENT_POINTER_BUTTON,
	ANX_EVENT_POINTER_SCROLL,
	ANX_EVENT_KEY_DOWN,
	ANX_EVENT_KEY_UP,
	ANX_EVENT_TOUCH_BEGIN,
	ANX_EVENT_TOUCH_MOVE,
	ANX_EVENT_TOUCH_END,
	ANX_EVENT_GESTURE,
	ANX_EVENT_VOICE_INTENT,     /* parsed voice command */
	ANX_EVENT_VOICE_RAW,        /* raw audio segment */
	ANX_EVENT_SENSOR,           /* generic sensor reading */
	ANX_EVENT_ACTUATOR_FEEDBACK,/* robot joint/motor feedback */
	ANX_EVENT_FOCUS_ENTER,
	ANX_EVENT_FOCUS_LEAVE,
	ANX_EVENT_SURFACE_MAPPED,
	ANX_EVENT_SURFACE_DESTROYED,
};

struct anx_event {
	anx_oid_t           oid;            /* State Object OID */
	enum anx_event_type type;
	uint64_t            timestamp_ns;   /* kernel monotonic time */
	anx_oid_t           target_surf;    /* surface this event is for */
	anx_cid_t           source_cell;    /* cell that produced the event */
	uint32_t            device_id;      /* input device or sensor ID */

	union {
		struct { int32_t x, y; uint32_t buttons; uint32_t modifiers; } pointer;
		struct { uint32_t keycode; uint32_t modifiers; uint32_t unicode; } key;
		struct { int32_t x, y, id; uint32_t pressure; }                   touch;
		struct { char intent[256]; float confidence; }                     voice;
		struct { uint32_t sensor_id; float values[8]; }                    sensor;
		struct { uint32_t joint_id; float position; float torque; }        actuator;
		uint8_t raw[256];
	} data;
};

/* ------------------------------------------------------------------ */
/* Environment Profile                                                  */
/* ------------------------------------------------------------------ */

#define ANX_ENV_NAME_MAX  64

struct anx_environment {
	char                name[ANX_ENV_NAME_MAX];  /* e.g. "visual-desktop" */
	char                schema[128];             /* e.g. "anx:env/visual-desktop/v1" */
	int                 renderer_class;          /* primary renderer */
	anx_eid_t           compositor_eid;          /* active compositor */
	uint32_t            input_device_mask;       /* bound input devices */
	anx_cid_t           manager_cid;             /* cell managing this env */
	int                 active;
};

/* ------------------------------------------------------------------ */
/* Interface Plane API                                                  */
/* ------------------------------------------------------------------ */

/* Surface management */
int anx_iface_surface_create(struct anx_cell *owner, int renderer_class,
                              struct anx_content_node *root,
                              struct anx_surface **out);
int anx_iface_surface_map(struct anx_surface *surf, anx_eid_t renderer_eid);
int anx_iface_surface_damage(struct anx_surface *surf,
                              int32_t x, int32_t y,
                              uint32_t w, uint32_t h);
int anx_iface_surface_commit(struct anx_surface *surf);
int anx_iface_surface_destroy(struct anx_surface *surf);

/* Event routing */
int anx_iface_event_post(struct anx_event *ev);
int anx_iface_event_subscribe(anx_oid_t surf_oid, anx_cid_t cell_cid);
int anx_iface_event_unsubscribe(anx_oid_t surf_oid, anx_cid_t cell_cid);

/* Renderer registration */
int anx_iface_renderer_register(int renderer_class, anx_eid_t eid,
                                 const char *name);
int anx_iface_renderer_unregister(anx_eid_t eid);
anx_eid_t anx_iface_renderer_find(int renderer_class);

/* Environment management */
int anx_iface_env_activate(const char *env_name);
int anx_iface_env_deactivate(const char *env_name);
int anx_iface_env_query(const char *env_name, struct anx_environment *out);

/* Surface tree query */
int anx_iface_surface_list(anx_oid_t *oids_out, uint32_t max, uint32_t *count_out);
int anx_iface_surface_lookup(anx_oid_t oid, struct anx_surface **out);

/* ------------------------------------------------------------------ */
/* Wayland compatibility bridge                                         */
/* ------------------------------------------------------------------ */

/*
 * anx_wayland_surface_wrap() bridges an existing Wayland pixel buffer
 * into the Interface Plane as a CANVAS content surface with RENDERER_GPU.
 * Called by the Wayland compat layer in userland when a wl_surface
 * commits a buffer.
 */
int anx_wayland_surface_wrap(void *wl_buffer, uint32_t width, uint32_t height,
                              anx_cid_t owner_cid,
                              struct anx_surface **out);

#endif /* ANX_INTERFACE_PLANE_H */
