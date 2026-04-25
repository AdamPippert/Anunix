/*
 * js_dom.c — DOM bindings.
 *
 * Native functions are registered as js_obj values with is_function=1
 * and a "__native_id__" integer property.  The VM's OP_CALL_METHOD
 * path detects these and dispatches through js_engine_call_native().
 *
 * To avoid needing a global dispatch table we instead use a simpler
 * approach: each native js_obj stores a C function pointer as two
 * 32-bit halves in properties "__np_lo__" and "__np_hi__".  The engine
 * extracts and calls it directly.
 */

#include "js_dom.h"
#include "js_engine.h"
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── Native function registration ─────────────────────────────────── */

typedef js_val (*js_native_fn)(struct js_engine *eng, js_val this_val,
                               js_val *args, uint8_t argc);

static js_val make_native(struct js_engine *eng, js_native_fn fn)
{
	struct js_obj *fo = js_obj_new(eng->heap, JV_NULL);
	if (!fo) return JV_NULL;
	fo->is_function = 1;
	uintptr_t ptr = (uintptr_t)fn;
	js_obj_set_cstr(eng->heap, fo, "__np_lo__", jv_int((int32_t)(ptr & 0xFFFFFFFFu)));
#if defined(__LP64__) || defined(_LP64)
	js_obj_set_cstr(eng->heap, fo, "__np_hi__", jv_int((int32_t)((ptr >> 32) & 0xFFFFFFFFu)));
#else
	js_obj_set_cstr(eng->heap, fo, "__np_hi__", jv_int(0));
#endif
	return jv_obj(fo);
}

js_val js_engine_call_native(struct js_engine *eng, js_val callee,
                             js_val this_val, js_val *args, uint8_t argc)
{
	if (!jv_is_obj(callee)) return JV_UNDEF;
	struct js_obj *fo = (struct js_obj *)jv_to_ptr(callee);
	js_val lo = js_obj_get_cstr(eng->heap, fo, "__np_lo__");
	js_val hi = js_obj_get_cstr(eng->heap, fo, "__np_hi__");
	if (jv_is_undef(lo)) return JV_UNDEF;
	uintptr_t ptr = (uintptr_t)(uint32_t)jv_to_int(lo);
#if defined(__LP64__) || defined(_LP64)
	ptr |= ((uintptr_t)(uint32_t)jv_to_int(hi)) << 32;
#endif
	(void)hi;
	js_native_fn fn = (js_native_fn)ptr;
	return fn(eng, this_val, args, argc);
}

/* ── Node wrapper cache ────────────────────────────────────────────── */

#define DOM_WRAP_CACHE  64

js_val js_dom_wrap_node(struct js_engine *eng, struct dom_node *node)
{
	if (!node) return JV_NULL;

	/* Check cache */
	uint32_t h = (uint32_t)((uintptr_t)node >> 3) % DOM_WRAP_CACHE;
	if (eng->wrap_cache_node[h] == node)
		return eng->wrap_cache_val[h];

	struct js_obj *o = js_obj_new(eng->heap, JV_NULL);
	if (!o) return JV_NULL;

	/* Store pointer as two 32-bit ints */
	uintptr_t ptr = (uintptr_t)node;
	js_obj_set_cstr(eng->heap, o, "__dom_lo__", jv_int((int32_t)(ptr & 0xFFFFFFFFu)));
#if defined(__LP64__) || defined(_LP64)
	js_obj_set_cstr(eng->heap, o, "__dom_hi__", jv_int((int32_t)((ptr >> 32) & 0xFFFFFFFFu)));
#else
	js_obj_set_cstr(eng->heap, o, "__dom_hi__", jv_int(0));
#endif

	/* Attach methods */
	js_obj_set_cstr(eng->heap, o, "getAttribute",
	                make_native(eng, js_dom_native_get_attr));
	js_obj_set_cstr(eng->heap, o, "setAttribute",
	                make_native(eng, js_dom_native_set_attr));
	js_obj_set_cstr(eng->heap, o, "addEventListener",
	                make_native(eng, js_dom_native_add_listener));
	js_obj_set_cstr(eng->heap, o, "removeEventListener",
	                make_native(eng, js_dom_native_remove_listener));
	js_obj_set_cstr(eng->heap, o, "appendChild",
	                make_native(eng, js_dom_native_append_child));
	js_obj_set_cstr(eng->heap, o, "removeChild",
	                make_native(eng, js_dom_native_remove_child));
	js_obj_set_cstr(eng->heap, o, "querySelector",
	                make_native(eng, js_dom_native_query_selector));

	/* innerHTML / textContent as getter-ish set via property */
	if (node->type == DOM_ELEMENT) {
		const char *tc = dom_text_content(node);
		if (tc) {
			struct js_str *s = js_str_from_cstr(eng->heap, tc);
			if (s) js_obj_set_cstr(eng->heap, o, "textContent", jv_str(s));
			js_obj_set_cstr(eng->heap, o, "innerHTML", jv_str(s ? s : (struct js_str*)0));
		}
		const char *tag = dom_tag_name(node);
		if (tag) {
			struct js_str *ts = js_str_from_cstr(eng->heap, tag);
			if (ts) js_obj_set_cstr(eng->heap, o, "tagName", jv_str(ts));
		}
	}

	js_val wrapped = jv_obj(o);
	eng->wrap_cache_node[h] = node;
	eng->wrap_cache_val[h]  = wrapped;
	return wrapped;
}

