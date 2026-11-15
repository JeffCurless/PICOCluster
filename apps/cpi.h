/*
 * cpi.h — distributed Monte Carlo pi.
 *
 *   run cpi [threads]     (default 6, max 64)
 *
 * The main app (cpi_entry) runs on the node it was started on and creates
 * `threads` "pi" cluster tasks, spread over every node with a free worker.
 * Each task throws CPI_SAMPLES random points and returns how many landed
 * inside the quarter circle; the main app sums them and prints pi and the
 * time the whole run took.
 */

#ifndef APPS_CPI_H
#define APPS_CPI_H

#include <stddef.h>
#include <stdint.h>

#define CPI_SAMPLES  10000000u

typedef struct {
    uint32_t samples;
    uint32_t seed;
} cpi_args_t;

typedef struct {
    uint32_t hits;
    uint32_t samples;
} cpi_result_t;

void cpi_entry(void *arg);

/* The "pi" cluster task. */
int  cpi_worker(const void *arg, size_t arg_len,
                void *ret, size_t ret_max, size_t *ret_len);

#endif /* APPS_CPI_H */
