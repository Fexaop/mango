#include "mango/animation/tag.h"
#include "mango/animation/client.h"
#include "mango/animation/common.h"
#include "mango/common/scene_node.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include <math.h>
#include <wlr/types/wlr_compositor.h>

static bool ordinary_tagset(uint32_t tags) {
	return tags && !(tags & ~TAGMASK) && !(tags & (tags - 1));
}

static double viewport_progress(const Monitor *m) {
	const WorkspaceViewport *v = &m->workspace_viewport;
	if (!v->running)
		return 1.0;
	if (v->gesture)
		return v->progress;
	if (!config.animations || !v->duration)
		return 1.0;
	double p = (double)(get_now_in_ms() - v->started) / v->duration;
	return p >= 1.0 ? 1.0 : find_animation_curve_at(p, TAG);
}

double workspace_viewport_position(const Monitor *m) {
	const WorkspaceViewport *v = &m->workspace_viewport;
	return v->from + (v->to - v->from) * viewport_progress(m);
}

void workspace_viewport_target(Monitor *m, uint32_t tag, bool animate) {
	WorkspaceViewport *v = &m->workspace_viewport;
	if (!tag || tag > (uint32_t)config.tag_num)
		return;
	if (!v->initialized) {
		v->initialized = true;
		v->from = v->to = tag - 1.0;
		v->source_tag = v->target_tag = tag;
	}
	if (v->target_tag == tag)
		return;
	double current = workspace_viewport_position(m);
	v->source_tag = v->target_tag;
	v->target_tag = tag;
	v->from = current;
	v->to = tag - 1.0;
	if (!v->running)
		v->direct_span = MANGO_MAX(1.0, fabs(v->to - current));
	v->presenting = true;
	if (animate)
		v->started = get_now_in_ms();
	v->duration = animate ? config.animation_duration_tag : 0;
	v->progress = 0.0;
	v->gesture = animate && server.gesture_drive_active &&
		server.gesture_drive_mon == m;
	v->running = v->gesture || (config.animations && v->duration && current != v->to);
	if (!v->running) {
		v->from = v->to;
		v->travel_tags = 0;
	} else {
		for (uint32_t t = MANGO_MIN(v->source_tag, tag);
			 t <= MANGO_MAX(v->source_tag, tag); t++)
			v->travel_tags |= 1u << (t - 1);
	}
	request_fresh_all_monitors();
}

void workspace_viewport_sync(Monitor *m, bool animate, bool from_view) {
	if (!m || m->layout_group || m->niri_ov || m->isoverview)
		return;
	WorkspaceViewport *v = &m->workspace_viewport;
	uint32_t tags = m->tagset[m->seltags];
	if (!ordinary_tagset(tags) || m->carousel_anim_dir ||
		(from_view && !ordinary_tagset(m->tagset[m->seltags ^ 1]))) {
		v->running = v->presenting = false;
		v->initialized = false;
		v->travel_tags = 0;
		return;
	}
	/* Relayouts never own an in-flight switch. Native tag renumbering must
	 * remap its coordinates, preserving the original completion deadline. */
	if (!from_view && v->running)
		return;
	uint32_t tag = get_tags_first_tag_num(tags);
	if (!v->initialized && from_view && ordinary_tagset(m->tagset[m->seltags ^ 1]))
		workspace_viewport_target(m, get_tags_first_tag_num(m->tagset[m->seltags ^ 1]), false);
	workspace_viewport_target(m, tag, animate && from_view);
	if (from_view && !animate) {
		v->running = v->gesture = false;
		v->from = v->to;
		v->travel_tags = 0;
	}
}

void workspace_viewport_set_progress(Monitor *m, double progress) {
	if (m && m->workspace_viewport.gesture)
		m->workspace_viewport.progress = CLAMP(progress, 0.0, 1.0);
}

void workspace_viewport_resume(Monitor *m, double remaining) {
	WorkspaceViewport *v = m ? &m->workspace_viewport : NULL;
	if (!v || !v->gesture)
		return;
	v->from = workspace_viewport_position(m);
	v->gesture = false;
	v->duration = remaining > 0.0 ? v->duration * remaining : 0;
	v->started = get_now_in_ms();
	if (!config.animations || !v->duration) {
		v->running = false;
		v->from = v->to;
	}
}

bool workspace_viewport_tick(Monitor *m) {
	WorkspaceViewport *v = &m->workspace_viewport;
	if (v->gesture && (!server.gesture_drive_active || server.gesture_drive_mon != m))
		workspace_viewport_resume(m, 1.0 - v->progress);
	if (v->running && !v->gesture && (!config.animations || !v->duration ||
		get_now_in_ms() - v->started >= v->duration)) {
		v->running = false;
		v->from = v->to;
		v->travel_tags = 0;
	}
	return v->running && !v->gesture;
}

