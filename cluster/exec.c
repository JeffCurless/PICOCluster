/*
 * exec.c — worker side: run cluster tasks in local picoOS processes.
 */

#include "cluster/exec.h"
#include "cluster/cluster.h"
#include "cluster/core.h"

#include "kernel/mem.h"
#include "kernel/task.h"

#include <string.h>

static cl_job_t g_jobs[CL_MAX_WORKERS];

const cluster_task_t *cl_task_find(const char *name)
{
    for (int i = 0; i < cluster_task_count; i++) {
        if (strcmp(cluster_task_table[i].name, name) == 0) {
            return &cluster_task_table[i];
        }
    }
    return NULL;
}

void cl_exec_init(void)
{
    memset(g_jobs, 0, sizeof(g_jobs));
}

int cl_exec_free_slots(void)
{
    int n = 0;
    for (int i = 0; i < CL_MAX_WORKERS; i++) {
        if (g_jobs[i].state == CL_JOB_FREE) n++;
    }
    return n;
}

cl_job_t *cl_exec_find(uint32_t req_id)
{
    for (int i = 0; i < CL_MAX_WORKERS; i++) {
        if (g_jobs[i].state != CL_JOB_FREE && g_jobs[i].req_id == req_id) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

cl_job_t *cl_exec_slot(int idx)
{
    if (idx < 0 || idx >= CL_MAX_WORKERS) {
        return NULL;
    }
    return &g_jobs[idx];
}

void cl_exec_free(cl_job_t *j)
{
    if (j->arg != NULL) kfree(j->arg);
    if (j->ret != NULL) kfree(j->ret);
    memset(j, 0, sizeof(*j));            /* state = CL_JOB_FREE */
}

/* -------------------------------------------------------------------------
 * Trampoline — entry point of every worker process.
 *
 * cl_exec_start() holds cl_lock while it creates this thread, so the
 * cl_lock() below cannot complete before the job's pid is recorded.
 * Returning from here lets picoOS reap the thread and the process.
 * ------------------------------------------------------------------------- */
static void cl_trampoline(void *a)
{
    cl_job_t *j = (cl_job_t *)a;
    size_t ret_len = 0u;
    int status = j->task->fn(j->arg, j->arg_len, j->ret,
                             CLUSTER_MAX_PAYLOAD, &ret_len);
    if (ret_len > CLUSTER_MAX_PAYLOAD) {
        ret_len = CLUSTER_MAX_PAYLOAD;
    }
    cl_lock();
    j->ret_len = ret_len;
    j->status  = status;
    j->err     = CLUSTER_OK;
    j->state   = CL_JOB_DONE;
    cl_unlock();
}

int cl_exec_start(uint32_t req_id, uint16_t origin, const cluster_task_t *task,
                  const void *arg, size_t arg_len)
{
    cl_job_t *j = NULL;
    for (int i = 0; i < CL_MAX_WORKERS; i++) {
        if (g_jobs[i].state == CL_JOB_FREE) {
            j = &g_jobs[i];
            break;
        }
    }
    if (j == NULL) {
        return CLUSTER_ERR_BUSY;
    }

    memset(j, 0, sizeof(*j));
    j->arg = (uint8_t *)kmalloc(arg_len > 0u ? arg_len : 1u);
    j->ret = (uint8_t *)kmalloc(CLUSTER_MAX_PAYLOAD);
    if (j->arg == NULL || j->ret == NULL) {
        cl_exec_free(j);
        return CLUSTER_ERR_NOMEM;
    }
    if (arg_len > 0u) {
        memcpy(j->arg, arg, arg_len);
    }
    j->arg_len = arg_len;
    j->req_id  = req_id;
    j->origin  = origin;
    j->task    = task;

    uint32_t pid = task_alloc_pid(PID_RANGE_SPAWN);
    pcb_t *proc = (pid != 0u) ? task_create_process(task->name, pid) : NULL;
    if (proc == NULL) {
        cl_exec_free(j);
        return CLUSTER_ERR_BUSY;
    }
    j->pid   = pid;
    j->state = CL_JOB_RUNNING;
    tcb_t *t = task_create_thread(proc, task->name, cl_trampoline, j,
                                  task->priority,
                                  task->stack != 0u ? task->stack
                                                    : DEFAULT_STACK_SIZE);
    if (t == NULL) {
        task_free_process(proc);
        cl_exec_free(j);
        return CLUSTER_ERR_BUSY;
    }
    j->started_us = cl_now_us();
    cl_stat_tasks_run++;
    return CLUSTER_OK;
}

void cl_exec_reap(void)
{
    for (int i = 0; i < CL_MAX_WORKERS; i++) {
        cl_job_t *j = &g_jobs[i];
        if (j->state == CL_JOB_RUNNING && task_find_process(j->pid) == NULL) {
            j->ret_len = 0u;
            j->status  = 0;
            j->err     = CLUSTER_ERR_KILLED;
            j->state   = CL_JOB_DONE;
        }
    }
}
