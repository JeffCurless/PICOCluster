/*
 * clusterd.c — the cluster daemon and the 'cluster' shell command.
 *
 * clusterd joins WiFi, opens the cluster sockets, then loops forever:
 *   - drain received packets (HELLO, SPAWN, SPAWN_ACK, RESULT, RESULT_ACK,
 *     KILL, KILL_ACK)
 *   - deliver finished local jobs, retransmit unacknowledged RESULTs
 *   - retransmit unacknowledged SPAWNs, expire join deadlines
 *   - forget silent nodes (their requests fail with NODE_LOST)
 *   - multicast a HELLO every CL_HELLO_INTERVAL_US
 */

#include "cluster/clusterd.h"
#include "cluster/cluster.h"
#include "cluster/core.h"
#include "cluster/diag.h"
#include "cluster/exec.h"
#include "cluster/net.h"
#include "cluster/node.h"
#include "cluster/rpc.h"
#include "cluster/status.h"

#include "apps/app_table.h"   /* app_table[]: clusterd's priority */
#include "kernel/mem.h"
#include "kernel/sync.h"
#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/wifi.h"
#include "shell/shell.h"

#include <stdlib.h>
#include <string.h>

#define CLD_LOOP_MS  5u

uint16_t cl_self_id;
uint32_t cl_build_id;
uint8_t  cl_tx_buf[CL_MAX_PKT];
uint32_t cl_stat_tasks_run;
uint32_t cl_stat_reqs_done;

kmutex_t                 cl_g_lock;           /* cl_lock() (core.h)       */
static volatile bool     g_running;
static bool              g_started;           /* clusterd entered once    */
static volatile bool     g_spawned;           /* cluster_start spawned it */
static cl_recent_t       g_recent;            /* finished remote req ids  */
static cl_rx_pkt_t       g_rx;                /* clusterd's receive copy  */
static uint64_t          g_next_hello_us;

bool cl_running(void) { return g_running; }

#define CLD_START_POLL_MS  100u
#define CLD_DEFAULT_PRIO   3u

int cluster_start(uint32_t timeout_ms, bool *started)
{
    if (started != NULL) *started = false;
    if (g_running) {
        return CLUSTER_OK;
    }
    if (!g_started && !g_spawned) {
        g_spawned = true;               /* one spawn, even if callers race */
        uint8_t prio = CLD_DEFAULT_PRIO;
        for (int i = 0; i < app_table_size; i++) {
            if (strcmp(app_table[i].name, "clusterd") == 0) {
                prio = app_table[i].priority;
                break;
            }
        }
        /* Its own process (as AUTORUN= would make it), so it outlives the
         * app that started it. */
        pcb_t *proc = task_create_process("clusterd",
                                          task_alloc_pid(PID_RANGE_AUTORUN));
        tcb_t *t = NULL;
        if (proc != NULL) {
            t = task_create_thread(proc, "clusterd", clusterd_entry, NULL,
                                   prio, DEFAULT_STACK_SIZE);
            if (t == NULL) {
                task_free_process(proc);
            }
        }
        if (t == NULL) {
            g_spawned = false;
            CL_LOG("cannot start clusterd (process or thread pool full)\r\n");
            return CLUSTER_ERR_NOTRUNNING;
        }
        CL_LOG("clusterd started by PID %d\r\n", sys_getpid());
        if (started != NULL) *started = true;
    }
    for (uint32_t waited = 0u; !g_running; waited += CLD_START_POLL_MS) {
        if (timeout_ms != 0u && waited >= timeout_ms) {
            return CLUSTER_ERR_NOTRUNNING;
        }
        sys_sleep(CLD_START_POLL_MS);
    }
    return CLUSTER_OK;
}
#ifndef PICOOS_LOCK_DEBUG
void cl_lock(void)    { kmutex_lock(&cl_g_lock); }
void cl_unlock(void)  { kmutex_unlock(&cl_g_lock); }
#endif

int cluster_self(void)
{
    return g_running ? (int)cl_self_id : -1;
}

int cluster_node_count(void)
{
    if (!g_running) {
        return 0;
    }
    int n = 1;                                 /* self */
    cl_lock();
    for (int i = 0; i < CL_MAX_NODES; i++) {
        if (cl_node_usable(cl_node_slot(i))) n++;
    }
    cl_unlock();
    return n;
}

/* -------------------------------------------------------------------------
 * Build id: nodes cooperate only when this matches.  It covers the
 * picoCluster version, the protocol version, the payload limit and the
 * task table, so bump CLUSTER_VER_* whenever task code changes in a way
 * that matters to other nodes.
 * ------------------------------------------------------------------------- */
