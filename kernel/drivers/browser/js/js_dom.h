/*
 * js_dom.h — DOM bindings for the JavaScript engine.
 */

#ifndef ANX_JS_DOM_H
#define ANX_JS_DOM_H

#include <anx/types.h>
#include "js_heap.h"
#include "js_val.h"
#include "js_obj.h"
#include "js_vm.h"
#include "../html/dom.h"
#include "../html/dom_extra.h"

struct js_engine;

/* Populate the global object with DOM + window + console bindings. */
void js_dom_init(struct js_engine *eng, struct dom_doc *doc);

/* Wrap a dom_node * in a js_obj.  Caches wrapped nodes in a 64-entry LUT. */
js_val js_dom_wrap_node(struct js_engine *eng, struct dom_node *node);

/* Dispatch a DOM event (click / input / submit) to registered listeners.
 * Returns true if any listener was called. */
bool js_dom_dispatch_event(struct js_engine *eng, struct dom_node *node,
                           const char *event_type);

#endif /* ANX_JS_DOM_H */