static double viewport_remap_position(double position, const uint32_t *map) {
	int32_t base = CLAMP((int32_t)floor(position), 0, config.tag_num - 1);
	double fraction = position - base;
	uint32_t a = map[base + 1] ? map[base + 1] : base + 1;
	uint32_t b = base + 2 <= config.tag_num
		? (map[base + 2] ? map[base + 2] : base + 2) : a + 1;
	return a - 1.0 + fraction * ((double)b - a);
}

void workspace_viewport_remap(Monitor *m, const uint32_t *map) {
	WorkspaceViewport *v = &m->workspace_viewport;
	if (!v->initialized)
		return;
	/* Transform the coordinate system, not the clock: compaction cannot
	 * extend the original completion deadline. */
	double progress = viewport_progress(m);
	double position = v->from + (v->to - v->from) * progress;
	double mapped = viewport_remap_position(position, map);
	v->to = viewport_remap_position(v->to, map);
	/* Rebasing endpoints at the existing eased progress also handles
	 * nonlinear maps (several empty workspaces collapsing into one). */
	v->from = progress < 1.0
		? (mapped - v->to * progress) / (1.0 - progress) : v->to;
	if (map[v->source_tag])
		v->source_tag = map[v->source_tag];
	if (map[v->target_tag])
		v->target_tag = map[v->target_tag];
	uint32_t travel = 0;
	for (uint32_t t = 1; t <= (uint32_t)config.tag_num; t++)
		if ((v->travel_tags & (1u << (t - 1))) && map[t])
			travel |= 1u << (map[t] - 1);
	v->travel_tags = travel;
}

/* Camera ownership survives its final frame; ordinary relayouts must not
 * fall back to per-client slides using historical prevtag state. */
static bool viewport_owns_client(Client *c) {
	return c && c->mon && !c->mon->layout_group && !c->mon->niri_ov &&
		!c->mon->isoverview && c->mon->workspace_viewport.initialized &&
		ordinary_tagset(c->mon->tagset[c->mon->seltags]) &&
		ordinary_tagset(c->tags) && !c->isglobal && !c->isunglobal &&
		!c->is_in_scratchpad && !c->isminimized && !c->iskilling &&
		!client_is_unmanaged(c) && !client_is_x11_popup(c);
}

bool workspace_viewport_client(Client *c) {
	return viewport_owns_client(c) &&
		(c->mon->workspace_viewport.running || c->mon->workspace_viewport.presenting);
}

bool workspace_viewport_draw_frame(Monitor *m) {
	WorkspaceViewport *v = &m->workspace_viewport;
	bool more = workspace_viewport_tick(m);
	if (m->niri_ov || m->isoverview || (!v->running && !v->presenting))
		return more;
	v->presenting = true;
	double camera = workspace_viewport_position(m);
	double scale = config.tag_animation_continuous || !v->running
					   ? 1.0
					   : 1.0 / v->direct_span;
	Client *c;
	wl_list_for_each(c, &server.clients, link) {
		if (c->mon != m || !workspace_viewport_client(c) || !client_surface(c)->mapped)
			continue;
		uint32_t tag = get_tags_first_tag_num(c->tags);
		bool on_path = config.tag_animation_continuous
			? (v->travel_tags & c->tags) != 0
			: tag == v->source_tag || tag == v->target_tag;
		c->animation.tagining = c->animation.tagouting = false;
		c->animation.tagouted = !v->running && !VISIBLEON(c, m);
		c->animation.running = c->animation.should_animate = false;
		c->need_output_flush = false;
		c->animation.current = c->geom;
		if (v->running && on_path && client_animations_enabled(c)) {
			double offset = (tag - 1.0 - camera) * scale;
			if (config.tag_animation_direction == VERTICAL)
				c->animation.current.y += lround(offset * m->m.height);
			else
				c->animation.current.x += lround(offset * m->m.width);
		}
		c->is_clip_to_hide = false;
		wlr_scene_node_set_position(&c->scene->node,
			c->animation.current.x, c->animation.current.y);
		client_set_clip(c, 1.0f);
		client_update_visibility(c);
	}
	v->presenting = v->running;
	return more;
}


