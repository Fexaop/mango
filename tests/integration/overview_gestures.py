"""Continuous gestures through Mango's native trackpad driver."""
import shlex
import socket
import subprocess
import time
from pathlib import Path

GESTURES = """
gesture_live=1
tag_animation_direction=0
gesturebind=none,up,4,enteroverview
gesturebind=none,down,4,leaveoverview
gesturebind=none,left,3,focusstack,next
gesturebind=none,right,3,focusstack,prev
gesturebind=none,up,3,viewtoright,0
gesturebind=none,down,3,viewtoleft,0
"""


def prepare_gestures(runtime):
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "wlroots-0.20", "wayland-server"],
        text=True))
    preload = runtime / "swipe-events.so"
    subprocess.run([
        "cc", "-shared", "-fPIC", "-DWLR_USE_UNSTABLE", "-Wall", "-Wextra",
        "-Wno-unused-parameter", str(Path(__file__).with_name("swipe-events.c")),
        "-o", str(preload), *flags, "-ldl",
    ], check=True)
    parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    parent.settimeout(5)
    return parent, child, preload


def run_gestures(swipe, msg, dispatch, clients, selected, check, spawn):
    def send(kind, x=0, y=0, dt=16, delay=.04):
        swipe.sendall(f"{kind} {x} {y} {dt}".encode())
        assert swipe.recv(32) == b"ok", "Mango did not process the swipe"
        time.sleep(delay)

    def begin(fingers, x, y):
        send("b", fingers)
        send("u", x, y)

    def end(cancelled=False, dt=16):
        send("e", int(cancelled), dt=dt, delay=.35)

    def native():
        return {c["id"]: tuple(c[k] for k in ("x", "y", "width", "height"))
                for c in clients()}

    def preview(cid, axis="x"):
        return next(c for c in clients() if c["id"] == cid)["overview_geometry"][axis]

    def blocked():
        return all(not c["has_keyboard_focus"] and not c["has_pointer_focus"]
                   for c in clients())

    def active_tag():
        return next(t["index"] for t in msg("get", "all-tags")["all_tags"][0]["tags"]
                    if t["is_active"])

    dispatch("setlayout,scroller")
    dispatch("setoption,scroller_focus_center,1")
    before = native()
    begin(4, 0, -20)
    send("u", 0, -70)
    c = selected()
    half = c["overview_geometry"]["width"]
    check("overview zoom follows fingers before release", c["width"] * .5 < half < c["width"])
    time.sleep(.25)
    check("held zoom stays at finger progress", selected()["overview_geometry"]["width"] == half)
    send("u", 0, 65)
    check("reversing fingers reverses zoom", selected()["overview_geometry"]["width"] > half)
    end()
    check("reversed overview swipe returns to desktop", "overview_geometry" not in selected())
    check("reversed zoom preserves native geometry", native() == before)

    begin(4, 0, -20)
    send("u", 0, -240)
    end(cancelled=True)
    check("cancelled overview entry restores desktop", "overview_geometry" not in selected())
    begin(4, 0, -20)
    send("u", 0, -40)
    end(dt=500)
    check("short held swipe does not become a flick", "overview_geometry" not in selected())

    begin(4, 0, -20)
    send("u", 0, -70)
    end()
    check("quick flick completes overview entry", all("overview_geometry" in c for c in clients()))
    check("entry settles at configured zoom", all(abs(c["overview_geometry"]["width"] - c["width"] * .5) <= 1 for c in clients()))
    check("gesture overview blocks application focus", blocked())
    begin(4, 0, 20)
    send("u", 0, 240)
    end(cancelled=True)
    check("cancelled exit returns to overview", all("overview_geometry" in c for c in clients()))
    check("cancelled exit settles at configured zoom", all(abs(c["overview_geometry"]["width"] - c["width"] * .5) <= 1 for c in clients()))

    # Both orientations use the same driver and native layout on release.
    for layout, axis in (("scroller", "x"), ("vertical_scroller", "y")):
        dispatch("setlayout," + layout)
        c = selected()
        cid = c["id"]
        center = c[axis] + c["width" if axis == "x" else "height"] / 2
        direction = -1 if any(w[axis] + w["width" if axis == "x" else "height"] / 2 > center + 1 for w in clients()) else 1
        before = native()
        at_start = preview(cid, axis)
        begin(3, direction * 20, 0)
        send("u", direction * 60, 0)
        at_pan = preview(cid, axis)
        check(layout + " preview follows window swipe", (at_pan - at_start) * direction > 5)
        check(layout + " pan leaves native geometry untouched", native() == before)
        time.sleep(.2)
        check(layout + " held pan stays at finger position", preview(cid, axis) == at_pan)
        send("u", direction * -60, 0)
        end(dt=500)
        check(layout + " reversed pan keeps selection", selected()["id"] == cid)
        check(layout + " reversed pan returns previews", abs(preview(cid, axis) - at_start) <= 1)
        begin(3, direction * 20, 0)
        send("u", direction * 220, 0)
        end(cancelled=True)
        check(layout + " cancelled pan keeps selection", selected()["id"] == cid)
        check(layout + " cancelled pan preserves geometry", native() == before)
        begin(3, direction * 20, 0)
        send("u", direction * 180, 0)
        end()
        check(layout + " release selects another window", selected()["id"] != cid)
        check(layout + " release keeps app focus blocked", blocked())
        settled = native()
        dispatch("leaveoverview")
        check(layout + " release survives overview exit", native() == settled)
        dispatch("enteroverview")

    # Workspace previews progress continuously even when layouts differ.
    dispatch("view,2")
    spawn()
    dispatch("setlayout,dwindle")
    dispatch("view,1")
    cid = selected()["id"]
    at_start = preview(cid, "y")
    begin(3, 0, -20)
    send("u", 0, -80)
    at_pan = preview(cid, "y")
    check("workspace preview follows vertical swipe", at_pan < at_start - 10)
    time.sleep(.2)
    check("workspace swipe holds its progress", preview(cid, "y") == at_pan)
    send("u", 0, 75)
    end()
    check("reversed workspace swipe restores original tag", selected()["tags"] == [1])
    check("reversed workspace swipe returns preview", abs(preview(cid, "y") - at_start) <= 1)
    begin(3, 0, -20)
    send("u", 0, -240)
    end(cancelled=True)
    check("cancelled workspace swipe restores original tag", selected()["tags"] == [1])
    begin(3, 0, -20)
    send("u", 0, -240)
    end()
    check("workspace release commits destination", selected()["tags"] == [2])
    check("workspace gesture keeps app focus blocked", blocked())
    begin(3, 0, 20)
    send("u", 0, 240)
    end()
    check("opposite workspace gesture returns", selected()["tags"] == [1])

    from overview import LAYOUTS
    for layout in LAYOUTS:
        dispatch("setlayout," + layout)
        begin(4, 0, 20)
        send("u", 0, 240)
        end()
        check(layout + " exits using Mango gesture driver", "overview_geometry" not in selected())
        begin(4, 0, -20)
        send("u", 0, -240)
        end()
        check(layout + " enters using Mango gesture driver", "overview_geometry" in selected() and blocked())
        if layout not in ("scroller", "vertical_scroller"):
            cid = selected()["id"]
            begin(3, -20, 0)
            send("u", -180, 0)
            end()
            check(layout + " window gesture uses native focus", selected()["id"] != cid and blocked())

    dispatch("leaveoverview")
    for tag in (1, 4):
        dispatch(f"view,{tag}")
        begin(3, 0, -20)
        send("u", 0, -240)
        end(cancelled=True)
        check(f"native tag {tag} cancels workspace swipe", active_tag() == tag)
        begin(3, 0, -20)
        send("u", 0, -240)
        end()
        check(f"native tag {tag} commits workspace swipe", active_tag() == tag + 1)
    dispatch("view,1")
    dispatch("setlayout,scroller")
    cid = selected()["id"]
    before = native()
    c = selected()
    direction = -1 if any(w["x"] > c["x"] for w in clients() if w["tags"] == [1]) else 1
    begin(3, direction * 20, 0)
    send("u", direction * 180, 0)
    end(cancelled=True)
    check("native window pan cancels selection", selected()["id"] == cid)
    check("native cancelled pan preserves geometry", native() == before)
    begin(3, direction * 20, 0)
    send("u", direction * 180, 0)
    end()
    check("native window pan selects destination", selected()["id"] != cid)
    dispatch("setoption,animations,0")
    begin(4, 0, -20)
    send("u", 0, -90)
    c = selected()
    check("live zoom works with clock animations disabled", c["width"] * .5 < c["overview_geometry"]["width"] < c["width"])
    end(cancelled=True)
    check("disabled animations still honour cancellation", "overview_geometry" not in selected())
