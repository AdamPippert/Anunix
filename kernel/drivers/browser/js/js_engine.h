/*
 * js_engine.h — Public JavaScript engine API.
 *
 * One js_engine per browser session.  Lifecycle:
 *   js_engine_init()   — allocate, zero, set up VM + globals
 *   js_engine_load()   — parse + compile + execute a <script> block
 *   js_engine_event()  — fire a DOM event (click, submit, input)
 *   js_engine_reset()  — tear down, free heap resources
 */

#ifndef ANX_JS_ENGINE_H
#define ANX_JS_ENGINE_H

#include <anx/types.h>
#include "js_heap.h"
#include "js_val.h"
#include "js_obj.h"
#include "js_vm.h"
#include "js_parse.h"
#include "js_compile.h"

#include "../html/dom.h"
#include "../html/dom_extra.h"

#define DOM_WRAP_CACHE  64

struct js_engine {
	struct js_heap      *heap;          /* borrowed from session */
	struct js_obj       *globals;       /* global object (owned by heap) */
	struct js_vm         vm;
	struct js_parser     parser;        /* reused across script loads */
	struct js_compiler   compiler;      /* accumulates all func protos */
	struct dom_doc      *doc;           /* current document (borrowed) */

	/* Node wrapper cache */
	struct dom_node     *wrap_cache_node[DOM_WRAP_CACHE];
	js_val               wrap_cache_val[DOM_WRAP_CACHE];

	bool                 initialized;
};

/* Initialize engine; heap is borrowed from the session. */
void js_engine_init(struct js_engine *eng, struct js_heap *heap,
                    struct dom_doc *doc);

/* Parse, compile, and execute src[0..len-1] as a script.
 * Returns false if parse or compile failed (runtime errors are silenced). */
bool js_engine_load(struct js_engine *eng, const char *src, uint32_t len);

/* Dispatch a DOM event to registered listeners.
 * Returns true if at least one listener fired. */
bool js_engine_event(struct js_engine *eng, struct dom_node *node,
                     const char *event_type);

/* Reset: re-zero parser/compiler state; keep global object intact. */
void js_engine_reset_script(struct js_engine *eng);

/* Full teardown (engine no longer usable without re-init). */
void js_engine_destroy(struct js_engine *eng);

/* Called by VM for native function calls (C functions stored in js_obj). */
js_val js_engine_call_native(struct js_engine *eng, js_val callee,
                             js_val this_val, js_val *args, uint8_t argc);

/* Used by js_std.c / js_dom.c to register native functions */
typedef js_val (*js_native_fn)(struct js_engine *eng, js_val this_val,
                               js_val *args, uint8_t argc);
js_val js_engine_make_native(struct js_engine *eng, js_native_fn fn);

/* Forward declarations needed by js_dom.c for native methods */
js_val js_dom_native_get_attr(struct js_engine *eng, js_val this_val, js_val *args, uint8_t argc);
js_val js_dom_native_set_attr(struct js_engine *eng, js_val this_val, js_val *args, uint8_t argc);
js_val js_dom_native_add_listener(struct js_engine *eng, js_val this_val, js_val *args, uint8_t argc);
js_val js_dom_native_remove_listener(struct js_engine *eng, js_val this_val, js_val *args, uint8_t argc);
js_val js_dom_native_append_child(struct js_engine *eng, js_val this_val, js_val *args, uint8_t argc);
js_val js_dom_native_remove_child(struct js_engine *eng, js_val this_val, js_val *args, uint8_t argc);
js_val js_dom_native_query_selector(struct js_engine *eng, js_val this_val, js_val *args, uint8_t argc);

#endif /* ANX_JS_ENGINE_H */
