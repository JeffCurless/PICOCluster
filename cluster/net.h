/*
 * net.h — picoCluster transport.
 *
 *   multicast  CL_MCAST_GROUP:CL_MCAST_PORT  HELLO heartbeats (picoOS wifi_mcast)
 *   unicast    UDP port CL_UDP_PORT           SPAWN / ACK / RESULT (lwIP raw API)
 *
 * Both receive callbacks run in the CYW43 async context.  They only copy the
 * packet into a fixed ring of slots; clusterd drains it with cl_net_recv().
 */

#ifndef CLUSTER_NET_H
#define CLUSTER_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cluster/proto.h"

#define CL_MCAST_GROUP  "239.255.0.2"   /* cray-one uses 239.255.0.1 */
#define CL_MCAST_PORT   4301u
#define CL_UDP_PORT     4300u

#define CL_IP_STR_MAX   16u

typedef struct {
    uint16_t len;
    char     src_ip[CL_IP_STR_MAX];
    uint8_t  data[CL_MAX_PKT];
} cl_rx_pkt_t;

/* Open the multicast socket and the unicast PCB.  WiFi must be up.
 * 0 on success, negative on failure. */
int  cl_net_open(void);

/* Copy the oldest received packet into *out.  False if none is waiting. */
bool cl_net_recv(cl_rx_pkt_t *out);

int  cl_net_send_to(const char *ip, const void *data, size_t len);
int  cl_net_send_mcast(const void *data, size_t len);

/* Packets dropped because the ring was full or the packet was oversize. */
uint32_t cl_net_drops(void);

#endif /* CLUSTER_NET_H */
