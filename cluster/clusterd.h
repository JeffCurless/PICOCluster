/*
 * clusterd.h — the cluster daemon (a picoOS app; start it with
 * AUTORUN=clusterd in config.txt, "run clusterd" from the shell, or from an
 * app with cluster_start(), see cluster.h).
 */

#ifndef CLUSTER_CLUSTERD_H
#define CLUSTER_CLUSTERD_H

void clusterd_entry(void *arg);

#endif /* CLUSTER_CLUSTERD_H */
