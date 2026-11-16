/*
 * mandel.c — distributed Mandelbrot on the Display Pack (see mandel.h).
 */

#include "mandel.h"

#include "cluster/cluster.h"
#include "cluster/display.h"
#include "cluster/node.h"            /* cl_config_value: MANDEL_ITER= */
#include "kernel/mem.h"
#include "kernel/syscall.h"
#include "shell/shell.h"

#include "pico/time.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(mandel_args_t) == 20u, "mandel_args_t is sent on the wire");
_Static_assert(MANDEL_BAND_ROWS * MANDEL_W <= CLUSTER_MAX_PAYLOAD,
               "a band's result must fit one cluster payload");
_Static_assert(MANDEL_H % MANDEL_BAND_ROWS == 0u, "bands must tile the image");

#define FIX(d)              ((int32_t)((d) * (double)(1L << MANDEL_FRAC)))
#define FOUR                ((int64_t)4 << MANDEL_FRAC)

/* =========================================================================
 * Worker — runs on whichever node the cluster picks.
 *
 * Image row py runs top to bottom along the panel's long side and carries
 * the real axis (larger real at the top); column px carries the imaginary
 * axis.  That fits the set's 3:2 shape into the tall portrait image.
 *
 * Fixed point, because the RP2040 has no FPU.  While a point has not
 * escaped, |x|, |y| <= 2, so the next x and y stay well inside Q4.28; the
 * squares are kept in 64 bits for the escape test.
 * ========================================================================= */
int mandel_worker(const void *arg, size_t arg_len,
                  void *ret, size_t ret_max, size_t *ret_len)
{
    mandel_args_t a;
    if (arg_len != sizeof(a)) {
        return -1;
    }
    memcpy(&a, arg, sizeof(a));
    if (a.nrows == 0u || a.nrows > MANDEL_BAND_ROWS ||
        (uint32_t)a.row0 + a.nrows > MANDEL_H ||
        ret_max < (size_t)a.nrows * MANDEL_W || a.max_iter == 0u) {
        return -1;
    }

    uint8_t *out = (uint8_t *)ret;
    for (unsigned r = 0; r < a.nrows; r++) {
        int py = (int)a.row0 + (int)r;
        int32_t c_r = (int32_t)((int64_t)a.cr +
                                (int64_t)((int)MANDEL_H / 2 - py) * a.step);
        for (unsigned px = 0; px < MANDEL_W; px++) {
            int32_t c_i = (int32_t)((int64_t)a.ci +
                                    (int64_t)((int)px - (int)MANDEL_W / 2) * a.step);
            int32_t x = 0, y = 0;
            unsigned it = 0;
            for (; it < a.max_iter; it++) {
                int64_t x2 = ((int64_t)x * x) >> MANDEL_FRAC;
                int64_t y2 = ((int64_t)y * y) >> MANDEL_FRAC;
                if (x2 + y2 > FOUR) {
                    break;
                }
                int64_t xy2 = ((int64_t)x * y) >> (MANDEL_FRAC - 1);   /* 2xy */
                y = (int32_t)(xy2 + c_i);
                x = (int32_t)(x2 - y2 + c_r);
            }
            *out++ = it >= a.max_iter ? (uint8_t)MANDEL_INSIDE
                                      : (uint8_t)(it < 254u ? it : 254u);
        }
        sys_yield();                             /* be polite to the node */
    }
    *ret_len = (size_t)a.nrows * MANDEL_W;
    return 0;
}

/* =========================================================================
 * Main app
 * ========================================================================= */
#include "drivers/display.h"
#include "kernel/dev.h"

#define MANDEL_DEFAULT_ITER   255
#define MANDEL_MIN_ITER       16
#define MANDEL_MAX_ITER       1000
#define MANDEL_BANDS          (MANDEL_H / MANDEL_BAND_ROWS)
#define MANDEL_JOIN_MS        30000u
#define MANDEL_BUSY_RETRIES   100              /* x 100 ms */
#define MANDEL_FLUSH_US       100000u          /* flush at least every 100 ms */
#define MANDEL_POLL_MS        50u
#define MANDEL_START_MS       30000u           /* wait for clusterd to come up */
#define MANDEL_SETTLE_MS      3000u            /* then 3 HELLOs to find peers  */

/* Demo mode: after AUTO_IDLE_US with no button pressed, act as if A (zoom
 * in) were pressed every AUTO_PERIOD_US, and X (next view) after every
 * AUTO_ZOOMS zooms.  Any button press pauses it for AUTO_IDLE_US again. */
