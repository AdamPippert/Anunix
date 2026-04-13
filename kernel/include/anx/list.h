/*
 * anx/list.h — Intrusive doubly-linked list (Linux-style).
 *
 * Header-only. The list head is embedded in the containing struct.
 * Use ANX_LIST_ENTRY to recover the containing struct from a list node.
 */

#ifndef ANX_LIST_H
#define ANX_LIST_H

#include <anx/types.h>

struct anx_list_head {
	struct anx_list_head *next;
	struct anx_list_head *prev;
};

#define ANX_LIST_HEAD_INIT(name) { &(name), &(name) }

/* Recover the containing struct from a list pointer */
#define ANX_CONTAINER_OF(ptr, type, member) \
	((type *)((uint8_t *)(ptr) - __builtin_offsetof(type, member)))

#define ANX_LIST_ENTRY(ptr, type, member) \
	ANX_CONTAINER_OF(ptr, type, member)

/* Iterate over a list */
#define ANX_LIST_FOR_EACH(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

/* Iterate safely (allows deletion of current node) */
#define ANX_LIST_FOR_EACH_SAFE(pos, tmp, head) \
	for (pos = (head)->next, tmp = pos->next; \
	     pos != (head); \
	     pos = tmp, tmp = pos->next)

static inline void anx_list_init(struct anx_list_head *head)
{
	head->next = head;
	head->prev = head;
}

static inline void anx_list_add(struct anx_list_head *entry,
				struct anx_list_head *head)
{
	entry->next = head->next;
	entry->prev = head;
	head->next->prev = entry;
	head->next = entry;
}

static inline void anx_list_add_tail(struct anx_list_head *entry,
				     struct anx_list_head *head)
{
	entry->next = head;
	entry->prev = head->prev;
	head->prev->next = entry;
	head->prev = entry;
}

static inline void anx_list_del(struct anx_list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->next = NULL;
	entry->prev = NULL;
}

static inline bool anx_list_empty(const struct anx_list_head *head)
{
	return head->next == head;
}

#endif /* ANX_LIST_H */
