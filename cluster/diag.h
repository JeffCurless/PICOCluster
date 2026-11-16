/*
 * diag.h — breadcrumbs for finding where a thread is stuck.
 *
 * A crumb records which thread last passed a point in the code, the source
 * line, and when.  Code that runs with cl_lock held drops crumbs as it goes;
 * "cluster diag" prints them without taking any lock, so it still works
 * when cl_lock is stuck.  A crumb with line 0 means "not inside".
 */

#ifndef CLUSTER_DIAG_H
#define CLUSTER_DIAG_H

#include <stdint.h>

#include "kernel/syscall.h"
#include "pico/time.h"

typedef struct {
    volatile int32_t  tid;
    volatile uint32_t line;
    volatile uint64_t since_us;
} cl_crumb_t;

extern cl_crumb_t cl_crumb_create;   /* cluster_thread_create (rpc.c)  */
extern cl_crumb_t cl_crumb_exec;     /* cl_exec_start (exec.c)         */
extern cl_crumb_t cl_crumb_net;      /* cl_net_send_to (net.c)         */
extern cl_crumb_t cl_crumb_loop;     /* clusterd main loop (clusterd.c)*/

static inline void cl_crumb_set(cl_crumb_t *c, uint32_t line)
{
    c->tid      = sys_gettid();
    c->since_us = time_us_64();
    c->line     = line;
}

#define CL_CRUMB(c)       cl_crumb_set(&(c), (uint32_t)__LINE__)
#define CL_CRUMB_DONE(c)  ((c).line = 0u)

/* Print the crumbs and cl_lock's owner (shell: cluster diag). */
void cl_diag_print(void);

#endif /* CLUSTER_DIAG_H */
