/* Feed wlroots swipe signals to Mango's own driver in an isolated test.
 * Virtual pointer protocol has no swipe events; no production test hooks. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>

static struct wlr_cursor *cursor;
static uint32_t fingers, time_msec = 1000;

struct wlr_cursor *wlr_cursor_create(void) {
	struct wlr_cursor *(*create)(void) = dlsym(RTLD_NEXT, "wlr_cursor_create");
	cursor = create();
	return cursor;
}

static int swipe_event(int fd, uint32_t mask, void *data) {
	char command[128], type;
	double x = 0, y = 0;
	unsigned dt = 0;
	ssize_t n = recv(fd, command, sizeof(command) - 1, 0);
	if (n <= 0)
		return 0;
	command[n] = '\0';
	if (sscanf(command, "%c %lf %lf %u", &type, &x, &y, &dt) != 4)
		abort();
	time_msec += dt;
	if (type == 'b') {
		fingers = x;
		struct wlr_pointer_swipe_begin_event event = {.time_msec = time_msec,
													  .fingers = fingers};
		wl_signal_emit_mutable(&cursor->events.swipe_begin, &event);
	} else if (type == 'u') {
		struct wlr_pointer_swipe_update_event event = {
			.time_msec = time_msec, .fingers = fingers, .dx = x, .dy = y};
		wl_signal_emit_mutable(&cursor->events.swipe_update, &event);
	} else if (type == 'e') {
		struct wlr_pointer_swipe_end_event event = {.time_msec = time_msec,
													.cancelled = x != 0};
		wl_signal_emit_mutable(&cursor->events.swipe_end, &event);
	} else {
		abort();
	}
	if (send(fd, "ok", 2, MSG_NOSIGNAL) != 2)
		abort();
	return 0;
}

void wl_display_run(struct wl_display *display) {
	void (*run)(struct wl_display *) = dlsym(RTLD_NEXT, "wl_display_run");
	const char *fd = getenv("MANGO_TEST_SWIPE_FD");
	struct wl_event_source *source = NULL;
	if (fd)
		source =
			wl_event_loop_add_fd(wl_display_get_event_loop(display), atoi(fd),
								 WL_EVENT_READABLE, swipe_event, NULL);
	run(display);
	if (source)
		wl_event_source_remove(source);
}
