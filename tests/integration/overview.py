#!/usr/bin/env python3
"""Run against an isolated headless compositor: python3 tests/integration/overview.py.

Requires a built build/mango and build/mmsg, foot, and a GLES renderer (software
Mesa works). The test creates its own runtime directory and never contacts the
running desktop. Set MANGO_BUILD_DIR to use another build directory.

Use --drag-only for virtual pointer coverage (also requires cc, pkg-config,
and wayland-client development files). Add --screenshots DIRECTORY to save
pull and landing images with grim from the isolated test compositor.
Use --lifecycle-only to check closing, restoring and replacing overview windows.
Use --gestures-only to feed swipe events through Mango's native trackpad driver
(also requires cc, pkg-config, wlroots and Wayland server development files).
Use --compaction-only to check gap-free occupied workspace numbering.
Use --viewport-only to check ordered workspace identities and scroll/zoom continuity
(also requires cc, pkg-config, wayland-scanner and Wayland client/protocol files).
Use --operations-only for native target-workspace sizing, close replay, and
immediate exit input (also requires grim, wtype, and Python Pillow).
Use --deferred-only to check active-empty retention and anchored compaction.
Use --aliases-only to check trailing empty workspace number aliases.
Use --scroller-resize-only for single/adjacent scrolling resize frames, including
output-edge clipping (requires grim and Python Pillow); --screenshots DIRECTORY
preserves the captured frames.
Use --maximized-only to check multiple maximized windows in scrolling overview.
Use --overview-resize-only for rendered scroller maximize/restore inside overview
(requires grim and Python Pillow); --screenshots DIRECTORY preserves the frames.
"""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("MANGO_BUILD_DIR", ROOT / "build"))
LAYOUTS = (
    "tile", "scroller", "grid", "monocle", "deck", "center_tile",
    "right_tile", "vertical_scroller", "vertical_tile", "vertical_grid",
    "vertical_deck", "dwindle", "fair", "vertical_fair",
)
CONFIG = """monitor_rule=name:HEADLESS-.*,width:1280,height:720,refresh:60,x:0,y:0,scale:1
overview_style=1
overview_zoom=0.5
animation_duration_move=100
animation_duration_tag=150
border_px=2
no_border_when_single=0
shadows=0
blur=0
dim_enable=0
tag_num=9
window_rule=is_tag_silent:1,tags:8,title:^overview-rule-test$
"""


def geometry(client):
    return tuple(client[key] for key in ("x", "y", "width", "height"))