static struct dom_node *unwrap_node(struct js_engine *eng, js_val v)
{
	if (!jv_is_obj(v)) return NULL;
	struct js_obj *o = (struct js_obj *)jv_to_ptr(v);
	js_val lo = js_obj_get_cstr(eng->heap, o, "__dom_lo__");
	js_val hi = js_obj_get_cstr(eng->heap, o, "__dom_hi__");
	if (jv_is_undef(lo)) return NULL;
	uintptr_t ptr = (uintptr_t)(uint32_t)jv_to_int(lo);
#if defined(__LP64__) || defined(_LP64)
	ptr |= ((uintptr_t)(uint32_t)jv_to_int(hi)) << 32;
#endif
	(void)hi;
	return (struct dom_node *)ptr;
}

/* ── Native method implementations ────────────────────────────────── */

js_val js_dom_native_get_attr(struct js_engine *eng, js_val this_val,
                              js_val *args, uint8_t argc)
{
	if (argc < 1 || !jv_is_str(args[0])) return JV_NULL;
	struct dom_node *node = unwrap_node(eng, this_val);
	if (!node) return JV_NULL;
	const char *val = dom_attr(node, js_str_data(jv_to_str(args[0])));
	if (!val) return JV_NULL;
	struct js_str *s = js_str_from_cstr(eng->heap, val);
	return s ? jv_str(s) : JV_NULL;
}

js_val js_dom_native_set_attr(struct js_engine *eng, js_val this_val,
                              js_val *args, uint8_t argc)
{
	if (argc < 2 || !jv_is_str(args[0])) return JV_UNDEF;
	struct dom_node *node = unwrap_node(eng, this_val);
	if (!node) return JV_UNDEF;
	char val_buf[256] = "";
	if (jv_is_str(args[1])) {
		const struct js_str *s = jv_to_str(args[1]);
		uint32_t cp = s->len < 255 ? s->len : 255;
		anx_memcpy(val_buf, js_str_data(s), cp);
		val_buf[cp] = '\0';
	}
	dom_attr_set(node, js_str_data(jv_to_str(args[0])), val_buf);
	return JV_UNDEF;
}

/* Event listener table — simple flat array */
#define MAX_LISTENERS  64

struct dom_listener {
	struct dom_node *node;
	char             event[32];
	js_val           fn;
};

static struct dom_listener s_listeners[MAX_LISTENERS];
static uint32_t s_n_listeners;

js_val js_dom_native_add_listener(struct js_engine *eng, js_val this_val,
                                  js_val *args, uint8_t argc)
{
	if (argc < 2 || !jv_is_str(args[0])) return JV_UNDEF;
	struct dom_node *node = unwrap_node(eng, this_val);
	if (!node || s_n_listeners >= MAX_LISTENERS) return JV_UNDEF;
	struct dom_listener *l = &s_listeners[s_n_listeners++];
	l->node = node;
	const struct js_str *ev = jv_to_str(args[0]);
	uint32_t cp = ev->len < 31 ? ev->len : 31;
	anx_memcpy(l->event, js_str_data(ev), cp);
	l->event[cp] = '\0';
	l->fn = args[1];
	(void)eng;
	return JV_UNDEF;
}

