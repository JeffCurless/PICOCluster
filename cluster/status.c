/*
 * status.c — Display Pack status page (see status.h).
 */

#include "cluster/status.h"

#ifdef PICOOS_DISPLAY_ENABLE

#include "cluster/cluster.h"
#include "cluster/core.h"
#include "cluster/exec.h"
#include "cluster/node.h"
#include "cluster/rpc.h"

#include "drivers/display.h"
#include "kernel/dev.h"
#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/wifi.h"

#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Geometry: 8x8 font, one text row per ROW_H pixels.
 *   Display Pack   240x135 -> 30 columns x 15 rows
 *   Display Pack 2 320x240 -> 40 columns x 24 rows
 * ------------------------------------------------------------------------- */
#define CHAR_W     8u
#ifdef PICOOS_DISPLAY_PACK2
#define ROW_H      10u
#else
#define ROW_H      9u
#endif
#define COLS       (DISP_WIDTH / CHAR_W)
#define ROWS       (DISP_HEIGHT / ROW_H)

/* Rows: title, network, NODES label | node rows | THIS NODE label,
 * one per worker, footer. */
#define NODE_ROW0  3u
#define TAIL_ROWS  (2u + (unsigned)CL_MAX_WORKERS)
#define NODE_ROWS  (ROWS - NODE_ROW0 - TAIL_ROWS)

#define C_BG       COLOR_BLACK
#define C_TEXT     COLOR_WHITE
#define C_DIM      RGB332(128, 128, 128)
#define C_LABEL    COLOR_CYAN
#define C_TITLE_BG RGB332(0, 64, 160)
#define C_SELF     COLOR_YELLOW
#define C_BUSY     RGB332(255, 128, 0)
#define C_FREE     RGB332(0, 128, 0)
#define C_ERR      COLOR_RED

/* -------------------------------------------------------------------------
 * Snapshot of everything drawn.  Zeroed before filling so memcmp() against
 * the previous one is a reliable "changed?" test.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t id;
    char     ip[CL_IP_STR_MAX];
    uint8_t  busy;
    uint8_t  max;
    uint8_t  compat;
    uint8_t  self;
} snap_node_t;

typedef struct {
    uint8_t  state;          /* cl_job_state_t */
    char     task[CL_TASK_NAME_MAX];
    uint16_t origin;
    uint32_t secs;
} snap_job_t;

typedef struct {
    uint8_t     phase;
    uint16_t    self;
    char        ip[CL_IP_STR_MAX];
    int         nnodes;                  /* self + compatible peers */
    int         count;                   /* rows in nodes[]         */
    snap_node_t nodes[CL_MAX_NODES + 1];
    snap_job_t  jobs[CL_MAX_WORKERS];
    int         pending;
    uint32_t    tasks_run;
    uint32_t    reqs_done;
} snap_t;

static volatile cl_phase_t g_phase = CL_PHASE_STARTING;
static snap_t              g_snap[2];        /* current, previous */

void cl_status_set_phase(cl_phase_t phase)
{
    g_phase = phase;
}

