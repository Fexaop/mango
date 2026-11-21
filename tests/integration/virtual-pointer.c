/* Minimal virtual pointer client for isolated overview integration tests.
 * Reads: move X Y, press BUTTON, release BUTTON, quit. Each line is acked
 * only after the compositor has processed it. No input reaches the host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

static const struct wl_interface pointer_interface;
static const struct wl_interface *create_types[] = {
	&wl_seat_interface,
	&pointer_interface,
};
static const struct wl_message manager_requests[] = {
	{"create_virtual_pointer", "?on", create_types},
	{"destroy", "", NULL},
};
static const struct wl_interface manager_interface = {
	"zwlr_virtual_pointer_manager_v1", 1, 2, manager_requests, 0, NULL,
};
static const struct wl_message pointer_requests[] = {
	{"motion", "uff", NULL},   {"motion_absolute", "uuuuu", NULL},
	{"button", "uuu", NULL},   {"axis", "uuf", NULL},
	{"frame", "", NULL},	   {"axis_source", "u", NULL},
	{"axis_stop", "uu", NULL}, {"axis_discrete", "uufi", NULL},
	{"destroy", "", NULL},
};
static const struct wl_interface pointer_interface = {
	"zwlr_virtual_pointer_v1", 1, 9, pointer_requests, 0, NULL,
};
static struct wl_proxy *manager;
static void global(void *data, struct wl_registry *registry, uint32_t name,
				   const char *interface, uint32_t version) {
	(void)data;
	(void)version;
	if (!strcmp(interface, manager_interface.name))
		manager = wl_registry_bind(registry, name, &manager_interface, 1);
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {
	(void)data;
	(void)registry;
	(void)name;
}
static const struct wl_registry_listener listener = {global, removed};

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (!display)
		return 1;
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &listener, NULL);
	if (wl_display_roundtrip(display) < 0 || !manager)
		return 2;
	struct wl_proxy *pointer = wl_proxy_marshal_flags(
		manager, 0, &pointer_interface, 1, 0, NULL, NULL);
	if (wl_display_roundtrip(display) < 0)
		return 3;
	char line[128], command[32];
	unsigned x, y, button;
	while (fgets(line, sizeof(line), stdin)) {
		if (sscanf(line, "%31s", command) != 1)
			continue;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		uint32_t time = now.tv_sec * 1000u + now.tv_nsec / 1000000u;
		if (!strcmp(command, "move") && sscanf(line, "%*s %u %u", &x, &y) == 2)
			wl_proxy_marshal_flags(pointer, 1, NULL, 1, 0, time, x, y, 1280u,
								   720u);
		else if ((!strcmp(command, "press") || !strcmp(command, "release")) &&
				 sscanf(line, "%*s %u", &button) == 1)
			wl_proxy_marshal_flags(pointer, 2, NULL, 1, 0, time, button,
								   (unsigned)!strcmp(command, "press"));
		else if (!strcmp(command, "quit"))
			break;
		else
			return 4;
		wl_proxy_marshal_flags(pointer, 4, NULL, 1, 0);
		if (wl_display_roundtrip(display) < 0)
			return 5;
		puts("ok");
		fflush(stdout);
	}
	wl_proxy_marshal_flags(pointer, 8, NULL, 1, WL_MARSHAL_FLAG_DESTROY);
	wl_display_roundtrip(display);
	wl_display_disconnect(display);
	return 0;
}
