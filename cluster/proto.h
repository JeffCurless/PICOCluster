/*
 * proto.h — picoCluster wire format.
 *
 * Every packet is a 16-byte header followed by a type-specific body.  All
 * multi-byte fields are little-endian and are read/written byte by byte, so
 * the code never makes an unaligned access (which faults on Cortex-M0+).
 *
 *   header   magic(4) ver(1) type(1) body_len(2) build_id(4) src_node(2) rsvd(2)
 *
 *   HELLO       free_slots(1) max_slots(1) rsvd(2)                 multicast
 *   SPAWN       req_id(4) task[CL_TASK_NAME_MAX] arg[...]           unicast
 *   SPAWN_ACK   req_id(4) code(1) rsvd(3)                           unicast
 *   RESULT      req_id(4) status(4) err(4) ret[...]                 unicast
 *   RESULT_ACK  req_id(4)                                           unicast
 *   KILL        req_id(4)                                           unicast
 *   KILL_ACK    req_id(4)                                           unicast
 *
 * This file has no picoOS dependencies so it can be unit-tested on the host.
 */

#ifndef CLUSTER_PROTO_H
#define CLUSTER_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest argument / result a cluster task can pass (bytes).  Set from CMake;
 * every node must use the same value (it is part of the build id). */
#ifndef CLUSTER_MAX_PAYLOAD
#define CLUSTER_MAX_PAYLOAD 512u
#endif

#define CL_MAGIC          0x53554C43u      /* "CLUS" as little-endian bytes */
#define CL_PROTO_VER      2u               /* 2: KILL / KILL_ACK */
#define CL_TASK_NAME_MAX  16u              /* including the NUL            */

#define CL_HDR_LEN        16u
#define CL_HELLO_BODY     4u
#define CL_SPAWN_FIXED    (4u + CL_TASK_NAME_MAX)
#define CL_ACK_BODY       8u
#define CL_RESULT_FIXED   12u
#define CL_RACK_BODY      4u
#define CL_KILL_BODY      4u

/* Largest packet on the wire (a SPAWN with a full argument). */
#define CL_MAX_PKT        (CL_HDR_LEN + CL_SPAWN_FIXED + CLUSTER_MAX_PAYLOAD)

typedef enum {
    CL_MSG_HELLO      = 1,
    CL_MSG_SPAWN      = 2,
    CL_MSG_SPAWN_ACK  = 3,
    CL_MSG_RESULT     = 4,
    CL_MSG_RESULT_ACK = 5,
    CL_MSG_KILL       = 6,
    CL_MSG_KILL_ACK   = 7,
} cl_msg_type_t;

typedef enum {
    CL_ACK_OK     = 0,
    CL_ACK_BUSY   = 1,     /* no free worker slot / process pool full */
    CL_ACK_NOTASK = 2,     /* task name not in this node's table      */
} cl_ack_code_t;

/* A decoded packet.  `data` points into the buffer passed to cl_decode(). */
typedef struct {
    cl_msg_type_t  type;
    uint32_t       build_id;
    uint16_t       src_node;

    uint32_t       req_id;                   /* all but HELLO          */
    uint8_t        free_slots;               /* HELLO                  */
    uint8_t        max_slots;                /* HELLO                  */
    char           task[CL_TASK_NAME_MAX];   /* SPAWN (NUL-terminated) */
    uint8_t        code;                     /* SPAWN_ACK              */
    int32_t        status;                   /* RESULT                 */
    int32_t        err;                      /* RESULT                 */
    const uint8_t *data;                     /* SPAWN arg / RESULT ret */
    uint16_t       data_len;
} cl_msg_t;

/* Encoders write a complete packet into buf and return its length, or 0 if
 * buf is too small, the payload exceeds CLUSTER_MAX_PAYLOAD, or the task
 * name does not fit. */
size_t cl_encode_hello(uint8_t *buf, size_t cap, uint32_t build_id,
                       uint16_t node, uint8_t free_slots, uint8_t max_slots);
size_t cl_encode_spawn(uint8_t *buf, size_t cap, uint32_t build_id,
                       uint16_t node, uint32_t req_id, const char *task,
                       const void *arg, size_t arg_len);
size_t cl_encode_spawn_ack(uint8_t *buf, size_t cap, uint32_t build_id,
                           uint16_t node, uint32_t req_id, uint8_t code);
size_t cl_encode_result(uint8_t *buf, size_t cap, uint32_t build_id,
                        uint16_t node, uint32_t req_id, int32_t status,
                        int32_t err, const void *ret, size_t ret_len);
size_t cl_encode_result_ack(uint8_t *buf, size_t cap, uint32_t build_id,
                            uint16_t node, uint32_t req_id);
/* type is CL_MSG_KILL or CL_MSG_KILL_ACK. */
size_t cl_encode_kill(uint8_t *buf, size_t cap, uint32_t build_id,
                      uint16_t node, cl_msg_type_t type, uint32_t req_id);

/* Validate and decode a packet.  False on a short packet, wrong magic or
 * protocol version, unknown type, or a body length that does not match. */
bool cl_decode(const uint8_t *buf, size_t len, cl_msg_t *out);

/* FNV-1a, used to build the build id. */
#define CL_FNV_INIT  0x811C9DC5u
uint32_t cl_fnv1a(uint32_t h, const void *data, size_t len);

/* Recently finished request ids — lets a worker drop a late duplicate SPAWN
 * instead of running the task twice. */
#define CL_RECENT_MAX 16u

typedef struct {
    uint32_t ids[CL_RECENT_MAX];
    uint32_t next;
    uint32_t count;
} cl_recent_t;

void cl_recent_init(cl_recent_t *r);
void cl_recent_add(cl_recent_t *r, uint32_t id);
bool cl_recent_has(const cl_recent_t *r, uint32_t id);

#endif /* CLUSTER_PROTO_H */