static void take_snapshot(snap_t *s)
{
    memset(s, 0, sizeof(*s));
    s->phase = (uint8_t)g_phase;
    if (g_phase != CL_PHASE_RUNNING) {
        return;
    }
    uint64_t now = cl_now_us();

    cl_lock();
    s->self = cl_self_id;
    strncpy(s->ip, wifi_get_ip_str(), sizeof(s->ip) - 1u);

    snap_node_t *me = &s->nodes[s->count++];
    me->id     = cl_self_id;
    me->max    = (uint8_t)CL_MAX_WORKERS;
    me->busy   = (uint8_t)(CL_MAX_WORKERS - cl_exec_free_slots());
    me->compat = 1u;
    me->self   = 1u;
    memcpy(me->ip, s->ip, sizeof(me->ip));
    s->nnodes = 1;

    for (int i = 0; i < CL_MAX_NODES; i++) {
        const cl_node_t *n = cl_node_slot(i);
        if (!n->used) continue;
        snap_node_t *d = &s->nodes[s->count++];
        d->id     = n->id;
        d->max    = n->max_slots;
        d->busy   = (uint8_t)(n->max_slots > n->free_slots
                              ? n->max_slots - n->free_slots : 0);
        d->compat = (uint8_t)(n->build_id == cl_build_id);
        memcpy(d->ip, n->ip, sizeof(d->ip));
        if (d->compat) s->nnodes++;
    }

    for (int i = 0; i < CL_MAX_WORKERS; i++) {
        const cl_job_t *j = cl_exec_slot(i);
        snap_job_t *d = &s->jobs[i];
        d->state = (uint8_t)j->state;
        if (j->state == CL_JOB_FREE) continue;
        strncpy(d->task, j->task->name, sizeof(d->task) - 1u);
        d->origin = j->origin;
        d->secs   = (uint32_t)((now - j->started_us) / 1000000u);
    }

    for (int i = 0; i < CL_MAX_PENDING; i++) {
        const cl_pending_t *p = cl_rpc_slot(i);
        if (p->state == CL_P_SENT || p->state == CL_P_RUNNING) s->pending++;
    }
    s->tasks_run = cl_stat_tasks_run;
    s->reqs_done = cl_stat_reqs_done;
    cl_unlock();

    /* Peers sorted by id, self stays first. */
    for (int i = 2; i < s->count; i++) {
        snap_node_t t = s->nodes[i];
        int k = i - 1;
        while (k >= 1 && s->nodes[k].id > t.id) {
            s->nodes[k + 1] = s->nodes[k];
            k--;
        }
        s->nodes[k + 1] = t;
    }
}

/* -------------------------------------------------------------------------
 * Drawing helpers
 * ------------------------------------------------------------------------- */
static void text(unsigned col, unsigned row, const char *str,
                 uint8_t fg, uint8_t bg)
{
    disp_text_arg_t a = { (uint16_t)(col * CHAR_W), (uint16_t)(row * ROW_H),
                          fg, bg, 1u, 0u, str };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_TEXT, &a);
}

static void rect(unsigned x, unsigned y, unsigned w, unsigned h,
                 uint8_t color, bool filled)
{
    disp_rect_arg_t a = { (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h,
                          color, filled ? 1u : 0u, 0u, 0u };
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_RECT, &a);
}

/* Slot boxes for one node row, starting at text column `col`. */
static void slots(unsigned col, unsigned row, const snap_node_t *n)
{
    unsigned x = col * CHAR_W;
    unsigned y = row * ROW_H;
    for (unsigned i = 0; i < n->max && i < 8u; i++) {
        bool busy = i < n->busy;
        rect(x + i * 9u, y, 7u, 7u, busy ? C_BUSY : C_FREE, busy);
    }
}

static void led(uint32_t rgb)
{
#ifdef PICOOS_LED_ENABLE
    dev_ioctl(DEV_LED, IOCTL_LED_SET_RGB, &rgb);
#else
    (void)rgb;
#endif
}

static const char *phase_msg(uint8_t phase)
{
    switch ((cl_phase_t)phase) {
    case CL_PHASE_STARTING:  return "starting...";
    case CL_PHASE_NO_CONFIG: return "config.txt: need NODEID, SSID";
    case CL_PHASE_WIFI:      return "joining WiFi...";
    case CL_PHASE_WIFI_FAIL: return "WiFi join failed";
    case CL_PHASE_NET_FAIL:  return "network setup failed";
    default:                 return "";
    }
}

