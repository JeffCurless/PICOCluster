/*
 * net.c — picoCluster transport (see net.h).
 *
 * This is the one cluster module that talks to lwIP directly.  picoOS apps
 * are expected to stay on picoOS APIs, but the cluster layer is an operating
 * system extension: picoOS only offers multicast, and the layer needs
 * unicast UDP with payloads larger than WIFI_MCAST_MAX_PAYLOAD.  Every lwIP
 * call is made inside cyw43_arch_lwip_begin/end, as kernel/wifi.c does.
 *
 * Receive ring: single producer (the CYW43 async context, which runs both
 * receive callbacks), single consumer (clusterd).  head is written only by
 * the producer, tail only by the consumer; memory barriers order the slot
 * copy against the index update.
 */

#include "cluster/net.h"
#include "cluster/core.h"

#include "kernel/wifi.h"

#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include <string.h>

#define CL_RX_SLOTS 6u

static cl_rx_pkt_t       g_ring[CL_RX_SLOTS];
static volatile uint32_t g_head;          /* next slot to fill  (producer) */
static volatile uint32_t g_tail;          /* next slot to read  (consumer) */
static volatile uint32_t g_drops;

static struct udp_pcb   *g_pcb;           /* unicast                       */
static int               g_mcast = -1;    /* picoOS multicast handle       */

/* -------------------------------------------------------------------------
 * Producer side — CYW43 async context only
 * ------------------------------------------------------------------------- */
static cl_rx_pkt_t *ring_claim(void)
{
    uint32_t next = (g_head + 1u) % CL_RX_SLOTS;
    if (next == g_tail) {
        g_drops++;
        return NULL;
    }
    return &g_ring[g_head];
}

static void ring_publish(void)
{
    __dmb();                               /* slot contents before index */
    g_head = (g_head + 1u) % CL_RX_SLOTS;
}

static void ucast_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                       const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)port;
    if (p == NULL) {
        return;
    }
    cl_rx_pkt_t *slot = NULL;
    if (p->tot_len > CL_MAX_PKT) {
        g_drops++;
    } else {
        slot = ring_claim();
    }
    if (slot != NULL) {
        slot->len = (uint16_t)pbuf_copy_partial(p, slot->data, p->tot_len, 0);
        ipaddr_ntoa_r(addr, slot->src_ip, (int)sizeof(slot->src_ip));
        ring_publish();
    }
    pbuf_free(p);
}

static void mcast_recv(const char *data, uint16_t len, const char *src_ip,
                       void *ctx)
{
    (void)ctx;
    cl_rx_pkt_t *slot = ring_claim();
    if (slot == NULL) {
        return;
    }
    if (len > CL_MAX_PKT) {
        len = (uint16_t)CL_MAX_PKT;
    }
    memcpy(slot->data, data, len);
    slot->len = len;
    strncpy(slot->src_ip, src_ip, sizeof(slot->src_ip) - 1u);
    slot->src_ip[sizeof(slot->src_ip) - 1u] = '\0';
    ring_publish();
}

/* -------------------------------------------------------------------------
 * Consumer side — clusterd
 * ------------------------------------------------------------------------- */
bool cl_net_recv(cl_rx_pkt_t *out)
{
    uint32_t tail = g_tail;
    if (tail == g_head) {
        return false;
    }
    __dmb();                               /* index before slot contents */
    const cl_rx_pkt_t *slot = &g_ring[tail];
    out->len = slot->len;
    memcpy(out->src_ip, slot->src_ip, sizeof(out->src_ip));
    memcpy(out->data, slot->data, slot->len);
    __dmb();                               /* finish reading before freeing */
    g_tail = (tail + 1u) % CL_RX_SLOTS;
    return true;
}

uint32_t cl_net_drops(void)
{
    return g_drops;
}

/* -------------------------------------------------------------------------
 * Open / send
 * ------------------------------------------------------------------------- */
int cl_net_open(void)
{
    /* Sockets outlive a killed clusterd; close any left from a previous run. */
    if (g_mcast >= 0) {
        wifi_mcast_close(g_mcast);
        g_mcast = -1;
    }
    cyw43_arch_lwip_begin();
    if (g_pcb != NULL) {
        udp_remove(g_pcb);
        g_pcb = NULL;
    }
    g_head = 0u;
    g_tail = 0u;

    struct udp_pcb *pcb = udp_new();
    if (pcb == NULL) {
        cyw43_arch_lwip_end();
        CL_LOG("udp_new failed\r\n");
        return -1;
    }
    if (udp_bind(pcb, IP_ADDR_ANY, CL_UDP_PORT) != ERR_OK) {
        udp_remove(pcb);
        cyw43_arch_lwip_end();
        CL_LOG("cannot bind UDP port %u\r\n", (unsigned)CL_UDP_PORT);
        return -1;
    }
    udp_recv(pcb, ucast_recv, NULL);
    g_pcb = pcb;
    cyw43_arch_lwip_end();

    g_mcast = wifi_mcast_open(CL_MCAST_GROUP, CL_MCAST_PORT, mcast_recv, NULL);
    if (g_mcast < 0) {
        CL_LOG("wifi_mcast_open failed (%d)\r\n", g_mcast);
        return g_mcast;
    }
    return 0;
}

int cl_net_send_to(const char *ip, const void *data, size_t len)
{
    if (ip == NULL || data == NULL || len == 0u || len > CL_MAX_PKT) {
        return -1;
    }
    ip_addr_t dst;
    if (!ipaddr_aton(ip, &dst)) {
        return -1;
    }

    cyw43_arch_lwip_begin();
    if (g_pcb == NULL) {
        cyw43_arch_lwip_end();
        return -1;
    }
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (p == NULL) {
        cyw43_arch_lwip_end();
        return -1;
    }
    memcpy(p->payload, data, len);
    err_t err = udp_sendto(g_pcb, p, &dst, CL_UDP_PORT);
    pbuf_free(p);
    cyw43_arch_lwip_end();
    return err == ERR_OK ? 0 : -1;
}

int cl_net_send_mcast(const void *data, size_t len)
{
    if (g_mcast < 0 || len > WIFI_MCAST_MAX_PAYLOAD) {
        return -1;
    }
    return wifi_mcast_send(g_mcast, data, (uint16_t)len);
}
