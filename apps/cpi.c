/*
 * cpi.c — distributed Monte Carlo pi (see cpi.h).
 */

#include "cpi.h"

#include "cluster/cluster.h"
#include "kernel/mem.h"
#include "kernel/syscall.h"
#include "shell/shell.h"

#include <stdlib.h>
#include <string.h>

#define CPI_DEFAULT_THREADS   6
#define CPI_MAX_THREADS      64
#define CPI_MAX_INFLIGHT     16           /* = CL_MAX_PENDING */
#define CPI_JOIN_TIMEOUT_MS  60000u
#define CPI_BUSY_RETRIES     100          /* x 100 ms */

/* -------------------------------------------------------------------------
 * Worker — runs on whichever node the cluster picks.
 * Integer only: coordinates are 15-bit, so x*x + y*y fits in 32 bits.
 * ------------------------------------------------------------------------- */
int cpi_worker(const void *arg, size_t arg_len,
               void *ret, size_t ret_max, size_t *ret_len)
{
    cpi_args_t a;
    if (arg_len != sizeof(a) || ret_max < sizeof(cpi_result_t)) {
        return -1;
    }
    memcpy(&a, arg, sizeof(a));

    uint32_t s = a.seed != 0u ? a.seed : 1u;     /* xorshift32 state */
    uint32_t hits = 0u;
    const uint32_t r2 = 32768u * 32768u;
    for (uint32_t i = 0; i < a.samples; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        uint32_t x = s & 0x7FFFu;
        uint32_t y = (s >> 15) & 0x7FFFu;
        if (x * x + y * y < r2) {
            hits++;
        }
        if ((i & 0x3FFFu) == 0u) {
            sys_yield();                         /* be polite to the node */
        }
    }

    cpi_result_t r = { hits, a.samples };
    memcpy(ret, &r, sizeof(r));
    *ret_len = sizeof(r);
    return 0;
}

/* -------------------------------------------------------------------------
 * Main app
 * ------------------------------------------------------------------------- */
typedef struct {
    cluster_thread_t t;
    int              index;
} cpi_slot_t;

void cpi_entry(void *arg)
{
    int threads = CPI_DEFAULT_THREADS;
    if (arg != NULL) {
        threads = atoi((const char *)arg);
        kfree(arg);
    }
    if (threads < 1) threads = 1;
    if (threads > CPI_MAX_THREADS) threads = CPI_MAX_THREADS;

    if (cluster_self() < 0) {
        shell_println("cpi: clusterd is not running");
        return;
    }
    shell_print("cpi: %d threads x %u samples across %d node(s)\r\n",
                threads, (unsigned)CPI_SAMPLES, cluster_node_count());

    cpi_slot_t inflight[CPI_MAX_INFLIGHT];
    int head = 0, count = 0;                 /* FIFO of running threads */
    int launched = 0, finished = 0, failed = 0, busy_waits = 0;
    uint64_t hits = 0u, samples = 0u;
    int per_node[8] = { 0 };                 /* tally for nodes 0..7 */

    while (finished + failed < threads) {
        /* Launch as many as the cluster will take. */
        while (launched < threads && count < CPI_MAX_INFLIGHT) {
            cpi_args_t a = { CPI_SAMPLES,
                             (uint32_t)(launched + 1) * 2654435761u };
            cpi_slot_t *s = &inflight[(head + count) % CPI_MAX_INFLIGHT];
            int rc = cluster_thread_create(&s->t, "pi", &a, sizeof(a),
                                           CLUSTER_NODE_ANY);
            if (rc == CLUSTER_ERR_BUSY) {
                break;                       /* every worker is in use */
            }
            if (rc != CLUSTER_OK) {
                shell_print("cpi: create failed: %s\r\n", cluster_strerror(rc));
                failed++;
                launched++;
                continue;
            }
            s->index = launched++;
            count++;
        }

        if (count == 0) {
            if (launched >= threads) break;
            if (++busy_waits > CPI_BUSY_RETRIES) {
                shell_println("cpi: no free workers, giving up");
                break;
            }
            sys_sleep(100);                  /* wait for HELLOs to refresh */
            continue;
        }

        /* Join the oldest. */
        cpi_slot_t *s = &inflight[head];
        cpi_result_t r;
        size_t len = 0u;
        int status = 0;
        int rc = cluster_thread_join(&s->t, &r, sizeof(r), &len, &status,
                                     CPI_JOIN_TIMEOUT_MS);
        if (rc == CLUSTER_OK && status == 0 && len == sizeof(r)) {
            hits    += r.hits;
            samples += r.samples;
            finished++;
            if (s->t.node >= 0 && s->t.node < 8) per_node[s->t.node]++;
        } else {
            shell_print("cpi: thread %d on node %d failed: %s\r\n", s->index,
                        s->t.node, rc != CLUSTER_OK ? cluster_strerror(rc)
                                                    : "bad result");
            failed++;
        }
        head = (head + 1) % CPI_MAX_INFLIGHT;
        count--;
    }

    if (samples == 0u) {
        shell_println("cpi: no results");
        return;
    }
    /* pi ~= 4 * hits / samples, printed with 6 decimals. */
    uint64_t pi_e6 = (4u * hits * 1000000u) / samples;
    shell_print("cpi: pi ~= %lu.%06lu  (%d ok, %d failed)\r\n",
                (unsigned long)(pi_e6 / 1000000u),
                (unsigned long)(pi_e6 % 1000000u), finished, failed);
    shell_print("cpi: threads per node:");
    for (int i = 0; i < 8; i++) {
        if (per_node[i] > 0) shell_print(" %d:%d", i, per_node[i]);
    }
    shell_println("");
}