static void draw(const snap_t *s)
{
    char line[64];          /* >= worst case; the panel clips at its edge */
    uint8_t bg = C_BG;
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_SET_BG, &bg);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_CLEAR, NULL);

    /* Title bar */
    rect(0u, 0u, DISP_WIDTH, ROW_H, C_TITLE_BG, true);
    if (s->phase == CL_PHASE_RUNNING) {
        snprintf(line, sizeof(line), "picoCluster  node %u", (unsigned)s->self);
    } else {
        snprintf(line, sizeof(line), "picoCluster");
    }
    text(0u, 0u, line, C_TEXT, C_TITLE_BG);
    snprintf(line, sizeof(line), "v%u.%u.%u", (unsigned)CLUSTER_VER_MAJOR,
             (unsigned)CLUSTER_VER_MINOR, (unsigned)CLUSTER_VER_EDIT);
    text(COLS - (unsigned)strlen(line), 0u, line, C_TEXT, C_TITLE_BG);

    if (s->phase != CL_PHASE_RUNNING) {
        bool error = s->phase != CL_PHASE_STARTING && s->phase != CL_PHASE_WIFI;
        text(0u, 2u, phase_msg(s->phase), error ? C_ERR : C_TEXT, C_BG);
        led(error ? 0x200000u : 0x201000u);          /* red / yellow */
        return;
    }

    /* Network line */
    snprintf(line, sizeof(line), "%s", s->ip);
    text(0u, 1u, line, C_TEXT, C_BG);
    snprintf(line, sizeof(line), "%d node%s", s->nnodes, s->nnodes == 1 ? "" : "s");
    text(COLS - (unsigned)strlen(line), 1u, line, C_TEXT, C_BG);

    /* Nodes */
    text(0u, 2u, "NODES", C_LABEL, C_BG);
    unsigned shown = 0u;
    for (int i = 0; i < s->count; i++) {
        unsigned row = NODE_ROW0 + shown;
        if (shown == NODE_ROWS - 1u && (unsigned)(s->count - i) > 1u) {
            snprintf(line, sizeof(line), " +%d more", s->count - i);
            text(0u, row, line, C_DIM, C_BG);
            break;
        }
        const snap_node_t *n = &s->nodes[i];
        snprintf(line, sizeof(line), "%c%3u %-15s", n->self ? '*' : ' ',
                 (unsigned)n->id, n->ip);
        text(0u, row, line, n->self ? C_SELF : (n->compat ? C_TEXT : C_DIM), C_BG);
        if (n->compat) {
            slots(21u, row, n);
        } else {
            text(21u, row, "build?", C_ERR, C_BG);
        }
        shown++;
    }

    /* This node's workers */
    unsigned row = ROWS - TAIL_ROWS;
    text(0u, row++, "THIS NODE", C_LABEL, C_BG);
    bool busy = false;
    for (int i = 0; i < CL_MAX_WORKERS; i++, row++) {
        const snap_job_t *j = &s->jobs[i];
        if (j->state == CL_JOB_FREE) {
            snprintf(line, sizeof(line), " w%d idle", i);
            text(0u, row, line, C_DIM, C_BG);
            continue;
        }
        busy = true;
        snprintf(line, sizeof(line), " w%d %-8.8s from %-3u %4lus", i, j->task,
                 (unsigned)j->origin, (unsigned long)j->secs);
        text(0u, row, line, j->state == CL_JOB_RUNNING ? C_BUSY : C_TEXT, C_BG);
    }

    /* Footer */
    snprintf(line, sizeof(line), "out %d  ran %lu  got %lu", s->pending,
             (unsigned long)s->tasks_run, (unsigned long)s->reqs_done);
    text(0u, row, line, C_DIM, C_BG);

    led(busy || s->pending > 0 ? 0x000020u : 0x002000u);   /* blue / green */
}

/* -------------------------------------------------------------------------
 * Thread
 * ------------------------------------------------------------------------- */
static void status_thread(void *arg)
{
    (void)arg;
    dev_open(DEV_DISPLAY);
    int cur = 0;
    bool first = true;
    for (;;) {
        take_snapshot(&g_snap[cur]);
        if (first || memcmp(&g_snap[cur], &g_snap[cur ^ 1], sizeof(snap_t)) != 0) {
            draw(&g_snap[cur]);
            dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);
            first = false;
            cur ^= 1;
        }
        sys_sleep(CL_STATUS_PERIOD_MS);
    }
}

void cl_status_start(void)
{
    pcb_t *proc = task_find_process((uint32_t)sys_getpid());
    if (proc == NULL ||
        task_create_thread(proc, "cl-disp", status_thread, NULL,
                           4u, DEFAULT_STACK_SIZE) == NULL) {
        CL_LOG("status display thread not started\r\n");
    }
}

#else  /* !PICOOS_DISPLAY_ENABLE */

void cl_status_start(void) {}
void cl_status_set_phase(cl_phase_t phase) { (void)phase; }

#endif /* PICOOS_DISPLAY_ENABLE */
