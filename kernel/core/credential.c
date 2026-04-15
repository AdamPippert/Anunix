/*
 * credential.c — Kernel credential store (RFC-0008 Phase 1).
 *
 * Stores secrets as opaque Credential Objects with kernel-enforced
 * invariants: payloads never appear in traces, provenance, or
 * kprintf output. Access is mediated and audited.
 *
 * Phase 1: flat store with shell access. No cell binding enforcement
 * yet — that comes in Phase 2.
 */

#include <anx/types.h>
#include <anx/credential.h>
#include <anx/arch.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/spinlock.h>
#include <anx/kprintf.h>

#define CREDSTORE_MAX	16

struct credential_entry {
	char name[128];
	enum anx_credential_type cred_type;
	void *secret;			/* heap-allocated, zeroed on free */
	uint32_t secret_len;
	anx_time_t created_at;
	anx_time_t last_accessed;
	uint32_t access_count;
	bool active;
};

static struct credential_entry credstore[CREDSTORE_MAX];
static struct anx_spinlock credstore_lock = ANX_SPINLOCK_INIT;

/* Constant-time memory zeroing that the compiler cannot optimize out */
static void secure_zero(void *ptr, uint32_t len)
{
	volatile uint8_t *p = (volatile uint8_t *)ptr;
	uint32_t i;

	for (i = 0; i < len; i++)
		p[i] = 0;
}

static struct credential_entry *find_by_name(const char *name)
{
	int i;

	for (i = 0; i < CREDSTORE_MAX; i++) {
		if (credstore[i].active &&
		    anx_strcmp(credstore[i].name, name) == 0)
			return &credstore[i];
	}
	return NULL;
}

static struct credential_entry *find_free_slot(void)
{
	int i;

	for (i = 0; i < CREDSTORE_MAX; i++) {
		if (!credstore[i].active)
			return &credstore[i];
	}
	return NULL;
}

void anx_credstore_init(void)
{
	anx_memset(credstore, 0, sizeof(credstore));
	anx_spin_init(&credstore_lock);
}

int anx_credential_create(const char *name,
			    enum anx_credential_type cred_type,
			    const void *secret, uint32_t secret_len)
{
	struct credential_entry *entry;
	bool irq_state;
	void *secret_copy;

	if (!name || !secret || secret_len == 0)
		return ANX_EINVAL;

	/* Allocate payload before taking the lock */
	secret_copy = anx_alloc(secret_len);
	if (!secret_copy)
		return ANX_ENOMEM;
	anx_memcpy(secret_copy, secret, secret_len);

	anx_spin_lock_irqsave(&credstore_lock, &irq_state);

	/* Check for duplicate name */
	if (find_by_name(name)) {
		anx_spin_unlock_irqrestore(&credstore_lock, irq_state);
		secure_zero(secret_copy, secret_len);
		anx_free(secret_copy);
		return ANX_EEXIST;
	}

	entry = find_free_slot();
	if (!entry) {
		anx_spin_unlock_irqrestore(&credstore_lock, irq_state);
		secure_zero(secret_copy, secret_len);
		anx_free(secret_copy);
		return ANX_ENOMEM;
	}

	anx_strlcpy(entry->name, name, sizeof(entry->name));
	entry->cred_type = cred_type;
	entry->secret = secret_copy;
	entry->secret_len = secret_len;
	entry->created_at = arch_time_now();
	entry->last_accessed = 0;
	entry->access_count = 0;
	entry->active = true;

	anx_spin_unlock_irqrestore(&credstore_lock, irq_state);

	kprintf("credential: %s stored (%u bytes)\n",
		name, secret_len);
	return ANX_OK;
}

int anx_credential_read(const char *name,
			 void *buf, uint32_t buf_len,
			 uint32_t *actual_len)
{
	struct credential_entry *entry;
	bool irq_state;
	uint32_t copy_len;

	if (!name || !buf)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&credstore_lock, &irq_state);

	entry = find_by_name(name);
	if (!entry) {
		anx_spin_unlock_irqrestore(&credstore_lock, irq_state);
		/* Audit: denied access (credential not found) */
		return ANX_ENOENT;
	}

	copy_len = entry->secret_len;
	if (copy_len > buf_len)
		copy_len = buf_len;

	anx_memcpy(buf, entry->secret, copy_len);
	entry->last_accessed = arch_time_now();
	entry->access_count++;

	if (actual_len)
		*actual_len = entry->secret_len;

	anx_spin_unlock_irqrestore(&credstore_lock, irq_state);

	/* Audit: successful access (name only, never payload) */
	return ANX_OK;
}

