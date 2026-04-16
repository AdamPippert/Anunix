
# RFC-0012: Interface Plane

| Field      | Value                                                                 |
|------------|-----------------------------------------------------------------------|
| RFC        | 0012                                                                  |
| Title      | Interface Plane — Kernel-Level Abstraction for Interactive Environments |
| Author     | Adam Pippert                                                          |
| Status     | Draft                                                                 |
| Created    | 2026-04-16                                                            |
| Updated    | 2026-04-16                                                            |
| Depends On | RFC-0001, RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0006, RFC-0007, RFC-0008 |

---

## Executive Summary

Every preceding RFC in Anunix has concerned itself with the interior of computation: how state is stored (RFC-0002), how work is executed (RFC-0003), how memory is tiered (RFC-0004), how execution is routed (RFC-0005), how the system extends across networks (RFC-0006), how operational knowledge is captured (RFC-0007), and how secrets are managed (RFC-0008). None of them has defined how the system presents itself to users, agents, or the physical world — the boundary where computation meets interaction.

RFC-0012 introduces the **Interface Plane**, a kernel-managed abstraction for all interactive environments. Its central claim is that an interactive surface is not a framebuffer. It is a typed, policy-governed, provenance-tracked State Object with semantic content: a **Surface Object**. The pixel grid rendered on an OLED panel, the speech synthesized from text, the vibration pattern sent to a haptic actuator, and the motor command issued to a robot arm are all rendering outcomes — different projections of the same underlying surface model onto different physical substrates.

This departure from Wayland's framebuffer-centric model is not merely aesthetic. It is required for an AI-native operating system where agents must programmatically create, inspect, modify, and compose interactive environments across modalities that cannot all be expressed as pixel buffers. An agent that transitions a session from visual to voice cannot do so by manipulating framebuffers; it needs a medium-agnostic semantic layer that renders differently depending on which environment is active.

The Interface Plane introduces four new kernel primitives: **Surface Objects** (`ANX_OBJ_SURFACE`), **Event Objects** (`ANX_OBJ_EVENT`), **Renderer Engine classes** (`ANX_RENDERER_*`), and **Environment Profiles** (`anx:env/*`). It defines **Compositor Cells** — Execution Cells, not privileged processes — as the orchestration mechanism for surface ordering, focus management, and frame scheduling. It provides a full **Wayland compatibility layer** so that existing visual applications continue to work. And it defines the **Agent API** that makes interactive environments first-class programmable surfaces rather than graphical side effects.

The design is protocol-composable in the tradition of Wayland (multiple compositor implementations, clean separation between surface clients and renderers), agent-native in the tradition of Anunix (every surface is a State Object, every event flows through the routing plane, every environment transition is auditable), and medium-agnostic in its fundamental model (the same surface tree drives visual, voice, haptic, robotic, and headless environments).

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert / public collaborators
**Depends on:** RFC-0001, RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0006, RFC-0007, RFC-0008
**Blocks:** None currently

---

## 2. Problem Statement

### 2.1 The Missing Interaction Layer

Anunix replaces files with State Objects, processes with Execution Cells, and disk I/O with tiered memory. Every major UNIX abstraction has an Anunix analog except one: the interactive environment. Classical systems treat the interface as either a framebuffer device (X11, Wayland) or a terminal (TTY, PTY). Neither is adequate for an AI-native operating system.

A framebuffer-first model forces every interactive surface into the pixel domain. Voice interactions have no natural representation as pixel buffers. Tactile feedback for a smartphone is not a texture. A robot's actuator state is not a window. When the kernel's interface model is "surfaces are framebuffers," every non-visual modality becomes a second-class citizen requiring out-of-band plumbing.

A terminal-first model is worse: it is a throwback to an era before graphics, before touch, before voice, and before the idea that software could speak, listen, feel, or move. Terminals remain valuable for text-heavy agents and CLI tools, but they are not a general interface model.

### 2.2 Why Wayland-as-Foundation Is Insufficient

Wayland is an excellent protocol for pixel-composited visual environments. Its clean client/server separation, damage-tracking model, and multiple compositor implementations make it architecturally superior to X11 for what it is designed to do. But:

1. **Surfaces are pixel buffers by definition.** The Wayland surface model (`wl_surface`) is a shared-memory pixel buffer with a damage region. There is no first-class concept of a semantic content tree. An agent cannot ask "what does this button do?" in Wayland; it can only read pixels.

2. **Events are opaque structs, not routable objects.** Wayland events (`wl_pointer`, `wl_keyboard`, `wl_touch`) are C structs marshaled over a Unix socket. They have no provenance, no policy, no routing semantics. An agent cannot subscribe to events for a surface it owns without running code inside the Wayland client.

3. **The compositor is a privileged process.** Wayland compositors (Weston, Mutter, KWin) are monolithic userland processes with special kernel access via DRM/KMS. There is no composable model: you cannot have two compositors coexist or swap compositors without a full display server restart.

4. **No medium-agnostic model.** Wayland was designed for displays. There is no path from Wayland to voice synthesis, haptic output, or robotics without a complete reimplementation.

The Interface Plane is Wayland-inspired in its protocol composability but Anunix-native in its object model, routing integration, and medium abstraction. Wayland compatibility is provided through an explicit compat layer, not through Wayland-as-foundation.

### 2.3 The Agent-Interaction Gap

RFC-0011 introduced hardware discovery agents and agent-native utilities. Agents can probe hardware, create State Objects, spawn child cells, and install Capability Objects. But they cannot programmatically create a user-visible interface. If an agent wants to present information to a user, it must either write to stdout (terminal-only, no structure) or rely on application-level GUI frameworks (not kernel-integrated, not provenance-tracked, not medium-agnostic).

An AI-native operating system must allow agents to:
- Create surfaces with semantic content, not pixel buffers
- Subscribe to user interaction events as routed State Objects
- Inspect the full surface tree of the current environment
- Transition environments (visual to voice, voice to robotics) programmatically
- Register new renderer engines for novel interaction modalities

None of this is possible without a first-class Interface Plane.

---

## 3. Goals

### 3.1 Primary Goals

1. **Semantic surfaces as first-class objects**
   Define `ANX_OBJ_SURFACE` as a new State Object type with a typed semantic content tree, not a pixel buffer. Pixel rendering is one possible renderer output, not the model.

2. **Medium-agnostic interaction model**
   The same Surface Object and Event Object model must work correctly across GPU-rendered visual environments, voice environments, haptic environments, robotic environments, and headless agent environments.

3. **Compositor as Execution Cell**
   Compositor logic must be implemented as an Execution Cell, not a privileged process. Multiple compositor implementations must be able to coexist. Compositors are routable, traceable, and replaceable.

4. **Events as routable State Objects**
   Input events must flow through the Routing Plane as typed State Objects with provenance, policy, and routing semantics. Agents must be able to subscribe to events for surfaces they own or are permitted to observe.

5. **Agent-native interface API**
   Agents must be able to create, inspect, modify, and destroy surfaces programmatically using the same Execution Cell and State Object model used for all other Anunix operations.

6. **Wayland compatibility**
   Existing Wayland applications must run without modification through a compat layer that maps Wayland pixel-buffer surfaces to `ANX_OBJ_SURFACE` objects with `ANX_RENDERER_GPU` renderer hints.

7. **Environment profiles**
   Named environment configurations must allow the system to be reconfigured for different interaction modalities (visual, voice, robotics, headless) through a first-class kernel mechanism.

8. **Security with capability-based access**
   Surface creation, event subscription, compositor control, and environment transitions must require explicit kernel-enforced capability claims, not ambient process authority.

### 3.2 Non-Goals

1. Implementing a full GPU driver stack (that belongs in `kernel/drivers/`).
2. Replacing the kernel framebuffer (`anx_fb`) for early-boot output.
3. Defining application-level widget toolkits or GUI frameworks (userland concerns).
4. Building a Wayland compositor from scratch (the compat layer wraps existing Wayland surfaces, not reimplements the compositor).
5. Guaranteeing real-time latency for all renderer classes in the first implementation.

---

## 4. Core Definitions

### 4.1 Interface Plane

The **Interface Plane** is the kernel subsystem that manages Surface Objects, Event Objects, Renderer Engines, Compositor Cells, and Environment Profiles. It provides the kernel-level abstraction for all interactive environments regardless of physical modality.

### 4.2 Surface Object

A **Surface Object** (`ANX_OBJ_SURFACE`) is a State Object representing an interactive surface. Its payload is a typed semantic content tree — not pixel data. It carries a render target hint, interaction policy, z-order, parent surface reference, and lifecycle state. Rendering to a specific medium is the responsibility of a Renderer Engine, not the surface itself.

### 4.3 Event Object

An **Event Object** (`ANX_OBJ_EVENT`) is a State Object representing an interaction event. Events flow through the Routing Plane from input sources (hardware, agents, synthesized input) to surface clients. Events carry full provenance: originating device, source cell, target surface, and timestamp.

### 4.4 Renderer Engine

A **Renderer Engine** is an Engine (in the sense of RFC-0005) registered in the Engine Registry under a renderer-class. Renderer Engines accept Surface Objects and produce physical output appropriate to their medium (pixels, speech, haptic patterns, motor commands, or no output for headless).

### 4.5 Compositor Cell

