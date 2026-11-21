#include "mango/overview/niri.h"
#include "mango/animation/client.h"
#include "mango/animation/common.h"
#include "mango/animation/tag.h"
#include "mango/common/input-event-codes.h"
#include "mango/common/scene_node.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/config/parse.h"
#include "mango/dispatch/bind.h"
#include "mango/ext-protocol/ext-workspace.h"
#include "mango/ext-protocol/text-input.h"
#include "mango/input/keyboard.h"
#include "mango/input/pointer.h"
#include "mango/layout/arrange.h"
#include "mango/layout/layout.h"
#include "mango/manage/client.h"
#include "mango/manage/layer.h"
#include "mango/manage/misc.h"
#include "mango/manage/monitor.h"
#include "mango/manage/window-group.h"
#include "mango/overview/overview.h"
#include <math.h>
#include <scenefx/types/wlr_scene.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

/* niri: gap between workspaces is 10% of the output height (times zoom). */
#define NIRI_WS_GAP_RATIO 0.1
/* niri: workspace-shadow is configured for a workspace 1080 px tall. */
#define NIRI_SHADOW_REF_HEIGHT 1080.0
/* Niri's interactive_move_start_threshold, in unscaled layout pixels. */
#define NIRI_DRAG_THRESHOLD 256.0
#define NIRI_RESIZE_THRESHOLD 8.0
/* niri: DnD hold over a workspace activates it ("copied from gnome-shell"). */
#define NIRI_DND_HOLD_MS 750
/* Edge scrolling while dragging a window in the overview. */
#define NIRI_EDGE_SCROLL_DELAY_MS 300
#define NIRI_EDGE_SCROLL_REPEAT_MS 400
/* Touchpad scroll distance that switches one workspace. */
#define NIRI_TOUCHPAD_SCROLL_STEP 50.0

typedef struct {
	bool running;
	bool gesture; /* driven by the trackpad swipe while frozen */
	double from, to;
	uint32_t started;
	uint32_t duration;
	int32_t curve;
} NiriAnim;

typedef struct {
	LayerSurface *l;
	struct wlr_surface *surface;
	struct wlr_scene_surface *scene_surface;
	struct wl_listener destroy;
	struct wl_list link;
} NiriLayerCopy;

typedef struct {
	uint32_t tag;
	struct wlr_scene_tree *tree; /* positioned at the workspace origin */
	struct wlr_scene_shadow *shadow;
	struct wlr_scene_rect *bg;
	struct wlr_scene_tree *layers; /* scaled background/bottom copies */
	struct wl_list layer_copies; /* NiriLayerCopy::link */
} NiriWorkspace;

struct NiriOverview {
	Monitor *m;
	bool closing;
	uint32_t active_tag;  /* the workspace that becomes current on close */
	uint32_t open_tag;	  /* tagset that was current when opening */

	uint32_t slots[tag_num_MAX + 1]; /* displayed tags, top to bottom */
	int32_t nslots;

	NiriAnim open;	 /* 0 closed .. 1 open */
	double gesture_p;
	double pan_x, pan_y; /* native scroller offset from Mango's swipe driver */

	struct wlr_scene_tree *root;
	struct wlr_scene_rect *backdrop;
	struct wlr_scene_tree *shadows;
	struct wlr_scene_tree *hint_tree;
	struct wlr_scene_rect *hint;
	NiriWorkspace *ws[tag_num_MAX + 1];

	uint64_t member_sig[tag_num_MAX + 1];
	uint32_t dirty_tags;
	bool laid_out;
	uint32_t retained_tags; /* departed slots survive until safe compaction */

	Client *press_client;
	uint32_t press_button;
	bool dragging;
	bool resizing;
	double press_x, press_y;
	double pull_x, pull_y;
	double anchor_x, anchor_y;
	double pickup_x, pickup_y;
	NiriAnim pickup;
	uint32_t drop_tag;
	bool drop_insert;
	struct wlr_box drop_box;
	uint32_t edge_since;
	uint32_t edge_last;

	uint32_t dnd_slot_tag;
	uint32_t dnd_since;

	double scroll_acc;
	uint32_t scroll_time;

	struct wl_event_source *finish_idle;

	WindowGroupContext edit_context;
};

static void niri_finish(Monitor *m);
static void niri_relayout(Monitor *m);
static void niri_end_grab(Monitor *m, Client *c);
static void niri_draw_drop_hint(Monitor *m);

static uint32_t tag_bit(uint32_t tag) { return 1u << (tag - 1); }

static bool tag_valid(uint32_t tag) {
	return tag >= 1 && tag <= (uint32_t)config.tag_num;
}

bool niri_overview_enabled(void) {
	return config.overview_style == OVERVIEW_STYLE_NIRI;
}

bool niri_overview_active(const Monitor *m) {
	return m && m->niri_ov && !m->niri_ov->closing;
}

static void niri_block_input(Monitor *m) {
	m->overview_input_blocked = true;
	if (server.seat->keyboard_state.focused_surface)
		client_activate_surface(server.seat->keyboard_state.focused_surface, 0);
	mango_im_relay_set_focus(server.input_method_relay, NULL);
	wlr_seat_keyboard_notify_clear_focus(server.seat);
	pointer_constrain_cursor(NULL);
	wlr_seat_pointer_clear_focus(server.seat);
	if (server.cursor_hidden)
		pointer_cursor_activity();
	else
		wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");
}

/* ---- animation --------------------------------------------------------- */

static bool niri_anim_gesture_live(const NiriOverview *ov, const NiriAnim *a) {
	return a->gesture && server.gesture_drive_active &&
		   server.gesture_drive_mon == ov->m;
}

static double niri_anim_value(const NiriOverview *ov, const NiriAnim *a) {
	if (!a->running)
		return a->to;

	double t;
	if (a->gesture) {
		t = ov->gesture_p; /* linear while the fingers drive it */
	} else {
		if (!config.animations || a->duration == 0)
			return a->to;
		t = (double)(get_now_in_ms() - a->started) / (double)a->duration;
		if (t >= 1.0)
			return a->to;
		t = find_animation_curve_at(t, a->curve);
	}
	return a->from + (a->to - a->from) * t;
}

static bool niri_anim_done(const NiriOverview *ov, const NiriAnim *a) {
	if (!a->running)
		return true;
	if (a->gesture)
		return false;
	return !config.animations || a->duration == 0 ||
		   get_now_in_ms() - a->started >= a->duration;
}

static void niri_anim_start(NiriOverview *ov, NiriAnim *a, double to,
							uint32_t duration, int32_t curve) {
	a->from = niri_anim_value(ov, a);
	a->to = to;
	a->started = get_now_in_ms();
	a->duration = duration;
	a->curve = curve;
	a->gesture =
		server.gesture_drive_active && server.gesture_drive_mon == ov->m;
	if (a->gesture)
		ov->gesture_p = 0.0;
	a->running = true;
}

/* Hands a gesture-driven animation back to the clock: continue from the
 * current value for the remaining fraction of its duration. */
static void niri_anim_resume(NiriOverview *ov, NiriAnim *a, double remaining) {
	if (!a->running || !a->gesture)
		return;
	a->from = a->from + (a->to - a->from) * ov->gesture_p;
	a->gesture = false;
	if (!config.animations || a->duration == 0 || remaining < 0.05) {
		a->running = false;
		return;
	}
	a->duration = (uint32_t)(a->duration * remaining);
	a->started = get_now_in_ms();
	a->curve = MOVE;
}

/* ---- workspace slots ----------------------------------------------------- */

static bool niri_client_placed(const Client *c, const Monitor *m) {
	return c->mon == m && c->ov_card_tree && !c->iskilling && !c->isminimized &&
		   tag_valid(c->niri_tag);
}

/*
 * niri shows every non-empty workspace plus one empty workspace at the end;
 * an empty workspace in the middle only stays while it is active (or while
 * the switch away from it is still animating).
 */
static void niri_compute_slots(Monitor *m) {
	NiriOverview *ov = m->niri_ov;
	uint32_t occupied = 0, last = 0, t;
	Client *c;

	wl_list_for_each(c, &server.clients, link) {
		if (niri_client_placed(c, m))
			occupied |= tag_bit(c->niri_tag);
	}
	if (tag_valid(ov->active_tag))
		occupied |= tag_bit(ov->active_tag);
	occupied |= m->workspace_viewport.travel_tags;
	occupied |= ov->retained_tags;

	ov->nslots = 0;
	for (t = 1; t <= (uint32_t)config.tag_num; t++) {
		if (occupied & tag_bit(t)) {
			ov->slots[ov->nslots++] = t;
			last = t;
		}
	}

	/* Trailing empty workspace, unless the last shown one already is empty. */
	bool last_has_clients = false;
	wl_list_for_each(c, &server.clients, link) {
		if (niri_client_placed(c, m) && c->niri_tag == last) {
			last_has_clients = true;
			break;
		}
	}
	if ((last_has_clients || last == 0) && last < (uint32_t)config.tag_num)
		ov->slots[ov->nslots++] = last + 1;
}

static int32_t niri_slot_of(const NiriOverview *ov, uint32_t tag) {
	for (int32_t i = 0; i < ov->nslots; i++) {
		if (ov->slots[i] == tag)
			return i;
	}
	return -1;
}


