/*
 * cluster.c — picoCluster application entry point (placeholder).
 *
 * Launch from the picoOS shell with:  run cluster
 */

#include "kernel/syscall.h"
#include "shell/shell.h"

#include "cluster.h"

void cluster_entry(void *arg)
{
    (void)arg;
    shell_print("cluster: picoCluster v%d.%d.%d - not yet implemented\r\n",
                CLUSTER_VER_MAJOR, CLUSTER_VER_MINOR, CLUSTER_VER_EDIT);
    sys_exit(0);
}