A **Compositor Cell** is an Execution Cell (`ANX_CELL_TASK_EXECUTION`) with compositor intent. It manages surface ordering (z-order), focus policy, damage tracking, and frame scheduling for a set of Surface Objects and Renderer Engines. Multiple Compositor Cells can run concurrently for different display domains.

### 4.6 Environment Profile

An **Environment Profile** is a named, kernel-managed configuration binding a set of Renderer Engines, input devices, and a compositor policy into a coherent interactive environment. Profiles can be activated, stacked, and transitioned. Examples: `anx:env/visual-desktop`, `anx:env/voice`, `anx:env/headless-agent`.

### 4.7 Content Node

A **Content Node** is a typed element in a Surface Object's semantic content tree. Content nodes have types (`text`, `button`, `canvas`, `viewport`, `sensor-stream`, `actuator-target`, `audio-stream`, etc.) and associated semantic properties. They are the unit of interaction for non-GPU renderers.

### 4.8 Surface Client

A **Surface Client** is an Execution Cell that owns or operates one or more Surface Objects. Surface clients communicate with the Compositor Cell through Surface Objects and Event Objects in the Routing Plane, not through a direct IPC channel.

---

## 5. Design Principles

### 5.1 Surfaces Are Semantic, Not Pixel
A surface's content model is a typed semantic tree. Rendering to pixels is one projection of that tree, not the definition of the surface.

### 5.2 Events Are State Objects, Not Structs
Every input event is a State Object with provenance, type, policy, and routing semantics. Events do not flow through raw kernel queues or Unix sockets; they flow through the Routing Plane.

### 5.3 The Compositor Is a Cell, Not a Process
Compositor logic is an Execution Cell. It has no special kernel privileges beyond what its capability claims permit. Compositor implementations are swappable without a display server restart.

### 5.4 Renderers Are Engines
GPU renderers, voice synthesizers, haptic drivers, and robot actuator controllers are all Renderer Engines registered in the Engine Registry. The routing plane selects renderers for surfaces using the same scoring logic it uses for all other engines.

### 5.5 Medium-Agnosticism Is a Hard Requirement
Every design decision in the Interface Plane must be compatible with all renderer classes. No design decision may embed assumptions that only make sense for pixel-based display.

### 5.6 Wayland Compatibility Is a Compat Layer, Not a Foundation
Wayland applications work through a well-defined translation layer that wraps pixel-buffer surfaces as `ANX_OBJ_SURFACE` objects with `ANX_RENDERER_GPU` hints. This layer is not the fundamental model.

### 5.7 Agent Access Is First-Class
Agents create and interact with surfaces through the same kernel APIs as native GUI applications. There is no "GUI mode" that requires special privileges agents cannot obtain.

### 5.8 Security Travels With Surfaces
The creator cell, access policy, and capability requirements of a surface are kernel-enforced properties, not application-level conventions. An agent cannot spoof input events or capture another agent's surfaces without explicit policy grants.

---

## 6. Architectural Overview

The Interface Plane sits alongside the Memory, Routing, and Network planes:

```text
Hardware Input Devices
  -> Input Subsystem (kernel/core/iface/input/)
  -> Event Objects (ANX_OBJ_EVENT)
  -> Routing Plane
  -> Surface client Execution Cells

Surface Client Cells
  -> Create ANX_OBJ_SURFACE State Objects
  -> Submit to Interface Plane registry
  -> Compositor Cell reads surface tree
  -> Routes surfaces to Renderer Engines
  -> Renderer Engines produce physical output

Physical Output Devices
  <- GPU driver (framebuffer)   <- ANX_RENDERER_GPU
  <- TTS/STT subsystem          <- ANX_RENDERER_VOICE
  <- Haptic driver              <- ANX_RENDERER_HAPTIC
  <- Actuator subsystem         <- ANX_RENDERER_ROBOT
  <- /dev/null equivalent       <- ANX_RENDERER_HEADLESS
```

### 6.1 Control Boundaries

- **Surface clients** own surface content and interaction policy
- **Interface Plane registry** owns the authoritative surface tree and lifecycle
- **Compositor Cell** owns ordering, focus, damage, and frame scheduling
- **Renderer Engines** own physical output
- **Input Subsystem** owns raw hardware input and event object creation
- **Routing Plane** routes events from input to surface clients
- **Capability system** enforces which cells may create, observe, or modify surfaces

---

## 7. Surface Objects

### 7.1 New Object Type: ANX_OBJ_SURFACE

The Interface Plane adds `ANX_OBJ_SURFACE` to `enum anx_object_type` in `state_object.h`. This is justified by the same criteria as prior type additions: the kernel must enforce surface lifecycle, access policy, compositor assignment, and provenance independently of userland conventions.

The schema URI for all surfaces is `anx:surface/v1`.

### 7.2 Surface Lifecycle States

```c
enum anx_surface_state {
    ANX_SURF_CREATED,    /* allocated, not yet mapped */
    ANX_SURF_MAPPED,     /* attached to compositor domain */
    ANX_SURF_VISIBLE,    /* actively rendered */
    ANX_SURF_OCCLUDED,   /* mapped but obscured by another surface */
    ANX_SURF_MINIMIZED,  /* mapped but not visible, held by compositor */
    ANX_SURF_DESTROYED,  /* lifecycle ended, pending tombstone */
};
```

### 7.3 Surface Types

Surface type declares the primary interaction modality of the surface, independent of which renderer will handle it:

```c
enum anx_surface_type {
    ANX_SURF_TYPE_WINDOW,        /* standard interactive window */
    ANX_SURF_TYPE_PANEL,         /* non-interactive status/info surface */
    ANX_SURF_TYPE_OVERLAY,       /* composited above all windows */
    ANX_SURF_TYPE_NOTIFICATION,  /* transient alert surface */
    ANX_SURF_TYPE_DIALOG,        /* modal interaction surface */
    ANX_SURF_TYPE_CANVAS,        /* free-form drawing or media surface */
    ANX_SURF_TYPE_SENSOR_FEED,   /* live sensor data stream */
    ANX_SURF_TYPE_ACTUATOR_CTL,  /* actuator command surface */
    ANX_SURF_TYPE_VOICE_CHANNEL, /* voice interaction surface */
    ANX_SURF_TYPE_HAPTIC_DOMAIN, /* haptic feedback surface */
    ANX_SURF_TYPE_HEADLESS,      /* no physical output, agent-only */
    ANX_SURF_TYPE_COUNT,
};
```

### 7.4 Content Node Types

The semantic content tree is composed of typed content nodes. Each content node has a type and a payload appropriate to that type:

```c
enum anx_content_node_type {
    ANX_CONTENT_TEXT,          /* plain or rich text */
    ANX_CONTENT_BUTTON,        /* interactive action trigger */
    ANX_CONTENT_INPUT_FIELD,   /* text or data entry field */
    ANX_CONTENT_IMAGE,         /* raster or vector image */
    ANX_CONTENT_CANVAS,        /* raw pixel or vector drawing surface */
    ANX_CONTENT_VIEWPORT,      /* embedded sub-surface reference */
    ANX_CONTENT_AUDIO_STREAM,  /* audio data stream */
    ANX_CONTENT_VIDEO_STREAM,  /* video data stream */
    ANX_CONTENT_SENSOR_STREAM, /* real-time sensor data */
    ANX_CONTENT_ACTUATOR_CMD,  /* motor/actuator command target */
    ANX_CONTENT_LABEL,         /* non-interactive descriptive text */
    ANX_CONTENT_SEPARATOR,     /* structural/layout divider */
    ANX_CONTENT_CONTAINER,     /* layout container grouping children */
    ANX_CONTENT_CUSTOM,        /* renderer-specific extension node */
    ANX_CONTENT_NODE_TYPE_COUNT,
};
```

Each content node carries semantic attributes (label, description, action intent, data binding) that non-GPU renderers (voice, haptic) can use independently of any visual layout specification.

### 7.5 Interaction Policy

```c
enum anx_interaction_policy {
    ANX_INTERACT_FULL,        /* receives all event types */
    ANX_INTERACT_READ_ONLY,   /* no input events delivered */
    ANX_INTERACT_FOCUS_ONLY,  /* only focus-change events */
    ANX_INTERACT_AGENT_ONLY,  /* events routed to agents, not humans */
    ANX_INTERACT_NONE,        /* surface is purely informational */
};
```

### 7.6 Render Target Hint

The render target hint guides the routing plane when selecting a Renderer Engine. It is advisory, not binding: the routing plane may override it based on the active environment profile and capability availability.

```c
enum anx_render_target_hint {
    ANX_RENDER_HINT_NONE,      /* let environment profile decide */
    ANX_RENDER_HINT_GPU,       /* prefer GPU renderer */
    ANX_RENDER_HINT_VOICE,     /* prefer voice renderer */
    ANX_RENDER_HINT_HAPTIC,    /* prefer haptic renderer */
    ANX_RENDER_HINT_ROBOT,     /* prefer robot actuator renderer */
    ANX_RENDER_HINT_HEADLESS,  /* prefer headless renderer */
    ANX_RENDER_HINT_ANY,       /* accept whatever renderer is available */
};
```

### 7.7 Surface Object Structure