static void niri_update_slots(Monitor *m) {
	niri_compute_slots(m);
}

/* ---- geometry -------------------------------------------------------------
 * Ported from niri src/layout/monitor.rs: workspaces are output-sized boxes
 * scaled by zoom, stacked with a gap of 0.1 * output_h * zoom, and the
 * workspace at the render index is centered on the output.
 */

typedef struct {
	double zoom;
	double ws_w, ws_h;
	double stride;
	double stride_x;
	double camera, workspace_scale;
	const uint32_t *slots;
	int32_t active_slot;
	bool scrolling;
	double off_x, off_y; /* static offset that centers a workspace */
} NiriFrame;
static NiriFrame niri_current_frame(Monitor *m);


static NiriFrame niri_frame(const Monitor *m, double open) {
	NiriFrame f;
	double W = m->m.width, H = m->m.height;
	double z = 1.0 - open * (1.0 - config.overview_zoom);

	if (z < 0.0001)
		z = 0.0001;
	f.zoom = z;
	f.ws_w = W * z;
	f.ws_h = H * z;
	f.stride_x = config.tag_animation_direction == VERTICAL
		? 0.0 : W * z * (1.0 - open);
	f.stride = H * z * (config.tag_animation_direction == VERTICAL
		? 1.0 + NIRI_WS_GAP_RATIO * open : (1.0 + NIRI_WS_GAP_RATIO) * open);
	/* Capture one immutable projection before native layout contexts borrow
	 * the monitor. Every workspace in this frame shares the same camera. */
	f.camera = workspace_viewport_position(m);
	f.scrolling = m->workspace_viewport.running;
	f.slots = m->niri_ov->slots;
	f.active_slot = niri_slot_of(m->niri_ov, m->niri_ov->active_tag);
	if (f.active_slot < 0)
		f.active_slot = 0;
	f.workspace_scale = 1.0;
	if (!config.tag_animation_continuous && f.scrolling) {
		/* Expand direct desktop spacing continuously as overview zooms out. */
		double span = m->workspace_viewport.direct_span;
		f.workspace_scale = (1.0 + (span - 1.0) * open) / span;
	}
	f.off_x = (W - f.ws_w) / 2.0;
	f.off_y = (H - f.ws_h) / 2.0;
	return f;
}

static void niri_slot_origin(const Monitor *m, const NiriFrame *f, int32_t i,
							 double *x, double *y) {
	double distance = f->scrolling
						  ? (f->slots[i] - 1.0 - f->camera) * f->workspace_scale
						  : i - f->active_slot;
	*x = m->m.x + f->off_x + distance * f->stride_x;
	*y = m->m.y + f->off_y + distance * f->stride;
}

/* Box of a workspace-space rectangle (zoom 1, layout coordinates) drawn in
 * slot i; edges are rounded separately so neighbours never gap. */
static struct wlr_box niri_map_box(const Monitor *m, const NiriFrame *f,
								   int32_t i, struct wlr_box g) {
	double ox, oy;
	niri_slot_origin(m, f, i, &ox, &oy);
	double x0 = ox + (g.x - m->m.x) * f->zoom;
	double y0 = oy + (g.y - m->m.y) * f->zoom;
	double x1 = ox + (g.x + g.width - m->m.x) * f->zoom;
	double y1 = oy + (g.y + g.height - m->m.y) * f->zoom;
	struct wlr_box b = {
		.x = (int32_t)lround(x0),
		.y = (int32_t)lround(y0),
	};
	b.width = MANGO_MAX(1, (int32_t)lround(x1) - b.x);
	b.height = MANGO_MAX(1, (int32_t)lround(y1) - b.y);
	return b;
}

static struct wlr_box niri_slot_box(const Monitor *m, const NiriFrame *f,
									int32_t i) {
	return niri_map_box(m, f, i, m->m);
}

static struct wlr_box niri_box_lerp(struct wlr_box a, struct wlr_box b,
									double t) {
	return (struct wlr_box){
		.x = (int32_t)lround(a.x + (b.x - a.x) * t),
		.y = (int32_t)lround(a.y + (b.y - a.y) * t),
		.width = (int32_t)lround(a.width + (b.width - a.width) * t),
		.height = (int32_t)lround(a.height + (b.height - a.height) * t),
	};
}

/* ---- workspace scene nodes ---------------------------------------------- */

static bool niri_reject_input(struct wlr_scene_buffer *buffer, double *sx,
							  double *sy) {
	return false;
}

static void niri_layer_copy_free(NiriLayerCopy *lc) {
	wl_list_remove(&lc->destroy.link);
	wl_list_remove(&lc->link);
	free(lc);
}

static void handle_niri_layer_copy_destroy(struct wl_listener *listener,
										   void *data) {
	NiriLayerCopy *lc = wl_container_of(listener, lc, destroy);
	/* The scene surface node goes away with the surface by itself. */
	niri_layer_copy_free(lc);
}

/* niri: the background and bottom layers zoom out together with the
 * workspaces; every workspace gets its own copy. */
static void niri_ws_add_layer_copies(NiriWorkspace *ws, Monitor *m) {
	static const int32_t layers[] = {ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
									 ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM};
	LayerSurface *l;

	for (size_t i = 0; i < LENGTH(layers); i++) {
		wl_list_for_each(l, &m->layers[layers[i]], link) {
			if (!l->mapped || !l->layer_surface || !l->layer_surface->surface ||
				l->place_within_backdrop)
				continue;

			NiriLayerCopy *lc = ecalloc(1, sizeof(*lc));
			lc->l = l;
			lc->surface = l->layer_surface->surface;
			lc->scene_surface =
				wlr_scene_surface_create(ws->layers, lc->surface);
			if (!lc->scene_surface) {
				free(lc);
				continue;
			}
			lc->scene_surface->buffer->point_accepts_input = niri_reject_input;
			wlr_scene_buffer_set_filter_mode(lc->scene_surface->buffer,
											 WLR_SCALE_FILTER_BILINEAR);
			lc->destroy.notify = handle_niri_layer_copy_destroy;
			wl_signal_add(&lc->surface->events.destroy, &lc->destroy);
			wl_list_insert(ws->layer_copies.prev, &lc->link);
		}
	}
}

static NiriWorkspace *niri_ws_get(Monitor *m, uint32_t tag) {
	NiriOverview *ov = m->niri_ov;
	NiriWorkspace *ws = ov->ws[tag];

	if (ws)
		return ws;

	ws = ecalloc(1, sizeof(*ws));
	ws->tag = tag;
	wl_list_init(&ws->layer_copies);
	ws->tree = wlr_scene_tree_create(ov->root);
	ws->bg = wlr_scene_rect_create(ws->tree, 1, 1, config.rootcolor);
	ws->layers = wlr_scene_tree_create(ws->tree);
	ws->shadow = wlr_scene_shadow_create(
		ov->shadows, 1, 1, 0, 0.0f, config.overview_workspace_shadow_color);
	niri_ws_add_layer_copies(ws, m);
	ov->ws[tag] = ws;
	return ws;
}

static void niri_ws_destroy(NiriWorkspace *ws) {
	NiriLayerCopy *lc, *tmp;

	if (!ws)
		return;
	wl_list_for_each_safe(lc, tmp, &ws->layer_copies, link)
		niri_layer_copy_free(lc);
	wlr_scene_node_destroy(&ws->shadow->node);
	wlr_scene_node_destroy(&ws->tree->node);
	free(ws);
}

/* Native compaction has already moved membership and layout groups. Keep
 * retained scenes and animation caches attached to those same workspaces. */
void niri_overview_remap_tags(Monitor *m, const uint32_t *map,
							 uint32_t occupied) {
	if (!m)
		return;
	workspace_viewport_remap(m, map);
	NiriOverview *ov = m ? m->niri_ov : NULL;
	if (!ov)
		return;

	NiriWorkspace *old_ws[tag_num_MAX + 1];
	uint64_t old_sig[tag_num_MAX + 1];
	uint32_t active = ov->active_tag;
	NiriWorkspace *empty_ws =
		tag_valid(active) && !(occupied & tag_bit(active)) ? ov->ws[active]
														 : NULL;
	memcpy(old_ws, ov->ws, sizeof(old_ws));
	memcpy(old_sig, ov->member_sig, sizeof(old_sig));
	memset(ov->ws, 0, sizeof(ov->ws));
	memset(ov->member_sig, 0, sizeof(ov->member_sig));
	ov->ws[0] = old_ws[0];
	ov->member_sig[0] = old_sig[0];

	/* Abandoned empty tags converge on one trailing slot. Retain at most
	 * one of their scenes, destroying each other old scene once rather
	 * than indexing destruction through the new tags. */
	if (!empty_ws) {
		for (uint32_t t = 1; t <= (uint32_t)config.tag_num; t++) {
			if (!(occupied & tag_bit(t)) && old_ws[t]) {
				empty_ws = old_ws[t];
				break;
			}
		}
	}
	for (uint32_t t = 1; t <= (uint32_t)config.tag_num; t++) {
		uint32_t target = map[t];
		NiriWorkspace *ws = old_ws[t];
		if (occupied & tag_bit(t)) {
			ov->ws[target] = ws;
			ov->member_sig[target] = old_sig[t];
		} else if (ws && ws == empty_ws) {
			ov->ws[target] = ws;
		} else {
			niri_ws_destroy(ws);
			continue;
		}
		if (ws)
			ws->tag = target;
	}
	for (uint32_t t = (uint32_t)config.tag_num + 1; t <= tag_num_MAX; t++)
		niri_ws_destroy(old_ws[t]);

	ov->active_tag = map[ov->active_tag];
	ov->open_tag = tag_remap_mask(ov->open_tag, map);
	ov->dirty_tags = tag_remap_mask(ov->dirty_tags, map);
	ov->drop_tag = map[ov->drop_tag];
	ov->dnd_slot_tag = map[ov->dnd_slot_tag];
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m)
			continue;
		if (tag_valid(c->niri_tag))
			c->niri_tag = map[c->niri_tag];
		if (tag_valid(c->niri_target_tag))
			c->niri_target_tag = map[c->niri_target_tag];
	}
	/* The camera is idle here: its remap is only a coordinate relabel.
	 * Reindex neighbours around the same active identity, never move the
	 * focused workspace or compensate by changing native window geometry. */
	ov->retained_tags = 0;
	niri_update_slots(m);
	refresh_monitors_workspaces_status(m);
	request_fresh_all_monitors();
}