static uint32_t compute_build_id(void)
{
    const uint32_t v[] = { CLUSTER_VER_MAJOR, CLUSTER_VER_MINOR,
                           CLUSTER_VER_EDIT, CL_PROTO_VER,
                           CLUSTER_MAX_PAYLOAD };
    uint32_t h = cl_fnv1a(CL_FNV_INIT, v, sizeof(v));
    for (int i = 0; i < cluster_task_count; i++) {
        const char *name = cluster_task_table[i].name;
        h = cl_fnv1a(h, name, strlen(name) + 1u);
    }
    return h;
}

/* -------------------------------------------------------------------------
 * Packet handling (cl_lock held)
 * ------------------------------------------------------------------------- */
static void send_to_node(uint16_t node, size_t len)
{
    const cl_node_t *n = cl_node_find(node);
    if (n != NULL && len > 0u) {
        (void)cl_net_send_to(n->ip, cl_tx_buf, len);
    }
}

static void on_spawn(const cl_msg_t *m)
{
    uint8_t code = CL_ACK_OK;
    cl_job_t *j = cl_exec_find(m->req_id);

    if (j == NULL && cl_recent_has(&g_recent, m->req_id)) {
        return;                         /* late duplicate of a finished job */
    }
    if (j == NULL) {
        const cluster_task_t *t = cl_task_find(m->task);
        if (t == NULL) {
            code = CL_ACK_NOTASK;
        } else if (cl_exec_start(m->req_id, m->src_node, t,
                                 m->data, m->data_len) != CLUSTER_OK) {
            code = CL_ACK_BUSY;
        }
    }
    /* New or duplicate: (re)send the ack. */
    send_to_node(m->src_node,
                 cl_encode_spawn_ack(cl_tx_buf, sizeof(cl_tx_buf), cl_build_id,
                                     cl_self_id, m->req_id, code));
}

static void on_result(const cl_msg_t *m)
{
    cl_rpc_deliver(m->req_id, m->status, m->err, m->data, m->data_len);
    /* Always ack, even for an unknown id, so the worker stops resending. */
    send_to_node(m->src_node,
                 cl_encode_result_ack(cl_tx_buf, sizeof(cl_tx_buf), cl_build_id,
                                      cl_self_id, m->req_id));
}

static void on_result_ack(const cl_msg_t *m)
{
    cl_job_t *j = cl_exec_find(m->req_id);
    if (j != NULL && j->state == CL_JOB_SENDING && j->origin == m->src_node) {
        cl_recent_add(&g_recent, j->req_id);
        cl_exec_free(j);
    }
}

static void on_kill(const cl_msg_t *m)
{
    cl_job_t *j = cl_exec_find(m->req_id);
    if (j == NULL || j->origin == m->src_node) {
        (void)cl_exec_kill(m->req_id);
        /* Also stops a SPAWN still in flight from starting the task. */
        if (!cl_recent_has(&g_recent, m->req_id)) {
            cl_recent_add(&g_recent, m->req_id);
        }
    }
    send_to_node(m->src_node,
                 cl_encode_kill(cl_tx_buf, sizeof(cl_tx_buf), cl_build_id,
                                cl_self_id, CL_MSG_KILL_ACK, m->req_id));
}

static void handle_packet(const cl_rx_pkt_t *pkt, uint64_t now)
{
    cl_msg_t m;
    if (!cl_decode(pkt->data, pkt->len, &m)) {
        return;
    }
    if (m.src_node == cl_self_id) {
        /* Our own multicast looped back — or another board with our id. */
        if (m.type == CL_MSG_HELLO && strcmp(pkt->src_ip, wifi_get_ip_str()) != 0) {
            CL_LOG("WARNING: %s also claims NODEID %u\r\n",
                   pkt->src_ip, (unsigned)cl_self_id);
        }
        return;
    }
    if (m.type == CL_MSG_HELLO) {
        cl_node_hello(m.src_node, pkt->src_ip, m.build_id,
                      m.free_slots, m.max_slots, now);
        return;
    }
    if (m.build_id != cl_build_id) {
        return;                         /* different firmware — ignore */
    }
    cl_node_touch(m.src_node, now);
    switch (m.type) {
    case CL_MSG_SPAWN:      on_spawn(&m);                              break;
    case CL_MSG_SPAWN_ACK:  cl_rpc_on_spawn_ack(m.req_id, m.src_node, m.code);     break;
    case CL_MSG_RESULT:     on_result(&m);                             break;
    case CL_MSG_RESULT_ACK: on_result_ack(&m);                         break;
    case CL_MSG_KILL:       on_kill(&m);                               break;
    case CL_MSG_KILL_ACK:   cl_rpc_on_kill_ack(m.req_id);              break;
    default:                                                           break;
    }
}

