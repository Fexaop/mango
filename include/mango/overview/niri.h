#ifndef __OVERVIEW_NIRI_H__
#define __OVERVIEW_NIRI_H__ 1

/*
 * niri-style overview: native workspace geometry is transformed by zoom.
 * Scrolling belongs to the monitor workspace viewport, independently of
 * overview lifetime. The native axis and zero gap morph continuously into
 * a vertical strip with a 10% workspace gap as zoom opens.
 */

#include "mango/common/types.h"
#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

typedef struct {
	bool detached, inserts_workspace;
	uint32_t target_tag;
	struct wlr_box highlight;
} NiriOverviewDrag;

/* overview_style values */
enum { OVERVIEW_STYLE_GRID = 0, OVERVIEW_STYLE_NIRI = 1 };

bool niri_overview_enabled(void);
bool niri_overview_active(const Monitor *m);

void niri_overview_open(Monitor *m);
/* Restore native target-workspace input immediately; retain the projection
 * until its zoom, camera and window animations have settled. */
void niri_overview_close(Monitor *m, uint32_t target_tag);
void niri_overview_toggle(Monitor *m);
/* Tear down immediately, without animation (monitor cleanup). */
void niri_overview_destroy(Monitor *m);

/* Layout callback while the overview projection exists, including closing. */
void niri_overview_arrange(Monitor *m);
/* Follow native strict compaction without rebuilding workspace/card scenes.
 * map includes every ordinary tag; occupied retains the active empty tag. */
void niri_overview_remap_tags(Monitor *m, const uint32_t *map,
							 uint32_t occupied);
/* True once overview camera, zoom, card movement and interactions are idle. */
bool niri_overview_compaction_ready(const Monitor *m);
/* Per-frame driver; returns true while more frames are needed. */
bool niri_overview_draw_frame(Monitor *m);

/* Moves the active workspace up (dir < 0) or down (dir > 0). */
bool niri_overview_focus_dir(Monitor *m, int dir);
/* Makes tag (1-based) the active workspace. */
bool niri_overview_view_tag(Monitor *m, uint32_t tag);
/* Moves c to the workspace above (dir < 0) or below (dir > 0). */
bool niri_overview_move_client_dir(Monitor *m, Client *c, int dir);
/* Run Mango's normal window/layout commands in workspace coordinates, then
 * refresh previews, including during close. No separate management rules. */
bool niri_overview_dispatch(int32_t (*command)(const Arg *), const Arg *arg);
NiriOverview *niri_overview_begin_edit(Monitor *m);
void niri_overview_end_edit(NiriOverview *ov);

/* Interactive overview input only; closing projections never consume events. */
bool niri_overview_button(Monitor *m, uint32_t button, bool pressed);
bool niri_overview_motion(Monitor *m);
bool niri_overview_get_drag(const Client *c, NiriOverviewDrag *state);
bool niri_overview_axis(Monitor *m, bool vertical, double delta,
						int32_t delta_discrete, uint32_t time_msec);

/* Trackpad gesture drive of the open/close and switch animations. */
bool niri_overview_animating(const Monitor *m);
void niri_overview_set_progress(Monitor *m, double p);
void niri_overview_set_pan(Monitor *m, double x, double y);
void niri_overview_resume(Monitor *m, double remaining);

/* A client left the monitor or is being destroyed. */
void niri_overview_client_gone(Client *c);

#endif