static void niri_gather(Monitor *m) {
	NiriOverview *ov = m->niri_ov;
	if (!ov || config.tag_gather != 2)
		return;
	/* Gathering is a frame-completion operation. Commands may still start
	 * zoom or native MOVE animations after requesting it. Keep their old
	 * displayed coordinates until that final frame has been presented. */
	m->tag_gather_pending = true;
	ov->retained_tags |= m->workspace_viewport.travel_tags;
	for (int32_t i = 0; i < ov->nslots; i++)
		ov->retained_tags |= tag_bit(ov->slots[i]);
}

static void niri_ws_draw(Monitor *m, NiriWorkspace *ws, const NiriFrame *f,
						 int32_t i, double open) {
	struct wlr_box box = niri_slot_box(m, f, i);
	NiriLayerCopy *lc;

	wlr_scene_node_set_enabled(&ws->tree->node, true);
	wlr_scene_node_set_position(&ws->tree->node, box.x, box.y);
	wlr_scene_rect_set_size(ws->bg, box.width, box.height);
	wlr_scene_rect_set_color(ws->bg, config.rootcolor);

	wl_list_for_each(lc, &ws->layer_copies, link) {
		struct wlr_scene_buffer *buf = lc->scene_surface->buffer;
		LayerSurface *l = lc->l;
		if (!l->mapped || l->geom.width <= 0 || l->geom.height <= 0) {
			wlr_scene_node_set_enabled(&buf->node, false);
			continue;
		}
		struct wlr_box lb = niri_map_box(m, f, i, l->geom);
		wlr_scene_node_set_enabled(&buf->node, true);
		wlr_scene_node_set_position(&lc->scene_surface->buffer->node,
									lb.x - box.x, lb.y - box.y);
		wlr_scene_buffer_set_dest_size(buf, lb.width, lb.height);
	}

	/* Shadow sizes are normalized to a 1080 px tall workspace and zoomed out
	 * together with it. */
	if (!config.overview_workspace_shadow || open <= 0.0) {
		wlr_scene_node_set_enabled(&ws->shadow->node, false);
		return;
	}
	double s = box.height / NIRI_SHADOW_REF_HEIGHT;
	double spread = config.overview_workspace_shadow_spread * s;
	double softness = config.overview_workspace_shadow_softness * s;
	double pad = spread + softness;
	float color[4];
	memcpy(color, config.overview_workspace_shadow_color, sizeof(color));
	color[3] *= (float)MANGO_MIN(open, 1.0);

	wlr_scene_node_set_enabled(&ws->shadow->node, true);
	wlr_scene_node_set_position(
		&ws->shadow->node,
		(int32_t)lround(box.x - pad +
						config.overview_workspace_shadow_offset_x * s),
		(int32_t)lround(box.y - pad +
						config.overview_workspace_shadow_offset_y * s));
	wlr_scene_shadow_set_size(ws->shadow, (int32_t)lround(box.width + 2 * pad),
							  (int32_t)lround(box.height + 2 * pad));
	wlr_scene_shadow_set_blur_sigma(ws->shadow, (float)(softness / 2.0));
	wlr_scene_shadow_set_color(ws->shadow, color);
}

/* Native groups own geometry; the overview only maps their boxes to screen. */
static void niri_start_move(Client *c) {
	Monitor *m = c->mon;
	NiriFrame f = niri_current_frame(m);
	int32_t slot = niri_slot_of(m->niri_ov, c->niri_tag);
	double x, y;
	niri_slot_origin(m, &f, slot, &x, &y);
	/* Window rearrangement animates in workspace coordinates. Camera motion
	 * and zoom are applied afterwards, so neither can be frozen by a MOVE. */
	c->niri_move_from = (struct wlr_box){
		.x = m->m.x + lround((c->niri_preview_geom.x - x) / f.zoom),
		.y = m->m.y + lround((c->niri_preview_geom.y - y) / f.zoom),
		.width = lround(c->niri_preview_geom.width / f.zoom),
		.height = lround(c->niri_preview_geom.height / f.zoom),
	};
	c->niri_move_started = get_now_in_ms();
	c->niri_move_running = true;
}

static struct wlr_box niri_animate_card(Client *c, struct wlr_box target,
										uint32_t now) {
	if (!c->niri_move_running)
		return target;
	uint32_t duration = config.animation_duration_move;
	double progress =
		duration ? (double)(now - c->niri_move_started) / duration : 1.0;
	if (!client_animations_enabled(c) || progress >= 1.0) {
		c->niri_move_running = false;
		return target;
	}
	struct wlr_box from = c->niri_move_from;
	const Layout *layout = window_group_layout(window_group_for_client(c));
	if (!c->isfloating &&
		(layout->id == SCROLLER || layout->id == VERTICAL_SCROLLER)) {
		/* Scrolling reflows snap size; only position follows the MOVE curve. */
		from.width = target.width;
		from.height = target.height;
	}
	return niri_box_lerp(from, target,
						 find_animation_curve_at(progress, MOVE));
}

bool niri_overview_compaction_ready(const Monitor *m) {
	const NiriOverview *ov = m ? m->niri_ov : NULL;
	if (!ov)
		return true;
	if (m->layout_group || ov->press_client || ov->dragging ||
		ov->pan_x || ov->pan_y ||
		(server.grab_client && server.grab_client->mon == m) ||
		(server.gesture_drive_active && server.gesture_drive_mon == m) ||
		(server.seat->drag &&
		 monitor_at_point(server.cursor->x, server.cursor->y) == m))
		return false;
	if (!niri_anim_done(ov, &ov->open) ||
		!niri_anim_done(ov, &ov->pickup))
		return false;

	const WorkspaceViewport *v = &m->workspace_viewport;
	uint32_t now = get_now_in_ms();
	if (v->running && (v->gesture ||
		(config.animations && v->duration && now - v->started < v->duration)))
		return false;

	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (!niri_client_placed(c, m))
			continue;
		/* A native arrange can precede the frame that starts its render MOVE.
		 * Disabled per-client animations must not leave stale flags blocking
		 * compaction after the target geometry has already been drawn. */
		if (!wlr_box_equal(&c->niri_target_geom, &c->geom) ||
			c->niri_target_tag != c->niri_tag)
			return false;
		if (c->niri_move_running && client_animations_enabled(c) &&
			config.animation_duration_move &&
			now - c->niri_move_started < config.animation_duration_move)
			return false;
	}
	return true;
}

static uint64_t niri_member_hash(const Client *c) {
	return (uint64_t)c->id * 0x9e3779b97f4a7c15ull +
		   (c->isfloating | c->isfullscreen << 1 | c->ismaximizescreen << 2);
}

static void niri_relayout(Monitor *m) {
	NiriOverview *ov = m->niri_ov;
	uint64_t sig[tag_num_MAX + 1] = {0};
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (niri_client_placed(c, m))
			sig[c->niri_tag] += niri_member_hash(c);
	}
	for (uint32_t t = 1; t <= (uint32_t)config.tag_num; t++) {
		bool membership_changed = sig[t] != ov->member_sig[t];
		bool dirty = (ov->dirty_tags & tag_bit(t)) || membership_changed;
		/* Preserve presented native geometry on entry, but never skip a
		 * membership change just because its workspace is already visible. */
		if (!ov->laid_out && !membership_changed &&
			((ov->open_tag | m->workspace_viewport.travel_tags) & tag_bit(t)))
			dirty = false;
		if (dirty)
			window_group_arrange(window_group_for_tag(m, t));
		ov->member_sig[t] = sig[t];
	}
	ov->laid_out = true;
	ov->dirty_tags = 0;
}

