#ifndef MANGO_WINDOW_GROUP_H
#define MANGO_WINDOW_GROUP_H

#include "mango/common/types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Tags and overview workspaces reference the same group and native layout. */
struct WindowGroup {
	Monitor *mon;
	uint32_t tag;
	uint32_t selected;
};

typedef struct {
	Monitor *mon;
	WindowGroup *group;
	NiriOverview *overview;
	uint32_t tagset[2], seltags, curtag, prevtag;
	uint32_t selected, previous;
	int32_t isoverview, tag_gather, warpcursor;
} WindowGroupContext;

void window_registry_add(Client *c);
void window_registry_remove(Client *c);
Client *window_registry_get(uint32_t id);
bool window_group_visible(const Client *c, const Monitor *m);
uint32_t window_group_client_tag(const Client *c);
WindowGroup *window_group_for_tag(Monitor *m, uint32_t tag);
WindowGroup *window_group_for_client(Client *c);
const Layout *window_group_layout(const WindowGroup *group);
void window_group_enter(WindowGroup *group, WindowGroupContext *ctx);
void window_group_leave(WindowGroupContext *ctx);
void window_group_arrange(WindowGroup *group);
Client *window_group_neighbour(Client *c, int direction);
void window_group_exchange(Client *a, Client *b);
void window_groups_destroy(Monitor *m);

#endif
