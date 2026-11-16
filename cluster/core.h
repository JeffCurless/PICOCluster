/*
 * core.h — state shared by the cluster layer modules (not for apps).
 *
 * Locking: one kernel mutex (cl_lock) guards the node table, the pending
 * request table, the worker job table and the shared transmit buffer.  The
 * receive ring (net.c) is lock-free and is the only thing the network
 * callbacks touch.
 */

#ifndef CLUSTER_CORE_H
#define CLUSTER_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/time.h"

#include "cluster/proto.h"

#define CL_LOG(...)  printf("[cluster] " __VA_ARGS__)

/* Timing (microseconds). */
#define CL_HELLO_INTERVAL_US   1000000u   /* HELLO period                    */
#define CL_HELLO_DEDUP_US       300000u   /* 2nd copy of a HELLO is ignored  */
#define CL_NODE_TIMEOUT_US     5000000u   /* node forgotten after silence    */
#define CL_RETRY_US             200000u   /* SPAWN / RESULT retransmit       */
#define CL_SPAWN_TRIES               5u   /* SPAWN sends before NODE_LOST    */
#define CL_RESULT_TRIES             10u   /* RESULT sends before giving up   */
#define CL_BUSY_REPLACES            20u   /* re-placements after busy refusal */

extern uint16_t cl_self_id;      /* NODEID from config.txt   */
extern uint32_t cl_build_id;     /* see clusterd.c           */

bool cl_running(void);
void cl_lock(void);
void cl_unlock(void);

/* Activity counters (for the status page); updated under cl_lock. */
extern uint32_t cl_stat_tasks_run;   /* tasks started on this node        */
extern uint32_t cl_stat_reqs_done;   /* our requests completed with OK    */

/* Transmit buffer shared by every sender; only touch it under cl_lock. */
extern uint8_t cl_tx_buf[CL_MAX_PKT];

static inline uint64_t cl_now_us(void)
{
    return time_us_64();
}

#endif /* CLUSTER_CORE_H */
