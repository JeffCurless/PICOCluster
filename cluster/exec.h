/*
 * exec.h — worker side: run cluster tasks in local picoOS processes.
 *
 * Each running task occupies one of CL_MAX_WORKERS job slots and one picoOS
 * process (PID from PID_RANGE_SPAWN).  The process entry is a trampoline that
 * calls the task function, marks the job DONE and returns; picoOS then frees
 * the thread and the process.  clusterd collects DONE jobs and delivers the
 * result (locally, or as a RESULT packet to the origin node).
 *
 * All functions are called with cl_lock held.
 */

#ifndef CLUSTER_EXEC_H
#define CLUSTER_EXEC_H

#include <stddef.h>
#include <stdint.h>

#include "cluster/cluster_task.h"

/* Per-node worker slots.  A Pico W has MAX_PROCESSES 8: kernel, shell,
 * clusterd and a main app (e.g. cpi) use four, leaving exactly four for
 * workers.  While all four are busy on the node running the main app, no
 * other process (e.g. a shell 'run') can start there. */
#ifndef CL_MAX_WORKERS
#define CL_MAX_WORKERS 4
#endif

typedef enum {
    CL_JOB_FREE = 0,
    CL_JOB_RUNNING,      /* task function executing                      */
    CL_JOB_DONE,         /* finished; clusterd has not delivered it yet  */
    CL_JOB_SENDING,      /* RESULT sent to origin, waiting for RESULT_ACK */
} cl_job_state_t;

typedef struct {
    volatile cl_job_state_t state;
    uint32_t              req_id;
    uint16_t              origin;      /* node that created the task  */
    const cluster_task_t *task;
    uint8_t              *arg;         /* kmalloc'd copy              */
    size_t                arg_len;
    uint8_t              *ret;         /* kmalloc'd, CLUSTER_MAX_PAYLOAD */
    size_t                ret_len;
    int32_t               status;      /* task return value           */
    int32_t               err;         /* CLUSTER_OK / CLUSTER_ERR_*  */
    uint32_t              pid;
    uint8_t               tries;       /* RESULT sends                */
    uint64_t              next_tx_us;
    uint64_t              started_us;
} cl_job_t;

const cluster_task_t *cl_task_find(const char *name);

void      cl_exec_init(void);
int       cl_exec_free_slots(void);
cl_job_t *cl_exec_find(uint32_t req_id);
cl_job_t *cl_exec_slot(int idx);         /* NULL past the end */

/* Start `task` for request req_id.  CLUSTER_OK, CLUSTER_ERR_BUSY (no slot or
 * process/thread pool full) or CLUSTER_ERR_NOMEM. */
int       cl_exec_start(uint32_t req_id, uint16_t origin,
                        const cluster_task_t *task,
                        const void *arg, size_t arg_len);

/* Mark RUNNING jobs whose process has disappeared (killed) as DONE with
 * CLUSTER_ERR_KILLED. */
void      cl_exec_reap(void);

/* Release a job's memory and slot. */
void      cl_exec_free(cl_job_t *j);

#endif /* CLUSTER_EXEC_H */
