/*
 * rpc.c — origin side of the cluster protocol and the public thread API.
 */

#include "cluster/rpc.h"
#include "cluster/cluster.h"
#include "cluster/core.h"
#include "cluster/exec.h"
#include "cluster/net.h"
#include "cluster/node.h"

#include "kernel/mem.h"

#include <string.h>

/* A KILL sent to a remote node, resent until KILL_ACK.  Kept apart from the
 * pending table because the request itself is finished (and may be joined
 * and freed) as soon as it is killed. */
typedef struct {
    uint32_t req_id;          /* 0 = free */
    uint16_t node;
    uint8_t  tries;
    uint64_t next_tx_us;
} cl_kill_t;

static cl_pending_t g_pending[CL_MAX_PENDING];
static cl_kill_t    g_kills[CL_MAX_KILLS];
static uint16_t     g_seq;
static uint16_t     g_rr;          /* round-robin cursor for placement */

void cl_rpc_init(void)
{
    memset(g_pending, 0, sizeof(g_pending));
    memset(g_kills, 0, sizeof(g_kills));
    g_seq = 0u;
    g_rr  = 0u;
}

cl_pending_t *cl_rpc_slot(int idx)
{
    if (idx < 0 || idx >= CL_MAX_PENDING) {
        return NULL;
    }
    return &g_pending[idx];
}

static cl_pending_t *find(uint32_t req_id)
{
    for (int i = 0; i < CL_MAX_PENDING; i++) {
        if (g_pending[i].state != CL_P_FREE && g_pending[i].req_id == req_id) {
            return &g_pending[i];
        }
    }
    return NULL;
}

/* Move a request to DONE and wake its joiner.  Idempotent. */
static void finish(cl_pending_t *p, int32_t status, int32_t err,
                   const void *ret, size_t ret_len)
{
    if (p->state == CL_P_DONE) {
        return;
    }
    if (p->arg != NULL) {
        kfree(p->arg);
        p->arg = NULL;
    }
    p->status  = status;
    p->err     = err;
    p->ret_len = 0u;
    if (err == CLUSTER_OK && ret_len > 0u) {
        p->ret = (uint8_t *)kmalloc(ret_len);
        if (p->ret != NULL) {
            memcpy(p->ret, ret, ret_len);
            p->ret_len = (uint16_t)ret_len;
        } else {
            p->err = CLUSTER_ERR_NOMEM;
        }
    }
    if (p->err == CLUSTER_OK) {
        cl_stat_reqs_done++;
    }
    p->state = CL_P_DONE;
    ksemaphore_signal(&p->done);
}

static void send_spawn(cl_pending_t *p, uint64_t now)
{
    const cl_node_t *n = cl_node_find(p->node);
    if (n == NULL) {
        return;
    }
    size_t len = cl_encode_spawn(cl_tx_buf, sizeof(cl_tx_buf), cl_build_id,
                                 cl_self_id, p->req_id, p->task,
                                 p->arg, p->arg_len);
    if (len > 0u) {
        (void)cl_net_send_to(n->ip, cl_tx_buf, len);
    }
    p->tries++;
    p->next_tx_us = now + CL_RETRY_US;
}

static void send_kill(cl_kill_t *k, uint64_t now)
{
    const cl_node_t *n = cl_node_find(k->node);
    if (n != NULL) {
        size_t len = cl_encode_kill(cl_tx_buf, sizeof(cl_tx_buf), cl_build_id,
                                    cl_self_id, CL_MSG_KILL, k->req_id);
        if (len > 0u) {
            (void)cl_net_send_to(n->ip, cl_tx_buf, len);
        }
    }
    k->tries++;
    k->next_tx_us = now + CL_RETRY_US;
}

/* -------------------------------------------------------------------------
 * Events from clusterd
 * ------------------------------------------------------------------------- */
void cl_rpc_deliver(uint32_t req_id, int32_t status, int32_t err,
                    const void *ret, size_t ret_len)
{
    cl_pending_t *p = find(req_id);
    if (p != NULL) {
        finish(p, status, err, ret, ret_len);
    }
}

void cl_rpc_on_spawn_ack(uint32_t req_id, uint8_t code)
{
    cl_pending_t *p = find(req_id);
    if (p == NULL || p->state != CL_P_SENT) {
        return;
    }
    switch (code) {
    case CL_ACK_OK:
        p->state = CL_P_RUNNING;
        kfree(p->arg);                      /* no more retransmits */
        p->arg = NULL;
        break;
    case CL_ACK_NOTASK:
        finish(p, 0, CLUSTER_ERR_NOTASK, NULL, 0u);
        break;
    default:
        finish(p, 0, CLUSTER_ERR_BUSY, NULL, 0u);
        break;
    }
}

void cl_rpc_on_kill_ack(uint32_t req_id)
{
    for (int i = 0; i < CL_MAX_KILLS; i++) {
        if (g_kills[i].req_id == req_id) {
            g_kills[i].req_id = 0u;
        }
    }
}