def run(runtime):
    env = os.environ | {
        "XDG_RUNTIME_DIR": str(runtime), "WAYLAND_DISPLAY": "wayland-0",
        "WLR_BACKENDS": "headless", "WLR_HEADLESS_OUTPUTS": "1",
        "WLR_RENDERER": "gles2", "WLR_LIBINPUT_NO_DEVICES": "1",
    }
    env.pop("WAYLAND_SOCKET", None)
    env.pop("MANGO_INSTANCE_SIGNATURE", None)
    env.pop("DBUS_SESSION_BUS_ADDRESS", None)
    config = runtime / "config.conf"
    config_text = CONFIG
    swipe = None
    pass_fds = ()
    compositor_env = env.copy()
    if "--gestures-only" in sys.argv:
        from overview_gestures import prepare_gestures, GESTURES
        swipe, child, preload = prepare_gestures(runtime)
        pass_fds = (child.fileno(),)
        compositor_env["LD_PRELOAD"] = str(preload)
        compositor_env["MANGO_TEST_SWIPE_FD"] = str(child.fileno())
        config_text += GESTURES
    config.write_text(config_text)
    apps = []
    passed = 0

    def msg(*args):
        result = subprocess.run(
            [str(BUILD / "mmsg"), *args], env=env, capture_output=True,
            text=True, check=True, timeout=5,
        )
        return json.loads(result.stdout)

    def dispatch(command):
        msg("dispatch", command)
        time.sleep(.25)

    def clients():
        return msg("get", "all-clients")["clients"]

    def selected():
        return msg("get", "focusing-client")

    def snapshot():
        return {c["id"]: geometry(c) for c in clients()}

    def check(label, condition):
        nonlocal passed
        if not condition:
            raise AssertionError(label)
        passed += 1
        print("PASS", label, flush=True)

    def spawn(title=None, command=None, *, foot_args=()):
        count = len(apps)
        expected = len(clients()) + 1
        with (runtime / f"foot-{count}.log").open("w") as log:
            app = subprocess.Popen(
                ["foot", *foot_args, "-T", title or f"overview-test-{count}", "-e",
                 *(command or ["sleep", "300"])],
                env=env, stdout=log, stderr=log,
            )
        apps.append(app)
        for _ in range(50):
            if len(clients()) == expected:
                time.sleep(.25)
                return app
            if app.poll() is not None:
                raise RuntimeError((runtime / f"foot-{count}.log").read_text())
            time.sleep(.1)
        raise TimeoutError("test window did not map")

    with (runtime / "compositor.log").open("w") as log:
        compositor = subprocess.Popen(
            [str(BUILD / "mango"), "-c", str(config)],
            env=compositor_env, pass_fds=pass_fds, stdout=log, stderr=log,
        )
    if swipe:
        child.close()
    try:
        socket = runtime / f"mango-{compositor.pid}.sock"
        for _ in range(100):
            if socket.exists() and (runtime / "wayland-0").exists():
                break
            if compositor.poll() is not None:
                raise RuntimeError((runtime / "compositor.log").read_text())
            time.sleep(.1)
        else:
            raise TimeoutError("headless compositor did not start")
        env["MANGO_INSTANCE_SIGNATURE"] = str(socket)
        for _ in range(3):
            spawn()
        if {"--scroller-resize-only", "--overview-resize-only"} & set(sys.argv):
            from scroller_resize import run_scroller_resize
            run_scroller_resize(runtime, env, msg, clients, selected, check, spawn)
            print(f"All {passed} scrolling resize checks passed.")
            return
        if "--aliases-only" in sys.argv:
            from workspace_aliases import run_aliases
            run_aliases(msg, dispatch, clients, selected, check, spawn)
            print(f"All {passed} workspace alias checks passed.")
            return
        if "--deferred-only" in sys.argv:
            from workspace_deferred import run_deferred
            run_deferred(msg, dispatch, clients, selected, check, spawn)
            print(f"All {passed} deferred workspace checks passed.")
            return
        if "--operations-only" in sys.argv:
            from workspace_operations import run_operations
            run_operations(runtime, env, msg, dispatch, clients, selected, check, spawn)
            print(f"All {passed} workspace operation checks passed.")
            return
        if "--viewport-only" in sys.argv:
            from workspace_protocol import run_workspace_protocol
            from workspace_viewport import run_viewport
            run_workspace_protocol(runtime, env, msg, dispatch, clients, check)
            run_viewport(msg, dispatch, clients, selected, check)
            print(f"All {passed} workspace viewport checks passed.")
            return
        if "--compaction-only" in sys.argv:
            from workspace_compaction import run_compaction
            run_compaction(msg, dispatch, clients, selected, check, spawn)
            print(f"All {passed} compaction checks passed.")
            return
        if "--gestures-only" in sys.argv:
            from overview_gestures import run_gestures
            run_gestures(swipe, msg, dispatch, clients, selected, check, spawn)
            print(f"All {passed} gesture checks passed.")
            return
        if "--lifecycle-only" in sys.argv:
            from overview_lifecycle import run_lifecycle
            run_lifecycle(msg, dispatch, clients, selected, check, spawn, LAYOUTS)
            print(f"All {passed} lifecycle checks passed.")
            return
        if "--drag-only" in sys.argv:
            from overview_drag import run_drag
            run_drag(runtime, env, BUILD, msg, dispatch, clients, check, spawn)
            print(f"All {passed} drag checks passed.")
            return
        if "--animations-only" not in sys.argv:
            for layout, axis, extent in (("scroller", "x", "width"),
                                         ("vertical_scroller", "y", "height")):
                dispatch("setlayout," + layout)
                ids = [c["id"] for c in clients()]
                for cid in ids:
                    dispatch(f"client,{cid},togglemaximizescreen")
                before = snapshot()
                dispatch("toggleoverview")
                # A configuration refresh arranges unchanged workspace members.
                dispatch("setoption,overview_zoom,0.5")
                for attempt in range(2):
                    windows = clients()
                    ordered = sorted(windows, key=lambda c: c["overview_geometry"][axis])
                    check(layout + " separates three maximized previews", all(
                        left["overview_geometry"][axis] + left["overview_geometry"][extent]
                        < right["overview_geometry"][axis]
                        for left, right in zip(ordered, ordered[1:])
                    ))
                    check(layout + " preserves maximized state and native size", all(
                        c["is_maximized"] and geometry(c)[2:] == before[c["id"]][2:]
                        for c in windows
                    ))
                    if attempt == 0:
                        dispatch("focusstack,next")
                dispatch("leaveoverview")
                for cid in ids:
                    dispatch(f"client,{cid},togglemaximizescreen")
            if "--maximized-only" in sys.argv:
                print(f"All {passed} maximized scrolling overview checks passed.")
                return

        for layout in (() if {"--animations-only", "--skip-layouts"} & set(sys.argv) else LAYOUTS):
            dispatch("setlayout," + layout)
            before = snapshot()
            dispatch("toggleoverview")
            check(layout + " keeps native geometry on entry", before == snapshot())
            check(layout + " blocks application input focus", all(
                not c["has_keyboard_focus"] and not c["has_pointer_focus"]
                for c in clients()
            ))
            check(layout + " renders half-size previews", all(
                abs(c["overview_geometry"]["width"] - c["width"] * .5) <= 1
                and abs(c["overview_geometry"]["height"] - c["height"] * .5) <= 1
                for c in clients()
            ))
            dispatch("leaveoverview")
            check(layout + " keeps native geometry on exit", before == snapshot())
            dispatch("toggleoverview")
            dispatch("exchange_client,left")
            before = snapshot()
            dispatch("leaveoverview")
            check(layout + " exchange survives exit", before == snapshot())

        if "--animations-only" not in sys.argv:
            for layout in ("scroller", "dwindle", "monocle", "vertical_scroller"):
                dispatch("setlayout," + layout)
                previous = selected()["id"]
                dispatch("toggleoverview")
                for direction in ("right", "down", "left", "up"):
                    dispatch("focusdir," + direction)
                    if selected()["id"] != previous:
                        break
                target = selected()["id"]
                check(layout + " selects another overview window", target != previous)
                dispatch("togglemaximizescreen")
                by_id = {c["id"]: c for c in clients()}
                check(layout + " maximizes current overview selection",
                      by_id[target]["is_maximized"] and not by_id[previous]["is_maximized"])
                dispatch("togglemaximizescreen")
                check(layout + " restores current overview selection",
                      not any(c["is_maximized"] for c in clients()))
                check(layout + " selection commands keep app input blocked", all(
                    not c["has_keyboard_focus"] and not c["has_pointer_focus"]
                    for c in clients()
                ))
                dispatch("leaveoverview")

        dispatch("setoption,animation_duration_move,400")
        dispatch("setoption,scroller_focus_center,1")
        dispatch("setlayout,scroller")
        dispatch("toggleoverview")
        before = {c["id"]: c["overview_geometry"] for c in clients()}
        direction = "left" if any(c["x"] < selected()["x"] for c in clients()) else "right"
        msg("dispatch", "focusdir," + direction)
        time.sleep(.08)
        moving = clients()
        changed = [c for c in moving if abs(
            before[c["id"]]["x"] - (320 + c["x"] * .5)
        ) > 2]
        check("scroller focus produces intermediate animation frames", bool(changed) and any(
            abs(c["overview_geometry"]["x"] - (320 + c["x"] * .5)) > 2
            for c in changed
        ))
        native = snapshot()
        time.sleep(.45)
        check("focus animation leaves native geometry authoritative", native == snapshot())
        dispatch("togglefloating")
        dispatch("movewin,100,120")
        dispatch("resizewin,400,260")
        time.sleep(.45)
        msg("dispatch", "resizewin,600,360")
        time.sleep(.08)
        c = selected()
        check("resize renders intermediate dimensions", c["width"] == 600 and
              200 < c["overview_geometry"]["width"] < 300)
        time.sleep(.45)
        msg("dispatch", "movewin,300,200")
        time.sleep(.08)
        c = selected()
        check("move renders intermediate positions", c["x"] == 300 and
              370 < c["overview_geometry"]["x"] < 470)
        # Closing during an animation must wait for the final native box.
        msg("dispatch", "leaveoverview")
        for _ in range(30):
            if "overview_geometry" not in selected():
                break
            time.sleep(.05)
        check("interrupted animation closes at native geometry",
              "overview_geometry" not in selected() and geometry(selected()) == (300, 200, 600, 360))
        dispatch("toggleoverview")
        dispatch("setoption,animations,0")
        msg("dispatch", "movewin,100,120")
        time.sleep(.05)
        c = selected()
        check("disabled animations place previews immediately", c["overview_geometry"]["x"] == 370)
        dispatch("leaveoverview")
        dispatch("setoption,animations,1")
        dispatch("setoption,animation_duration_move,100")
        dispatch("setoption,scroller_focus_center,0")
        # Normal pointer focus may select a different window when overview
        # closes. Restore floating state by ID before the monocle checks.
        for c in clients():
            if c["is_floating"]:
                dispatch(f"client,{c['id']},togglefloating")
        if "--animations-only" in sys.argv:
            print(f"All {passed} animation checks passed.")
            return
        dispatch("setlayout,monocle")
        dispatch("toggleoverview")
        visited = set()
        for _ in clients():
            visited.add(selected()["id"])
            dispatch("focusdir,right")
        check("graph visits coincident windows", len(visited) == len(clients()))
        dispatch("leaveoverview")
        dispatch("setlayout,dwindle")
        dispatch("toggleoverview")
        spawn()
        before = snapshot()
        dispatch("leaveoverview")
        check("new window does not jump on exit", before == snapshot())
        dispatch("toggleoverview")
        dispatch("view,3")
        spawn()
        check("new window uses selected overview group", selected()["tags"] == [3])
        before = snapshot()
        dispatch("leaveoverview")
        check("tag groups keep geometry on exit", before == snapshot())
        # Destroy a registered window and then traverse its logical neighbours.
        apps[-1].terminate()
        apps[-1].wait(timeout=3)
        time.sleep(.3)
        dispatch("view,1")
        dispatch("setlayout,monocle")
        dispatch("toggleoverview")
        dispatch("focusdir,right")
        check("graph excludes destroyed IDs", selected()["id"] in snapshot())
        dispatch("leaveoverview")
        # A hidden scroller group must keep its own layout when the visible
        # group uses tile, including after the client's size commits arrive.
        for _ in range(2):
            dispatch("tag,3")
            dispatch("view,1")
        dispatch("view,3")
        dispatch("setlayout,scroller")
        native = {c["id"]: geometry(c) for c in clients() if c["tags"] == [3]}
        dispatch("view,1")
        dispatch("setlayout,tile")
        dispatch("toggleoverview")
        time.sleep(.4)
        check("hidden scroller uses its own native group layout", all(
            geometry(c) == native[c["id"]] for c in clients() if c["id"] in native
        ))
        dispatch("view,3")
        dispatch("move_client,left")
        before = snapshot()
        dispatch("leaveoverview")
        check("mixed layouts preserve all group geometry on exit", before == snapshot())
        print(f"All {passed} checks passed.")
    finally:
        for app in apps:
            if app.poll() is None:
                app.terminate()
            app.wait(timeout=3)
        if compositor.poll() is None:
            compositor.terminate()
        compositor.wait(timeout=5)
        if swipe:
            swipe.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="mango-overview-test-") as directory:
        runtime = Path(directory)
        try:
            run(runtime)
        except Exception:
            print((runtime / "compositor.log").read_text())
            raise
