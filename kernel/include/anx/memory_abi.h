#ifndef ANX_MEMORY_ABI_H
#define ANX_MEMORY_ABI_H
#include <anx/types.h>
#define ANX_MEMORY_ABI_MAX 16U
#define ANX_MEMORY_ABI_SOURCES 8U
#define ANX_MEMORY_ABI_DIMENSIONS 4U
#define ANX_MEMORY_READER_SCHEMA "anx:memory/toy-reader/v1"
#define ANX_MEMORY_SPACE_SCHEMA "anx:memory/toy-space/v1"
struct anx_memory_reader_format { uint32_t format, semantic_schema, dimensions; };
struct anx_memory_space_format { uint32_t format, dimensions, transform; };
struct anx_memory_contract_spec { anx_oid_t reader, space; };
struct anx_memory_contract_view { uint64_t epoch; anx_cid_t owner; struct anx_memory_contract_spec sources; uint32_t semantic_schema, dimensions; };
struct anx_memory_index_view { uint64_t id; anx_cid_t owner; struct anx_memory_contract_spec sources; uint32_t count, dimensions; };
struct anx_memory_match { uint64_t index, contract_epoch; anx_oid_t source; uint32_t squared_distance; };
int anx_memory_contract_set(const anx_cid_t *owner, uint64_t expected_epoch, const struct anx_memory_contract_spec *spec, struct anx_memory_contract_view *out);
int anx_memory_contract_get(const anx_cid_t *owner, struct anx_memory_contract_view *out);
int anx_memory_contract_remove(const anx_cid_t *owner);
int anx_memory_index_build(const anx_cid_t *owner, const anx_oid_t *sources, uint32_t count, struct anx_memory_index_view *out);
int anx_memory_index_query(uint64_t id, const anx_oid_t *query, struct anx_memory_match *out);
int anx_memory_index_destroy(uint64_t id);
#endif