```c
#define ANX_SURFACE_SCHEMA_URI    "anx:surface/v1"
#define ANX_MAX_CONTENT_NODES     256
#define ANX_MAX_SURFACE_CHILDREN  64
#define ANX_MAX_SURFACE_TITLE     128
#define ANX_MAX_SURFACE_APPID     128

struct anx_content_node {
    anx_oid_t                   node_oid;
    enum anx_content_node_type  node_type;
    char                        label[128];
    char                        description[256];
    char                        action_intent[128]; /* for BUTTON, ACTUATOR_CMD */
    void                       *type_payload;       /* type-specific data */
    uint64_t                    type_payload_size;
    anx_oid_t                   children[16];
    uint32_t                    child_count;
};

struct anx_surface_geometry {
    /* Advisory layout hints; renderers may ignore for non-GPU modalities */
    int32_t  x, y;              /* position hint in compositor space */
    uint32_t width, height;     /* size hint in logical pixels */
    uint32_t min_width, min_height;
    uint32_t max_width, max_height;
    bool     resizable;
};

struct anx_surface_object {
    /* Identity (in the State Object wrapper) */
    anx_oid_t                  surf_oid;
    enum anx_surface_type      surf_type;
    enum anx_surface_state     surf_state;

    /* Content */
    char                       title[ANX_MAX_SURFACE_TITLE];
    char                       app_id[ANX_MAX_SURFACE_APPID];
    struct anx_content_node    content[ANX_MAX_CONTENT_NODES];
    uint32_t                   content_count;

    /* Layout */
    struct anx_surface_geometry geometry;

    /* Rendering */
    enum anx_render_target_hint  render_hint;
    anx_eid_t                    assigned_renderer; /* set by compositor */

    /* Interaction */
    enum anx_interaction_policy  interaction_policy;

    /* Compositor context */
    uint32_t                   z_order;        /* higher = in front */
    bool                       has_focus;
    anx_oid_t                  parent_surf;    /* nil = top-level */
    anx_oid_t                  children[ANX_MAX_SURFACE_CHILDREN];
    uint32_t                   child_count;

    /* Damage tracking */
    bool                       damaged;        /* renderer must re-render */
    uint32_t                   damage_seq;     /* incremented on each damage */

    /* Provenance — who created this surface */
    anx_cid_t                  creator_cell;
    anx_nid_t                  creator_node;
};
```

### 7.8 Surface Provenance

Surface provenance follows the standard `anx_prov_log` model. Creation, mapping, visibility transitions, focus changes, content mutations, and destruction are all logged as provenance events. The `creator_cell` field in `anx_surface_object` identifies which Execution Cell created the surface; this is immutable after creation.

Provenance event types relevant to surfaces:

```c
ANX_PROV_CREATED        /* surface object allocated */
ANX_PROV_MUTATED        /* content tree modified */
ANX_PROV_SURF_MAPPED    /* surface entered MAPPED state */
ANX_PROV_SURF_FOCUSED   /* surface received focus */
ANX_PROV_SURF_BLURRED   /* surface lost focus */
ANX_PROV_SURF_OCCLUDED  /* surface became occluded */
ANX_PROV_SURF_DAMAGED   /* damage region marked */
ANX_PROV_SEALED         /* surface destroyed, object sealed */
```

The last three (SURF_MAPPED, SURF_FOCUSED, SURF_BLURRED, SURF_OCCLUDED, SURF_DAMAGED) are Interface Plane extensions to `enum anx_prov_type` added in `provenance.h`.

### 7.9 Memory Tier Placement

Surface objects default to `ANX_MEM_L0` (active execution-local working set) for their content nodes while visible. When a surface is minimized, its content tree may be demoted to `ANX_MEM_L1`. When a surface is destroyed, its object is sealed and may be archived at `ANX_MEM_L2` for audit and replay purposes (respecting the retention policy set by the creator cell).

The compositor cell is responsible for calling `anx_memplane_promote()` and `anx_memplane_demote()` as surfaces transition between lifecycle states.

---

## 8. Renderer Engine Classes

### 8.1 New Engine Classes

The Interface Plane adds the following engine classes to `enum anx_engine_class` in `engine.h`:

```c
ANX_ENGINE_RENDERER_GPU,        /* GPU-backed framebuffer renderer */
ANX_ENGINE_RENDERER_VOICE,      /* TTS/STT voice renderer */
ANX_ENGINE_RENDERER_HAPTIC,     /* Tactile/vibration renderer */
ANX_ENGINE_RENDERER_ROBOT,      /* Actuator control renderer */
ANX_ENGINE_RENDERER_HEADLESS,   /* No-output renderer for agents/testing */
ANX_ENGINE_RENDERER_COMPOSITOR, /* Meta-renderer compositing sub-renderers */
```

### 8.2 New Capability Flags

```c
#define ANX_CAP_RENDER_GPU          (1U << 16)
#define ANX_CAP_RENDER_VOICE        (1U << 17)
#define ANX_CAP_RENDER_HAPTIC       (1U << 18)
#define ANX_CAP_RENDER_ROBOT        (1U << 19)
#define ANX_CAP_RENDER_HEADLESS     (1U << 20)
#define ANX_CAP_COMPOSITOR          (1U << 21)
#define ANX_CAP_INPUT_POINTER       (1U << 22)
#define ANX_CAP_INPUT_KEYBOARD      (1U << 23)
#define ANX_CAP_INPUT_TOUCH         (1U << 24)
#define ANX_CAP_INPUT_VOICE         (1U << 25)
#define ANX_CAP_INPUT_SENSOR        (1U << 26)
```

### 8.3 ANX_ENGINE_RENDERER_GPU

The GPU renderer consumes Surface Objects and produces pixel output to a display framebuffer or DRM/KMS surface. For surfaces with `ANX_CONTENT_CANVAS` nodes, it renders the canvas directly. For surfaces with semantic content trees (text, buttons, labels), it applies a layout engine to produce pixel output.

Registration constraints for GPU renderers:
- Must declare `ANX_CAP_RENDER_GPU`
- Must set `is_local = true` (GPU renderers are always local)
- `requires_network = false`
- `supports_private_data = true` (pixels never leave the node)

The first GPU renderer implementation wraps the existing `anx_fb` framebuffer driver. Later implementations will use DRM/KMS for hardware-accelerated compositing.

### 8.4 ANX_ENGINE_RENDERER_VOICE

The voice renderer consumes Surface Objects and produces audio output (TTS) for content nodes with text or label content, and produces event objects from audio input (STT) for voice channel surfaces. It maps the semantic content tree to a spoken representation: text nodes are read aloud, buttons are announced with their action intent, dialogs are spoken with response options.

Registration constraints:
- Must declare `ANX_CAP_RENDER_VOICE`
- May set `requires_network = true` if backed by a remote TTS/STT engine
- Should declare `offline_capable` status accurately (offline TTS is less capable)

### 8.5 ANX_ENGINE_RENDERER_HAPTIC

The haptic renderer consumes Surface Objects and produces vibration/tactile output. For notification surfaces it produces alert patterns. For button surfaces it produces confirmation feedback. For sensor stream surfaces it produces continuous haptic encodings of sensor data.

Registration constraints:
- Must declare `ANX_CAP_RENDER_HAPTIC`
- `is_local = true`
- Declares a `haptic_channels` count in renderer-specific metadata

### 8.6 ANX_ENGINE_RENDERER_ROBOT

The robot renderer consumes `ANX_SURF_TYPE_ACTUATOR_CTL` surfaces and produces actuator commands to registered robot subsystems. It is the bridge between the abstract interface plane and physical robotics hardware. A robot renderer may also produce `ANX_OBJ_EVENT` sensor feedback events from its hardware inputs.

Registration constraints:
- Must declare `ANX_CAP_RENDER_ROBOT`
- `is_local = true` for directly-connected robots; `requires_network` may be true for remote robots
- Declares supported actuator classes in renderer-specific metadata

### 8.7 ANX_ENGINE_RENDERER_HEADLESS

The headless renderer accepts any surface type and discards all output. It is used for:
- CI/CD testing of surface-producing agents
- Agent environments where the agent reads surface content programmatically
- Background agents that create surfaces solely for the purpose of event subscription

Registration constraints:
- Must declare `ANX_CAP_RENDER_HEADLESS`
- Always available, zero resource cost
- Should be the fallback renderer when no other renderer is available

### 8.8 ANX_ENGINE_RENDERER_COMPOSITOR

The compositor meta-renderer manages sub-renderers. It implements surface ordering, focus management, and frame scheduling, and dispatches individual surfaces to leaf renderers (GPU, voice, haptic, robot) based on surface type and render hint. The compositor meta-renderer class is distinct from the `ANX_ENGINE_RENDERER_*` leaf classes because it does not produce output directly.

### 8.9 Renderer Registration and Routing

Renderers register through the standard `anx_engine_register()` API. A renderer describes its capabilities, constraints, and resource requirements using the standard engine registration schema extended with renderer-specific fields.

When the routing plane must select a renderer for a surface, it applies the following logic:

1. **Policy filter**: exclude renderers incompatible with the surface's interaction policy or creator cell's execution policy.
2. **Capability filter**: require `ANX_CAP_RENDER_*` matching the surface's `render_hint` (if set), or accept any renderer if hint is `ANX_RENDER_HINT_NONE`.
3. **Environment profile filter**: the active environment profile declares which renderer classes are eligible.
4. **Scoring**: prefer renderers with lower resource cost, higher quality score, and better availability status.
5. **Selection**: the highest-scoring feasible renderer is assigned to the surface. The compositor cell records the assignment in the surface's `assigned_renderer` field.