#define AUTO_IDLE_US          60000000u        /* 1 minute   */
#define AUTO_PERIOD_US        15000000u        /* 15 seconds */
#define AUTO_ZOOMS            8
#define STRIPE_W              3u               /* node colour stripe, pixels */

/* Panel mounted portrait.  The picoOS framebuffer is landscape (240 x 135),
 * so image pixel (px, py) is rotated 90 degrees into it.  Which way depends
 * on the mount; flip this if the image comes out upside down. */
#define MANDEL_USB_BOTTOM     1     /* 1: USB end at the bottom (hexagon rig) */

/* Smallest step before Q4.28 runs out of resolution, and the widest view. */
#define STEP_MIN              16
#define STEP_MAX              FIX(0.05)

typedef struct {
    const char *name;
    int32_t     cr, ci, step;
} view_t;

static const view_t k_views[] = {
    { "full set",        FIX(-0.75),      FIX(0.0),        FIX(0.0175)   },
    { "seahorse valley", FIX(-0.7453),    FIX(0.1127),     FIX(0.00004)  },
    { "elephant valley", FIX(0.2850),     FIX(0.0110),     FIX(0.00006)  },
    { "spiral",          FIX(-0.761574),  FIX(-0.0847596), FIX(0.00001)  },
};
#define NUM_VIEWS ((int)(sizeof(k_views) / sizeof(k_views[0])))

/* Stripe colour for the node that computed a band. */
static const uint8_t k_node_colors[8] = {
    COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW,
    COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE, RGB332(255, 128, 0),
};

typedef struct {
    cluster_thread_t t;
    uint16_t         row0;
} band_slot_t;

/* One mandel runs at a time (it holds the display), so these are static to
 * keep them off the app's stack. */
static band_slot_t g_inflight[CLUSTER_MAX_PENDING];
static uint8_t     g_band[MANDEL_BAND_ROWS * MANDEL_W];
static uint8_t     g_palette[256];

/* Last time a button was seen pressed (demo mode waits AUTO_IDLE_US). */
static uint64_t    g_last_user_us;

/* Node tally for the frame summary. */
#define TALLY_MAX 16
static struct { int node; int bands; } g_tally[TALLY_MAX];
static int g_tally_n;

static void build_palette(void)
{
    /* Escape counts cycle through a hue wheel; inside points are black. */
    for (unsigned i = 0; i < 255u; i++) {
        unsigned t = (i * 6u) % 192u;          /* 6 segments of 32 */
        unsigned f = (t % 32u) * 8u;           /* 0..248 ramp      */
        unsigned r, g, b;
        switch (t / 32u) {
        case 0:  r = 255;     g = f;       b = 0;       break;
        case 1:  r = 255 - f; g = 255;     b = 0;       break;
        case 2:  r = 0;       g = 255;     b = f;       break;
        case 3:  r = 0;       g = 255 - f; b = 255;     break;
        case 4:  r = f;       g = 0;       b = 255;     break;
        default: r = 255;     g = 0;       b = 255 - f; break;
        }
        g_palette[i] = RGB332(r, g, b);
    }
    g_palette[MANDEL_INSIDE] = COLOR_BLACK;
}

static uint8_t buttons(void)
{
    uint8_t b = 0u;
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_GET_BTNS, &b);
    return b;
}

static void put(unsigned px, unsigned py, uint8_t color)
{
#if MANDEL_USB_BOTTOM
    disp_pixel_arg_t a = { (uint16_t)(DISP_WIDTH - 1u - py), (uint16_t)px, color, 0u };
#else
    disp_pixel_arg_t a = { (uint16_t)py, (uint16_t)(DISP_HEIGHT - 1u - px), color, 0u };
#endif
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_DRAW_PIXEL, &a);
}

static void draw_band(unsigned row0, unsigned nrows, const uint8_t *px, int node)
{
    uint8_t stripe = k_node_colors[(unsigned)node % 8u];
    for (unsigned r = 0; r < nrows; r++) {
        for (unsigned x = 0; x < MANDEL_W; x++) {
            put(x, row0 + r, x < STRIPE_W ? stripe : g_palette[px[r * MANDEL_W + x]]);
        }
    }
}

static void draw_failed_band(unsigned row0, unsigned nrows)
{
    for (unsigned r = 0; r < nrows; r++) {
        for (unsigned x = 0; x < MANDEL_W; x++) {
            put(x, row0 + r, RGB332(64, 64, 64));
        }
    }
}

static void tally(int node)
{
    for (int i = 0; i < g_tally_n; i++) {
        if (g_tally[i].node == node) { g_tally[i].bands++; return; }
    }
    if (g_tally_n < TALLY_MAX) {
        g_tally[g_tally_n].node  = node;
        g_tally[g_tally_n].bands = 1;
        g_tally_n++;
    }
}

