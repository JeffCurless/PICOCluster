/*
 * cluster_task.h — the cluster task table ABI.
 *
 * A cluster task is a plain function that every node has compiled in.  The
 * creating node sends the task *name* plus argument bytes; the node that runs
 * it looks the name up in cluster_task_table[], runs the function in its own
 * picoOS process, and ships the result bytes back.
 *
 * The project defines the table once (apps/cluster_tasks.c):
 *
 *   const cluster_task_t cluster_task_table[] = {
 *       { "echo", task_echo, 4u, 0u },
 *   };
 *   const int cluster_task_count = (int)(sizeof(cluster_task_table) /
 *                                        sizeof(cluster_task_table[0]));
 */

#ifndef CLUSTER_TASK_H
#define CLUSTER_TASK_H

#include <stddef.h>
#include <stdint.h>

/*
 * A cluster task.
 *   arg, arg_len   the bytes passed to cluster_thread_create() (copy; read-only)
 *   ret, ret_max   buffer for the result (ret_max == CLUSTER_MAX_PAYLOAD)
 *   *ret_len       set to the number of result bytes written (starts at 0)
 * The return value is handed to the joiner as *status.
 */
typedef int (*cluster_fn_t)(const void *arg, size_t arg_len,
                            void *ret, size_t ret_max, size_t *ret_len);

typedef struct {
    const char   *name;       /* < 16 characters                         */
    cluster_fn_t  fn;
    uint8_t       priority;   /* picoOS priority 0..7; tasks usually 4-5 */
    uint32_t      stack;      /* stack bytes; 0 = DEFAULT_STACK_SIZE     */
} cluster_task_t;

extern const cluster_task_t cluster_task_table[];
extern const int            cluster_task_count;

#endif /* CLUSTER_TASK_H */
