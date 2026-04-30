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
#include <anx/list.h>
#include <anx/spinlock.h>

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
/* Semantic content node types                                          */
/* ------------------------------------------------------------------ */

enum anx_content_type {
	ANX_CONTENT_CANVAS,          /* raw pixel buffer (Wayland compat) */
	ANX_CONTENT_TEXT,            /* UTF-8 text, NUL-terminated in data */
	ANX_CONTENT_BUTTON,          /* activatable label, text in label[] */
	ANX_CONTENT_FORM,            /* structured input collection */
	ANX_CONTENT_VIEWPORT,        /* sub-surface region */
	ANX_CONTENT_AUDIO_STREAM,    /* PCM/compressed audio */
	ANX_CONTENT_SENSOR_STREAM,   /* live sensor data */
	ANX_CONTENT_ACTUATOR_TARGET, /* robot actuator control point */
	ANX_CONTENT_HAPTIC_PATTERN,  /* vibration/force-feedback spec */
	ANX_CONTENT_VOID,            /* headless / agent-only surface */
};

struct anx_content_node {
	enum anx_content_type    type;
	char                     label[128];   /* accessible name or button text */
	void                    *data;         /* type-specific payload */
	uint32_t                 data_len;
	struct anx_content_node *children;
	uint32_t                 child_count;
};

/* ------------------------------------------------------------------ */
/* Renderer engine classes                                              */
/* ------------------------------------------------------------------ */

/*
 * Renderer classes use values above ANX_ENGINE_RENDERER_BASE so they
 * never collide with the base anx_engine_class enum (values 0-8).
 */
#define ANX_ENGINE_RENDERER_BASE       0x100
#define ANX_ENGINE_RENDERER_GPU        (ANX_ENGINE_RENDERER_BASE + 0)
#define ANX_ENGINE_RENDERER_VOICE      (ANX_ENGINE_RENDERER_BASE + 1)
#define ANX_ENGINE_RENDERER_HAPTIC     (ANX_ENGINE_RENDERER_BASE + 2)
#define ANX_ENGINE_RENDERER_ROBOT      (ANX_ENGINE_RENDERER_BASE + 3)
#define ANX_ENGINE_RENDERER_HEADLESS   (ANX_ENGINE_RENDERER_BASE + 4)
#define ANX_ENGINE_RENDERER_COMPOSITOR (ANX_ENGINE_RENDERER_BASE + 5)

/* ------------------------------------------------------------------ */
/* Renderer operations (filled by each renderer implementation)        */
/* ------------------------------------------------------------------ */

struct anx_surface;  /* forward */

struct anx_renderer_ops {
	/* Called when a surface is mapped to this renderer. */
	int  (*map)(struct anx_surface *surf);
	/* Called to push committed content to the output medium. */
	int  (*commit)(struct anx_surface *surf);
	/* Called to mark a sub-region dirty (hint; commit follows). */
	void (*damage)(struct anx_surface *surf,
	               int32_t x, int32_t y, uint32_t w, uint32_t h);
	/* Called when a surface is unmapped or destroyed. */
	void (*unmap)(struct anx_surface *surf);
};

/* ------------------------------------------------------------------ */
/* Capability flags for Interface Plane operations                     */
/* ------------------------------------------------------------------ */

#define ANX_CAP_SURF_CREATE    (1u << 20)
#define ANX_CAP_SURF_MAP       (1u << 21)
#define ANX_CAP_SURF_DESTROY   (1u << 22)
#define ANX_CAP_INPUT_RECEIVE  (1u << 23)
#define ANX_CAP_INPUT_INJECT   (1u << 24)
#define ANX_CAP_COMPOSITOR     (1u << 25)
#define ANX_CAP_ENV_SWITCH     (1u << 26)
#define ANX_CAP_RENDERER_REG   (1u << 27)

struct anx_event;	/* forward — defined below in Event Objects section */

/* ------------------------------------------------------------------ */
/* Surface Object                                                       */
/* ------------------------------------------------------------------ */

#define ANX_SURF_MAX  256

