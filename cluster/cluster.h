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
 * Requires the cluster daemon (clusterd) to be running — normally started at
 * boot with AUTORUN=clusterd in config.txt.
 *
 * Every created thread must be joined exactly once; joining frees its slot.
 */

#ifndef CLUSTER_H
#define CLUSTER_H

#include <stddef.h>
#include <stdint.h>

#include "cluster/cluster_task.h"
#include "cluster/proto.h"         /* CLUSTER_MAX_PAYLOAD */

#define CLUSTER_NODE_ANY        (-1)

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
    int      node;      /* node the task was placed on  */
} cluster_thread_t;

/*
 * Start `task` with a copy of arg[0..arg_len) on `node`, or on the node with
 * the most free worker slots when node == CLUSTER_NODE_ANY (this node
 * included).  Returns CLUSTER_OK and fills *t, or a CLUSTER_ERR_* code.
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

int         cluster_self(void);          /* this node's NODEID, -1 if not running */
int         cluster_node_count(void);    /* live compatible nodes, self included  */
const char *cluster_strerror(int err);

#endif /* CLUSTER_H */
