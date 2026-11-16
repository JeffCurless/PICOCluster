/*
 * status.h — Display Pack status page for the cluster layer.
 *
 * clusterd starts a "cl-disp" thread in its own process.  Every
 * CL_STATUS_PERIOD_MS it snapshots the cluster state under cl_lock, and
 * redraws the panel only when the snapshot changed:
 *
 *   title bar     node id and picoCluster version
 *   network       IP and node count — or the startup / error phase
 *   NODES         one row per node: id, IP, worker slots (busy / free)
 *   THIS NODE     one row per local worker: task, origin node, run time
 *   footer        requests outstanding, tasks run here, results received
 *
 * The Display Pack RGB LED mirrors the state: yellow starting, green idle,
 * blue running tasks, red error.  Without a display this is a no-op.
 *
 * The panel can be handed to apps: see cluster/display.h (claim/release)
 * and cl_status_set_enabled() (config.txt STATUS=OFF).
 */

#include <stdbool.h>

#ifndef CLUSTER_STATUS_H
#define CLUSTER_STATUS_H

#define CL_STATUS_PERIOD_MS  500u

typedef enum {
    CL_PHASE_STARTING = 0,
    CL_PHASE_NO_CONFIG,      /* config.txt missing NODEID / SSID */
    CL_PHASE_WIFI,           /* joining WiFi                     */
    CL_PHASE_WIFI_FAIL,
    CL_PHASE_NET_FAIL,       /* sockets could not be opened      */
    CL_PHASE_RUNNING,
} cl_phase_t;

/* Start the status thread in the calling process (clusterd).  cl_lock must
 * already be initialised. */
void cl_status_start(void);

void cl_status_set_phase(cl_phase_t phase);

/* false: clear the panel once and stop drawing the status page (the LED keeps
 * working).  Set from config.txt STATUS= by clusterd.  Default true. */
void cl_status_set_enabled(bool enabled);

#endif /* CLUSTER_STATUS_H */