static void set_tagin_animation(Monitor *m, Client *c) {
	if (c->animation.running) {
		c->animainit_geom.x = c->animation.current.x;
		c->animainit_geom.y = c->animation.current.y;
		return;
	}

	if ((c->isglobal || c->isunglobal) ||
		(c->tags & (1 << (m->pertag->prevtag - 1)) &&
		 c->tags & (1 << (m->pertag->curtag - 1)))) {
		c->animation.tagouting = false;
		c->animation.tagouted = false;
		c->animation.tagining = false;
		c->animation.action = MOVE;
		return;
	}


	bool going_forward = m->carousel_anim_dir
							 ? m->carousel_anim_dir > 0
							 : m->pertag->curtag > m->pertag->prevtag;

	if (going_forward) {

		c->animainit_geom.x = config.tag_animation_direction == VERTICAL
								  ? c->animation.current.x
								  : MANGO_MAX(c->mon->m.x + c->mon->m.width,
											  c->geom.x + c->mon->m.width);
		c->animainit_geom.y = config.tag_animation_direction == VERTICAL
								  ? MANGO_MAX(c->mon->m.y + c->mon->m.height,
											  c->geom.y + c->mon->m.height)
								  : c->animation.current.y;

	} else {

		c->animainit_geom.x = config.tag_animation_direction == VERTICAL
								  ? c->animation.current.x
								  : MANGO_MIN(m->m.x - c->geom.width,
											  c->geom.x - c->mon->m.width);
		c->animainit_geom.y = config.tag_animation_direction == VERTICAL
								  ? MANGO_MIN(m->m.y - c->geom.height,
											  c->geom.y - c->mon->m.height)
								  : c->animation.current.y;
	}
}

void set_arrange_visible(Monitor *m, Client *c, bool want_animation,
						 bool from_view) {
	c->tag_visible = true;
	if (workspace_viewport_client(c)) {
		c->animation.tagining = c->animation.tagouting = false;
		c->animation.tagouted = false;
		c->animainit_geom = c->geom;
		resize(c, c->geom, (ResizeOpts){.skip_ov_enter_anim = true});
		return;
	}

	bool was_enabled = c->scene->node.enabled;
	bool in_place = was_enabled && !c->animation.running &&
					wlr_box_equal(&c->animation.current, &c->geom);
	bool tag_switch = from_view && !viewport_owns_client(c) && !in_place &&
					  !c->animation.tag_from_rule && want_animation &&
					  m->pertag->prevtag != 0 && m->pertag->curtag != 0 &&
					  client_animations_enabled(c);

	if (!ISTILED(c) || (!c->is_clip_to_hide || !is_scroller_layout(c->mon))) {
		c->is_clip_to_hide = false;
		client_update_visibility(c);
	}

	/* Scratchpad clients slide in from above the monitor when shown */
	if (SCRATCHPAD_SHOWN(c) && c->scratchpad_tagin) {
		c->scratchpad_tagin = false;
		c->animation.tag_from_rule = false;
		c->animation.tagouted = false;
		/* Reverse an in-flight hide instead of restarting from the top. */
		bool reversing =
			c->animation.tagouting && c->animation.running &&
			!wlr_box_equal(&c->animation.current, &c->animation.initial);
		c->animation.tagouting = false;
		if (client_animations_enabled(c)) {
			c->animation.tagining = true;
			c->animainit_geom = c->geom;
			if (reversing) {
				c->animainit_geom.x = c->animation.current.x;
				c->animainit_geom.y = c->animation.current.y;
			} else {
				c->animainit_geom.y = c->mon->m.y - c->geom.height;
			}
		} else {
			c->animation.tagining = false;
			c->animainit_geom.x = c->animation.current.x;
			c->animainit_geom.y = c->animation.current.y;
		}
		resize(c, c->geom, (ResizeOpts){.skip_ov_enter_anim = true});
		return;
	}

	/* Special workspace clients slide in from above the monitor */
	if (c->tags & TAG0_MASK) {
		c->animation.tag_from_rule = false;
		c->animation.tagouting = false;
		c->animation.tagouted = false;
		if (want_animation && client_animations_enabled(c)) {
			c->animation.tagining = true;
			c->animainit_geom = c->geom;
			c->animainit_geom.y = c->mon->m.y - c->geom.height;
		} else {
			c->animainit_geom.x = c->animation.current.x;
			c->animainit_geom.y = c->animation.current.y;
		}
		resize(c, c->geom, (ResizeOpts){.skip_ov_enter_anim = true});
		return;
	}

	if (tag_switch) {
		c->animation.tagining = true;
		set_tagin_animation(m, c);
	} else {
		c->animainit_geom.x = c->animation.current.x;
		c->animainit_geom.y = c->animation.current.y;
	}

	c->animation.tag_from_rule = false;
	c->animation.tagouting = false;
	c->animation.tagouted = false;
	resize(c, c->geom, (ResizeOpts){.skip_ov_enter_anim = true});
}