---

## 9. Event Objects

### 9.1 New Object Type: ANX_OBJ_EVENT

`ANX_OBJ_EVENT` is a new entry in `enum anx_object_type`. Event objects are State Objects with short retention (default `ttl_ns` of 5 seconds for input events, longer for synthetic or recorded events) and `ANX_MEM_L0` tier placement. They are write-once: once created, their payload is sealed.

Schema URIs:
- `anx:event/pointer/v1`
- `anx:event/keyboard/v1`
- `anx:event/touch/v1`
- `anx:event/gesture/v1`
- `anx:event/voice-intent/v1`
- `anx:event/sensor/v1`
- `anx:event/actuator-feedback/v1`
- `anx:event/focus-change/v1`
- `anx:event/surface-lifecycle/v1`

### 9.2 Event Object Structure

```c
enum anx_event_type {
    ANX_EVT_POINTER_MOVE,
    ANX_EVT_POINTER_BUTTON,
    ANX_EVT_POINTER_SCROLL,
    ANX_EVT_POINTER_ENTER,
    ANX_EVT_POINTER_LEAVE,
    ANX_EVT_KEY_DOWN,
    ANX_EVT_KEY_UP,
    ANX_EVT_KEY_REPEAT,
    ANX_EVT_TOUCH_DOWN,
    ANX_EVT_TOUCH_MOTION,
    ANX_EVT_TOUCH_UP,
    ANX_EVT_TOUCH_CANCEL,
    ANX_EVT_GESTURE_SWIPE,
    ANX_EVT_GESTURE_PINCH,
    ANX_EVT_GESTURE_ROTATE,
    ANX_EVT_VOICE_INTENT,
    ANX_EVT_VOICE_UTTERANCE,
    ANX_EVT_SENSOR_READING,
    ANX_EVT_ACTUATOR_FEEDBACK,
    ANX_EVT_FOCUS_GAINED,
    ANX_EVT_FOCUS_LOST,
    ANX_EVT_SURF_MAPPED,
    ANX_EVT_SURF_UNMAPPED,
    ANX_EVT_SURF_DESTROYED,
    ANX_EVT_SURF_DAMAGED,
    ANX_EVT_TYPE_COUNT,
};

struct anx_event_provenance {
    anx_oid_t   source_device_oid;  /* registered input device object */
    anx_cid_t   source_cell;        /* cell that created this event */
    anx_oid_t   target_surface;     /* destination surface OID */
    anx_nid_t   source_node;        /* node where event originated */
    anx_time_t  hardware_timestamp; /* hardware timestamp if available */
};

struct anx_event_object {
    anx_oid_t                   event_oid;
    enum anx_event_type         event_type;
    anx_time_t                  created_at;
    struct anx_event_provenance provenance;
    bool                        synthetic;    /* true = not from hardware */
    bool                        replayed;     /* true = event replay */
    void                       *type_payload; /* event-specific data */
    uint64_t                    type_payload_size;
};
```

### 9.3 Event Routing Architecture

The event routing path is:

```text
Hardware IRQ
  -> arch input driver (kernel/arch/*/input/)
  -> anx_input_event_create()    [creates ANX_OBJ_EVENT]
  -> anx_route_plan()            [routing plane selects target surface]
  -> target surface's creator cell receives ANX_OBJ_EVENT via output binding
```

The routing plane uses event type, target surface, and surface access policy to determine delivery. The routing criteria for events are:

1. **Surface ownership**: events are delivered to the creator cell of the target surface unless an explicit event delegation policy is set.
2. **Focus routing**: `ANX_EVT_KEY_*` events are routed to the currently focused surface's creator cell.
3. **Hit testing**: `ANX_EVT_POINTER_*` and `ANX_EVT_TOUCH_*` events use z-order and surface geometry to determine the target surface. The compositor cell provides the hit-test service.
4. **Policy filter**: a surface may set `ANX_INTERACT_READ_ONLY` to refuse input events. A surface with `ANX_INTERACT_AGENT_ONLY` refuses events from non-agent cells.

### 9.4 Voice Intent Events

Voice intent events (`ANX_EVT_VOICE_INTENT`) are created by the Voice Renderer Engine after STT processing. They carry a structured intent payload rather than a raw transcript:

```c
struct anx_voice_intent_payload {
    char    transcript[512];  /* raw transcription */
    char    intent[128];      /* classified intent, e.g. "navigate.back" */
    float   confidence;       /* 0.0-1.0 */
    char    entities[8][128]; /* extracted named entities */
    uint32_t entity_count;
};
```

Voice intent events are routed to the currently active voice-focused surface or, if no surface has voice focus, to the active compositor cell for global dispatch.

### 9.5 Sensor and Actuator Events

Sensor reading events (`ANX_EVT_SENSOR_READING`) originate from device drivers or the Robot Renderer Engine and carry sensor-class-specific payloads. Actuator feedback events (`ANX_EVT_ACTUATOR_FEEDBACK`) confirm command execution and carry error codes if the actuator failed.

### 9.6 Surface Lifecycle Events

Surface lifecycle events are generated by the Interface Plane registry and the compositor cell when surfaces change state. They are delivered to:
- The creator cell of the affected surface
- Any cell that has subscribed to lifecycle events for that surface via `anx_iface_subscribe_events()`
- The compositor cell (always)

### 9.7 Event Security

Event objects are sealed immediately after creation and cannot be modified. Their provenance is recorded at creation time by the kernel, not by userland. A cell cannot create a synthetic event with a forged `source_device_oid` unless it holds the `ANX_CAP_IFACE_SYNTH_INPUT` capability (see Section 14). This prevents input injection attacks.

---

## 10. Compositor Cells

### 10.1 The Compositor as an Execution Cell

In Wayland, the compositor is a privileged userland process with special kernel access through DRM/KMS. In Anunix, the compositor is an Execution Cell of type `ANX_CELL_TASK_EXECUTION` with intent `"interface.compositor"`. It has no special kernel privileges beyond the capability claims required for compositor operations.

This has several consequences:
- Compositor implementations are swappable: any cell with `ANX_CAP_IFACE_COMPOSITOR` can become the active compositor for a display domain.
- Multiple compositor cells can run concurrently, each managing a different domain (e.g., one compositor per physical display, one headless compositor for agent-only surfaces).
- Compositor crashes are cell failures, not system failures. The kernel can restart a compositor cell without affecting surface object state.
- Compositor decisions are traceable: the compositor cell writes its ordering, focus, and damage decisions to its cell trace.

### 10.2 Compositor Responsibilities

A compositor cell is responsible for:

1. **Surface ordering (z-order)**
   Maintaining the z-order stack of surfaces in its domain. Z-order changes are requested by surface clients and approved by the compositor based on compositor policy.

2. **Focus management**
   Tracking which surface holds keyboard focus. Focus policy (click-to-focus, hover-focus, agent-assigned focus) is a compositor implementation choice.

3. **Damage tracking**
   Tracking which surfaces have changed (`damaged = true`) and need re-rendering. On each frame, the compositor identifies damaged surfaces and dispatches them to their assigned Renderer Engine.

4. **Frame scheduling**
   Coordinating the rendering of all surfaces in its domain at the appropriate refresh rate. For GPU renderers, this involves DRM/KMS vblank synchronization. For voice and haptic renderers, it involves output buffering.

5. **Hit testing**
   Providing event routing with surface-point hit-test queries: "which surface contains point (x, y) in compositor space?"

6. **Renderer assignment**
   When a new surface is registered, the compositor routes to the routing plane to select an appropriate Renderer Engine and records the assignment in the surface's `assigned_renderer` field.

### 10.3 Compositor-to-Client Protocol

Surface clients communicate with the compositor through the kernel Interface Plane API (`anx_iface_*`). There is no private IPC socket between compositor and client. The protocol is:

**Client → Compositor** (via `anx_iface_*` kernel calls):
- `anx_iface_surface_create()` — register a new surface with the Interface Plane
- `anx_iface_surface_map()` — request that the compositor make a surface visible
- `anx_iface_surface_unmap()` — request that the compositor hide a surface
- `anx_iface_surface_damage()` — notify the compositor that surface content has changed
- `anx_iface_surface_set_zorder()` — request z-order change (compositor may deny)
- `anx_iface_surface_request_focus()` — request keyboard focus

**Compositor → Client** (via ANX_OBJ_EVENT State Objects):
- `ANX_EVT_SURF_MAPPED` — compositor has mapped the surface
- `ANX_EVT_SURF_UNMAPPED` — compositor has unmapped the surface
- `ANX_EVT_FOCUS_GAINED` — surface has received focus
- `ANX_EVT_FOCUS_LOST` — surface has lost focus
- `ANX_EVT_SURF_DAMAGED` — compositor requests a content update from the client

The compositor also sends the client a **frame callback**: after each render, the compositor creates an `ANX_EVT_SURF_DAMAGED` event addressed to each rendered surface client, signaling that the client may safely update the surface for the next frame.

### 10.4 Multi-Compositor Coexistence

Two compositor cells may run concurrently in different domains. Domain assignment is set at compositor cell creation time and recorded in the Interface Plane registry. The kernel prevents two compositor cells from claiming the same display domain. Domain identifiers are:

```c
#define ANX_DOMAIN_DISPLAY_0   "display.0"
#define ANX_DOMAIN_DISPLAY_1   "display.1"
/* ... */
#define ANX_DOMAIN_VOICE       "voice.primary"
#define ANX_DOMAIN_HAPTIC      "haptic.primary"
#define ANX_DOMAIN_ROBOT       "robot.primary"
#define ANX_DOMAIN_HEADLESS    "headless.primary"
```

### 10.5 Compositor-Renderer Interaction

For each damaged surface, the compositor cell:

1. Prepares a **render request**: a structured data object containing the surface OID, damage region, z-order context, and frame sequence number.
2. Creates an Execution Cell (`ANX_CELL_TASK_SIDE_EFFECT`) with intent `"interface.render"` bound to the target surface and the assigned Renderer Engine.
3. The render cell executes against the Renderer Engine and returns.
4. The compositor records the frame as committed when all damaged surfaces in the domain have been rendered.

This makes rendering fully traceable: every render operation is an Execution Cell with provenance back to the surface that triggered it.

---

## 11. Wayland Compatibility Layer

### 11.1 Design Philosophy

The Wayland compatibility layer translates Wayland protocol messages into Interface Plane operations. It does not re-implement the Interface Plane as a Wayland compositor. The layer is a translation shim that runs in userland (with a thin kernel component for privilege management).