struct anx_surface {
	anx_oid_t                oid;            /* globally unique ID */
	enum anx_surface_state   state;
	anx_cid_t                owner_cid;      /* cell that created this */
	anx_cid_t                compositor_cid; /* compositor cell managing it */

	/* Semantic content */
	struct anx_content_node *content_root;

	/* Renderer assignment */
	int                      renderer_class; /* ANX_ENGINE_RENDERER_* */
	const struct anx_renderer_ops *renderer_ops;

	/* Geometry (renderer-specific interpretation) */
	int32_t                  x, y;
	uint32_t                 width, height;
	uint32_t                 z_order;        /* higher = closer to front */

	/* P1-001: Dirty-rect damage tracking (surface-relative coordinates). */
	int32_t                  damage_x, damage_y;
	uint32_t                 damage_w, damage_h;
	bool                     damage_valid;   /* pending damage to render */
	uint32_t                 commit_count;   /* incremented on each committed frame */

	/* Hierarchy */
	anx_oid_t                parent_oid;
	struct anx_list_head     children;       /* child surfaces */

	/* Optional window title (set by creator; shown in menubar) */
	char                     title[64];

	/* Set by WM when user explicitly minimizes; cleared on restore.
	 * Distinguishes user-minimized from workspace-hidden surfaces. */
	bool                     user_minimized;

	/* Internal: hashtable chain and z-order list membership */
	struct anx_list_head     ht_node;
	struct anx_list_head     z_node;

	struct anx_spinlock      lock;

	/* Optional key event handler — called by WM dispatch for focused surface */
	void (*on_event)(struct anx_surface *surf, const struct anx_event *ev);
};

/* ------------------------------------------------------------------ */
/* Event Objects                                                        */
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
	ANX_EVENT_VOICE_INTENT,
	ANX_EVENT_VOICE_RAW,
	ANX_EVENT_SENSOR,
	ANX_EVENT_ACTUATOR_FEEDBACK,
	ANX_EVENT_FOCUS_ENTER,
	ANX_EVENT_FOCUS_LEAVE,
	ANX_EVENT_SURFACE_MAPPED,
	ANX_EVENT_SURFACE_DESTROYED,
};

/* Event priority levels for QoS — ordered from highest to lowest for integer comparison */
enum anx_event_prio {
	ANX_EVENT_PRIO_CRITICAL = 0,  /* input events — pointer, key, touch; also default for uninit */
	ANX_EVENT_PRIO_NORMAL = 1,     /* default for most events */
	ANX_EVENT_PRIO_LOW = 2,        /* cosmetic updates — surface damage hints */
	ANX_EVENT_PRIO_COUNT,          /* must be last */
	ANX_EVENT_PRIO_UNSET = -1,     /* sentinel for uninitialized (outside valid range) */
};