js_val js_dom_native_remove_listener(struct js_engine *eng, js_val this_val,
                                     js_val *args, uint8_t argc)
{
	(void)eng;
	if (argc < 2 || !jv_is_str(args[0])) return JV_UNDEF;
	struct dom_node *node = unwrap_node(eng, this_val);
	if (!node) return JV_UNDEF;
	const struct js_str *ev = jv_to_str(args[0]);
	char event[32];
	uint32_t cp = ev->len < 31 ? ev->len : 31;
	anx_memcpy(event, js_str_data(ev), cp);
	event[cp] = '\0';
	uint32_t i;
	for (i = 0; i < s_n_listeners; i++) {
		if (s_listeners[i].node == node &&
		    anx_strcmp(s_listeners[i].event, event) == 0) {
			/* Shift remaining entries down */
			uint32_t j;
			for (j = i; j + 1 < s_n_listeners; j++)
				s_listeners[j] = s_listeners[j + 1];
			s_n_listeners--;
			return JV_UNDEF;
		}
	}
	return JV_UNDEF;
}

js_val js_dom_native_append_child(struct js_engine *eng, js_val this_val,
                                  js_val *args, uint8_t argc)
{
	if (argc < 1) return JV_UNDEF;
	struct dom_node *parent = unwrap_node(eng, this_val);
	struct dom_node *child  = unwrap_node(eng, args[0]);
	if (!parent || !child) return JV_UNDEF;
	dom_append_child(parent, child);
	return args[0];
}

js_val js_dom_native_remove_child(struct js_engine *eng, js_val this_val,
                                  js_val *args, uint8_t argc)
{
	if (argc < 1) return JV_UNDEF;
	struct dom_node *parent = unwrap_node(eng, this_val);
	struct dom_node *child  = unwrap_node(eng, args[0]);
	if (!parent || !child) return JV_UNDEF;
	dom_remove_child(parent, child);
	return args[0];
}

js_val js_dom_native_query_selector(struct js_engine *eng, js_val this_val,
                                    js_val *args, uint8_t argc)
{
	if (argc < 1 || !jv_is_str(args[0])) return JV_NULL;
	struct dom_node *ctx = unwrap_node(eng, this_val);
	if (!ctx) ctx = eng->doc->root;
	struct dom_node *found = dom_query_selector(ctx, js_str_data(jv_to_str(args[0])));
	return found ? js_dom_wrap_node(eng, found) : JV_NULL;
}

/* ── document object ───────────────────────────────────────────────── */

static js_val native_get_by_id(struct js_engine *eng, js_val this_val,
                                js_val *args, uint8_t argc)
{
	(void)this_val;
	if (argc < 1 || !jv_is_str(args[0])) return JV_NULL;
	struct dom_node *n = dom_get_element_by_id(eng->doc->root,
	                                           js_str_data(jv_to_str(args[0])));
	return n ? js_dom_wrap_node(eng, n) : JV_NULL;
}

static js_val native_query_selector(struct js_engine *eng, js_val this_val,
                                    js_val *args, uint8_t argc)
{
	(void)this_val;
	if (argc < 1 || !jv_is_str(args[0])) return JV_NULL;
	struct dom_node *n = dom_query_selector(eng->doc->root,
	                                        js_str_data(jv_to_str(args[0])));
	return n ? js_dom_wrap_node(eng, n) : JV_NULL;
}

static js_val native_create_element(struct js_engine *eng, js_val this_val,
                                    js_val *args, uint8_t argc)
{
	(void)this_val;
	if (argc < 1 || !jv_is_str(args[0])) return JV_NULL;
	struct dom_node *n = dom_create_element(eng->doc,
	                                        js_str_data(jv_to_str(args[0])));
	return n ? js_dom_wrap_node(eng, n) : JV_NULL;
}

static js_val native_create_text(struct js_engine *eng, js_val this_val,
                                 js_val *args, uint8_t argc)
{
	(void)this_val;
	if (argc < 1 || !jv_is_str(args[0])) return JV_NULL;
	struct dom_node *n = dom_create_text(eng->doc,
	                                     js_str_data(jv_to_str(args[0])));
	return n ? js_dom_wrap_node(eng, n) : JV_NULL;
}

/* ── console ───────────────────────────────────────────────────────── */

