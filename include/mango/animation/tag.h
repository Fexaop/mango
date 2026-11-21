#ifndef __ANIMATION_TAG_H__
#define __ANIMATION_TAG_H__ 1

#include "mango/common/types.h"
#include <stdbool.h>
#include <stdint.h>

/* Relayouts preserve active switches; coordinate relabels use remap. */
void workspace_viewport_sync(Monitor *m, bool animate, bool from_view);
void workspace_viewport_target(Monitor *m, uint32_t tag, bool animate);
double workspace_viewport_position(const Monitor *m);
bool workspace_viewport_tick(Monitor *m);
bool workspace_viewport_client(Client *c);
bool workspace_viewport_draw_frame(Monitor *m);
void workspace_viewport_remap(Monitor *m, const uint32_t *map);
void workspace_viewport_set_progress(Monitor *m, double progress);
void workspace_viewport_resume(Monitor *m, double remaining);
void set_arrange_visible(Monitor *m, Client *c, bool want_animation,
						 bool from_view);
void set_arrange_hidden(Monitor *m, Client *c, bool want_animation,
						bool from_view);

#endif