An existing Wayland application works as follows:
1. The app creates a Wayland socket connection (via the compat layer's `libwayland-server` shim).
2. When the app creates a `wl_surface`, the compat layer creates an `ANX_OBJ_SURFACE` with `surf_type = ANX_SURF_TYPE_WINDOW`, `render_hint = ANX_RENDER_HINT_GPU`, and a single `ANX_CONTENT_CANVAS` content node.
3. When the app attaches a `wl_buffer` (pixel buffer), the compat layer stores the buffer reference in the canvas node's type payload.
4. When the app calls `wl_surface_damage()`, the compat layer calls `anx_iface_surface_damage()`.
5. The compositor cell renders the surface by passing the canvas content node's pixel buffer to the GPU Renderer Engine.
6. Input events generated by Wayland input objects are translated to `ANX_OBJ_EVENT` objects and routed back to the compat layer, which reconstructs Wayland event messages for the app.

### 11.2 What the Compat Layer Translates

| Wayland Concept | Interface Plane Equivalent |
|---|---|
| `wl_surface` | `ANX_OBJ_SURFACE` with `ANX_CONTENT_CANVAS` |
| `wl_buffer` (shm/dmabuf) | Canvas content node pixel payload |
| `wl_surface_damage` | `anx_iface_surface_damage()` |
| `wl_compositor.create_surface` | `anx_iface_surface_create()` |
| `wl_shell_surface.set_toplevel` | `anx_iface_surface_map()` |
| `wl_pointer` events | `ANX_EVT_POINTER_*` |
| `wl_keyboard` events | `ANX_EVT_KEY_*` |
| `wl_touch` events | `ANX_EVT_TOUCH_*` |
| `wl_output` | Environment profile GPU renderer info |
| `wl_seat` | Input device registry subset |

### 11.3 Kernel vs Userland Split

**Kernel component** (`kernel/core/iface/compat/`):
- `ANX_OBJ_SURFACE` creation on behalf of the compat process (requires `ANX_CAP_IFACE_COMPAT_BRIDGE` capability)
- Pixel buffer privilege management (mapping shared memory regions for GPU renderer access)
- Event object creation from translated Wayland input events

**Userland component** (`tools/wayland-compat/`):
- Full `libwayland-server` protocol implementation
- `wl_drm`, `xdg_shell`, `zwp_linux_dmabuf` protocol extensions
- `wl_shm` buffer allocation and management
- Wayland socket server (`/run/wayland-anx-0`)

The compat process runs as a Compositor Cell (type `ANX_CELL_TASK_EXECUTION`, intent `"interface.wayland-compat"`) with `ANX_CAP_IFACE_COMPAT_BRIDGE` capability. It registers Wayland client surfaces in the Interface Plane on behalf of those clients.

### 11.4 XWayland Analog

X11 applications are supported through a standard XWayland process that connects to the Wayland compat layer. The XWayland process translates X11 requests to Wayland protocol messages, which the compat layer then translates to Interface Plane operations. The kernel is not aware of X11; the entire X11 stack terminates at the Wayland compat boundary.

### 11.5 Compat Layer Limitations

The Wayland compat layer cannot provide:
- Access to semantic content tree properties of native Anunix surfaces from Wayland clients
- Voice or haptic rendering for Wayland surfaces (all Wayland surfaces use `ANX_RENDER_HINT_GPU`)
- Provenance visibility from within the Wayland client (Wayland clients do not know they are running on the Interface Plane)
- Multi-compositor coexistence for Wayland surfaces (all Wayland surfaces go to the compat compositor domain)

These limitations are acceptable because Wayland applications are compatibility guests, not native citizens of the Interface Plane.

---

## 12. Agent API

### 12.1 Creating Surfaces Programmatically

An agent creates a surface by spawning an Execution Cell that calls `anx_iface_surface_create()`. The cell must hold `ANX_CAP_IFACE_CREATE_SURFACE` to proceed:

```c
/*
 * Create a new surface and register it with the Interface Plane.
 * Requires ANX_CAP_IFACE_CREATE_SURFACE.
 * Returns ANX_OK on success; out is populated with the new surface's OID.
 */
int anx_iface_surface_create(const struct anx_surface_object *spec,
                              anx_cid_t creator_cell,
                              anx_oid_t *out_surf_oid);
```

Agents can create surfaces of any `anx_surface_type`. A headless agent typically creates `ANX_SURF_TYPE_HEADLESS` surfaces with `ANX_RENDER_HINT_HEADLESS`. An agent presenting information to a user creates `ANX_SURF_TYPE_WINDOW` or `ANX_SURF_TYPE_NOTIFICATION` surfaces.

### 12.2 Subscribing to Events

An agent subscribes to events for a surface it owns or is permitted to observe:

```c
/*
 * Subscribe to event types for a surface.
 * Requires ANX_CAP_IFACE_SUBSCRIBE_EVENTS and read access to the surface.
 * Subsequent events of the specified types are routed to subscriber_cell.
 */
int anx_iface_subscribe_events(const anx_oid_t *surf_oid,
                                uint32_t event_type_mask,
                                anx_cid_t subscriber_cell);

/*
 * Unsubscribe from events for a surface.
 */
int anx_iface_unsubscribe_events(const anx_oid_t *surf_oid,
                                  anx_cid_t subscriber_cell);
```

`event_type_mask` is a bitmask over `enum anx_event_type`. An agent cannot subscribe to events for surfaces it does not own and does not hold `ANX_CAP_IFACE_OBSERVE_FOREIGN_SURFACES` for.

### 12.3 Inspecting the Surface Tree

The Interface Plane registry exposes a read API:

```c
/*
 * Query all surfaces in a compositor domain.
 * Requires ANX_CAP_IFACE_INSPECT_SURFACES.
 * Returns surface OIDs; caller reads each surface via anx_so_open().
 */
int anx_iface_list_surfaces(const char *domain,
                             anx_oid_t *out_oids,
                             uint32_t max_surfaces,
                             uint32_t *found_count);

/*
 * Get the current surface tree root (top-level surfaces in z-order).
 */
int anx_iface_surface_tree(const char *domain,
                            anx_oid_t *out_oids,
                            uint32_t max_surfaces,
                            uint32_t *found_count);
```

Agents use the standard `anx_so_open()` API to read surface object payloads. Because surface objects are State Objects, all standard provenance and access policy mechanisms apply: an agent cannot read a surface's content if the surface's access policy denies it.

### 12.4 Modifying Surface Content

An agent that owns a surface can modify its content tree:

```c
/*
 * Replace the content tree of a surface (triggers damage).
 * Requires ANX_CAP_IFACE_CREATE_SURFACE and write access to the surface.
 */
int anx_iface_surface_update_content(const anx_oid_t *surf_oid,
                                      const struct anx_content_node *content,
                                      uint32_t content_count);
```

Content updates automatically set `damaged = true` and increment `damage_seq`, triggering re-render by the compositor.

### 12.5 Creating Custom Renderer Engines

An agent can register a new Renderer Engine for novel interaction modalities using the standard Engine Registry API:

```c
struct anx_engine *renderer;
int ret = anx_engine_register("my-custom-renderer",
                               ANX_ENGINE_RENDERER_GPU,  /* or custom class */
                               ANX_CAP_RENDER_GPU | ANX_CAP_COMPOSITOR,
                               &renderer);
```

For completely novel renderer classes not covered by the existing `ANX_ENGINE_RENDERER_*` enum, the agent must hold `ANX_CAP_IFACE_REGISTER_RENDERER` and call the extended registration API.

### 12.6 Environment Transitions

An agent with `ANX_CAP_IFACE_SWITCH_ENV` can transition the active environment:

```c
/*
 * Activate an environment profile.
 * If stack=true, push the current environment onto the stack rather than
 * replacing it. Allows restore via anx_iface_env_pop().
 */
int anx_iface_env_activate(const char *env_uri, bool stack);

/*
 * Pop the top environment from the stack and restore the previous one.
 */
int anx_iface_env_pop(void);

/*
 * Query the currently active environment profile URI.
 */
int anx_iface_env_current(char *out_uri, uint32_t len);
```

Example: an agent that detects the user has walked away from their desk might call `anx_iface_env_activate("anx:env/headless-agent", true)` to disable visual output while preserving surface state. When the user returns, it calls `anx_iface_env_pop()` to restore the visual environment.

---

## 13. Environment Profiles

### 13.1 Profile Structure

An environment profile binds renderer engine classes, input device classes, compositor policy, and interaction defaults:

```c
#define ANX_MAX_ENV_RENDERERS  8
#define ANX_MAX_ENV_INPUTS     8
#define ANX_MAX_ENV_URI        128

struct anx_env_renderer_binding {
    enum anx_engine_class   renderer_class;
    char                    preferred_engine_name[64]; /* "" = any */
    bool                    required;
};

struct anx_env_input_binding {
    uint32_t                input_cap_flag;   /* ANX_CAP_INPUT_* */
    bool                    required;
};

struct anx_environment_profile {
    char                             uri[ANX_MAX_ENV_URI];
    char                             description[256];

    struct anx_env_renderer_binding  renderers[ANX_MAX_ENV_RENDERERS];
    uint32_t                         renderer_count;

    struct anx_env_input_binding     inputs[ANX_MAX_ENV_INPUTS];
    uint32_t                         input_count;

    /* Compositor policy */
    bool    allow_multiple_windows;
    bool    enforce_single_focus;
    bool    auto_damage_on_content_update;

    /* Fallback */
    char    fallback_env_uri[ANX_MAX_ENV_URI]; /* "" = headless */
};
```

### 13.2 Built-in Environment Profiles

#### `anx:env/visual-desktop`

Standard desktop visual environment:

- Renderers: `ANX_ENGINE_RENDERER_GPU` (required), `ANX_ENGINE_RENDERER_HEADLESS` (fallback)
- Inputs: `ANX_CAP_INPUT_POINTER`, `ANX_CAP_INPUT_KEYBOARD`, `ANX_CAP_INPUT_TOUCH` (optional)
- Policy: multiple windows allowed, click-to-focus, auto-damage on content update
- Fallback: `anx:env/headless-agent`

#### `anx:env/voice`

Voice-only interaction environment:

- Renderers: `ANX_ENGINE_RENDERER_VOICE` (required), `ANX_ENGINE_RENDERER_HEADLESS` (fallback)
- Inputs: `ANX_CAP_INPUT_VOICE` (required)
- Policy: single-window focus equivalent (one active voice channel), auto-damage on content update
- Fallback: `anx:env/headless-agent`
- Note: surfaces are mapped but not visually rendered; TTS produces audio from content trees

#### `anx:env/robotics`

Robot actuator control environment:

- Renderers: `ANX_ENGINE_RENDERER_ROBOT` (required), `ANX_ENGINE_RENDERER_HEADLESS` (fallback)
- Inputs: `ANX_CAP_INPUT_SENSOR` (required)
- Policy: actuator surfaces are the focus domain; sensor surfaces feed event streams continuously
- Fallback: `anx:env/headless-agent`

#### `anx:env/smartphone-tactile`

Smartphone touchscreen with haptic feedback:

- Renderers: `ANX_ENGINE_RENDERER_GPU` (required), `ANX_ENGINE_RENDERER_HAPTIC` (required), `ANX_ENGINE_RENDERER_VOICE` (optional)
- Inputs: `ANX_CAP_INPUT_TOUCH` (required), `ANX_CAP_INPUT_VOICE` (optional)
- Policy: single-window focus, touch-to-focus
- Fallback: `anx:env/headless-agent`

#### `anx:env/headless-agent`

Agent-only environment with no physical I/O:

- Renderers: `ANX_ENGINE_RENDERER_HEADLESS` (required)
- Inputs: none (agents generate synthetic events if needed)
- Policy: unlimited windows, programmatic focus
- Fallback: none (always available)

### 13.3 Environment Activation

When an environment profile is activated:

1. The Interface Plane registry records the new active profile.
2. Existing surfaces are re-evaluated against the new renderer set. Surfaces with `render_hint` incompatible with the new profile are assigned the nearest compatible renderer or the headless renderer.
3. The compositor cell for each domain is notified via `ANX_EVT_SURF_DAMAGED` for all mapped surfaces, triggering a full re-render pass.
4. Input bindings are updated: input devices not listed in the new profile are detached from event routing.

### 13.4 Environment Stacking

Environments may be stacked to allow temporary modality overlays. The stack preserves surface state from the previous environment. A stack depth limit of 8 is enforced.

Example stack: `[visual-desktop, voice]` — voice overlay active, visual desktop suspended. `anx_iface_env_pop()` restores `visual-desktop` and re-renders all previously mapped surfaces.

### 13.5 Environment Transitions Must Be Explicit

Environments do not switch automatically. An agent with `ANX_CAP_IFACE_SWITCH_ENV` must explicitly call `anx_iface_env_activate()`. This prevents accidental modality changes from hardware state (e.g., screen going dark should not activate voice mode without explicit policy).

---

## 14. Kernel Implementation Plan

### 14.1 New Object Types

Extend `enum anx_object_type` in `kernel/include/anx/state_object.h`:

```c
ANX_OBJ_SURFACE,   /* RFC-0012 */
ANX_OBJ_EVENT,     /* RFC-0012 */
```

### 14.2 New Engine Classes

Extend `enum anx_engine_class` in `kernel/include/anx/engine.h`:

```c
ANX_ENGINE_RENDERER_GPU,
ANX_ENGINE_RENDERER_VOICE,
ANX_ENGINE_RENDERER_HAPTIC,
ANX_ENGINE_RENDERER_ROBOT,
ANX_ENGINE_RENDERER_HEADLESS,
ANX_ENGINE_RENDERER_COMPOSITOR,
```

### 14.3 New Capability Flags

Add renderer and input capability flags to `kernel/include/anx/engine.h`:

```c
#define ANX_CAP_RENDER_GPU          (1U << 16)
#define ANX_CAP_RENDER_VOICE        (1U << 17)
#define ANX_CAP_RENDER_HAPTIC       (1U << 18)
#define ANX_CAP_RENDER_ROBOT        (1U << 19)
#define ANX_CAP_RENDER_HEADLESS     (1U << 20)
#define ANX_CAP_COMPOSITOR          (1U << 21)
#define ANX_CAP_INPUT_POINTER       (1U << 22)
#define ANX_CAP_INPUT_KEYBOARD      (1U << 23)
#define ANX_CAP_INPUT_TOUCH         (1U << 24)
#define ANX_CAP_INPUT_VOICE         (1U << 25)
#define ANX_CAP_INPUT_SENSOR        (1U << 26)
```

### 14.4 New Header: `include/anx/interface_plane.h`

The master header for the Interface Plane subsystem. It includes:
- `enum anx_surface_type`, `enum anx_surface_state`
- `struct anx_surface_object`, `struct anx_content_node`
- `enum anx_event_type`, `struct anx_event_object`
- `struct anx_environment_profile`
- `anx_iface_*` API declarations

### 14.5 New Subsystem: `kernel/core/iface/`

```
kernel/core/iface/
  surface.c         -- surface object lifecycle, registry
  event.c           -- event object creation and routing dispatch
  compositor.c      -- compositor cell management, domain registry
  input.c           -- raw input driver bridge, event object factory
  env.c             -- environment profile store, activation, stack
  compat/
    wayland.c       -- kernel-side Wayland compat bridge
  iface.h           -- internal header
```

### 14.6 Relationship to Existing GPU/Framebuffer Driver

The existing `kernel/drivers/fb/` (framebuffer) and `kernel/include/anx/fb.h` are **not replaced**. They remain as the low-level pixel output mechanism. The GPU Renderer Engine wraps the framebuffer driver.

The relationship is:

```text
ANX_OBJ_SURFACE (content tree)
  -> ANX_ENGINE_RENDERER_GPU
  -> layout engine (userland, linked as a render library)
  -> pixel buffer
  -> anx_fb_putpixel() / anx_fb_fill_rect() [early boot, simple cases]
  -> DRM/KMS [production GPU path, kernel/drivers/gpu/]
```

The existing `anx_gui.h` / `anx_gui.c` (simple tiled window manager) becomes an early-boot compositor cell implementation: it handles the `anx:env/visual-desktop` domain during early boot before a full compositor cell is spawned.

### 14.7 What Must Be in the Kernel vs Userland

**Kernel** (`kernel/core/iface/`):
- Surface object lifecycle enforcement
- Event object creation and routing dispatch
- Compositor domain registry (which cell owns which domain)
- Environment profile store and activation
- Access policy enforcement for all `anx_iface_*` calls
- Input driver bridge (translating hardware IRQ data to Event Objects)
- Wayland compat bridge kernel component (shared memory privilege management)

**Userland** (`tools/`, `lib/`):
- Compositor cell implementations (multiple; the kernel only tracks which cell is active)
- GPU renderer (layout engine, font rendering, pixel compositing)
- Voice renderer (TTS/STT engine, audio driver interface)
- Haptic renderer (vibration pattern library, driver interface)
- Robot renderer (actuator command serializer, hardware interface)
- Wayland compat server (`libwayland-server` protocol handler)
- XWayland process
- Widget/layout libraries (application concern, not kernel)

This split follows the Interface Plane principle: the kernel enforces object model, access policy, and routing. Rendering is userland work executed by Renderer Engine cells.

---

## 15. New CLI Utilities

### 15.1 `surfctl` — Surface Object Control

```bash
# List all surfaces in a compositor domain
surfctl list [--domain display.0]

# Inspect a surface's content tree and metadata
surfctl inspect <surf-oid>

# Show provenance for a surface
surfctl prov <surf-oid>

# Create a new headless surface from a content spec file
surfctl create --type headless --content spec.json

# Destroy a surface (requires ownership or ANX_CAP_IFACE_COMPOSITOR)
surfctl destroy <surf-oid>

# Set surface z-order
surfctl zorder <surf-oid> <z-value>

# Request focus for a surface
surfctl focus <surf-oid>

# Dump the full surface tree for a domain
surfctl tree [--domain display.0]
```

`surfctl` is implemented as an Execution Cell (`ANX_CELL_TASK_EXECUTION`, intent `"surfctl"`) that uses the `anx_iface_*` API directly. Its output is structured data (one State Object per surface) rendered to the terminal for human inspection.

### 15.2 `evctl` — Event Object Control

```bash
# Subscribe to events for a surface (live stream)
evctl subscribe <surf-oid> [--types pointer,keyboard]

# Subscribe to all events in a domain
evctl subscribe --domain display.0 [--types all]

# Replay recorded events for a surface
evctl replay <surf-oid> [--from-time 2026-04-16T12:00:00Z]

# Inject a synthetic event (requires ANX_CAP_IFACE_SYNTH_INPUT)
evctl inject --surface <surf-oid> --type key_down --key Return

# Show recent events for a surface
evctl history <surf-oid> [--limit 50]

# Show event routing trace for a specific event OID
evctl trace <event-oid>
```

`evctl` is particularly useful for debugging agent-surface interactions and replaying event sequences for testing.

### 15.3 `compctl` — Compositor Cell Control

```bash
# List active compositor cells and their domains
compctl list

# Show compositor cell details and stats
compctl inspect <domain>

# Spawn a new compositor cell for a domain
compctl spawn --domain display.0 --impl weston-anx

# Replace the active compositor in a domain (graceful handoff)
compctl replace --domain display.0 --impl kwin-anx

# Stop the compositor in a domain (surfaces become unmapped)
compctl stop --domain display.0

# Show damage history for a domain
compctl damage --domain display.0

# Show z-order stack for a domain
compctl zstack --domain display.0
```

### 15.4 `envctl` — Environment Control

```bash
# Show the currently active environment
envctl current

# List all registered environment profiles
envctl list

# Activate an environment profile
envctl activate anx:env/voice

# Push an environment profile onto the stack
envctl push anx:env/voice

# Pop the top environment from the stack
envctl pop

# Show the environment stack
envctl stack

# Register a custom environment profile from a spec file
envctl register --spec my-env.json

# Show renderer engine availability for an environment
envctl check anx:env/visual-desktop
```

---

## 16. Security Model

### 16.1 Capability Claims Required

All Interface Plane operations require explicit kernel-enforced capability claims. No operation relies on ambient process authority.

| Operation | Required Capability |
|---|---|
| Create a surface | `ANX_CAP_IFACE_CREATE_SURFACE` |
| Map a surface (make visible) | `ANX_CAP_IFACE_CREATE_SURFACE` (own surface) |
| Inspect own surfaces | (none, owner access) |
| Inspect foreign surfaces | `ANX_CAP_IFACE_INSPECT_SURFACES` |
| Subscribe to own surface events | (none, owner access) |
| Subscribe to foreign surface events | `ANX_CAP_IFACE_OBSERVE_FOREIGN_SURFACES` |
| Inject synthetic input events | `ANX_CAP_IFACE_SYNTH_INPUT` |
| Act as compositor | `ANX_CAP_IFACE_COMPOSITOR` |
| Switch environments | `ANX_CAP_IFACE_SWITCH_ENV` |
| Register renderer engines | `ANX_CAP_IFACE_REGISTER_RENDERER` |
| Use Wayland compat bridge | `ANX_CAP_IFACE_COMPAT_BRIDGE` |

These capability names map to `anx_capability` objects installed in the capability store. They follow the same `anx_cap_*` lifecycle (DRAFT, VALIDATED, INSTALLED) defined in RFC-0007.

### 16.2 Input Event Spoofing Prevention

Input events are created by the kernel's input subsystem (`kernel/core/iface/input.c`) directly from hardware IRQ handlers. The `source_device_oid` in the event provenance is set by the kernel to the registered input device OID at the time the IRQ fires. Userland cannot set `source_device_oid` to an arbitrary value.

A cell with `ANX_CAP_IFACE_SYNTH_INPUT` can create synthetic events, but the kernel marks them with `synthetic = true` in the event object. Synthetic events are distinguishable from hardware events at the routing layer, allowing surface clients to apply different trust levels.

### 16.3 Surface Capture Prevention

A cell cannot read the content of another cell's surface without:
1. Holding `ANX_CAP_IFACE_INSPECT_SURFACES` (allows reading surface metadata, not necessarily pixel content), OR
2. Being the compositor cell for that domain (compositors must read surface content to render it — this is the minimal privilege needed and is scoped to the domain)

Pixel buffer content (for Wayland compat surfaces) is mapped only to the compositor's address space and the originating client's address space. No other cell can access the pixel buffer without a kernel-brokered explicit grant.

For native `ANX_OBJ_SURFACE` objects, content is read through `anx_so_open()` which enforces the surface's `anx_access_policy`. A surface creator can explicitly deny read access to all other cells.

### 16.4 Compositor Trust

The compositor cell is trusted only within its declared domain. A cell holding `ANX_CAP_IFACE_COMPOSITOR` can only:
- Manage surfaces in domains it has claimed
- Read surface content for surfaces in its domains (for rendering)
- Create `ANX_EVT_FOCUS_*` and `ANX_EVT_SURF_*` lifecycle events
- Assign `assigned_renderer` on surfaces in its domains

The compositor cannot:
- Read surfaces in other domains
- Create input events (pointer, keyboard, touch) — only the input subsystem can
- Modify surface content (only the surface creator can)
- Grant capabilities to cells

### 16.5 Comparison to Wayland Security Model

| Property | Wayland | Anunix Interface Plane |
|---|---|---|
| Input isolation | Per-surface, compositor-enforced | Per-surface, kernel-enforced via event routing policy |
| Screenshot/screen capture | Requires compositor protocol extension | Requires `ANX_CAP_IFACE_INSPECT_SURFACES` + owner consent |
| Compositor trust | Compositor is trusted global | Compositor is a cell trusted only in its domain |
| Input injection | No standard mechanism | Allowed with `ANX_CAP_IFACE_SYNTH_INPUT`, events marked `synthetic=true` |
| Multi-compositor | Not supported in protocol | First-class, per-domain |
| Event provenance | None | Full provenance on every event object |
| Cross-surface observation | No kernel mechanism | Requires explicit capability claim |

The Interface Plane is meaningfully stronger than Wayland in:
- Compositor privilege scoping (domain-limited)
- Event provenance (every event is traceable)
- Input injection transparency (synthetic events are marked)
- Programmatic surface inspection (gated by capability, not binary root/non-root)

---

## 17. POSIX Compatibility

### 17.1 Compatibility Thesis

The Interface Plane generalizes interactive environments without removing classical terminal and framebuffer access.

### 17.2 Mapping

| Classical Concept | Interface Plane Equivalent |
|---|---|
| TTY / PTY | `ANX_SURF_TYPE_WINDOW` with `ANX_CONTENT_TEXT` content, voice-compatible |
| X11 window | `ANX_OBJ_SURFACE` with `ANX_CONTENT_CANVAS`, via XWayland → Wayland compat |
| Wayland surface | `ANX_OBJ_SURFACE` with `ANX_CONTENT_CANVAS`, via Wayland compat layer |
| `/dev/input/*` events | `ANX_OBJ_EVENT` objects routed through Routing Plane |
| DRM/KMS output | GPU Renderer Engine backend |
| Framebuffer device | `anx_fb` driver wrapped by GPU Renderer Engine |

### 17.3 Compatibility Mode

In the prototype, the existing `anx_gui` simple window manager acts as the initial compositor cell. Wayland applications work through the compat layer. X11 applications work through XWayland. None of this requires Interface Plane kernel changes beyond the object type additions.

---

## 18. Reference Prototype Architecture

### 18.1 Phase 1: Foundation

- Add `ANX_OBJ_SURFACE` and `ANX_OBJ_EVENT` to the object type enum
- Implement `kernel/core/iface/surface.c` (surface registry, lifecycle)
- Implement `kernel/core/iface/event.c` (event object factory, routing dispatch)
- Implement `kernel/core/iface/env.c` (environment profile store, activation)
- Register `ANX_ENGINE_RENDERER_HEADLESS` at boot
- Implement `surfctl list` and `surfctl inspect` using the new API
- Tests: surface create/map/unmap/destroy lifecycle; event creation and routing; headless environment

### 18.2 Phase 2: GPU Renderer and Visual Desktop

- Implement `ANX_ENGINE_RENDERER_GPU` backed by existing `anx_fb` driver
- Implement a minimal layout engine (text, button, label content nodes to pixels)
- Register `anx:env/visual-desktop` profile
- Migrate `anx_gui` to be a compositor cell
- Implement `compctl` and `envctl` utilities
- Tests: surface render to framebuffer; compositor z-order; focus management

### 18.3 Phase 3: Wayland Compat

- Implement `kernel/core/iface/compat/wayland.c` (kernel side)
- Implement `tools/wayland-compat/` (userland Wayland server)
- Test: run a simple Wayland application (e.g., `weston-terminal`) through the compat layer
- Test: XWayland via standard XWayland binary

### 18.4 Phase 4: Voice Environment

- Implement `ANX_ENGINE_RENDERER_VOICE` backed by an offline TTS engine
- Implement voice intent event routing
- Register `anx:env/voice` profile
- Test: TTS output from text content nodes; voice intent events from STT

### 18.5 Phase 5: Haptic, Robot, and Multi-Compositor

- Implement `ANX_ENGINE_RENDERER_HAPTIC`
- Implement `ANX_ENGINE_RENDERER_ROBOT`
- Implement multi-compositor domain support
- Implement environment stack and `anx_iface_env_pop()`
- Implement `evctl subscribe` and `evctl replay`

---

## 19. Failure Modes

### 19.1 Compositor Cell Crash

The compositor cell for a domain crashes.

**Mitigation:** The kernel detects cell failure via the standard cell lifecycle. It sends `ANX_EVT_SURF_UNMAPPED` to all surface clients in the domain and marks all surfaces as `ANX_SURF_MINIMIZED`. A watchdog respawns the compositor cell. On restart, the compositor re-reads the surface tree from the Interface Plane registry and re-maps all previously mapped surfaces.

### 19.2 Renderer Engine Unavailability

The assigned renderer for a surface goes offline (e.g., GPU driver failure).

**Mitigation:** The compositor detects engine status change via the Engine Registry. It re-routes affected surfaces to the next available renderer class or falls back to the headless renderer. Surface clients receive `ANX_EVT_SURF_DAMAGED` after reassignment.

### 19.3 Environment Profile Missing Renderer

The active environment requires a renderer class that is not registered.

**Mitigation:** Environment activation fails with `ANX_ENOENT`. The fallback profile URI is used if activation is marked as non-mandatory. If no fallback exists, `anx:env/headless-agent` is activated automatically.

### 19.4 Surface Tree Corruption

A bug in a compositor cell corrupts the z-order or focus state.

**Mitigation:** The surface tree is authoritative in the Interface Plane kernel registry, not in the compositor cell. The compositor reads surface state from the registry and writes only ordering metadata. A corrupted compositor cell can be killed and replaced; the kernel's surface registry remains intact.

### 19.5 Event Queue Overflow

High-frequency sensor or input events overwhelm the event routing path.

**Mitigation:** Event objects for `ANX_EVT_SENSOR_READING` and continuous input events support a sampling rate hint. The input subsystem applies rate limiting per device. Events beyond the rate limit are dropped with a counter increment rather than queued indefinitely. The drop counter is accessible via `evctl history`.

### 19.6 Wayland Compat Protocol Mismatch

A Wayland application uses a protocol extension not supported by the compat layer.

**Mitigation:** Unsupported extensions are advertised as unavailable in the Wayland globals list. Applications that handle extension absence gracefully (most do) will fall back to core Wayland. Applications that require specific extensions may fail to start; this is documented as a known compat limitation.

---

## 20. Observability

### 20.1 Required Metrics

- Surface count per domain and state
- Event objects created per second (by type)
- Compositor frame rate per domain
- Renderer engine latency per engine class
- Environment profile activations and transitions
- Capability claim denials for `ANX_CAP_IFACE_*`
- Wayland compat sessions active

### 20.2 Debug Questions the System Must Answer

- Which cell created this surface, and when?
- Which events have been delivered to this surface in the last minute?
- Why was this surface not rendered in the last frame?
- Which renderer engine is assigned to this surface, and why?
- Why did the environment transition fail?
- Which cells hold `ANX_CAP_IFACE_COMPOSITOR` right now?
- Which surfaces does this agent own?

---

## 21. Open Questions

1. Should `ANX_CONTENT_CANVAS` support vector graphics (SVG-style) in addition to raster pixel buffers? Vector canvases are more voice-renderer-friendly (paths can be described semantically) but add layout engine complexity.

2. Should content nodes support data binding — a live reference to a State Object whose changes automatically trigger `anx_iface_surface_update_content()`? This would make reactive agent UIs natural but adds a subscription mechanism to the kernel.

3. What is the right frame scheduling contract between the compositor cell and GPU renderer for variable-refresh-rate displays? The current model assumes a fixed cadence.

4. How should environment profiles interact with the network plane? A remote node displaying output on a local display requires compositor cells and renderer engines whose outputs cross node boundaries. This is not specified in this RFC.

5. Should the Interface Plane support explicit accessibility layers — content trees that carry additional semantic annotations for screen readers, braille renderers, or switch access devices?

6. Is a single `compositor cell per domain` the right granularity, or should there be a multi-compositor protocol within a domain to support picture-in-picture and similar patterns?

7. Should the `anx:env/*` URI namespace be open (any cell can register a profile) or closed (only kernel-recognized profiles)? Open is more extensible; closed is easier to secure and reason about.

---

## 22. Decision Summary

This RFC makes the following decisions:

1. Interactive surfaces are State Objects (`ANX_OBJ_SURFACE`) with typed semantic content trees, not pixel buffers.
2. Rendering is performed by Renderer Engines registered in the Engine Registry; the routing plane selects renderers for surfaces.
3. Events are State Objects (`ANX_OBJ_EVENT`) with full provenance, routed through the Routing Plane.
4. The compositor is an Execution Cell, not a privileged process; multiple compositors may coexist by domain.
5. Wayland compatibility is a translation layer over the Interface Plane, not the foundation of the Interface Plane.
6. Environment profiles are named kernel-managed configurations; transitions require `ANX_CAP_IFACE_SWITCH_ENV` and are stackable.
7. All Interface Plane operations are capability-gated; no operation relies on ambient process authority.
8. The existing framebuffer driver (`anx_fb`) is preserved as the low-level output mechanism; GPU Renderer Engines wrap it.
9. The `anx_gui` tiled window manager becomes the initial compositor cell implementation.
10. The kernel is authoritative for the surface registry; compositor cells read from and write ordering metadata to the kernel, but cannot corrupt the object model.

---

## 23. Conclusion

The Interface Plane completes the circle begun by RFC-0001. Anunix has now defined how state is stored, how work is executed, how memory is tiered, how execution is routed, how the system extends across networks, how operational knowledge persists, and — with this RFC — how the system presents itself to the world.

The fundamental insight of the Interface Plane is the same insight that runs through every preceding RFC: the right abstraction level for a kernel primitive is semantic, not physical. Files were abstracted to State Objects. Processes were abstracted to Execution Cells. Disk I/O was abstracted to the Memory Control Plane. The kernel's job is not to manage pixels — it is to manage typed, policy-governed, provenance-tracked units of interaction. Pixels, speech, haptic patterns, and motor commands are rendering outcomes, not defining properties.

An operating system that treats all interactive surfaces as framebuffers is forever incapable of voice-native interfaces, haptic-first smartphone interactions, robotics control surfaces, or agent environments that programmatically inspect and modify what the system is showing. The Interface Plane removes that limitation without removing compatibility with the pixel-centric applications that already exist.

The result is an interface system that is:
- **Protocol-composable**: multiple compositor implementations, clean client/compositor separation
- **Medium-agnostic**: the same surface model drives visual, voice, haptic, robotic, and headless environments
- **Agent-native**: surfaces are State Objects; events are routed through the Routing Plane; everything is inspectable, traceable, and programmatically controllable
- **Backward-compatible**: existing Wayland and X11 applications run through the compat layer without modification
- **Secure**: capability-gated, compositor-scoped, event-provenance-tracked

That is what RFC-0001 promised. This RFC delivers it for the interface layer.

---

### Critical Files for Implementation

- `/Users/apippert/Development/Anunix/kernel/include/anx/state_object.h` — requires `ANX_OBJ_SURFACE` and `ANX_OBJ_EVENT` additions to `enum anx_object_type`
- `/Users/apippert/Development/Anunix/kernel/include/anx/engine.h` — requires new `ANX_ENGINE_RENDERER_*` engine classes and `ANX_CAP_RENDER_*` / `ANX_CAP_INPUT_*` capability flags
- `/Users/apippert/Development/Anunix/kernel/include/anx/provenance.h` — requires new surface-specific provenance event types (`ANX_PROV_SURF_MAPPED`, `ANX_PROV_SURF_FOCUSED`, `ANX_PROV_SURF_DAMAGED`, etc.)
- `/Users/apippert/Development/Anunix/kernel/include/anx/interface_plane.h` — new file to create: the master Interface Plane header with all surface, event, environment, and API declarations
- `/Users/apippert/Development/Anunix/kernel/include/anx/gui.h` — the existing `anx_gui` simple window manager becomes the bootstrap compositor cell; its relationship to the new Interface Plane must be formalized
