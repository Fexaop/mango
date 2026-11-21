#include "mango/manage/window-group.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/dispatch/bind.h"
#include "mango/layout/arrange.h"
#include "mango/layout/layout.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include <stdlib.h>
#include <string.h>

static Client **registry;
static size_t registry_size;

void window_registry_add(Client *c) {
	if (c->id >= registry_size) {
		size_t size = registry_size ? registry_size : 64;
		while (c->id >= size)
			size *= 2;
		Client **entries = realloc(registry, size * sizeof(*entries));
		if (!entries)
			die("allocating window registry");
		memset(entries + registry_size, 0,
			   (size - registry_size) * sizeof(*entries));
		registry = entries;
		registry_size = size;
	}
	registry[c->id] = c;
}

void window_registry_remove(Client *c) {
	if (c->id < registry_size && registry[c->id] == c)
		registry[c->id] = NULL;
}

Client *window_registry_get(uint32_t id) {
	Client *c = id < registry_size ? registry[id] : NULL;
	return c && !c->iskilling ? c : NULL;
}

bool window_group_visible(const Client *c, const Monitor *m) {
	if (!c || !m || c->mon != m || c->isminimized)
		return false;
	if (m->layout_group && c->niri_tag)
		return window_group_client_tag(c) == get_mon_curtag(m);
	return (c->tags & m->tagset[m->seltags]) || c->isglobal || c->isunglobal;
}

uint32_t window_group_client_tag(const Client *c) {
	/* Preview placement is a cache of native membership, never its owner. */
	if ((c->mon->niri_ov || c->mon->layout_group) && c->niri_tag &&
		(c->isglobal || (c->tags & (1u << (c->niri_tag - 1)))))
		return c->niri_tag;
	return c->isglobal ? get_mon_curtag(c->mon) : get_client_tag_idx(c);
}

WindowGroup *window_group_for_tag(Monitor *m, uint32_t tag) {
	if (!m || tag > (uint32_t)config.tag_num)
		return NULL;
	if (!m->window_groups[tag]) {
		WindowGroup *group = ecalloc(1, sizeof(*group));
		group->mon = m;
		group->tag = tag;
		m->window_groups[tag] = group;
	}
	return m->window_groups[tag];
}

WindowGroup *window_group_for_client(Client *c) {
	if (!c || !c->mon)
		return NULL;
	Monitor *m = c->mon;
	if (m->layout_group && VISIBLEON(c, m))
		return m->layout_group;
	if (m->niri_ov)
		return window_group_for_tag(m, window_group_client_tag(c));
	return window_group_for_tag(m, VISIBLEON(c, m) ? get_mon_curtag(m)
												   : get_client_tag_idx(c));
}

const Layout *window_group_layout(const WindowGroup *group) {
	return group->mon->pertag->ltidxs[group->tag];
}

void window_group_enter(WindowGroup *group, WindowGroupContext *ctx) {
	Monitor *m = group->mon;
	*ctx = (WindowGroupContext){
		.mon = m,
		.group = m->layout_group,
		.overview = m->niri_ov,
		.seltags = m->seltags,
		.curtag = m->pertag->curtag,
		.prevtag = m->pertag->prevtag,
		.selected = m->sel ? m->sel->id : 0,
		.previous = m->prevsel ? m->prevsel->id : 0,
		.isoverview = m->isoverview,
		.tag_gather = config.tag_gather,
		.warpcursor = config.warpcursor,
	};
	memcpy(ctx->tagset, m->tagset, sizeof(ctx->tagset));
	m->layout_group = group;
	m->tagset[m->seltags] = group->tag ? 1u << (group->tag - 1) : TAG0_MASK;
	m->pertag->curtag = group->tag;
	m->niri_ov = NULL;
	m->isoverview = 0;
	config.tag_gather = 0;
	config.warpcursor = 0;
	Client *selected = window_registry_get(group->selected);
	if (selected && VISIBLEON(selected, m))
		m->sel = selected;
	else if (!VISIBLEON(m->sel, m))
		m->sel = client_focus_top(m);
}

void window_group_leave(WindowGroupContext *ctx) {
	Monitor *m = ctx->mon;
	if (m->layout_group)
		window_group_for_tag(m, get_mon_curtag(m))->selected =
			VISIBLEON(m->sel, m) ? m->sel->id : 0;
	memcpy(m->tagset, ctx->tagset, sizeof(ctx->tagset));
	m->seltags = ctx->seltags;
	m->pertag->curtag = ctx->curtag;
	m->pertag->prevtag = ctx->prevtag;
	m->niri_ov = ctx->overview;
	m->isoverview = ctx->isoverview;
	m->layout_group = ctx->group;
	Client *selected = window_registry_get(ctx->selected);
	Client *previous = window_registry_get(ctx->previous);
	m->sel = VISIBLEON(selected, m) ? selected : NULL;
	m->prevsel = previous && previous->mon == m ? previous : NULL;
	config.tag_gather = ctx->tag_gather;
	config.warpcursor = ctx->warpcursor;
}

void window_group_arrange(WindowGroup *group) {
	WindowGroupContext ctx;
	window_group_enter(group, &ctx);
	arrange(group->mon, false, false);
	window_group_leave(&ctx);
}

/* Logical edges are resolved from native geometry on demand. Coincident
 * windows form a stack in client order, independent of the chosen layout. */
Client *window_group_neighbour(Client *c, int dir) {
	if (!c || !c->mon || dir < UP || dir > RIGHT)
		return NULL;
	Client *next = find_client_by_direction(c, &(Arg){.i = dir}, true);
	if (next)
		return next;
	next = get_next_stack_client(c, dir == LEFT || dir == UP);
	return next && wlr_box_equal(&c->geom, &next->geom) ? next : NULL;
}

void window_group_exchange(Client *a, Client *b) {
	if (!a || !b || !a->mon || !b->mon || a == b ||
		(!config.exchange_cross_monitor && a->mon != b->mon))
		return;
	const Layout *first = window_group_layout(window_group_for_client(a));
	const Layout *second = window_group_layout(window_group_for_client(b));
	if (first->exchange &&
		(first->exchange == second->exchange || first->exchange_mixed))
		first->exchange(a, b);
	else if (second->exchange && second->exchange_mixed)
		second->exchange(a, b);
	else
		client_exchange_default(a, b);
}

void window_groups_destroy(Monitor *m) {
	for (size_t i = 0; i < PERTAG_SLOTS; i++) {
		if (m->window_groups[i]) {
			free(m->window_groups[i]);
			m->window_groups[i] = NULL;
		}
	}
}