/*
 * Render one frame.  Returns true if Y was pressed during the frame (the
 * remaining bands are killed and the app should quit).
 */
static bool render(const view_t *v, int max_iter)
{
    uint64_t t0 = time_us_64();
    uint64_t last_flush = t0;
    int head = 0, count = 0;
    unsigned next_row = 0u;
    int done = 0, failed = 0, busy_waits = 0;
    bool quit = false;
    g_tally_n = 0;

    while (next_row < MANDEL_H || count > 0) {
        /* Launch as many bands as the cluster will take. */
        while (!quit && next_row < MANDEL_H && count < CLUSTER_MAX_PENDING) {
            mandel_args_t a = { v->cr, v->ci, v->step, (uint16_t)next_row,
                                (uint16_t)MANDEL_BAND_ROWS, (uint16_t)max_iter, 0u };
            band_slot_t *s = &g_inflight[(head + count) % CLUSTER_MAX_PENDING];
            int rc = cluster_thread_create(&s->t, "mandel", &a, sizeof(a),
                                           CLUSTER_NODE_ANY);
            if (rc == CLUSTER_ERR_BUSY) {
                break;                   /* every worker is in use */
            }
            if (rc != CLUSTER_OK) {
                shell_print("mandel: create failed: %s\r\n", cluster_strerror(rc));
                draw_failed_band(next_row, MANDEL_BAND_ROWS);
                failed++;
                next_row += MANDEL_BAND_ROWS;
                continue;
            }
            s->row0 = (uint16_t)next_row;
            next_row += MANDEL_BAND_ROWS;
            count++;
        }
        if (quit && next_row < MANDEL_H) {
            next_row = MANDEL_H;         /* launch nothing more */
        }

        if (count == 0) {
            if (next_row >= MANDEL_H) break;
            if (++busy_waits > MANDEL_BUSY_RETRIES) {
                shell_println("mandel: no free workers, giving up");
                break;
            }
            sys_sleep(100);              /* wait for HELLOs to refresh */
            continue;
        }

        /* Join the oldest band. */
        band_slot_t *s = &g_inflight[head];
        size_t len = 0u;
        int status = 0;
        int rc = cluster_thread_join(&s->t, g_band, sizeof(g_band), &len,
                                     &status, MANDEL_JOIN_MS);
        if (rc == CLUSTER_OK && status == 0 &&
            len == (size_t)MANDEL_BAND_ROWS * MANDEL_W) {
            draw_band(s->row0, MANDEL_BAND_ROWS, g_band, s->t.node);
            tally(s->t.node);
            done++;
        } else if (rc != CLUSTER_ERR_KILLED) {
            shell_print("mandel: band %u on node %d failed: %s\r\n",
                        (unsigned)s->row0, s->t.node,
                        rc != CLUSTER_OK ? cluster_strerror(rc) : "bad result");
            draw_failed_band(s->row0, MANDEL_BAND_ROWS);
            failed++;
        }
        head = (head + 1) % CLUSTER_MAX_PENDING;
        count--;

        uint64_t now = time_us_64();
        if (now - last_flush >= MANDEL_FLUSH_US) {
            dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);
            last_flush = now;
        }

        /* Y aborts the frame: kill what is still running, then join it.
         * Any other button only counts as activity (pauses demo mode). */
        uint8_t held = buttons();
        if (held != 0u) {
            g_last_user_us = time_us_64();
        }
        if (!quit && (held & DISP_BTN_Y) != 0u) {
            quit = true;
            for (int i = 0; i < count; i++) {
                cluster_thread_kill(&g_inflight[(head + i) % CLUSTER_MAX_PENDING].t);
            }
        }
    }
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);

    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000u);
    shell_print("mandel: %s, step %ld, %d iter: %lu.%03lu s, %d bands",
                v->name, (long)v->step, max_iter,
                (unsigned long)(ms / 1000u), (unsigned long)(ms % 1000u), done);
    if (failed > 0) shell_print(" (%d failed)", failed);
    if (quit) shell_print(" (stopped)");
    shell_print("\r\nmandel: bands per node:");
    for (int i = 0; i < g_tally_n; i++) {
        shell_print(" %d:%d", g_tally[i].node, g_tally[i].bands);
    }
    shell_println("");
    return quit;
}