void niri_overview_arrange(Monitor *m) {
	NiriOverview *ov = m ? m->niri_ov : NULL;
	Client *c;

	if (!ov)
		return;
	ov->active_tag = get_mon_curtag(m);
	niri_gather(m);

	/* Native tags own membership, including moves, minimization and rules.
	 * Rendering follows them rather than silently retagging applications. */
	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m || client_is_unmanaged(c) || client_is_x11_popup(c) ||
			c->isunglobal || c->isminimized || (c->tags & TAG0_MASK) ||
			c->iskilling || !client_surface(c)->mapped)
			continue;
		uint32_t tag = window_group_client_tag(c);
		if (!tag_valid(tag))
			continue;
		if (!c->ov_card_tree) {
			c->animation.overining = false;
			c->animation.overview_enter_anim_set = false;
			c->niri_preview_geom = c->niri_target_geom = (struct wlr_box){0};
			c->niri_target_tag = 0;
			overview_backup_surface(c);
		}
		if (tag != c->niri_tag) {
			c->niri_tag = tag;
			c->niri_move_running = false;
		}
	}

	niri_relayout(m);
	niri_update_slots(m);
	request_fresh_all_monitors();
}

/* ---- per-frame driver ---------------------------------------------------- */

static bool niri_drag_tick(Monitor *m);

static void niri_finish_idle(void *data) {
	Monitor *m = data;
	if (!m->niri_ov)
		return;
	m->niri_ov->finish_idle = NULL;
	niri_finish(m);
}

static struct wlr_box niri_card_target(Monitor *m, const NiriFrame *f,
									   Client *c, int32_t idx) {
	struct wlr_box g = c->geom;
	if (VISIBLEON(c, m) && ISSCROLLTILED(c)) {
		g.x += lround(m->niri_ov->pan_x);
		g.y += lround(m->niri_ov->pan_y);
	}
	return niri_map_box(m, f, idx, g);
}

/* Draws a card at box; borders shrink together with the workspace. */
static void niri_card_apply(Client *c, struct wlr_box box, double zoom) {
	struct wlr_box geom = c->geom, current = c->animation.current;
	uint32_t bw = c->bw;
	bool clip = c->is_clip_to_hide;
	c->niri_preview_bw = bw ? MANGO_MAX(1, (uint32_t)lround(bw * zoom)) : 0;
	box.width = MANGO_MAX(box.width, 2 * (int32_t)c->niri_preview_bw + 1);
	box.height = MANGO_MAX(box.height, 2 * (int32_t)c->niri_preview_bw + 1);
	c->niri_preview_geom = box;
	/* Decorations share the rendering API, so scope the transformed geometry
	 * to this synchronous draw. Protocol/configure state stays at native size.
	 */
	c->geom = c->animation.current = box;
	c->bw = c->niri_preview_bw;
	c->is_clip_to_hide = false;
	wlr_scene_node_set_enabled(&c->scene->node, true);
	wlr_scene_node_set_position(&c->scene->node, box.x, box.y);
	client_set_clip(c, 1.0f);
	c->geom = geom;
	c->animation.current = current;
	c->bw = bw;
	c->is_clip_to_hide = clip;
}

bool niri_overview_draw_frame(Monitor *m) {
	NiriOverview *ov = m ? m->niri_ov : NULL;
	bool more = false;
	bool windows_moving = false;
	Client *c;

	if (!ov)
		return false;

	/* A gesture that ended without handing its animation back. */
	if (!niri_anim_gesture_live(ov, &ov->open))
		niri_anim_resume(ov, &ov->open, 1.0 - ov->gesture_p);
	if (!m->tag_gather_pending)
		ov->retained_tags = 0;
	else
		ov->retained_tags |= m->workspace_viewport.travel_tags;

	double open = niri_anim_value(ov, &ov->open);
	if (niri_anim_done(ov, &ov->open))
		ov->open.running = false;
	if (!ov->press_client ||
		!client_animations_enabled(ov->press_client) ||
		niri_anim_done(ov, &ov->pickup))
		ov->pickup.running = false;
	more = workspace_viewport_tick(m);
	niri_update_slots(m);
	/* Finger updates request their own frames; a held gesture needs no tick. */
	more |= ov->open.running && !ov->open.gesture;

	NiriFrame f = niri_frame(m, open);

	wlr_scene_node_set_position(&ov->root->node, 0, 0);
	wlr_scene_node_set_position(&ov->backdrop->node, m->m.x, m->m.y);
	wlr_scene_rect_set_size(ov->backdrop, m->m.width, m->m.height);
	float backdrop[4];
	memcpy(backdrop, config.overview_backdrop_color, sizeof(backdrop));
	backdrop[3] = 1.0f; /* niri ignores the backdrop alpha */
	wlr_scene_rect_set_color(ov->backdrop, backdrop);
	bool has_backdrop = false;
	LayerSurface *layer;
	wl_list_for_each(layer, &m->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND],
					 link) {
		if (!layer->place_within_backdrop)
			continue;
		bool mapped = layer->layer_surface->surface->mapped;
		wlr_scene_node_set_enabled(&layer->scene->node, mapped);
		has_backdrop |= mapped && layer->layer_surface->surface->buffer;
	}
	wlr_scene_node_set_enabled(&ov->backdrop->node, !has_backdrop);

	bool shown[tag_num_MAX + 1] = {false};
	for (int32_t i = 0; i < ov->nslots; i++) {
		NiriWorkspace *ws = niri_ws_get(m, ov->slots[i]);
		shown[ov->slots[i]] = true;
		niri_ws_draw(m, ws, &f, i, open);
	}
	for (uint32_t t = 1; t <= (uint32_t)config.tag_num; t++) {
		if (ov->ws[t] && !shown[t]) {
			wlr_scene_node_set_enabled(&ov->ws[t]->tree->node, false);
			wlr_scene_node_set_enabled(&ov->ws[t]->shadow->node, false);
		}
	}

	uint32_t now = get_now_in_ms();
	wl_list_for_each(c, &server.clients, link) {
		if (!niri_client_placed(c, m))
			continue;
		int32_t idx = niri_slot_of(ov, c->niri_tag);
		if (idx < 0) {
			wlr_scene_node_set_enabled(&c->scene->node, false);
			continue;
		}

		struct wlr_box native = c->geom;
		bool direct =
			(ov->dragging && c == ov->press_client) ||
			((ov->pan_x || ov->pan_y) && VISIBLEON(c, m) && ISSCROLLTILED(c));
		bool changed = !wlr_box_equal(&c->niri_target_geom, &c->geom) ||
					   c->niri_target_tag != c->niri_tag;
		if (changed && !direct && c->niri_preview_geom.width > 0 &&
			c->niri_target_geom.width > 0) {
			/* Advance an interrupted animation before retargeting, so
			 * repeated resize/move events keep progressing each frame. */
			struct wlr_box from = c->niri_move_running
				? niri_animate_card(c, c->niri_target_geom, now)
				: c->niri_target_geom;
			niri_start_move(c);
			c->niri_move_from = from;
		}
		c->niri_target_geom = c->geom;
		c->niri_target_tag = c->niri_tag;
		if (direct)
			c->niri_move_running = false;
		else
			native = niri_animate_card(c, native, now);
		struct wlr_box box = niri_map_box(m, &f, idx, native);
		if (VISIBLEON(c, m) && ISSCROLLTILED(c)) {
			box.x += lround(ov->pan_x * f.zoom);
			box.y += lround(ov->pan_y * f.zoom);
		}
		if (c == ov->press_client && !ov->resizing) {
			if (ov->dragging) {
				/* The lifted tile follows the pointer across workspace gaps
				 * and scrolling, independent of native monitor bounds. */
				double p = niri_anim_value(ov, &ov->pickup);
				box.x = lround(server.cursor->x - ov->anchor_x * box.width +
							   ov->pickup_x * p);
				box.y = lround(server.cursor->y - ov->anchor_y * box.height +
							   ov->pickup_y * p);
				if (niri_anim_done(ov, &ov->pickup))
					ov->pickup.running = false;
				more |= ov->pickup.running;
			} else {
				box.x += lround(ov->pull_x);
				box.y += lround(ov->pull_y);
			}
		}
		windows_moving |= c->niri_move_running;
		more |= c->niri_move_running;
		niri_card_apply(c, box, f.zoom);
		/* Niri dims a detached tile to 75%, revealing its landing marker. */
		overview_card_set_opacity(
			c, direct && !ov->resizing && c->drag_to_tile
				   ? .75f + .25f * niri_anim_value(ov, &ov->pickup)
				   : 1.0f);
	}
	niri_draw_drop_hint(m);

	if (!ov->closing)
		more = niri_drag_tick(m) || more;

	/* Retry only requested work, after camera, zoom and card MOVE completion
	 * have been processed. Successful renumbering is drawn next frame, with
	 * the focused card anchored and neighbours closing the removed gap. */
	if (m->tag_gather_pending) {
		tag_gather_apply(m);
		if (!m->tag_gather_pending) {
			ov->retained_tags = 0;
			niri_update_slots(m);
			more = true;
		}
	}

	if (ov->closing && !ov->open.running && !m->workspace_viewport.running &&
		!windows_moving && !ov->finish_idle) {
		/* Hand surfaces back only after the final rendered boxes settle. */
		ov->finish_idle =
			wl_event_loop_add_idle(server.event_loop, niri_finish_idle, m);
	}
	return more;
}

/* ---- lifecycle ------------------------------------------------------------
 */

