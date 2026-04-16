/*
 * anx/json.h — Minimal JSON parser.
 *
 * Recursive descent parser producing a tree of anx_json_value nodes.
 * Supports objects, arrays, strings, numbers, booleans, and null.
 * No streaming — parses the entire input into a heap-allocated tree.
 */

#ifndef ANX_JSON_H
#define ANX_JSON_H

#include <anx/types.h>

enum anx_json_type {
	ANX_JSON_NULL,
	ANX_JSON_BOOL,
	ANX_JSON_NUMBER,
	ANX_JSON_STRING,
	ANX_JSON_ARRAY,
	ANX_JSON_OBJECT,
};

struct anx_json_value {
	enum anx_json_type type;
	union {
		bool boolean;
		int64_t number;			/* integers only for simplicity */
		struct {
			char *str;
			uint32_t len;
		} string;
		struct {
			struct anx_json_value *items;
			uint32_t count;
		} array;
		struct {
			struct anx_json_kv *pairs;
			uint32_t count;
		} object;
	} v;
};

struct anx_json_kv {
	char *key;
	uint32_t key_len;
	struct anx_json_value value;
};

/* Parse a JSON string into a value tree. Returns ANX_OK or error. */
int anx_json_parse(const char *input, uint32_t len,
		    struct anx_json_value *out);

/* Free all memory in a parsed value tree */
void anx_json_free(struct anx_json_value *val);

/* Query helpers — return NULL/0/false on type mismatch or missing key */

/* Get a value from an object by key */
struct anx_json_value *anx_json_get(struct anx_json_value *obj,
				     const char *key);

/* Get an array element by index */
struct anx_json_value *anx_json_array_get(struct anx_json_value *arr,
					   uint32_t index);

/* Extract typed values (return default on type mismatch) */
const char *anx_json_string(struct anx_json_value *val);
int64_t anx_json_number(struct anx_json_value *val);
bool anx_json_bool(struct anx_json_value *val);
uint32_t anx_json_array_len(struct anx_json_value *val);

#endif /* ANX_JSON_H */