/*
 * Wait for a button press (edge, not level) and return it.  In demo mode
 * (no button for AUTO_IDLE_US) a simulated press is returned instead, every
 * AUTO_PERIOD_US: A, A, ... (AUTO_ZOOMS times), then X, and round again.
 * *zooms counts the demo's A presses since its last X; a real press resets
 * it, so the demo starts a fresh cycle after a pause.  *auto_press is set
 * when the press was simulated.
 */
static uint8_t wait_button(uint64_t *last_auto_us, int *zooms, bool *auto_press)
{
    uint8_t prev = buttons();            /* ignore a button still held */
    *auto_press = false;
    for (;;) {
        sys_sleep(MANDEL_POLL_MS);
        uint8_t now_b = buttons();
        uint8_t pressed = (uint8_t)(now_b & (uint8_t)~prev);
        prev = now_b;
        uint64_t now = time_us_64();
        if (now_b != 0u) {
            g_last_user_us = now;
        }
        if (pressed != 0u) {
            *zooms = 0;
            if (pressed & DISP_BTN_Y) return DISP_BTN_Y;
            if (pressed & DISP_BTN_A) return DISP_BTN_A;
            if (pressed & DISP_BTN_B) return DISP_BTN_B;
            if (pressed & DISP_BTN_X) return DISP_BTN_X;
        }
        if (now - g_last_user_us >= AUTO_IDLE_US &&
            now - *last_auto_us >= AUTO_PERIOD_US) {
            *last_auto_us = now;
            *auto_press = true;
            if (*zooms >= AUTO_ZOOMS) {
                *zooms = 0;
                return DISP_BTN_X;
            }
            (*zooms)++;
            return DISP_BTN_A;
        }
    }
}

void mandel_entry(void *arg)
{
    /* Iterations: argument, else config.txt MANDEL_ITER=, else default.
     * AUTORUN= and BUTTONx= starts never pass an argument. */
    int max_iter = MANDEL_DEFAULT_ITER;
    char val[8];
    if (arg != NULL) {
        max_iter = atoi((const char *)arg);
        kfree(arg);
    } else if (cl_config_value("MANDEL_ITER=", val, sizeof(val)) > 0) {
        max_iter = atoi(val);
    }
    if (max_iter < MANDEL_MIN_ITER) max_iter = MANDEL_MIN_ITER;
    if (max_iter > MANDEL_MAX_ITER) max_iter = MANDEL_MAX_ITER;

    /* Start clusterd if needed (AUTORUN=mandel).  Claim the display only
     * after it is up: before its status thread exists a claim is not
     * recorded, and the status page would draw over the image. */
    bool started = false;
    if (cluster_start(MANDEL_START_MS, &started) != CLUSTER_OK) {
        shell_println("mandel: clusterd did not start (see the status page / config.txt)");
        return;
    }
    if (started) {
        sys_sleep(MANDEL_SETTLE_MS);     /* let peers booting with us be heard */
    }
    if (cl_display_claim() != CLUSTER_OK) {
        shell_println("mandel: the display is in use by another app");
        return;
    }

    dev_open(DEV_DISPLAY);               /* re-arm SPI0 after WiFi */
    uint8_t bg = COLOR_BLACK;
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_SET_BG, &bg);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_CLEAR, NULL);
    dev_ioctl(DEV_DISPLAY, IOCTL_DISP_FLUSH, NULL);
    build_palette();

    shell_print("mandel: %d node(s); A zoom in, B zoom out, X next view, Y quit\r\n",
                cluster_node_count());

    int preset = 0;
    view_t v = k_views[0];
    g_last_user_us = time_us_64();       /* demo mode starts after a minute */
    uint64_t last_auto_us = 0u;
    int zooms = 0;
    for (;;) {
        if (render(&v, max_iter)) break;

        bool auto_press;
        uint8_t b = wait_button(&last_auto_us, &zooms, &auto_press);
        if (b == DISP_BTN_A && auto_press && v.step / 2 < STEP_MIN) {
            b = DISP_BTN_X;              /* demo can't zoom further: move on */
            zooms = 0;
        }
        if (auto_press) {
            shell_print("mandel: demo %s\r\n",
                        b == DISP_BTN_A ? "zoom in" : "next view");
        }
        if (b == DISP_BTN_Y) {
            break;
        } else if (b == DISP_BTN_A) {
            if (v.step / 2 >= STEP_MIN) v.step /= 2;
        } else if (b == DISP_BTN_B) {
            v.step = v.step <= STEP_MAX / 2 ? v.step * 2 : STEP_MAX;
        } else if (b == DISP_BTN_X) {
            preset = (preset + 1) % NUM_VIEWS;
            v = k_views[preset];
        }
    }

    shell_println("mandel: done");
    cl_display_release();
}