static void niri_free(Monitor *m) {
	NiriOverview *ov = m->niri_ov;

	if (!ov)
		return;
	if (ov->finish_idle)
		wl_event_source_remove(ov->finish_idle);
	LayerSurface *layer;
	wl_list_for_each(layer, &m->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND],
					 link) {
		if (layer->place_within_backdrop)
			wlr_scene_node_set_enabled(&layer->scene->node, false);
	}
	for (uint32_t t = 0; t <= tag_num_MAX; t++)
		niri_ws_destroy(ov->ws[t]);
	wlr_scene_node_destroy(&ov->hint_tree->node);
	wlr_scene_node_destroy(&ov->root->node);
	free(ov);
	m->niri_ov = NULL;
}

/* Close animation done: hand every window back to the real layout of the
 * active workspace, which at zoom 1 sits exactly where its card was. */
static void niri_finish(Monitor *m) {
	Client *c;

	niri_free(m);
	m->isoverview = 0;
	m->overview_input_blocked = false;
	m->workspace_viewport.presenting = false;
	m->ov_normal_mode = 0;
	fix_mon_tagset_from_overview(m);
	if (config.tag_gather) {
		if (config.tag_gather == 1)
			window_groups_destroy(m);
		tag_gather_apply(m);
	}
	pre_calculate_before_arrange(m, false, false, true);

	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m || c->iskilling)
			continue;
		c->niri_tag = 0;
		c->niri_move_running = false;
		overview_destroy_card(c);
		c->overview_scene_surface = NULL;
		c->animation.current = c->geom;
		c->animation.tagining = c->animation.tagouting = false;
		c->animation.running = false;
		c->tag_visible = VISIBLEON(c, m);
		c->is_clip_to_hide = false;
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		client_update_visibility(c);
		client_set_clip(c, 1.0f);
	}

	refresh_monitors_workspaces_status(m);
}

void niri_overview_destroy(Monitor *m) {
	if (!m || !m->niri_ov)
		return;
	m->niri_ov->closing = true;
	niri_finish(m);
}

static Client *niri_top_client(Monitor *m, uint32_t tag) {
	Client *c;

	wl_list_for_each(c, &server.focus_stack, flink) {
		if (niri_client_placed(c, m) && c->niri_tag == tag)
			return c;
	}
	return NULL;
}

static void niri_activate_tag(Monitor *m, uint32_t tag) {
	NiriOverview *ov = m->niri_ov;

	if (!tag_valid(tag))
		return;
	tag = monitor_resolve_tag(m, tag);
	if (ov->finish_idle) {
		wl_event_source_remove(ov->finish_idle);
		ov->finish_idle = NULL;
	}
	if (tag == ov->active_tag && tag == get_mon_curtag(m)) {
		niri_gather(m);
		return;
	}

	workspace_viewport_target(m, tag, true);
	ov->active_tag = tag;
	/* Native focus, unmap and commands share the active workspace. */
	m->tagset[m->seltags] = tag_bit(tag);
	m->pertag->prevtag = m->pertag->curtag;
	m->pertag->curtag = tag;
	niri_gather(m);
	tag = ov->active_tag;
	niri_update_slots(m);

	if (m == server.selected_monitor) {
		Client *selected =
			window_registry_get(window_group_for_tag(m, tag)->selected);
		client_focus(selected && selected->mon == m && selected->niri_tag == tag
						 ? selected
						 : niri_top_client(m, tag),
					 0);
	}
	if (config.tag_gather == 2)
		refresh_monitors_workspaces_status(m);
	request_fresh_all_monitors();
}

/* Bind ordinary commands to the selected tag's native group. */
NiriOverview *niri_overview_begin_edit(Monitor *m) {
	NiriOverview *ov = m ? m->niri_ov : NULL;
	if (!ov || ov->press_client)
		return NULL;
	ov->active_tag = get_mon_curtag(m);
	window_group_enter(window_group_for_tag(m, ov->active_tag),
					   &ov->edit_context);
	return ov;
}

void niri_overview_end_edit(NiriOverview *ov) {
	Monitor *m = ov->m;
	uint32_t mask = m->tagset[m->seltags] & TAGMASK;
	uint32_t active = mask ? get_tags_first_tag_num(mask) : ov->active_tag;
	arrange(m, false, false);
	uint32_t selected_id = m->sel ? m->sel->id : 0;
	window_group_leave(&ov->edit_context);
	Client *selected = window_registry_get(selected_id);
	Client *target =
		selected && VISIBLEON(selected, m) ? selected : client_focus_top(m);
	if (target != m->sel ||
		(!m->overview_input_blocked &&
		 server.seat->keyboard_state.focused_surface !=
			 (target ? client_surface(target) : NULL)))
		client_focus(target, 0);
	/* The edited native group was just arranged. Membership signatures find
	 * other affected groups without relaying out every compacted workspace. */
	if (config.tag_gather != 2)
		ov->dirty_tags = TAGMASK;
	if (config.tag_gather == 2) {
		/* Select the command's old-coordinate destination before gathering;
		 * the compaction callback then carries the selection with it. */
		niri_activate_tag(m, active);
		niri_overview_arrange(m);
	} else {
		niri_overview_arrange(m);
		niri_activate_tag(m, active);
	}
	niri_overview_draw_frame(m);
	request_fresh_all_monitors();
}

bool niri_overview_dispatch(int32_t (*command)(const Arg *), const Arg *arg) {
	if (!arg)
		return false;
	Monitor *m = arg->tc ? arg->tc->mon : server.selected_monitor;
	if (!m || !m->niri_ov)
		return false;
	/* Window and layout commands use native coordinates and visibility.
	 * Workspace navigation and session/presentation commands stay outside. */
	static int32_t (*const native_commands[])(const Arg *) = {
		exchange_client,
		move_client,
		exchange_stack_client,
		focus_direction,
		focus_stack,
		inc_nmaster,
		set_master_factor,
		move_window,
		resize_window,
		set_layout,
		set_proportion,
		switch_proportion_preset,
		smart_move_window,
		smart_resize_window,
		center_window,
		switch_layout,
		tag,
		tag_silent,
		toggle_fake_fullscreen,
		toggle_floating,
		toggle_fullscreen,
		toggle_gaps,
		toggle_maximize_screen,
		zoom,
		scroller_stack,
		toggle_all_floating,
		dwindle_toggle_split_direction,
		dwindle_split_horizontal,
		dwindle_split_vertical,
		dwindle_toggle_current_split,
		kill_client,
		smart_kill_client,
		minimize_window,
		restore_minimized,
		toggle_global,
		toggle_overlay,
		toggle_render_border,
		toggle_group_bar,
		reset_gaps,
		increase_gaps,
		increase_inner_gap,
		increase_inner_horizontal_gap,
		increase_inner_vertical_gap,
		increase_outer_gap,
		increase_outer_horizontal_gap,
		increase_outer_vertical_gap,
		focus_window_or_workspace,
		group_focus,
		group_join,
		group_leave,
	};
	bool native = false;
	for (size_t i = 0; i < LENGTH(native_commands); i++)
		native |= command == native_commands[i];
	if (!native)
		return false;
	/* A mouse binding can target a card on a neighbouring workspace. Run
	 * the native command with that workspace's layout and visibility. */
	if (niri_overview_active(m) && !m->niri_ov->press_client && arg->tc &&
		niri_client_placed(arg->tc, m))
		niri_activate_tag(m, arg->tc->niri_tag);
	NiriOverview *ov = niri_overview_begin_edit(m);
	if (!ov)
		return true; /* An interactive overview drag owns native edits. */
	command(arg);
	niri_overview_end_edit(ov);
	return true;
}

void niri_overview_open(Monitor *m) {
	NiriOverview *ov;

	if (!m || server.grab_client)
		return;

	if (m->niri_ov) {
		ov = m->niri_ov;
		if (ov->closing) {
			/* Reopened while closing: reverse the animation. */
			ov->closing = false;
			niri_block_input(m);
			if (ov->finish_idle) {
				wl_event_source_remove(ov->finish_idle);
				ov->finish_idle = NULL;
			}
			niri_anim_start(ov, &ov->open, 1.0, config.animation_duration_tag,
							OVERVIEW);
			request_fresh_all_monitors();
		}
		return;
	}

	if (m->isoverview)
		return;
	if (is_special_active(m))
		toggle_special_tag_mon(m);

	uint32_t curmask = m->tagset[m->seltags] & TAGMASK;
	uint32_t cur = curmask ? get_tags_first_tag_num(curmask) : 1;
	if (!tag_valid(cur))
		cur = 1;
	workspace_viewport_sync(m, false, false);

	ov = ecalloc(1, sizeof(*ov));
	ov->m = m;
	ov->active_tag = cur;
	ov->open_tag = curmask;
	ov->root = wlr_scene_tree_create(server.layers[LyrOverview]);
	ov->backdrop = wlr_scene_rect_create(ov->root, m->m.width, m->m.height,
										 config.overview_backdrop_color);
	ov->shadows = wlr_scene_tree_create(ov->root);
	ov->hint_tree = wlr_scene_tree_create(server.layers[LyrTop]);
	mango_scene_node_set(&ov->hint_tree->node, 0, NULL);
	mango_scene_node_set_ignore_hit(&ov->hint_tree->node, true);
	ov->hint = wlr_scene_rect_create(ov->hint_tree, 1, 1, config.dropcolor);
	wlr_scene_node_set_enabled(&ov->hint_tree->node, false);
	m->niri_ov = ov;

	m->isoverview = 1;
	window_group_for_tag(m, cur)->selected = m->sel ? m->sel->id : 0;
	m->ov_normal_mode = 0;
	m->ov_tab_layout = 0;
	m->ovbk_current_tagset = m->tagset[m->seltags];
	m->ovbk_prev_tagset = m->tagset[m->seltags ^ 1];

	/* Snapshot membership before the first arrange so entry can preserve
	 * existing presented geometry without hiding newly mapped clients. */
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m || client_is_unmanaged(c) || client_is_x11_popup(c) ||
			c->isunglobal || c->isminimized || (c->tags & TAG0_MASK) ||
			c->iskilling || !client_surface(c)->mapped)
			continue;
		uint32_t tag = window_group_client_tag(c);
		if (tag_valid(tag))
			ov->member_sig[tag] += niri_member_hash(c);
	}
	ov->dirty_tags = TAGMASK;
	niri_block_input(m);

	niri_overview_arrange(m);
	refresh_monitors_workspaces_status(m);

	niri_anim_start(ov, &ov->open, 1.0, config.animation_duration_tag,
					OVERVIEW);
	request_fresh_all_monitors();
}

