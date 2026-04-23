/*
 * js_std.h — JavaScript standard library: Math, JSON, parseInt, etc.
 */

#ifndef ANX_JS_STD_H
#define ANX_JS_STD_H

#include <anx/types.h>

struct js_engine;

/* Populate global object with Math, JSON, parseInt, parseFloat,
 * isNaN, isFinite, String(), Number(), Boolean(). */
void js_std_init(struct js_engine *eng);

#endif /* ANX_JS_STD_H */
