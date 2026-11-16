/*
 * display.h — sharing the Display Pack between the status page and apps.
 *
 * clusterd's status page draws on the panel unless:
 *   - config.txt has STATUS=OFF (the panel is cleared once and left to apps), or
 *   - an app has claimed the display.
 *
 * An app that draws on the panel claims it first and releases it when done:
 *
 *   if (cl_display_claim() != CLUSTER_OK) { ...someone else has it...; return; }
 *   ...draw, flush...
 *   cl_display_release();
 *
 * A claim never lands in the middle of a status-page redraw.  On release the
 * status page redraws in full.  If the claiming process is killed, the status
 * page notices within one period and takes the panel back.
 *
 * When clusterd's status page is not running there is nothing to share with,
 * and the claim always succeeds.  The LED keeps showing the cluster state
 * whether or not the panel is claimed.
 */

#ifndef CLUSTER_DISPLAY_H
#define CLUSTER_DISPLAY_H

/* CLUSTER_OK, or CLUSTER_ERR_BUSY if another process holds the display. */
int  cl_display_claim(void);

/* Release a claim held by the calling process (no-op otherwise). */
void cl_display_release(void);

#endif /* CLUSTER_DISPLAY_H */