void cl_rpc_node_lost(uint16_t node)
{
    for (int i = 0; i < CL_MAX_PENDING; i++) {
        cl_pending_t *p = &g_pending[i];
        if ((p->state == CL_P_SENT || p->state == CL_P_RUNNING) &&
            p->node == node && node != cl_self_id) {
            finish(p, 0, CLUSTER_ERR_NODE_LOST, NULL, 0u);
        }
    }
}

void cl_rpc_tick(uint64_t now)
{
    for (int i = 0; i < CL_MAX_PENDING; i++) {
        cl_pending_t *p = &g_pending[i];
        if (p->state == CL_P_SENT && now >= p->next_tx_us) {
            if (p->tries >= CL_SPAWN_TRIES) {
                finish(p, 0, CLUSTER_ERR_NODE_LOST, NULL, 0u);
            } else {
                send_spawn(p, now);
            }
        }
        if (p->state != CL_P_DONE && p->state != CL_P_FREE &&
            p->deadline_us != 0u && now >= p->deadline_us) {
            finish(p, 0, CLUSTER_ERR_TIMEOUT, NULL, 0u);
        }
    }
    for (int i = 0; i < CL_MAX_KILLS; i++) {
        cl_kill_t *k = &g_kills[i];
        if (k->req_id == 0u || now < k->next_tx_us) continue;
        if (k->tries >= CL_SPAWN_TRIES || cl_node_find(k->node) == NULL) {
            k->req_id = 0u;           /* give up: node gone or not answering */
        } else {
            send_kill(k, now);
        }
    }
}

/* -------------------------------------------------------------------------
 * Placement: the node (self included) with the most free worker slots,
 * round-robin among ties.  A remote node's count is its last HELLO minus the
 * SPAWNs we have sent it since.
 * ------------------------------------------------------------------------- */
static int pick_node(void)
{
    int best_id   = -1;
    int best_free = 0;
    int best_rank = 0;

    /* Rank = distance after the round-robin cursor, so ties rotate. */
    #define RANK(id) ((int)(uint16_t)((uint16_t)(id) - g_rr))

    int self_free = cl_exec_free_slots();
    if (self_free > 0) {
        best_id   = cl_self_id;
        best_free = self_free;
        best_rank = RANK(cl_self_id);
    }
    for (int i = 0; i < CL_MAX_NODES; i++) {
        const cl_node_t *n = cl_node_slot(i);
        if (!cl_node_usable(n)) {
            continue;
        }
        int f = (int)n->free_slots - (int)n->sent_since_hello;
        if (f <= 0) {
            continue;
        }
        int r = RANK(n->id);
        if (f > best_free || (f == best_free && r < best_rank)) {
            best_id   = n->id;
            best_free = f;
            best_rank = r;
        }
    }
    #undef RANK

    if (best_id >= 0) {
        g_rr = (uint16_t)(best_id + 1);
    }
    return best_id;
}

/* -------------------------------------------------------------------------
 * Public API (cluster.h)
 * ------------------------------------------------------------------------- */
int cluster_thread_create(cluster_thread_t *t, const char *task,
                          const void *arg, size_t arg_len, int node)
{
    if (t == NULL || task == NULL || (arg_len > 0u && arg == NULL)) {
        return CLUSTER_ERR_ARG;
    }
    if (!cl_running()) {
        return CLUSTER_ERR_NOTRUNNING;
    }
    const cluster_task_t *ct = cl_task_find(task);
    if (ct == NULL || strlen(task) >= CL_TASK_NAME_MAX) {
        return CLUSTER_ERR_NOTASK;
    }
    if (arg_len > CLUSTER_MAX_PAYLOAD) {
        return CLUSTER_ERR_TOOBIG;
    }

    cl_lock();

    if (node == CLUSTER_NODE_ANY) {
        node = pick_node();
        if (node < 0) {
            cl_unlock();
            return CLUSTER_ERR_BUSY;
        }
    } else if (node != (int)cl_self_id &&
               (node < 0 || node > 0xFFFF ||
                !cl_node_usable(cl_node_find((uint16_t)node)))) {
        cl_unlock();
        return CLUSTER_ERR_NONODE;
    }

    cl_pending_t *p = NULL;
    for (int i = 0; i < CL_MAX_PENDING; i++) {
        if (g_pending[i].state == CL_P_FREE) {
            p = &g_pending[i];
            break;
        }
    }
    if (p == NULL) {
        cl_unlock();
        return CLUSTER_ERR_BUSY;
    }

    memset(p, 0, sizeof(*p));
    if (++g_seq == 0u) g_seq = 1u;
    p->req_id = ((uint32_t)cl_self_id << 16) | g_seq;
    p->node   = (uint16_t)node;
    strncpy(p->task, task, sizeof(p->task) - 1u);
    ksemaphore_init(&p->done, 0);

    if (node == (int)cl_self_id) {
        int rc = cl_exec_start(p->req_id, cl_self_id, ct, arg, arg_len);
        if (rc != CLUSTER_OK) {
            p->state = CL_P_FREE;
            cl_unlock();
            return rc;
        }
        p->state = CL_P_RUNNING;
    } else {
        p->arg = (uint8_t *)kmalloc(arg_len > 0u ? arg_len : 1u);
        if (p->arg == NULL) {
            p->state = CL_P_FREE;
            cl_unlock();
            return CLUSTER_ERR_NOMEM;
        }
        if (arg_len > 0u) {
            memcpy(p->arg, arg, arg_len);
        }
        p->arg_len = (uint16_t)arg_len;
        p->state   = CL_P_SENT;
        cl_node_t *n = cl_node_find(p->node);
        if (n != NULL) {
            n->sent_since_hello++;
        }
        send_spawn(p, cl_now_us());
    }

    t->req_id = p->req_id;
    t->node   = node;
    cl_unlock();
    return CLUSTER_OK;
}

