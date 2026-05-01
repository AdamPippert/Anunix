/*
 * anx/types.h — Fundamental types for the Anunix kernel.
 *
 * All kernel code includes this header. It provides fixed-width integers,
 * boolean, NULL, and the core Anunix ID types used across every subsystem.
 */

#ifndef ANX_TYPES_H
#define ANX_TYPES_H

/* Integer literal suffixes for portable 64-bit constants */
#ifndef UINT64_C
#define UINT64_C(c)  (c ## ULL)
#endif
#ifndef INT64_C
#define INT64_C(c)   (c ## LL)
#endif

/* Fixed-width integers — kernel freestanding, no libc */
typedef unsigned char		uint8_t;
typedef unsigned short		uint16_t;
typedef unsigned int		uint32_t;
typedef unsigned long long	uint64_t;

typedef signed char		int8_t;
typedef signed short		int16_t;
typedef signed int		int32_t;
typedef signed long long	int64_t;

typedef uint64_t		size_t;
typedef int64_t			ssize_t;
typedef uint64_t		uintptr_t;
typedef int64_t			intptr_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef bool
typedef _Bool bool;
#define true  1
#define false 0
#endif

/*
 * Core ID types.
 * All IDs are 128-bit UUIDs (v7, time-ordered) stored as two 64-bit words.
 * This gives us globally unique, temporally sortable identifiers for every
 * object, cell, engine, and node in the system.
 */
struct anx_uuid {
	uint64_t hi;
	uint64_t lo;
};

typedef struct anx_uuid anx_oid_t;	/* State Object ID */
typedef struct anx_uuid anx_cid_t;	/* Cell ID */
typedef struct anx_uuid anx_eid_t;	/* Engine ID */
typedef struct anx_uuid anx_nid_t;	/* Node ID */
typedef struct anx_uuid anx_pid_t;	/* Plan ID */
typedef struct anx_uuid anx_tid_t;	/* Trace ID */

/* Content hash — SHA-256 */
struct anx_hash {
	uint8_t bytes[32];
};

/* Timestamp — nanoseconds since epoch */
typedef uint64_t anx_time_t;

/* Return codes — 0 success, negative error */
#define ANX_OK		 0
#define ANX_ENOMEM	-1
#define ANX_EINVAL	-2
#define ANX_ENOENT	-3
#define ANX_EEXIST	-4
#define ANX_EPERM	-5
#define ANX_EIO		-6
#define ANX_EBUSY	-7
#define ANX_ENOSYS	-8
#define ANX_ETIMEDOUT	-9
#define ANX_ECONNRESET	-10
#define ANX_EHOSTUNREACH -11
#define ANX_ENOTIMPL	-12   /* not yet implemented */
#define ANX_EFULL	-13   /* store or queue is full */
#define ANX_ENODEV	-14   /* no such device */
#define ANX_ENOTSUP	-15   /* operation not supported */

/* Kernel version string — YYYY.M.D */
#define ANX_VERSION	"2026.4.29"

#endif /* ANX_TYPES_H */
