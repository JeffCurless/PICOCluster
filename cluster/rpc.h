/*
 * rpc.h — origin side: requests this node has created and is waiting on.
 *
 * Lifecycle of a pending request:
 *   remote  SENT --SPAWN_ACK ok--> RUNNING --RESULT--> DONE --join--> FREE
 *   local   RUNNING (job in exec.c) --------------->   DONE --join--> FREE
 * Any error (busy, unknown task, node lost, timeout, killed) also ends in
 * DONE with `err` set.
 *
 * A request placed with CLUSTER_NODE_ANY that a remote node refuses as busy
 * is not failed: it goes back to SENT with `unplaced` set, and the next tick
 * places it again (on another node, or the same one after its next HELLO),
 * up to CL_BUSY_REPLACES times.  The node estimate it was placed on comes
 * from a HELLO that may not count SPAWNs still in flight, so this race is
 * expected under load.  join() blocks on the `done` semaphore, which is
 * signalled exactly once, when the request reaches DONE.
 *
 * Everything except the public API in cluster.h is called with cl_lock held.
 */

#ifndef CLUSTER_RPC_H
#define CLUSTER_RPC_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/sync.h"
#include "cluster/cluster.h"
#include "cluster/proto.h"

#define CL_MAX_PENDING CLUSTER_MAX_PENDING
#define CL_MAX_KILLS    8     /* KILLs awaiting KILL_ACK */

typedef enum {
    CL_P_FREE = 0,
    CL_P_SENT,        /* SPAWN sent, no ACK yet          */
    CL_P_RUNNING,     /* accepted, task running          */
    CL_P_DONE,        /* result or error ready for join  */
} cl_pstate_t;

typedef struct {
    cl_pstate_t  state;
    uint32_t     req_id;
    uint16_t     node;
    char         task[CL_TASK_NAME_MAX];
    uint8_t     *arg;            /* kept for retransmit until ACK (remote) */
    uint16_t     arg_len;
    uint8_t      tries;
    uint8_t      any;            /* placed with CLUSTER_NODE_ANY           */
    uint8_t      unplaced;       /* refused as busy; waiting to re-place   */
    uint8_t      replaced;       /* times re-placed                        */
    uint64_t     next_tx_us;
    uint64_t     deadline_us;    /* join timeout, 0 = none                 */
    int32_t      status;
    int32_t      err;
    uint8_t     *ret;
    uint16_t     ret_len;
    ksemaphore_t done;
} cl_pending_t;

void          cl_rpc_init(void);
cl_pending_t *cl_rpc_slot(int idx);       /* NULL past the end */

/* Complete a request (local job finished, or RESULT arrived). */
void cl_rpc_deliver(uint32_t req_id, int32_t status, int32_t err,
                    const void *ret, size_t ret_len);

void cl_rpc_on_spawn_ack(uint32_t req_id, uint16_t src_node, uint8_t code);
void cl_rpc_on_kill_ack(uint32_t req_id);

/* cluster_thread_kill() by request id (for the shell).  Takes cl_lock. */
int  cl_rpc_kill_req(uint32_t req_id);
void cl_rpc_node_lost(uint16_t node);

/* Retransmit unacknowledged SPAWNs and KILLs; expire join deadlines. */
void cl_rpc_tick(uint64_t now);

#endif /* CLUSTER_RPC_H */