/* Deliver finished jobs; resend RESULTs still waiting for an ack. */
static void service_jobs(uint64_t now)
{
    cl_exec_reap();
    for (int i = 0; i < CL_MAX_WORKERS; i++) {
        cl_job_t *j = cl_exec_slot(i);
        if (j->state == CL_JOB_DONE) {
            if (j->origin == cl_self_id) {
                cl_rpc_deliver(j->req_id, j->status, j->err, j->ret, j->ret_len);
                cl_exec_free(j);
                continue;
            }
            j->state = CL_JOB_SENDING;
            j->tries = 0u;
            j->next_tx_us = now;
        }
        if (j->state == CL_JOB_SENDING && now >= j->next_tx_us) {
            /* An origin missing from the node table just makes this send a
             * no-op; it still counts as a try. */
            if (j->tries >= CL_RESULT_TRIES) {
                CL_LOG("dropping result %08lx: origin %u unreachable\r\n",
                       (unsigned long)j->req_id, (unsigned)j->origin);
                cl_recent_add(&g_recent, j->req_id);
                cl_exec_free(j);
                continue;
            }
            send_to_node(j->origin,
                         cl_encode_result(cl_tx_buf, sizeof(cl_tx_buf),
                                          cl_build_id, cl_self_id, j->req_id,
                                          j->status, j->err, j->ret, j->ret_len));
            j->tries++;
            j->next_tx_us = now + CL_RETRY_US;
        }
    }
}

/* HELLO multicasts that failed to send, and the last error (wifi.h). */
static uint32_t g_hello_fail;
static int      g_hello_err;

static void send_hello(uint64_t now)
{
    int free_slots = cl_exec_free_slots();
    size_t len = cl_encode_hello(cl_tx_buf, sizeof(cl_tx_buf), cl_build_id,
                                 cl_self_id, (uint8_t)free_slots,
                                 (uint8_t)CL_MAX_WORKERS);
    if (len > 0u) {
        /* Multicast finds new nodes, but WiFi multicast is unacknowledged
         * and easily lost, so known peers also get a unicast copy (acked
         * and retried by the radio).  That keeps them from timing out. */
        int rc = cl_net_send_mcast(cl_tx_buf, len);
        if (rc != 0) {
            if (g_hello_fail == 0u) {
                CL_LOG("HELLO send failed (%d)\r\n", rc);
            }
            g_hello_fail++;
            g_hello_err = rc;
        }
        for (int i = 0; i < CL_MAX_NODES; i++) {
            const cl_node_t *n = cl_node_slot(i);
            if (n != NULL && n->used) {
                (void)cl_net_send_to(n->ip, cl_tx_buf, len);
            }
        }
    }
    g_next_hello_us = now + CL_HELLO_INTERVAL_US;
}

/* -------------------------------------------------------------------------
 * 'cluster' shell command
 * ------------------------------------------------------------------------- */
static void print_hex_or_text(const uint8_t *buf, size_t len)
{
    bool text = true;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\0' && i == len - 1u) break;     /* trailing NUL ok */
        if (buf[i] < 0x20u || buf[i] > 0x7Eu) { text = false; break; }
    }
    if (text) {
        shell_print("\"%.*s\"", (int)len, (const char *)buf);
        return;
    }
    for (size_t i = 0; i < len && i < 32u; i++) {
        shell_print("%02x", buf[i]);
    }
    if (len > 32u) shell_print("...");
}

static void cmd_status(void)
{
    shell_print("picoCluster v%u.%u.%u  build %08lx\r\n",
                (unsigned)CLUSTER_VER_MAJOR, (unsigned)CLUSTER_VER_MINOR,
                (unsigned)CLUSTER_VER_EDIT, (unsigned long)cl_build_id);
    if (!g_running) {
        shell_println("clusterd: not running (run clusterd, or AUTORUN=clusterd)");
        return;
    }
    shell_print("node %u  %s  workers %d/%d free  nodes %d  rx drops %lu\r\n",
                (unsigned)cl_self_id, wifi_get_ip_str(),
                cl_exec_free_slots(), CL_MAX_WORKERS, cluster_node_count(),
                (unsigned long)cl_net_drops());
    shell_print("hello send failures %lu (last error %d)\r\n",
                (unsigned long)g_hello_fail, g_hello_err);
}