void niri_overview_close(Monitor *m, uint32_t target_tag) {
	NiriOverview *ov = m ? m->niri_ov : NULL;

	if (!ov || ov->closing)
		return;

	if (ov->press_client && ov->dragging)
		niri_end_grab(m, ov->press_client);
	ov->press_client = NULL;
	ov->dragging = false;
	ov->dnd_slot_tag = 0;

	if (tag_valid(target_tag))
		niri_activate_tag(m, target_tag);
	ov->active_tag = get_mon_curtag(m);
	niri_gather(m);

	window_group_arrange(window_group_for_tag(m, ov->active_tag));
	ov->closing = true;
	niri_anim_start(ov, &ov->open, 0.0, config.animation_duration_tag,
					OVERVIEW);
	/* Closing relinquishes input now; the projection lives until zoom,
	 * camera travel and native window movement have all settled. */
	m->overview_input_blocked = false;
	if (m == server.selected_monitor) {
		Client *selected = m->sel;
		if (!selected || selected->iskilling ||
			!VISIBLEON(selected, m) || !client_surface(selected)->mapped)
			selected = window_registry_get(
				window_group_for_tag(m, ov->active_tag)->selected);
		client_focus(selected && VISIBLEON(selected, m)
						 ? selected
						 : client_focus_top(m),
					 0);
	}
	request_fresh_all_monitors();
}

void niri_overview_toggle(Monitor *m) {
	if (niri_overview_active(m))
		niri_overview_close(m, 0);
	else
		niri_overview_open(m);
}

bool niri_overview_focus_dir(Monitor *m, int dir) {
	NiriOverview *ov = m ? m->niri_ov : NULL;

	if (!ov)
		return false;
	int32_t idx = niri_slot_of(ov, ov->active_tag);
	int32_t next = idx + (dir < 0 ? -1 : 1);
	if (idx < 0 || next < 0 || next >= ov->nslots)
		return false;
	niri_activate_tag(m, ov->slots[next]);
	return true;
}

bool niri_overview_view_tag(Monitor *m, uint32_t tag) {
	NiriOverview *ov = m ? m->niri_ov : NULL;

	if (!ov || !tag_valid(tag))
		return false;
	niri_activate_tag(m, tag);
	return true;
}

static void niri_retag(Monitor *m, Client *c, uint32_t tag) {
	uint32_t old = c->niri_tag;

	niri_start_move(c);
	if (tag == old) {
		request_fresh_all_monitors();
		return;
	}
	/* Use the same tag move and layout bookkeeping as a normal Mango bind. */
	dispatch_action(tag_silent, &(Arg){.tc = c, .ui = tag_bit(tag)});
}

bool niri_overview_move_client_dir(Monitor *m, Client *c, int dir) {
	NiriOverview *ov = m ? m->niri_ov : NULL;

	if (!ov || !c || !niri_client_placed(c, m))
		return false;
	int32_t idx = niri_slot_of(ov, c->niri_tag);
	int32_t next = idx + (dir < 0 ? -1 : 1);
	if (idx < 0 || next < 0 || next >= ov->nslots)
		return false;
	uint32_t tag = ov->slots[next];
	niri_retag(m, c, tag);
	/* Native dispatch can compact as it leaves its edit context. */
	niri_activate_tag(m, config.tag_gather == 2 ? c->niri_tag : tag);
	if (m == server.selected_monitor)
		client_focus(c, 0);
	return true;
}

void niri_overview_client_gone(Client *c) {
	Monitor *m;

	wl_list_for_each(m, &server.monitors, link) {
		if (m->niri_ov && m->niri_ov->press_client == c) {
			m->niri_ov->press_client = NULL;
			m->niri_ov->dragging = false;
		}
	}
	c->niri_tag = 0;
	c->niri_preview_geom = c->niri_target_geom = (struct wlr_box){0};
	c->niri_target_tag = 0;
	c->niri_move_running = false;
}

/* ---- pointer input --------------------------------------------------------
 */

static NiriFrame niri_current_frame(Monitor *m) {
	NiriOverview *ov = m->niri_ov;
	return niri_frame(m, niri_anim_value(ov, &ov->open));
}

/* Slot whose workspace box contains (x, y), or -1. */
static int32_t niri_slot_at(Monitor *m, double x, double y) {
	NiriOverview *ov = m->niri_ov;
	NiriFrame f = niri_current_frame(m);

	for (int32_t i = 0; i < ov->nslots; i++) {
		struct wlr_box b = niri_slot_box(m, &f, i);
		if (x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height)
			return i;
	}
	return -1;
}

/* Drop target for a window at height y: either a workspace (slot index) or,
 * in the gap between two workspaces / above the first, a new workspace to
 * insert before slot *insert_at. */
static int32_t niri_drop_target(Monitor *m, double y, int32_t *insert_at) {
	NiriOverview *ov = m->niri_ov;
	NiriFrame f = niri_current_frame(m);

	*insert_at = -1;
	for (int32_t i = 0; i < ov->nslots; i++) {
		struct wlr_box b = niri_slot_box(m, &f, i);
		if (y < b.y) {
			*insert_at = i;
			return -1;
		}
		if (y < b.y + b.height)
			return i;
	}
	return ov->nslots - 1; /* below everything: the trailing empty one */
}

static uint32_t niri_occupied_tags(Monitor *m) {
	Client *c;
	uint32_t occupied = 0;

	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m)
			continue;
		occupied |= c->tags & TAGMASK;
		if (niri_client_placed(c, m))
			occupied |= tag_bit(c->niri_tag);
	}
	return occupied;
}

/* Renumber only from..empty-1. Preserve groups, layout trees and cached
 * workspace scenes; no window content is copied or recreated. */
static void niri_shift_tags(Monitor *m, uint32_t from, uint32_t empty) {
	NiriOverview *ov = m->niri_ov;
	uint32_t n = (uint32_t)config.tag_num;
	Client *c;

	view_insert_shift_tags_until(m, from, empty);

	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m)
			continue;
		if (c->niri_tag >= from && c->niri_tag < empty)
			c->niri_tag++;
		if (config.tag_gather == 2 && c->niri_target_tag >= from &&
			c->niri_target_tag < empty)
			c->niri_target_tag++;
	}
#define NIRI_SHIFT(v)                                                          \
	do {                                                                       \
		if ((v) >= from && (v) < empty)                                        \
			(v)++;                                                             \
	} while (0)
	NIRI_SHIFT(ov->active_tag);
	for (int32_t i = 0; i < ov->nslots; i++)
		NIRI_SHIFT(ov->slots[i]);
#undef NIRI_SHIFT

	uint32_t open = 0;
	for (uint32_t t = 1; t <= n; t++) {
		if (ov->open_tag & tag_bit(t))
			open |= tag_bit(t >= from && t < empty ? t + 1 : t);
	}
	ov->open_tag = open;

	WindowGroup *spare = m->window_groups[empty];
	if (spare) {
		spare->selected = 0;
		spare->tag = from;
	}
	niri_ws_destroy(ov->ws[empty]);
	for (uint32_t t = empty; t > from; t--) {
		m->window_groups[t] = m->window_groups[t - 1];
		if (m->window_groups[t])
			m->window_groups[t]->tag = t;
		ov->ws[t] = ov->ws[t - 1];
		if (ov->ws[t])
			ov->ws[t]->tag = t;
		ov->member_sig[t] = ov->member_sig[t - 1];
	}
	m->window_groups[from] = spare;
	ov->ws[from] = NULL;
	ov->member_sig[from] = 0;
	uint32_t map[tag_num_MAX + 1] = {0};
	for (uint32_t t = from; t < empty; t++)
		map[t] = t + 1;
	ov->dirty_tags = tag_remap_mask(ov->dirty_tags, map);
}

/* Resolve before committing so a full tag list never advertises an invalid
 * landing. The same plan is used by the highlight and the button release. */
