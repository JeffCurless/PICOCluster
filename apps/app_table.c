/*
 * app_table.c — applications the picoOS shell 'run' command can launch.
 *
 * Priority: 0 (highest) .. 7 (lowest).  User apps typically use 4-5.
 */

#include "apps/app_table.h"   /* app_entry_t — resolved via picoOS/src */

#include "cluster.h"

const app_entry_t app_table[] = {
    { "cluster", cluster_entry, 4u },
};

const int app_table_size = (int)(sizeof(app_table) / sizeof(app_table[0]));