static void cmd_nodes(void)
{
    uint64_t now = cl_now_us();
    shell_println("  NODE  IP               FREE  AGE(ms)  BUILD");
    cl_lock();
    shell_print("  %4u  %-15s  %d/%d  %7s  self\r\n", (unsigned)cl_self_id,
                wifi_get_ip_str(), cl_exec_free_slots(), CL_MAX_WORKERS, "-");
    for (int i = 0; i < CL_MAX_NODES; i++) {
        const cl_node_t *n = cl_node_slot(i);
        if (!n->used) continue;
        shell_print("  %4u  %-15s  %u/%u  %7lu  %s\r\n", (unsigned)n->id, n->ip,
                    (unsigned)n->free_slots, (unsigned)n->max_slots,
                    (unsigned long)((now - n->last_seen_us) / 1000u),
                    n->build_id == cl_build_id ? "ok" : "INCOMPATIBLE");
    }
    cl_unlock();
}

static void cmd_jobs(void)
{
    static const char *const pstate[] = { "free", "sent", "running", "done" };
    static const char *const jstate[] = { "free", "running", "done", "sending",
                                          "killing" };
    cl_lock();
    shell_println("Requests from this node:");
    for (int i = 0; i < CL_MAX_PENDING; i++) {
        const cl_pending_t *p = cl_rpc_slot(i);
        if (p->state == CL_P_FREE) continue;
        shell_print("  %08lx  %-15s  node %-4u  %s\r\n", (unsigned long)p->req_id,
                    p->task, (unsigned)p->node, pstate[p->state]);
    }
    shell_println("Tasks running here:");
    for (int i = 0; i < CL_MAX_WORKERS; i++) {
        const cl_job_t *j = cl_exec_slot(i);
        if (j->state == CL_JOB_FREE) continue;
        shell_print("  %08lx  %-15s  from %-4u  pid %-3lu  %s\r\n",
                    (unsigned long)j->req_id, j->task->name, (unsigned)j->origin,
                    (unsigned long)j->pid, jstate[j->state]);
    }
    cl_unlock();
}

static int cmd_run(int argc, char **argv)
{
    if (argc < 3) {
        shell_println("Usage: cluster run <task> [text-arg] [node]");
        shell_print("Tasks:");
        for (int i = 0; i < cluster_task_count; i++) {
            shell_print(" %s", cluster_task_table[i].name);
        }
        shell_println("");
        return -1;
    }
    const char *task = argv[2];
    const char *arg  = (argc >= 4) ? argv[3] : "";
    int node = CLUSTER_NODE_ANY;
    if (argc >= 5) {
        node = atoi(argv[4]);
    }

    cluster_thread_t t;
    int rc = cluster_thread_create(&t, task, arg, strlen(arg) + 1u, node);
    if (rc != CLUSTER_OK) {
        shell_print("cluster run: %s\r\n", cluster_strerror(rc));
        return -1;
    }
    shell_print("cluster run: %s on node %d (req %08lx)\r\n", task, t.node,
                (unsigned long)t.req_id);

    static uint8_t ret[CLUSTER_MAX_PAYLOAD];   /* only the shell runs this */
    size_t ret_len = 0u;
    int status = 0;
    rc = cluster_thread_join(&t, ret, sizeof(ret), &ret_len, &status, 30000u);
    if (rc != CLUSTER_OK) {
        shell_print("cluster run: %s\r\n", cluster_strerror(rc));
        return -1;
    }
    shell_print("status %d  result %u bytes ", status, (unsigned)ret_len);
    print_hex_or_text(ret, ret_len);
    shell_println("");
    return 0;
}

static int cmd_kill(int argc, char **argv)
{
    if (argc < 3) {
        shell_println("Usage: cluster kill <req>   (req id from 'cluster jobs')");
        return -1;
    }
    uint32_t req = (uint32_t)strtoul(argv[2], NULL, 16);
    int rc = cl_rpc_kill_req(req);
    if (rc != CLUSTER_OK) {
        shell_print("cluster kill: %s\r\n",
                    rc == CLUSTER_ERR_ARG ? "no such request on this node"
                                          : cluster_strerror(rc));
        return -1;
    }
    shell_print("cluster kill: %08lx killed\r\n", (unsigned long)req);
    return 0;
}

/* -------------------------------------------------------------------------
 * Breadcrumbs (diag.h) — read without any lock
 * ------------------------------------------------------------------------- */
cl_crumb_t cl_crumb_create;
cl_crumb_t cl_crumb_exec;
cl_crumb_t cl_crumb_net;
cl_crumb_t cl_crumb_loop;

