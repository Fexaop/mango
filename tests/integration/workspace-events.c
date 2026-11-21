/* Observe actual ext-workspace identities and state for integration regressions. */
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "workspace-client-protocol.h"

struct entry {
	struct ext_workspace_handle_v1 *handle;
	char name[64];
	uint32_t state;
	int32_t position;
	bool removed;
};
static struct entry entries[256];
static size_t count;
static struct ext_workspace_manager_v1 *manager;

static void workspace_id(void *data, struct ext_workspace_handle_v1 *w,
						 const char *id) {}
static void workspace_name(void *data, struct ext_workspace_handle_v1 *w,
						   const char *name) {
	struct entry *entry = data;
	snprintf(entry->name, sizeof(entry->name), "%s", name);
}
static void workspace_coordinates(void *data, struct ext_workspace_handle_v1 *w,
								  struct wl_array *coordinates) {
	if (coordinates->size == sizeof(uint32_t))
		((struct entry *)data)->position = *(uint32_t *)coordinates->data;
}
static void workspace_state(void *data, struct ext_workspace_handle_v1 *w,
							uint32_t state) {
	((struct entry *)data)->state = state;
}
static void workspace_capabilities(void *data, struct ext_workspace_handle_v1 *w,
								   uint32_t capabilities) {}
static void workspace_removed(void *data, struct ext_workspace_handle_v1 *w) {
	((struct entry *)data)->removed = true;
	ext_workspace_handle_v1_destroy(w);
}
static const struct ext_workspace_handle_v1_listener workspace_listener = {
	.id = workspace_id, .name = workspace_name,
	.coordinates = workspace_coordinates, .state = workspace_state,
	.capabilities = workspace_capabilities, .removed = workspace_removed,
};
static void group(void *data, struct ext_workspace_manager_v1 *m,
				  struct ext_workspace_group_handle_v1 *group) {
	ext_workspace_group_handle_v1_destroy(group);
}
static void workspace(void *data, struct ext_workspace_manager_v1 *m,
					  struct ext_workspace_handle_v1 *w) {
	if (count == sizeof(entries) / sizeof(entries[0]))
		exit(2);
	struct entry *entry = &entries[count++];
	entry->handle = w;
	entry->position = -1;
	ext_workspace_handle_v1_add_listener(w, &workspace_listener, entry);
}
static void done(void *data, struct ext_workspace_manager_v1 *m) {}
static void finished(void *data, struct ext_workspace_manager_v1 *m) { exit(3); }
static const struct ext_workspace_manager_v1_listener manager_listener = {
	.workspace_group = group, .workspace = workspace,
	.done = done, .finished = finished,
};
static void global(void *data, struct wl_registry *registry, uint32_t name,
				   const char *interface, uint32_t version) {
	if (!strcmp(interface, ext_workspace_manager_v1_interface.name)) {
		manager = wl_registry_bind(registry, name,
								   &ext_workspace_manager_v1_interface, 1);
		ext_workspace_manager_v1_add_listener(manager, &manager_listener, NULL);
	}
}
static void global_remove(void *data, struct wl_registry *registry,
						  uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	.global = global, .global_remove = global_remove,
};
static void snapshot(void) {
	printf("[");
	bool first = true;
	for (size_t i = 0; i < count; i++) {
		if (entries[i].removed)
			continue;
		printf("%s{\"name\":\"%s\",\"id\":%u,\"active\":%s,\"hidden\":%s,\"position\":%d}",
			   first ? "" : ",", entries[i].name,
			   wl_proxy_get_id((struct wl_proxy *)entries[i].handle),
			   entries[i].state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE ? "true" : "false",
			   entries[i].state & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN ? "true" : "false",
			   entries[i].position);
		first = false;
	}
	puts("]");
	fflush(stdout);
}
int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(display) < 0 || !manager ||
		wl_display_roundtrip(display) < 0)
		return 1;
	struct pollfd fds[] = {{.fd = 0, .events = POLLIN},
		{.fd = wl_display_get_fd(display), .events = POLLIN}};
	for (;;) {
		wl_display_dispatch_pending(display);
		wl_display_flush(display);
		if (poll(fds, 2, -1) < 0)
			break;
		if (fds[1].revents & POLLIN && wl_display_dispatch(display) < 0)
			break;
		if (fds[0].revents & POLLIN) {
			char command[64];
			if (!fgets(command, sizeof(command), stdin))
				break;
			if (!strncmp(command, "activate ", 9)) {
				for (size_t i = 0; i < count; i++) {
					if (!entries[i].removed &&
						atoi(entries[i].name) == atoi(command + 9)) {
						ext_workspace_handle_v1_activate(entries[i].handle);
						ext_workspace_manager_v1_commit(manager);
						break;
					}
				}
			}
			if (wl_display_roundtrip(display) < 0)
				break;
			snapshot();
		}
		if ((fds[0].revents & POLLHUP) ||
			(fds[1].revents & (POLLHUP | POLLERR)))
			break;
	}
	wl_display_disconnect(display);
	return 0;
}
