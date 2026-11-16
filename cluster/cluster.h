/*
 * cluster.h — picoCluster application API.
 *
 * Start a "thread" (a cluster task) on any node, then join it to collect its
 * return code and result bytes:
 *
 *   cluster_thread_t t;
 *   int rc = cluster_thread_create(&t, "pi", &args, sizeof(args), CLUSTER_NODE_ANY);
 *   ...
 *   rc = cluster_thread_join(&t, &hits, sizeof(hits), &len, &status, 60000u);
 *
 * Requires the cluster daemon (clusterd) to be running: started at boot with
 * AUTORUN=clusterd in config.txt, or by an app calling cluster_start() (so
 * AUTORUN=<that app> works on its own).
 *
 * Every created thread must be joined exactly once; joining frees its slot.
 * cluster_thread_kill() stops a thread early; it must still be joined.
 */

#ifndef CLUSTER_H
#define CLUSTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cluster/cluster_task.h"
#include "cluster/proto.h"         /* CLUSTER_MAX_PAYLOAD */

#define CLUSTER_NODE_ANY        (-1)

/* Most threads one node can have created and not yet joined. */
#define CLUSTER_MAX_PENDING     32

/* Error codes (all negative). */
#define CLUSTER_OK              0
#define CLUSTER_ERR_ARG         (-1)   /* bad argument or unknown handle        */
#define CLUSTER_ERR_NOTASK      (-2)   /* task name not in cluster_task_table   */
#define CLUSTER_ERR_NONODE      (-3)   /* requested node is not available       */
#define CLUSTER_ERR_BUSY        (-4)   /* no free worker slot / request slot    */
#define CLUSTER_ERR_TOOBIG      (-5)   /* arg longer than CLUSTER_MAX_PAYLOAD   */
#define CLUSTER_ERR_TIMEOUT     (-6)   /* join timed out                        */
#define CLUSTER_ERR_NODE_LOST   (-7)   /* node stopped responding               */
#define CLUSTER_ERR_NOTRUNNING  (-8)   /* clusterd is not running               */
#define CLUSTER_ERR_NOMEM       (-9)   /* kernel heap exhausted                 */
#define CLUSTER_ERR_KILLED      (-10)  /* worker process was killed             */

typedef struct {
    uint32_t req_id;    /* cluster-wide request id      */
    int      node;      /* node the task runs on; join updates it if the
                         * request was re-placed (see below)            */
} cluster_thread_t;

/*
 * Start `task` with a copy of arg[0..arg_len) on `node`, or on the node with
 * the most free worker slots when node == CLUSTER_NODE_ANY (this node
 * included).  Returns CLUSTER_OK and fills *t, or a CLUSTER_ERR_* code.
 *
 * With CLUSTER_NODE_ANY, if the chosen node turns out to be busy when the
 * request reaches it, the request is placed again elsewhere instead of
 * failing; t->node is then updated by join.  A request for a specific node
 * that is busy there completes with CLUSTER_ERR_BUSY.
 */
int cluster_thread_create(cluster_thread_t *t, const char *task,
                          const void *arg, size_t arg_len, int node);

/*
 * Wait for the task to finish (timeout_ms == 0 waits forever).  On
 * CLUSTER_OK, *status is the task's return value and up to ret_max result
 * bytes are copied to ret (*ret_len = bytes copied).  ret, ret_len and
 * status may each be NULL.  The handle is released whatever the outcome.
 */
int cluster_thread_join(cluster_thread_t *t, void *ret, size_t ret_max,
                        size_t *ret_len, int *status, uint32_t timeout_ms);

/*
 * Stop the task.  The request completes at once with CLUSTER_ERR_KILLED, so
 * a pending (or later) cluster_thread_join() returns that error; the handle
 * must still be joined to release it.  The worker process is killed and its
 * result discarded.  If the task has already finished, nothing changes and
 * join returns its result.  CLUSTER_OK, CLUSTER_ERR_ARG (unknown handle) or
 * CLUSTER_ERR_NOTRUNNING.
 */
int cluster_thread_kill(cluster_thread_t *t);

/*
 * Start clusterd in its own process if it is not running, then wait up to
 * timeout_ms for it to come up (join WiFi, open sockets).  *started (may be
 * NULL) is set true when this call did the start, i.e. the node has only
 * just booted and peers may not have been heard from yet.  CLUSTER_OK, or
 * CLUSTER_ERR_NOTRUNNING if it did not come up in time (config.txt or WiFi
 * problem: the status page shows which).
 */
int cluster_start(uint32_t timeout_ms, bool *started);

int         cluster_self(void);          /* this node's NODEID, -1 if not running */
int         cluster_node_count(void);    /* live compatible nodes, self included  */
const char *cluster_strerror(int err);

#endif /* CLUSTER_H */