static uint32_t niri_insert_target(Monitor *m, int32_t at, uint32_t *empty) {
	NiriOverview *ov = m->niri_ov;
	uint32_t next = ov->slots[at];
	uint32_t prev = at > 0 ? ov->slots[at - 1] : 0;
	uint32_t occupied = niri_occupied_tags(m);

	*empty = 0;
	if (prev + 1 < next && !(occupied & tag_bit(prev + 1)))
		return prev + 1;
	for (uint32_t t = next; t <= (uint32_t)config.tag_num; t++) {
		if (!(occupied & tag_bit(t))) {
			*empty = t;
			return next;
		}
	}
	return 0;
}

static uint32_t niri_insert_workspace(Monitor *m, int32_t at) {
	uint32_t empty;
	uint32_t tag = niri_insert_target(m, at, &empty);
	if (tag && empty > tag)
		niri_shift_tags(m, tag, empty);
	return tag;
}

static void niri_grab_coordinates(Monitor *m, uint32_t tag, double x, double y,
								  double *wx, double *wy) {
	NiriFrame f = niri_current_frame(m);
	double ox, oy;
	niri_slot_origin(m, &f, niri_slot_of(m->niri_ov, tag), &ox, &oy);
	*wx = m->m.x + (x - ox) / f.zoom;
	*wy = m->m.y + (y - oy) / f.zoom;
}

static void niri_clear_native_drop(void) {
	if (server.drop_client) {
		server.drop_client->enable_drop_area_draw = false;
		client_set_drop_area(server.drop_client);
		server.drop_client = NULL;
	}
	server.drop_to_group = false;
}

static void niri_draw_drop_hint(Monitor *m) {
	NiriOverview *ov = m->niri_ov;
	Client *c = ov->press_client;
	ov->drop_tag = 0;
	ov->drop_insert = false;
	if (!c || !ov->dragging || ov->resizing || ov->closing)
		goto hide;

	niri_clear_native_drop();
	double px = server.cursor->x, py = server.cursor->y;
	if (monitor_at_point(px, py) != m)
		goto hide;
	int32_t at;
	int32_t idx = niri_drop_target(m, py, &at);
	NiriFrame f = niri_current_frame(m);
	struct wlr_box box;
	uint32_t tag;
	if (at >= 0) {
		uint32_t empty;
		tag = niri_insert_target(m, at, &empty);
		if (!tag)
			goto hide;
		struct wlr_box ws = niri_slot_box(m, &f, at);
		double gap = f.stride - f.ws_h;
		/* Niri's gap hint: 75% of workspace width, 80% of gap height. */
		box = (struct wlr_box){
			.x = ws.x + lround(ws.width * .125),
			.y = ws.y - lround(gap * .9),
			.width = lround(ws.width * .75),
			.height = MANGO_MAX(2, (int32_t)lround(gap * .8)),
		};
		ov->drop_insert = true;
	} else {
		if (idx < 0)
			goto hide;
		tag = ov->slots[idx];
		niri_grab_coordinates(m, tag, px, py, &server.cursor->x,
							  &server.cursor->y);
		WindowGroupContext ctx;
		window_group_enter(window_group_for_tag(m, tag), &ctx);
		Client *target = c->drag_to_tile ? find_closest_tiled_client(c) : NULL;
		if (target) {
			/* Share native drop geometry without touching scene nodes or
			 * requesting another frame while the pointer is stationary. */
			struct wlr_box native;
			int32_t direction;
			if (!client_get_drop_area(target, server.cursor->x,
									  server.cursor->y, &native, &direction))
				native = (struct wlr_box){0};
			native.x += target->geom.x;
			native.y += target->geom.y;
			box = niri_map_box(m, &f, idx, native);
		} else if (c->drag_to_tile) {
			box = niri_map_box(m, &f, idx, m->w);
		} else {
			box = niri_map_box(m, &f, idx,
							   (struct wlr_box){
								   .x = lround(server.cursor->x -
											   ov->anchor_x * c->geom.width),
								   .y = lround(server.cursor->y -
											   ov->anchor_y * c->geom.height),
								   .width = c->geom.width,
								   .height = c->geom.height,
							   });
		}
		window_group_leave(&ctx);
		server.cursor->x = px;
		server.cursor->y = py;
	}
	ov->drop_tag = tag;
	ov->drop_box = box;
	if (ov->hint_tree->node.parent != c->scene->node.parent)
		wlr_scene_node_reparent(&ov->hint_tree->node, c->scene->node.parent);
	wlr_scene_node_place_below(&ov->hint_tree->node, &c->scene->node);
	wlr_scene_node_set_position(&ov->hint_tree->node, box.x, box.y);
	wlr_scene_rect_set_size(ov->hint, MANGO_MAX(1, box.width),
							MANGO_MAX(1, box.height));
	wlr_scene_rect_set_color(ov->hint, config.dropcolor);
	wlr_scene_node_set_enabled(&ov->hint_tree->node, true);
	return;
hide:
	wlr_scene_node_set_enabled(&ov->hint_tree->node, false);
}

static void niri_end_grab(Monitor *m, Client *c) {
	NiriOverview *ov = m->niri_ov;
	double px = server.cursor->x, py = server.cursor->y;
	uint32_t tag = c->niri_tag;
	int32_t insert_at;
	if (!ov->resizing) {
		int32_t idx = niri_drop_target(m, py, &insert_at);
		uint32_t target = insert_at >= 0 ? niri_insert_workspace(m, insert_at)
										 : (idx >= 0 ? ov->slots[idx] : 0);
		if (tag_valid(target))
			tag = target;
	}
	uint32_t source = c->niri_tag;
	/* A newly inserted workspace needs a slot before pointer conversion. */
	c->tags = tag_bit(tag);
	c->niri_tag = tag;
	client_reparent_group(c);
	niri_update_slots(m);
	niri_grab_coordinates(m, tag, px, py, &server.cursor->x, &server.cursor->y);
	WindowGroupContext ctx;
	window_group_enter(window_group_for_tag(m, tag), &ctx);
	if (!ov->resizing) {
		server.grab_offset_x = lround(ov->anchor_x * c->geom.width);
		server.grab_offset_y = lround(ov->anchor_y * c->geom.height);
	}
	pointer_update_grab();
	pointer_end_grab_client(true);
	window_group_leave(&ctx);
	server.cursor->x = px;
	server.cursor->y = py;
	ov->dirty_tags |= tag_bit(source) | tag_bit(tag);
	ov->drop_tag = 0;
	wlr_scene_node_set_enabled(&ov->hint_tree->node, false);
	niri_overview_arrange(m);
	client_focus(c, 0);
	niri_activate_tag(m, tag);
	niri_start_move(c);
}

static Monitor *niri_press_monitor(void) {
	Monitor *m;

	wl_list_for_each(m, &server.monitors, link) {
		if (m->niri_ov && m->overview_input_blocked &&
			m->niri_ov->press_client)
			return m;
	}
	return NULL;
}

bool niri_overview_get_drag(const Client *c, NiriOverviewDrag *state) {
	NiriOverview *ov = c && c->mon ? c->mon->niri_ov : NULL;
	if (!ov || ov->press_client != c)
		return false;
	*state = (NiriOverviewDrag){
		.detached = ov->dragging,
		.inserts_workspace = ov->drop_insert,
		.target_tag = ov->drop_tag,
		.highlight = ov->drop_box,
	};
	return true;
}

bool niri_overview_motion(Monitor *unused) {
	Monitor *m = niri_press_monitor();
	NiriOverview *ov = m ? m->niri_ov : NULL;
	if (!ov)
		return false;
	Client *c = ov->press_client;
	double px = server.cursor->x, py = server.cursor->y;
	double dx = px - ov->press_x, dy = py - ov->press_y;
	NiriFrame f = niri_current_frame(m);
	if (!ov->dragging) {
		double sq = (dx * dx + dy * dy) / (f.zoom * f.zoom);
		double threshold =
			ov->resizing ? NIRI_RESIZE_THRESHOLD : NIRI_DRAG_THRESHOLD;
		if (!ov->resizing) {
			/* Niri's RubberBand { stiffness: 1, limit: .5 }, applied to
			 * squared pointer distance. This is displacement, not a timer. */
			double force = sq / (threshold * threshold);
			double factor = .5 * (1.0 - 1.0 / (force * 2.0 + 1.0));
			ov->pull_x = dx * factor;
			ov->pull_y = dy * factor;
		}
		if ((!c->isfloating || ov->resizing) && sq < threshold * threshold) {
			niri_overview_draw_frame(m);
			request_fresh_all_monitors();
			return true;
		}
	}
	WindowGroupContext ctx;
	uint32_t tag = c->niri_tag;
	if (!ov->dragging) {
		struct wlr_box lifted =
			niri_card_target(m, &f, c, niri_slot_of(ov, tag));
		lifted.x += lround(ov->pull_x);
		lifted.y += lround(ov->pull_y);
		double wx, wy;
		niri_grab_coordinates(m, tag, ov->press_x, ov->press_y, &wx, &wy);
		window_group_enter(window_group_for_tag(m, tag), &ctx);
		server.cursor->x = wx;
		server.cursor->y = wy;
		int warp = config.drag_warp_cursor;
		int small = config.drag_tile_small;
		config.drag_warp_cursor = 0;
		config.drag_tile_small = 0;
		if (!ov->resizing) {
			if (c->isfullscreen)
				client_set_fullscreen(c, false, true);
			if (c->ismaximizescreen)
				client_set_maximize_screen(c, false, true);
		}
		ov->dragging = pointer_begin_move_resize(
			c, ov->resizing ? CurResize : CurMove, wx, wy, 0);
		config.drag_warp_cursor = warp;
		config.drag_tile_small = small;
		window_group_leave(&ctx);
		if (!ov->dragging) {
			server.cursor->x = px;
			server.cursor->y = py;
			return true;
		}
		ov->edge_since = 0;
		c->niri_move_running = false;
		if (!ov->resizing) {
			server.grab_offset_x = lround(ov->anchor_x * c->geom.width);
			server.grab_offset_y = lround(ov->anchor_y * c->geom.height);
			ov->pickup_x =
				lifted.x - (px - ov->anchor_x * c->geom.width * f.zoom);
			ov->pickup_y =
				lifted.y - (py - ov->anchor_y * c->geom.height * f.zoom);
			ov->pickup = (NiriAnim){.from = 1,
									.to = 0,
									.running = true,
									.started = get_now_in_ms(),
									.duration = config.animation_duration_move,
									.curve = MOVE};
		}
		wlr_scene_node_raise_to_top(&c->scene->node);
	}
	niri_grab_coordinates(m, tag, px, py, &server.cursor->x, &server.cursor->y);
	window_group_enter(window_group_for_tag(m, tag), &ctx);
	pointer_update_grab();
	window_group_leave(&ctx);
	server.cursor->x = px;
	server.cursor->y = py;
	niri_overview_draw_frame(m);
	request_fresh_all_monitors();
	return true;
}