struct anx_event {
	anx_oid_t           oid;            /* event's own OID */
	enum anx_event_type type;
	enum anx_event_prio priority;      /* QoS priority */
	uint64_t            timestamp_ns;   /* posted timestamp */
	anx_oid_t           target_surf;    /* surface this event targets */
	anx_cid_t           source_cell;
	uint32_t            device_id;

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
#define ANX_ENV_MAX        8

struct anx_environment {
	char     name[ANX_ENV_NAME_MAX];
	char     schema[128];
	int      renderer_class;
	uint32_t input_device_mask;
	int      active;
};

/* ------------------------------------------------------------------ */
/* Interface Plane API                                                  */
/* ------------------------------------------------------------------ */

/* Subsystem init — call after anx_engine_registry_init() */
int anx_iface_init(void);

/* Surface management */
int anx_iface_surface_create(int renderer_class,
                              struct anx_content_node *root,
                              int32_t x, int32_t y,
                              uint32_t width, uint32_t height,
                              struct anx_surface **out);
int anx_iface_surface_map(struct anx_surface *surf);
int anx_iface_surface_damage(struct anx_surface *surf,
                              int32_t x, int32_t y, uint32_t w, uint32_t h);
int anx_iface_surface_commit(struct anx_surface *surf);
int anx_iface_surface_destroy(struct anx_surface *surf);

/* Surface queries */
int anx_iface_surface_list(anx_oid_t *oids_out, uint32_t max,
                            uint32_t *count_out);
/* Query the current accumulated damage rect for a surface. */
void anx_iface_surface_damage_query(struct anx_surface *surf,
                                     int32_t *x_out, int32_t *y_out,
                                     uint32_t *w_out, uint32_t *h_out,
                                     bool *valid_out);
int anx_iface_surface_lookup(anx_oid_t oid, struct anx_surface **out);

/* Event routing */
int anx_iface_event_post(struct anx_event *ev);
int anx_iface_event_subscribe(anx_oid_t surf_oid, anx_cid_t cell_cid);
int anx_iface_event_unsubscribe(anx_oid_t surf_oid, anx_cid_t cell_cid);
/* Poll for the next event addressed to cell_cid; returns ANX_ENOENT if empty */
int anx_iface_event_poll(anx_cid_t cell_cid, struct anx_event *out);
/* Poll for next WM-targeted event (target_surf == null) — used by anx_wm_run() */
int anx_iface_event_poll_wm(struct anx_event *out);
/* Poll for next event addressed to a specific surface OID — used by WM dispatch */
int anx_iface_event_poll_surf(anx_oid_t surf_oid, struct anx_event *out);

#define ANX_IFACE_EVENT_RING_SIZE 256u

/* Latency histogram buckets (nanoseconds) */
#define ANX_LAT_BUCKET_0_NS   1000000ULL   /* <1ms:     <1,000,000 ns */
#define ANX_LAT_BUCKET_1_NS   5000000ULL   /* 1-5ms:    1,000,000-5,000,000 ns */
#define ANX_LAT_BUCKET_2_NS  10000000ULL   /* 5-10ms:   5,000,000-10,000,000 ns */
#define ANX_LAT_BUCKET_3_NS  10000001ULL   /* >10ms:    >=10,000,001 ns */
#define ANX_LAT_BUCKETS       4

struct anx_iface_event_stats {
	uint64_t posted;
	uint64_t overflow_drops;
	uint32_t current_depth;               /* events currently in ring */
	uint64_t latency_histogram[ANX_LAT_BUCKETS]; /* <1ms, 1-5ms, 5-10ms, >10ms */
};

/* Resets event queue state (ring + subscriptions + counters). */
void anx_iface_event_reset(void);
void anx_iface_event_stats(struct anx_iface_event_stats *out);
void anx_iface_event_stats_full(struct anx_iface_event_stats *out);

/* Backpressure threshold configuration (fraction of ring size, 1-255). */
void anx_iface_event_set_backpressure_threshold(uint32_t fraction_of_ring);
uint32_t anx_iface_event_backpressure_threshold(void);

/* Renderer registration */
int       anx_iface_renderer_register(int renderer_class,
                                       const struct anx_renderer_ops *ops,
                                       const char *name);
int       anx_iface_renderer_unregister(int renderer_class);
const struct anx_renderer_ops *anx_iface_renderer_ops(int renderer_class);

/* Environment management */
int anx_iface_env_define(const char *name, const char *schema,
                          int renderer_class);
int anx_iface_env_activate(const char *env_name);
int anx_iface_env_deactivate(const char *env_name);
int anx_iface_env_query(const char *env_name, struct anx_environment *out);

/* ------------------------------------------------------------------ */
/* Renderer registration helpers (called by renderer_*.c at boot)      */
/* ------------------------------------------------------------------ */

int anx_renderer_gpu_register(void);
int anx_renderer_headless_register(void);

/* ------------------------------------------------------------------ */
/* Shared-memory IPC v0 (userspace <-> compositor)                     */
/* ------------------------------------------------------------------ */

#define ANX_IFACE_SHM_MAX            32u
#define ANX_IFACE_SHM_MAX_BYTES      (64u * 1024u * 1024u)
#define ANX_IFACE_SHM_RIGHT_PRODUCE  (1u << 0)
#define ANX_IFACE_SHM_RIGHT_CONSUME  (1u << 1)

/* Create a shared buffer owned by owner_cid. */
int anx_iface_shm_create(anx_cid_t owner_cid, uint32_t size_bytes, anx_oid_t *out_oid);
/* Owner grants rights to another cell. */
int anx_iface_shm_grant(anx_oid_t shm_oid, anx_cid_t owner_cid,
                        anx_cid_t grantee_cid, uint32_t rights_mask);
/* Map requires granted rights; returns raw pointer and size for zero-copy access. */
int anx_iface_shm_map(anx_oid_t shm_oid, anx_cid_t cell_cid, uint32_t required_right,
                      void **out_ptr, uint32_t *out_size, uint32_t *out_sequence);
/* Producer updates payload and bumps sequence. */
int anx_iface_shm_publish(anx_oid_t shm_oid, anx_cid_t producer_cid,
                          const void *data, uint32_t len, uint32_t *out_sequence);
/* Consumer copies latest payload + sequence. */
int anx_iface_shm_consume(anx_oid_t shm_oid, anx_cid_t consumer_cid,
                          void *out, uint32_t out_max,
                          uint32_t *out_len, uint32_t *out_sequence);

/* ------------------------------------------------------------------ */
/* Compositor support                                                   */
/* ------------------------------------------------------------------ */

#define ANX_IFACE_COMPOSITOR_MAX 4u

struct anx_iface_compositor_stats {
	char domain[ANX_ENV_NAME_MAX];
	anx_cid_t cell_cid;
	bool running;
	bool crashed;
	uint64_t repaint_cycles;
	uint64_t committed_surfaces;
	uint32_t last_cycle_commits;
	uint64_t last_cycle_ns;       /* wall time of last repaint cycle (ns) */
};

/* Start compositor cell for a domain. Exactly one per domain. */
int anx_iface_compositor_start(const char *domain);
/* Stop a running compositor cell for a domain. */
int anx_iface_compositor_stop(const char *domain);
/* Simulate compositor cell crash for recovery tests. */
int anx_iface_compositor_crash(const char *domain);
/* Restart crashed compositor cell while preserving surface registry. */
int anx_iface_compositor_restart(const char *domain);
/* Run one deterministic repaint cycle through compositor cell runtime. */
int anx_iface_compositor_tick(const char *domain, uint32_t *committed_out);
/* Query compositor cell state and counters. */
int anx_iface_compositor_stats(const char *domain, struct anx_iface_compositor_stats *out);

/* Legacy helper: repaint via visual-desktop compositor domain. */
int anx_iface_compositor_repaint(void);

/* Start a PIT-driven periodic compositor repaint at target_fps (≤100). */
void anx_iface_frame_scheduler_init(uint32_t target_fps);

/* ------------------------------------------------------------------ */
/* Wayland compatibility bridge                                         */
/* ------------------------------------------------------------------ */

/*
 * Wrap an existing pixel buffer as a CANVAS surface on the GPU renderer.
 * The caller retains ownership of pixels; surface must be destroyed before
 * the pixel buffer is freed.
 */
int anx_wayland_surface_wrap(void *pixels, uint32_t width, uint32_t height,
                              int32_t x, int32_t y,
                              struct anx_surface **out);

/* Move a surface to a new screen position without unmapping it. */
int anx_iface_surface_move(struct anx_surface *surf, int32_t x, int32_t y);

/* Raise surface to front of z-order (appears on top of all others). */
int anx_iface_surface_raise(struct anx_surface *surf);

/* Lower surface to back of z-order (appears behind all others). */
int anx_iface_surface_lower(struct anx_surface *surf);

/* Set the parent of child; child is repositioned just above parent in z-order. */
int anx_iface_surface_set_parent(struct anx_surface *child, anx_oid_t parent_oid);

/* Return topmost VISIBLE surface whose bounds contain (x, y), or NULL. */
struct anx_surface *anx_iface_surface_at(int32_t x, int32_t y);

/* Set the display title for a surface (shown in menubar, switcher, etc.). */
void anx_iface_surface_set_title(struct anx_surface *surf, const char *title);

#endif /* ANX_INTERFACE_PLANE_H */
