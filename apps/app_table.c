/*
 * app_table.c — applications the picoOS shell 'run' command can launch.
 *
 * Priority: 0 (highest) .. 7 (lowest).  User apps typically use 4-5.
 *
 * Cluster tasks (the "threads" other nodes run) are registered separately,
 * in cluster_tasks.c.
 */

#include "apps/app_table.h"   /* app_entry_t — resolved via picoOS/src */

#include "cluster/clusterd.h"
#include "cpi.h"

const app_entry_t app_table[] = {
    /* name       entry           prio  stack (0 = DEFAULT_STACK_SIZE) */
    { "clusterd", clusterd_entry, 3u,   0u },   /* AUTORUN=clusterd in config.txt */
    { "cpi",      cpi_entry,      4u,   0u },   /* run cpi [threads]              */
};

const int app_table_size = (int)(sizeof(app_table) / sizeof(app_table[0]));