/* Edge scrolling while dragging a window, and niri's DnD hold: hovering a
 * data-device drag over a workspace for a moment activates it. */
static bool niri_drag_tick(Monitor *m) {
	NiriOverview *ov = m->niri_ov;
	uint32_t now = get_now_in_ms();

	if (ov->dragging && ov->press_client && !ov->resizing) {
		double rel = (server.cursor->y - m->m.y) / (double)m->m.height;
		int dir = rel < 0.1 ? -1 : (rel > 0.9 ? 1 : 0);
		bool on_mon = monitor_at_point(server.cursor->x, server.cursor->y) == m;
		if (!dir || !on_mon) {
			ov->edge_since = 0;
		} else if (!ov->edge_since) {
			ov->edge_since = now;
			ov->edge_last = 0;
		} else if (now - ov->edge_since >= NIRI_EDGE_SCROLL_DELAY_MS &&
				   (!ov->edge_last ||
					now - ov->edge_last >= NIRI_EDGE_SCROLL_REPEAT_MS)) {
			ov->edge_last = now;
			niri_overview_focus_dir(m, dir);
		}
		return ov->edge_since != 0;
	}

	if (!server.seat->drag ||
		monitor_at_point(server.cursor->x, server.cursor->y) != m) {
		ov->dnd_slot_tag = 0;
		return false;
	}
	int32_t idx = niri_slot_at(m, server.cursor->x, server.cursor->y);
	uint32_t tag = idx >= 0 ? ov->slots[idx] : 0;
	if (tag != ov->dnd_slot_tag) {
		ov->dnd_slot_tag = tag;
		ov->dnd_since = now;
	} else if (tag && now - ov->dnd_since >= NIRI_DND_HOLD_MS) {
		ov->dnd_slot_tag = 0;
		niri_overview_close(m, tag);
		return true;
	}
	return tag != 0;
}

bool niri_overview_button(Monitor *m, uint32_t button, bool pressed) {
	Monitor *pm;
	NiriOverview *ov;

	if (!pressed) {
		pm = niri_press_monitor();
		if (!pm)
			return false;
		ov = pm->niri_ov;
		if (button != ov->press_button)
			return true;
		Client *c = ov->press_client;
		if (ov->dragging)
			niri_end_grab(pm, c);
		else {
			niri_activate_tag(pm, c->niri_tag);
			client_focus(c, 0);
			niri_start_move(c);
		}
		ov->press_client = NULL;
		ov->dragging = false;
		ov->pull_x = ov->pull_y = 0;
		server.cursor_mode = CurNormal;
		niri_gather(pm);
		niri_overview_draw_frame(pm);
		request_fresh_all_monitors();
		return true;
	}

	ov = m ? m->niri_ov : NULL;
	if (!ov || !m->overview_input_blocked)
		return false;

	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	LayerSurface *l = NULL;
	node_at_point(server.cursor->x, server.cursor->y, &surface, &c, &l, NULL,
				  NULL, NULL);
	/* The top and overlay layers (bars) stay interactive above the overview. */
	if (l && l->layer_surface &&
		l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
		return false;

	bool move = false, resizing = false;
	uint32_t mods = keyboard_hard_modifiers();
	if (c && niri_client_placed(c, m)) {
		for (int32_t i = 0; i < config.mouse_bindings_count; i++) {
			const MouseBinding *binding = &config.mouse_bindings[i];
			if (!(binding->iscommonmode ||
				  (binding->isdefaultmode && server.key_mode.isdefault) ||
				  strcmp(server.key_mode.mode, binding->mode) == 0) ||
				CLEANMASK(mods) != CLEANMASK(binding->mod) ||
				button != binding->button || !binding->func)
				continue;
			client_focus(c, 0);
			if (binding->func == move_resize) {
				move = true;
				resizing = binding->arg.ui == CurResize;
				break;
			}
			Arg arg = binding->arg;
			arg.tc = c;
			dispatch_action(binding->func, &arg);
			return true;
		}
	}
	if (button != BTN_LEFT && !move) {
		if (button == BTN_RIGHT && c && niri_client_placed(c, m))
			pending_kill_client(c);
		return true;
	}

	if (c && niri_client_placed(c, c->mon) && c->mon->niri_ov == ov) {
		client_focus(c, 0);
		ov->press_client = c;
		ov->press_button = button;
		ov->dragging = false;
		ov->resizing = resizing;
		ov->press_x = server.cursor->x;
		ov->press_y = server.cursor->y;
		ov->pull_x = ov->pull_y = 0;
		struct wlr_box b = c->niri_preview_geom;
		ov->anchor_x =
			CLAMP((ov->press_x - b.x) / MANGO_MAX(1, b.width), 0.0, 1.0);
		ov->anchor_y =
			CLAMP((ov->press_y - b.y) / MANGO_MAX(1, b.height), 0.0, 1.0);
		return true;
	}

	/* niri: clicking a workspace switches to it and leaves the overview. */
	int32_t idx = niri_slot_at(m, server.cursor->x, server.cursor->y);
	if (idx >= 0)
		niri_overview_close(m, ov->slots[idx]);
	return true;
}

/* niri: in the overview the scroll wheel switches workspaces without a
 * modifier; touchpad scrolling does the same in steps. */
bool niri_overview_axis(Monitor *m, bool vertical, double delta,
						int32_t delta_discrete, uint32_t time_msec) {
	NiriOverview *ov = m ? m->niri_ov : NULL;

	if (!ov || !m->overview_input_blocked)
		return false;
	if (!vertical)
		return true;

	if (time_msec - ov->scroll_time > 300)
		ov->scroll_acc = 0.0;
	ov->scroll_time = time_msec;

	double step;
	if (delta_discrete) {
		ov->scroll_acc += delta_discrete / 120.0;
		step = 1.0;
	} else {
		ov->scroll_acc += delta;
		step = NIRI_TOUCHPAD_SCROLL_STEP;
	}
	while (ov->scroll_acc >= step) {
		ov->scroll_acc -= step;
		niri_overview_focus_dir(m, 1);
	}
	while (ov->scroll_acc <= -step) {
		ov->scroll_acc += step;
		niri_overview_focus_dir(m, -1);
	}
	return true;
}

/* ---- trackpad gesture drive ------------------------------------------------
 */

bool niri_overview_animating(const Monitor *m) {
	const NiriOverview *ov = m ? m->niri_ov : NULL;
	return (m && m->workspace_viewport.running) || (ov && ov->open.running);
}

void niri_overview_set_progress(Monitor *m, double p) {
	NiriOverview *ov = m ? m->niri_ov : NULL;
	workspace_viewport_set_progress(m, p);
	if (!ov)
		return;
	ov->gesture_p = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
	request_fresh_all_monitors();
}

void niri_overview_set_pan(Monitor *m, double x, double y) {
	NiriOverview *ov = m ? m->niri_ov : NULL;
	if (!ov)
		return;
	if ((ov->pan_x || ov->pan_y) && !x && !y) {
		Client *c;
		wl_list_for_each(c, &server.clients, link) {
			if (niri_client_placed(c, m) && VISIBLEON(c, m) && ISSCROLLTILED(c))
				niri_start_move(c);
		}
	}
	ov->pan_x = x;
	ov->pan_y = y;
	request_fresh_all_monitors();
}

void niri_overview_resume(Monitor *m, double remaining) {
	NiriOverview *ov = m ? m->niri_ov : NULL;
	workspace_viewport_resume(m, remaining);
	if (!ov)
		return;
	niri_anim_resume(ov, &ov->open, remaining);
	niri_gather(m);
	request_fresh_all_monitors();
}
