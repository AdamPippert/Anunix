/*
 * anx/retention.h — Object retention policy.
 */

#ifndef ANX_RETENTION_H
#define ANX_RETENTION_H

#include <anx/types.h>

struct anx_retention_policy {
	uint64_t ttl_ns;	/* time-to-live in nanoseconds; 0 = indefinite */
	uint32_t min_versions;	/* minimum historical versions to retain */
	uint32_t max_versions;	/* maximum historical versions */
	bool deletion_hold;	/* legal hold: prevents deletion */
	uint8_t replicas;	/* minimum storage replicas */
};

#define ANX_RETENTION_DEFAULT { \
	.ttl_ns = 0, \
	.min_versions = 1, \
	.max_versions = 1, \
	.deletion_hold = false, \
	.replicas = 1, \
}

#endif /* ANX_RETENTION_H */
