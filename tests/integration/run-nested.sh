#!/bin/sh
set -eu

if [ -z "${WAYLAND_DISPLAY:-}" ]; then
	printf '%s\n' 'Run this from a terminal inside Niri (or another Wayland desktop).' >&2
	exit 1
fi

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

# Mango skips its D-Bus/systemd session setup and teardown when this variable
# is absent. This also prevents shutdown from stopping the host desktop's
# graphical-session.target. Use the Wayland backend to stay inside a window.
exec env -u DBUS_SESSION_BUS_ADDRESS -u MANGO_INSTANCE_SIGNATURE \
	-u WAYLAND_SOCKET WLR_BACKENDS=wayland WLR_WL_OUTPUTS=1 \
	"$project_dir/build/mango" -c "$project_dir/tests/integration/nested.conf"
