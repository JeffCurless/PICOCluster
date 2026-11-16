/*
 * mandel.h — distributed Mandelbrot on the Display Pack.
 *
 *   run mandel [max_iter]     (default 255, range 16..1000)
 *
 * Without an argument (always the case for AUTORUN=mandel and BUTTONX=mandel)
 * the limit comes from config.txt MANDEL_ITER=, if set.  mandel starts
 * clusterd itself when it is not running (cluster_start()).
 *
 * The main app (mandel_entry) runs on the node it was started on and draws
 * the image on that node's panel, upright for a portrait mount (135 x 240).
 * The image is split into bands of MANDEL_BAND_ROWS rows; each band is a
 * "mandel" cluster task placed on whichever node has a free worker.  A
 * coloured stripe on the left edge of each band shows which node computed
 * it.  Frame time and bands per node are printed to the shell.
 *
 * Buttons: A zoom in, B zoom out, X next preset view, Y quit.
 * Demo mode: with no button pressed for a minute, it zooms in (as if A were
 * pressed) every 15 s, and after 8 zooms moves to the next view (X).  Any
 * button pauses the demo for another minute; Y still quits.
 */

#ifndef APPS_MANDEL_H
#define APPS_MANDEL_H

#include <stddef.h>
#include <stdint.h>

/* Portrait image size: the 240x135 panel turned on its side. */
#define MANDEL_W          135u
#define MANDEL_H          240u
#define MANDEL_BAND_ROWS  3u          /* 3 x 135 = 405 result bytes */

/* Fixed point: Q4.28 (range -8..8, resolution 2^-28). */
#define MANDEL_FRAC       28

typedef struct {
    int32_t  cr;        /* real part at the image centre       (Q4.28) */
    int32_t  ci;        /* imaginary part at the image centre  (Q4.28) */
    int32_t  step;      /* distance between pixels             (Q4.28) */
    uint16_t row0;      /* first image row of the band                 */
    uint16_t nrows;     /* rows in the band (<= MANDEL_BAND_ROWS)      */
    uint16_t max_iter;
    uint16_t _pad;
} mandel_args_t;

/* Result: one byte per pixel, row by row: the escape iteration (0..254),
 * or MANDEL_INSIDE for points that never escaped. */
#define MANDEL_INSIDE     255u

void mandel_entry(void *arg);

/* The "mandel" cluster task. */
int  mandel_worker(const void *arg, size_t arg_len,
                   void *ret, size_t ret_max, size_t *ret_len);

#endif /* APPS_MANDEL_H */