int anx_credential_info(const char *name,
			 struct anx_credential_info *info)
{
	struct credential_entry *entry;
	bool irq_state;

	if (!name || !info)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&credstore_lock, &irq_state);

	entry = find_by_name(name);
	if (!entry) {
		anx_spin_unlock_irqrestore(&credstore_lock, irq_state);
		return ANX_ENOENT;
	}

	anx_strlcpy(info->name, entry->name, sizeof(info->name));
	info->cred_type = entry->cred_type;
	info->secret_len = entry->secret_len;
	info->created_at = entry->created_at;
	info->last_accessed = entry->last_accessed;
	info->access_count = entry->access_count;
	info->active = entry->active;

	anx_spin_unlock_irqrestore(&credstore_lock, irq_state);
	return ANX_OK;
}

int anx_credential_rotate(const char *name,
			    const void *new_secret, uint32_t new_len)
{
	struct credential_entry *entry;
	bool irq_state;
	void *new_copy;
	void *old_secret;
	uint32_t old_len;

	if (!name || !new_secret || new_len == 0)
		return ANX_EINVAL;

	new_copy = anx_alloc(new_len);
	if (!new_copy)
		return ANX_ENOMEM;
	anx_memcpy(new_copy, new_secret, new_len);

	anx_spin_lock_irqsave(&credstore_lock, &irq_state);

	entry = find_by_name(name);
	if (!entry) {
		anx_spin_unlock_irqrestore(&credstore_lock, irq_state);
		secure_zero(new_copy, new_len);
		anx_free(new_copy);
		return ANX_ENOENT;
	}

	old_secret = entry->secret;
	old_len = entry->secret_len;
	entry->secret = new_copy;
	entry->secret_len = new_len;

	anx_spin_unlock_irqrestore(&credstore_lock, irq_state);

	/* Zero old payload after releasing lock */
	secure_zero(old_secret, old_len);
	anx_free(old_secret);

	kprintf("credential: %s rotated (%u bytes)\n", name, new_len);
	return ANX_OK;
}

int anx_credential_revoke(const char *name)
{
	struct credential_entry *entry;
	bool irq_state;
	void *old_secret;
	uint32_t old_len;

	anx_spin_lock_irqsave(&credstore_lock, &irq_state);

	entry = find_by_name(name);
	if (!entry) {
		anx_spin_unlock_irqrestore(&credstore_lock, irq_state);
		return ANX_ENOENT;
	}

	old_secret = entry->secret;
	old_len = entry->secret_len;
	entry->secret = NULL;
	entry->secret_len = 0;
	entry->active = false;

	anx_spin_unlock_irqrestore(&credstore_lock, irq_state);

	secure_zero(old_secret, old_len);
	anx_free(old_secret);

	kprintf("credential: %s revoked\n", name);
	return ANX_OK;
}

int anx_credential_list(struct anx_credential_info *out,
			 uint32_t max_entries, uint32_t *count)
{
	bool irq_state;
	uint32_t n = 0;
	int i;

	anx_spin_lock_irqsave(&credstore_lock, &irq_state);

	for (i = 0; i < CREDSTORE_MAX && n < max_entries; i++) {
		if (!credstore[i].active)
			continue;
		anx_strlcpy(out[n].name, credstore[i].name,
			     sizeof(out[n].name));
		out[n].cred_type = credstore[i].cred_type;
		out[n].secret_len = credstore[i].secret_len;
		out[n].created_at = credstore[i].created_at;
		out[n].last_accessed = credstore[i].last_accessed;
		out[n].access_count = credstore[i].access_count;
		out[n].active = credstore[i].active;
		n++;
	}

	anx_spin_unlock_irqrestore(&credstore_lock, irq_state);

	if (count)
		*count = n;
	return ANX_OK;
}

bool anx_credential_exists(const char *name)
{
	bool irq_state;
	bool found;

	anx_spin_lock_irqsave(&credstore_lock, &irq_state);
	found = (find_by_name(name) != NULL);
	anx_spin_unlock_irqrestore(&credstore_lock, irq_state);

	return found;
}
