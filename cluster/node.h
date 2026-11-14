/*
 * node.h — node identity, config.txt, WiFi join, and the table of peers.
 *
 * config.txt keys (same names as cray-one):
 *   NODEID=<0..65534>     this node's id, unique in the cluster  (required)
 *   SSID=, PASSWORD=      WiFi network                          (required)
 *   SSIDALT=, PASSWORDALT= fallback network                     (optional)
 *
 * The node table is guarded by cl_lock (core.h).
 */

#ifndef CLUSTER_NODE_H
#define CLUSTER_NODE_H

#include <stdbool.h>
#include <stdint.h>

#include "cluster/net.h"

#define CL_MAX_NODES   16

typedef struct {
    char ssid[33];
    char password[65];
    char ssid_alt[33];
    char pass_alt[65];
    int  nodeid;               /* -1 if missing */
} cl_config_t;

typedef struct {
    bool     used;
    uint16_t id;
    char     ip[CL_IP_STR_MAX];
    uint32_t build_id;
    uint8_t  free_slots;       /* as of the last HELLO                    */
    uint8_t  max_slots;
    uint8_t  sent_since_hello; /* SPAWNs we sent it since that HELLO      */
    uint64_t last_seen_us;
} cl_node_t;

/* Read config.txt.  0 on success, -1 if NODEID or SSID is missing. */
int  cl_config_load(cl_config_t *cfg);

/* Join WiFi (primary, then alternate) unless already up.  0 on success. */
int  cl_wifi_up(const cl_config_t *cfg);

void       cl_nodes_init(void);
void       cl_node_hello(uint16_t id, const char *ip, uint32_t build_id,
                         uint8_t free_slots, uint8_t max_slots, uint64_t now);
cl_node_t *cl_node_find(uint16_t id);
cl_node_t *cl_node_slot(int idx);          /* NULL past the end          */
bool       cl_node_usable(const cl_node_t *n);   /* live and compatible  */

/* Forget nodes not heard from in CL_NODE_TIMEOUT_US.  Writes the ids of
 * forgotten compatible nodes to lost[] and returns how many. */
int        cl_nodes_expire(uint64_t now, uint16_t *lost, int max_lost);

#endif /* CLUSTER_NODE_H */