int cluster_thread_join(cluster_thread_t *t, void *ret, size_t ret_max,
                        size_t *ret_len, int *status, uint32_t timeout_ms)
{
    if (ret_len != NULL) *ret_len = 0u;
    if (status  != NULL) *status  = 0;
    if (t == NULL) {
        return CLUSTER_ERR_ARG;
    }

    cl_lock();
    cl_pending_t *p = find(t->req_id);
    if (p == NULL) {
        cl_unlock();
        return CLUSTER_ERR_ARG;
    }
    if (timeout_ms != 0u && p->state != CL_P_DONE) {
        p->deadline_us = cl_now_us() + (uint64_t)timeout_ms * 1000u;
    }
    cl_unlock();

    ksemaphore_wait(&p->done);          /* clusterd signals exactly once */

    cl_lock();
    int err = p->err;
    if (err == CLUSTER_OK) {
        size_t n = p->ret_len < ret_max ? p->ret_len : ret_max;
        if (ret != NULL && n > 0u) {
            memcpy(ret, p->ret, n);
        }
        if (ret_len != NULL) *ret_len = (ret != NULL) ? n : 0u;
        if (status  != NULL) *status  = p->status;
    }
    if (p->ret != NULL) {
        kfree(p->ret);
    }
    memset(p, 0, sizeof(*p));           /* state = CL_P_FREE */
    cl_unlock();

    t->req_id = 0u;
    return err;
}

int cluster_thread_kill(cluster_thread_t *t)
{
    if (t == NULL) {
        return CLUSTER_ERR_ARG;
    }
    if (!cl_running()) {
        return CLUSTER_ERR_NOTRUNNING;
    }
    cl_lock();
    cl_pending_t *p = find(t->req_id);
    if (p == NULL) {
        cl_unlock();
        return CLUSTER_ERR_ARG;
    }
    if (p->state == CL_P_DONE) {
        cl_unlock();
        return CLUSTER_OK;            /* already finished — keep its result */
    }

    if (p->node == cl_self_id) {
        (void)cl_exec_kill(p->req_id);
    } else {
        /* SENT or RUNNING: tell the worker.  If the kill table is full the
         * KILL is still sent once, just not retried. */
        cl_kill_t spare = { 0u, 0u, 0u, 0u };
        cl_kill_t *k = &spare;
        for (int i = 0; i < CL_MAX_KILLS; i++) {
            if (g_kills[i].req_id == 0u) {
                k = &g_kills[i];
                break;
            }
        }
        k->req_id = p->req_id;
        k->node   = p->node;
        k->tries  = 0u;
        send_kill(k, cl_now_us());
    }
    finish(p, 0, CLUSTER_ERR_KILLED, NULL, 0u);
    cl_unlock();
    return CLUSTER_OK;
}

int cl_rpc_kill_req(uint32_t req_id)
{
    cluster_thread_t t = { req_id, 0 };
    return cluster_thread_kill(&t);
}

const char *cluster_strerror(int err)
{
    switch (err) {
    case CLUSTER_OK:             return "ok";
    case CLUSTER_ERR_ARG:        return "bad argument";
    case CLUSTER_ERR_NOTASK:     return "unknown task";
    case CLUSTER_ERR_NONODE:     return "node not available";
    case CLUSTER_ERR_BUSY:       return "no free worker";
    case CLUSTER_ERR_TOOBIG:     return "argument too large";
    case CLUSTER_ERR_TIMEOUT:    return "timed out";
    case CLUSTER_ERR_NODE_LOST:  return "node lost";
    case CLUSTER_ERR_NOTRUNNING: return "clusterd not running";
    case CLUSTER_ERR_NOMEM:      return "out of memory";
    case CLUSTER_ERR_KILLED:     return "worker killed";
    default:                     return "unknown error";
    }
}