static void set_tagout_animation(Monitor *m, Client *c) {
	if ((c->isglobal || c->isunglobal) ||
		(c->tags & (1 << (m->pertag->prevtag - 1)) &&
		 c->tags & (1 << (m->pertag->curtag - 1)))) {
		c->animation.tagouting = false;
		c->animation.tagouted = false;
		c->animation.tagining = false;
		c->animation.action = MOVE;
		return;
	}


	bool going_forward = m->carousel_anim_dir
							 ? m->carousel_anim_dir > 0
							 : m->pertag->curtag > m->pertag->prevtag;
	if (going_forward) {
		c->pending = c->geom;
		c->pending.x = config.tag_animation_direction == VERTICAL
						   ? c->animation.current.x
						   : MANGO_MIN(c->mon->m.x - c->geom.width,
									   c->geom.x - c->mon->m.width);
		c->pending.y = config.tag_animation_direction == VERTICAL
						   ? MANGO_MIN(c->mon->m.y - c->geom.height,
									   c->geom.y - c->mon->m.height)
						   : c->animation.current.y;

		resize(c, c->geom, (ResizeOpts){.interact = 0});
	} else {
		c->pending = c->geom;
		c->pending.x = config.tag_animation_direction == VERTICAL
						   ? c->animation.current.x
						   : MANGO_MAX(c->mon->m.x + c->mon->m.width,
									   c->geom.x + c->mon->m.width);
		c->pending.y = config.tag_animation_direction == VERTICAL
						   ? MANGO_MAX(c->mon->m.y + c->mon->m.height,
									   c->geom.y + c->mon->m.height)
						   : c->animation.current.y;
		resize(c, c->geom, (ResizeOpts){.interact = 0});
	}
}
void set_arrange_hidden(Monitor *m, Client *c, bool want_animation,
						bool from_view) {
	c->tag_visible = false;
	if (workspace_viewport_client(c)) {
		c->animation.running = false;
		c->animation.tagining = c->animation.tagouting = false;
		c->animation.tagouted = false;
		return;
	}

	/* In overview every tag window must show its card and must not be disabled
	 * by the hiding logic. */
	if (c->ov_card_tree) {
		c->is_clip_to_hide = false;
		client_update_visibility(c);
		c->animation.running = false;
		c->animation.tagining = false;
		c->animation.tagouting = false;
		return;
	}

	/* Scratchpad windows slide up and out when hidden */
	if (!(c->tags & TAG0_MASK) && c->is_in_scratchpad && c->isminimized) {
		if (client_animations_enabled(c) && !c->animation.tagouted) {
			c->animation.tagouting = true;
			c->animation.tagining = false;
			c->pending = c->geom;
			c->pending.y = c->mon->m.y - c->geom.height;
			resize(c, c->geom, (ResizeOpts){.interact = 0});
		} else {
			c->animation.running = false;
			c->animation.tagouting = false;
			c->animation.tagining = false;
			client_update_visibility(c);
			c->animainit_geom = c->current = c->pending = c->animation.current =
				c->geom;
		}
		return;
	}

	/* Special workspace windows should animate out or hide when special
	 * workspace is not active */
	if (c->tags & TAG0_MASK) {
		if (want_animation && !m->isoverview && client_animations_enabled(c) &&
			!c->animation.tagouted) {
			c->animation.tagouting = true;
			c->animation.tagining = false;
			c->pending = c->geom;
			c->pending.y = c->mon->m.y - c->geom.height;
			resize(c, c->geom, (ResizeOpts){.interact = 0});
		} else {
			c->animation.running = false;
			c->animation.tagouting = false;
			c->animation.tagining = false;
			client_update_visibility(c);
			c->animainit_geom = c->current = c->pending = c->animation.current =
				c->geom;
		}
		return;
	}


	if (from_view && want_animation && !viewport_owns_client(c) &&
		!c->is_tab_hidden && !c->animation.tagouted &&
		m->pertag->prevtag != 0 && m->pertag->curtag != 0 &&
		(c->tags & (1u << (m->pertag->prevtag - 1))) &&
		client_animations_enabled(c)) {
		c->animation.tagouting = true;
		c->animation.tagining = false;
		set_tagout_animation(m, c);
	} else {
		c->animation.running = false;
		c->animation.tagining = false;
		c->animation.tagouting = false;
		c->animation.tagouted = true;
		client_update_visibility(c);
		c->animainit_geom = c->current = c->pending = c->animation.current =
			c->geom;
	}
}
