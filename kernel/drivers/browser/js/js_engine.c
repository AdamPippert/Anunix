/*
 * js_engine.c — JavaScript engine integration layer.
 */

#include "js_engine.h"
#include "js_dom.h"
#include "js_std.h"
#include "js_str.h"
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── make_native — shared by js_dom.c and js_std.c ─────────────────── */

js_val js_engine_make_native(struct js_engine *eng, js_native_fn fn)
{
	struct js_obj *fo = js_obj_new(eng->heap, JV_NULL);
	if (!fo) return JV_NULL;
	fo->is_function = 1;
	uintptr_t ptr = (uintptr_t)fn;
	js_obj_set_cstr(eng->heap, fo, "__np_lo__",
	                jv_int((int32_t)(ptr & 0xFFFFFFFFu)));
#if defined(__LP64__) || defined(_LP64)
	js_obj_set_cstr(eng->heap, fo, "__np_hi__",
	                jv_int((int32_t)((ptr >> 32) & 0xFFFFFFFFu)));
#else
	js_obj_set_cstr(eng->heap, fo, "__np_hi__", jv_int(0));
#endif
	return jv_obj(fo);
}

/* ── Native call dispatch ───────────────────────────────────────────── */

js_val js_engine_call_native(struct js_engine *eng, js_val callee,
                             js_val this_val, js_val *args, uint8_t argc)
{
	if (!jv_is_obj(callee)) return JV_UNDEF;
	struct js_obj *fo = (struct js_obj *)jv_to_ptr(callee);
	js_val lo = js_obj_get_cstr(eng->heap, fo, "__np_lo__");
	if (jv_is_undef(lo)) return JV_UNDEF;
	js_val hi = js_obj_get_cstr(eng->heap, fo, "__np_hi__");
	uintptr_t ptr = (uintptr_t)(uint32_t)jv_to_int(lo);
#if defined(__LP64__) || defined(_LP64)
	ptr |= ((uintptr_t)(uint32_t)jv_to_int(hi)) << 32;
#endif
	(void)hi;
	js_native_fn fn = (js_native_fn)ptr;
	return fn(eng, this_val, args, argc);
}

/* ── Init ───────────────────────────────────────────────────────────── */

void js_engine_init(struct js_engine *eng, struct js_heap *heap,
                    struct dom_doc *doc)
{
	uint32_t i;
	eng->heap = heap;
	eng->doc  = doc;
	eng->initialized = true;

	/* Global object */
	eng->globals = js_obj_new(heap, JV_NULL);
	if (!eng->globals) {
		kprintf("js_engine: failed to allocate globals\n");
		return;
	}

	/* Clear wrap cache */
	for (i = 0; i < DOM_WRAP_CACHE; i++) {
		eng->wrap_cache_node[i] = NULL;
		eng->wrap_cache_val[i]  = JV_UNDEF;
	}

	/* Clear timer queue */
	eng->next_timer_id = 0;
	for (i = 0; i < JS_TIMER_MAX; i++)
		eng->timers[i].id = -1;

	/* Zero compiler */
	eng->compiler.n_funcs   = 0;
	eng->compiler.cur_func  = 0;
	eng->compiler.error     = false;
	eng->compiler.error_msg = NULL;

	/* Init VM */
	js_vm_init(&eng->vm, heap, eng->globals);

	/* Populate standard library */
	js_std_init(eng);

	/* Populate DOM bindings */
	js_dom_init(eng, (struct dom_doc *)doc);
}

/* ── Load script ────────────────────────────────────────────────────── */

bool js_engine_load(struct js_engine *eng, const char *src, uint32_t len)
{
	if (!eng->initialized) return false;

	/* Parse */
	uint16_t root = js_parse(&eng->parser, eng->heap, src, len);
	if (root == JS_NODE_NULL || eng->parser.error) {
		kprintf("js_engine: parse error at line %u: %s\n",
			eng->parser.error_line,
			eng->parser.error_msg ? eng->parser.error_msg : "unknown");
		return false;
	}

	/* Compile into the shared compiler (accumulates funcs across loads) */
	eng->compiler.parser = &eng->parser;
	eng->compiler.heap   = eng->heap;
	eng->compiler.error  = false;

	if (eng->compiler.n_funcs == 0) {
		/* First load — reserve slot 0 as script body */
		eng->compiler.n_funcs = 1;
		eng->compiler.funcs[0].code_len = 0;
		eng->compiler.funcs[0].n_consts = 0;
		eng->compiler.funcs[0].n_params = 0;
		eng->compiler.funcs[0].n_locals = 0;
		anx_strlcpy(eng->compiler.funcs[0].name, "<script>",
		            sizeof(eng->compiler.funcs[0].name));
		eng->compiler.scope_depth = 0;
		eng->compiler.scopes[0].n_locals = 0;
	} else {
		/* Subsequent load: fresh script slot */
		if (eng->compiler.n_funcs >= JS_FUNC_MAX) {
			kprintf("js_engine: too many script blocks\n");
			return false;
		}
		eng->compiler.cur_func = eng->compiler.n_funcs;
		eng->compiler.n_funcs++;
		struct js_func_proto *fn = &eng->compiler.funcs[eng->compiler.cur_func];
		fn->code_len = 0;
		fn->n_consts = 0;
		fn->n_params = 0;
		fn->n_locals = 0;
		anx_strlcpy(fn->name, "<script>", sizeof(fn->name));
		eng->compiler.scope_depth = 0;
		eng->compiler.scopes[0].n_locals = 0;
	}

	if (!js_compile(&eng->compiler, &eng->parser, eng->heap, root)) {
		kprintf("js_engine: compile error: %s\n",
			eng->compiler.error_msg ? eng->compiler.error_msg : "unknown");
		return false;
	}

	/* Execute the script body */
	js_val no_args[1];
	js_vm_exec(&eng->vm,
	           &eng->compiler.funcs[eng->compiler.cur_func],
	           eng->compiler.funcs,
	           eng->compiler.n_funcs,
	           jv_obj(eng->globals),
	           no_args, 0);

	if (eng->vm.threw) {
		kprintf("js_engine: uncaught exception\n");
		eng->vm.threw = false;
		eng->vm.exception = JV_UNDEF;
	}

	return true;
}

/* ── Event dispatch ─────────────────────────────────────────────────── */

bool js_engine_event(struct js_engine *eng, struct dom_node *node,
                     const char *event_type)
{
	if (!eng->initialized) return false;
	return js_dom_dispatch_event(eng, node, event_type);
}

/* ── Reset / destroy ────────────────────────────────────────────────── */

void js_engine_reset_script(struct js_engine *eng)
{
	uint32_t i;
	eng->compiler.n_funcs  = 0;
	eng->compiler.cur_func = 0;
	eng->compiler.error    = false;
	for (i = 0; i < DOM_WRAP_CACHE; i++) {
		eng->wrap_cache_node[i] = NULL;
		eng->wrap_cache_val[i]  = JV_UNDEF;
	}
}

void js_engine_destroy(struct js_engine *eng)
{
	eng->initialized = false;
	eng->globals     = NULL;
	eng->doc         = NULL;
}