static js_val native_console_log(struct js_engine *eng, js_val this_val,
                                 js_val *args, uint8_t argc)
{
	(void)this_val; (void)eng;
	uint8_t i;
	for (i = 0; i < argc; i++) {
		if (i) kprintf(" ");
		if (jv_is_str(args[i]))
			kprintf("%s", js_str_data(jv_to_str(args[i])));
		else if (jv_is_int(args[i]))
			kprintf("%d", jv_to_int(args[i]));
		else if (jv_is_double(args[i]))
			kprintf("%d", (int)jv_to_double(args[i]));
		else if (jv_is_bool(args[i]))
			kprintf("%s", jv_to_bool_raw(args[i]) ? "true" : "false");
		else if (jv_is_null(args[i]))
			kprintf("null");
		else if (jv_is_undef(args[i]))
			kprintf("undefined");
		else
			kprintf("[object]");
	}
	kprintf("\n");
	return JV_UNDEF;
}

/* ── setTimeout stub ───────────────────────────────────────────────── */

static js_val native_set_timeout(struct js_engine *eng, js_val this_val,
                                  js_val *args, uint8_t argc)
{
	(void)this_val;
	/* Stub: execute callback immediately with 0 args */
	if (argc < 1) return jv_int(0);
	js_val no_args[1];
	js_vm_call(&eng->vm, args[0], JV_UNDEF, no_args, 0,
	           eng->compiler.funcs, eng->compiler.n_funcs);
	return jv_int(1);
}

/* ── init ──────────────────────────────────────────────────────────── */

void js_dom_init(struct js_engine *eng, struct dom_doc *doc)
{
	eng->doc = doc;
	s_n_listeners = 0;

	/* Build document object */
	struct js_obj *document = js_obj_new(eng->heap, JV_NULL);
	if (!document) return;
	js_obj_set_cstr(eng->heap, document, "getElementById",
	                make_native(eng, native_get_by_id));
	js_obj_set_cstr(eng->heap, document, "querySelector",
	                make_native(eng, native_query_selector));
	js_obj_set_cstr(eng->heap, document, "createElement",
	                make_native(eng, native_create_element));
	js_obj_set_cstr(eng->heap, document, "createTextNode",
	                make_native(eng, native_create_text));
	/* document.body */
	struct dom_node *body = doc->root ? dom_query_selector(doc->root, "body") : NULL;
	if (body)
		js_obj_set_cstr(eng->heap, document, "body",
		                js_dom_wrap_node(eng, body));

	js_obj_set_cstr(eng->heap, eng->vm.globals,"document", jv_obj(document));

	/* Build console object */
	struct js_obj *console_obj = js_obj_new(eng->heap, JV_NULL);
	if (console_obj) {
		js_obj_set_cstr(eng->heap, console_obj, "log",
		                make_native(eng, native_console_log));
		js_obj_set_cstr(eng->heap, console_obj, "error",
		                make_native(eng, native_console_log));
		js_obj_set_cstr(eng->heap, console_obj, "warn",
		                make_native(eng, native_console_log));
		js_obj_set_cstr(eng->heap, eng->vm.globals,"console", jv_obj(console_obj));
	}

	/* window = global proxy */
	js_obj_set_cstr(eng->heap, eng->vm.globals,"window", jv_obj(eng->vm.globals));

	/* setTimeout */
	js_obj_set_cstr(eng->heap, eng->vm.globals,"setTimeout",
	                make_native(eng, native_set_timeout));
	js_obj_set_cstr(eng->heap, eng->vm.globals,"clearTimeout",
	                make_native(eng, native_set_timeout));
}

/* ── Event dispatch ────────────────────────────────────────────────── */

bool js_dom_dispatch_event(struct js_engine *eng, struct dom_node *node,
                           const char *event_type)
{
	bool any = false;
	uint32_t i;
	for (i = 0; i < s_n_listeners; i++) {
		struct dom_listener *l = &s_listeners[i];
		if (l->node == node && anx_strcmp(l->event, event_type) == 0) {
			/* Build a minimal event object */
			struct js_obj *ev_obj = js_obj_new(eng->heap, JV_NULL);
			js_val ev_val = ev_obj ? jv_obj(ev_obj) : JV_UNDEF;
			if (ev_obj) {
				struct js_str *ts = js_str_from_cstr(eng->heap, event_type);
				if (ts) js_obj_set_cstr(eng->heap, ev_obj, "type", jv_str(ts));
				js_obj_set_cstr(eng->heap, ev_obj, "target",
				                js_dom_wrap_node(eng, node));
			}
			js_val args[1] = { ev_val };
			js_vm_call(&eng->vm, l->fn, JV_UNDEF, args, 1,
			           eng->compiler.funcs, eng->compiler.n_funcs);
			any = true;
		}
	}
	return any;
}
