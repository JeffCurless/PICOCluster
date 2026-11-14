/*
 * cluster_tasks.c — tasks any node can run on behalf of any other.
 *
 * Every node must be flashed with the same table: the task names are part of
 * the build id, and nodes with a different build id are never used.
 */

#include "cluster/cluster_task.h"

#include "cpi.h"

#include <string.h>

/* echo — returns its argument unchanged.  Bring-up: cluster run echo hi 1 */
static int task_echo(const void *arg, size_t arg_len,
                     void *ret, size_t ret_max, size_t *ret_len)
{
    size_t n = arg_len < ret_max ? arg_len : ret_max;
    memcpy(ret, arg, n);
    *ret_len = n;
    return 0;
}

const cluster_task_t cluster_task_table[] = {
    /* name     function     priority  stack (0 = default) */
    { "echo",   task_echo,   4u,       0u },
    { "pi",     cpi_worker,  5u,       0u },
};

const int cluster_task_count =
    (int)(sizeof(cluster_task_table) / sizeof(cluster_task_table[0]));