static void print_crumb(const char *name, const char *file, const cl_crumb_t *c,
                        uint64_t now)
{
    if (c->line == 0u) {
        shell_print("  %-7s idle (last TID %ld)\r\n", name, (long)c->tid);
    } else {
        shell_print("  %-7s TID %ld at %s:%lu for %lu ms\r\n", name, (long)c->tid,
                    file, (unsigned long)c->line,
                    (unsigned long)((now - c->since_us) / 1000u));
    }
}

void cl_diag_print(void)
{
    uint64_t now = time_us_64();
    shell_print("cl_lock owner TID %ld\r\n", (long)cl_g_lock.owner_tid);
    print_crumb("loop",   "clusterd.c", &cl_crumb_loop,   now);
    print_crumb("create", "rpc.c",      &cl_crumb_create, now);
    print_crumb("exec",   "exec.c",     &cl_crumb_exec,   now);
    print_crumb("net",    "net.c",      &cl_crumb_net,    now);
}

static int cmd_cluster(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "diag") == 0) {
        cl_diag_print();                /* no locks: works while stuck */
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        cmd_status();
        return 0;
    }
    if (!g_running) {
        shell_println("clusterd: not running");
        return -1;
    }
    if (strcmp(argv[1], "nodes") == 0) { cmd_nodes(); return 0; }
    if (strcmp(argv[1], "jobs")  == 0) { cmd_jobs();  return 0; }
    if (strcmp(argv[1], "run")   == 0) { return cmd_run(argc, argv); }
    if (strcmp(argv[1], "kill")  == 0) { return cmd_kill(argc, argv); }
    shell_println("Usage: cluster [status|nodes|jobs|run <task> [arg] [node]|kill <req>|diag]");
    return -1;
}

static const shell_cmd_t g_cluster_cmd = {
    "cluster",
    "cluster [status|nodes|jobs|run <task> [arg] [node]|kill <req>|diag]",
    cmd_cluster,
};

/* -------------------------------------------------------------------------
 * Daemon entry
 * ------------------------------------------------------------------------- */
void clusterd_entry(void *arg)
{
    if (arg != NULL) {
        kfree(arg);                    /* 'run clusterd <x>' — unused */
    }
    if (g_started) {
        CL_LOG("clusterd already started (restart the board to restart it)\r\n");
        return;
    }
    g_started = true;

    cl_build_id = compute_build_id();
    kmutex_init(&cl_g_lock);
    shell_register_cmd(&g_cluster_cmd);
    cl_status_start();      /* keeps showing the phase even if we stop below */

    cl_config_t cfg;
    if (cl_config_load(&cfg) != 0) {
        cl_status_set_phase(CL_PHASE_NO_CONFIG);
        return;
    }
    cl_self_id = (uint16_t)cfg.nodeid;
    cl_status_set_enabled(cfg.status_page);

    cl_status_set_phase(CL_PHASE_WIFI);
    if (cl_wifi_up(&cfg) != 0) {
        cl_status_set_phase(CL_PHASE_WIFI_FAIL);
        CL_LOG("WiFi join failed — clusterd stopped\r\n");
        return;
    }
    if (cl_net_open() != 0) {
        cl_status_set_phase(CL_PHASE_NET_FAIL);
        CL_LOG("network setup failed — clusterd stopped\r\n");
        return;
    }

    cl_nodes_init();
    cl_exec_init();
    cl_rpc_init();
    cl_recent_init(&g_recent);
    g_next_hello_us = 0u;
    g_running = true;
    cl_status_set_phase(CL_PHASE_RUNNING);
    CL_LOG("node %u up at %s, build %08lx, %d workers\r\n",
           (unsigned)cl_self_id, wifi_get_ip_str(),
           (unsigned long)cl_build_id, CL_MAX_WORKERS);

    uint16_t lost[CL_MAX_NODES];
    for (;;) {
        uint64_t now = cl_now_us();

        CL_CRUMB(cl_crumb_loop);
        cl_lock();
        CL_CRUMB(cl_crumb_loop);
        while (cl_net_recv(&g_rx)) {
            handle_packet(&g_rx, now);
        }
        CL_CRUMB(cl_crumb_loop);
        service_jobs(now);
        CL_CRUMB(cl_crumb_loop);
        cl_rpc_tick(now);
        CL_CRUMB(cl_crumb_loop);
        int nlost = cl_nodes_expire(now, lost, CL_MAX_NODES);
        for (int i = 0; i < nlost; i++) {
            cl_rpc_node_lost(lost[i]);
        }
        if (now >= g_next_hello_us) {
            CL_CRUMB(cl_crumb_loop);
            send_hello(now);
        }
        CL_CRUMB_DONE(cl_crumb_loop);
        cl_unlock();

        sys_sleep(CLD_LOOP_MS);
    }
}
