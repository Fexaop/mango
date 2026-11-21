"""Real pointer regressions, invoked by overview.py --drag-only."""
import select
import shlex
import subprocess
import sys
import time
from pathlib import Path


def run_drag(runtime, env, build, msg, dispatch, clients, check, spawn):
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "wayland-client"], text=True))
    binary = runtime / "virtual-pointer"
    subprocess.run(["cc", str(Path(__file__).with_name("virtual-pointer.c")),
                    "-o", str(binary), *flags], check=True)
    pointer = subprocess.Popen([str(binary)], env=env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True)

    def send(command, delay=.08):
        pointer.stdin.write(command + "\n")
        pointer.stdin.flush()
        assert select.select([pointer.stdout], [], [], 5)[0], "pointer timed out"
        assert pointer.stdout.readline().strip() == "ok", "pointer disconnected"
        time.sleep(delay)

    def move(x, y, delay=.08):
        send(f"move {round(x)} {round(y)}", delay)

    def by_id(cid):
        return next(c for c in clients() if c["id"] == cid)

    def box(c):
        return tuple(c[k] for k in ("x", "y", "width", "height"))

    def begin(c):
        b = c["overview_geometry"]
        x = max(325, min(950, b["x"] + b["width"] / 2))
        y = max(185, min(530, b["y"] + b["height"] / 2))
        move(x, y)
        send("press 272")
        assert "overview_drag" in by_id(c["id"]), "press missed window"
        return x, y

    def release():
        send("release 272", .4)

    def screenshot(name):
        if "--screenshots" in sys.argv:
            destination = Path(sys.argv[sys.argv.index("--screenshots") + 1])
            destination.mkdir(parents=True, exist_ok=True)
            subprocess.run(["grim", str(destination / name)], env=env, check=True)

    def no_app_focus():
        return all(not c["has_keyboard_focus"] and not c["has_pointer_focus"]
                   for c in clients())

    try:
        dispatch("setoption,animations,0")
        dispatch("setlayout,scroller")
        dispatch("toggleoverview")
        c = msg("get", "focusing-client")
        cid = c["id"]
        native = {w["id"]: box(w) for w in clients()}
        preview = c["overview_geometry"]
        x, y = begin(c)
        move(x + 60, y)
        pulled = by_id(cid)
        shift = pulled["overview_geometry"]["x"] - preview["x"]
        screenshot("overview-pull.png")
        check("pull moves preview with resistance", 1 < shift < 60)
        check("pull keeps window in its native layout", not pulled["overview_drag"]["detached"]
              and native == {w["id"]: box(w) for w in clients()})
        time.sleep(.45)
        check("holding does not detach the window", not by_id(cid)["overview_drag"]["detached"])
        release()
        check("release below threshold restores preview", by_id(cid)["overview_geometry"] == preview)
        x, y = begin(by_id(cid))
        move(x - 135, y)
        c = by_id(cid)
        check("distance immediately detaches the tile", c["overview_drag"]["detached"] and c["is_floating"])
        screenshot("overview-landing.png")
        check("existing workspace has a landing highlight", c["overview_drag"]["target_tag"] == 1
              and c["overview_drag"]["highlight_geometry"]["width"] > 0)
        move(640, 558)
        c = by_id(cid)
        screenshot("overview-gap.png")
        check("gap has a narrow insertion highlight", c["overview_drag"]["inserts_workspace"]
              and 1 <= c["overview_drag"]["highlight_geometry"]["height"] < 36)
        check("detached window follows pointer across workspace boundary",
              abs(c["overview_geometry"]["x"] + c["overview_geometry"]["width"] * .5 - 640) < 2
              and abs(c["overview_geometry"]["y"] + c["overview_geometry"]["height"] * .5 - 558) < 2)
        check("hover changes no workspace membership", all(w["tags"] == [1] for w in clients()))
        release()
        check("gap drop assigns the new workspace", by_id(cid)["tags"] == [2] and not by_id(cid)["is_floating"])
        check("drop keeps overview open and app input blocked", "overview_geometry" in by_id(cid) and no_app_focus())
        before = {w["id"]: box(w) for w in clients()}
        dispatch("leaveoverview")
        check("inserted workspace closes without geometry jump", before == {w["id"]: box(w) for w in clients()})

        # Occupied 2,3 shift into the nearest empty 4, while occupied 9 stays.
        ids = [w["id"] for w in clients()]
        for target in ids:
            dispatch(f"client,{target},tagsilent,1")
        dispatch(f"client,{ids[-1]},tagsilent,2")
        spawn()
        at_three = next(w["id"] for w in clients() if w["id"] not in ids)
        dispatch(f"client,{at_three},tagsilent,3")
        spawn()
        far = next(w["id"] for w in clients() if w["id"] not in ids + [at_three])
        dispatch(f"client,{far},tagsilent,9")
        for tag, layout in ((2, "dwindle"), (3, "monocle"), (9, "tile"), (1, "scroller")):
            dispatch(f"view,{tag}")
            dispatch("setlayout," + layout)
        dispatch("toggleoverview")
        native = {w["id"]: box(w) for w in clients()}
        layout_before = msg("get", "all-tags")["all_tags"][0]["tags"]
        c = msg("get", "focusing-client")
        cid = c["id"]
        x, y = begin(c)
        move(x - 135, y)
        move(640, 558)
        c = by_id(cid)
        check("chain insertion available with last tag occupied", c["overview_drag"]["target_tag"] == 2
              and c["overview_drag"]["inserts_workspace"])
        check("chain stays unchanged during hover", by_id(ids[-1])["tags"] == [2]
              and by_id(at_three)["tags"] == [3] and by_id(far)["tags"] == [9])
        # A stationary drag should not spin an unbounded frame loop.
        stat = Path(f"/proc/{int(env['MANGO_INSTANCE_SIGNATURE'].rsplit('-', 1)[1].split('.')[0])}/stat")
        def ticks():
            fields = stat.read_text().split()
            return int(fields[13]) + int(fields[14])
        before_ticks = ticks()
        time.sleep(.5)
        check("stationary drag remains cheap", ticks() - before_ticks < 20)
        release()
        check("drop shifts only the occupied chain", by_id(cid)["tags"] == [2]
              and by_id(ids[-1])["tags"] == [3] and by_id(at_three)["tags"] == [4]
              and by_id(far)["tags"] == [9])
        layout_after = msg("get", "all-tags")["all_tags"][0]["tags"]
        check("shifted workspaces preserve layouts", layout_after[2]["layout"] == layout_before[1]["layout"]
              and layout_after[3]["layout"] == layout_before[2]["layout"])
        check("shifted workspaces preserve native geometry", all(
            box(by_id(i)) == native[i] for i in (ids[-1], at_three, far)))
        check("chain insertion preserves registry IDs", set(native) == {w["id"] for w in clients()})
        check("chain insertion keeps application input blocked", no_app_focus())
        # The native layout remains editable after its group and tree move.
        dispatch("view,3")
        dispatch("focusdir,right")
        dispatch("togglemaximizescreen")
        check("shifted native layout still accepts commands", by_id(ids[-1])["is_maximized"])
        dispatch("togglemaximizescreen")
        dispatch("leaveoverview")

        # All Mango layouts use the same pickup and native drop path.
        from overview import LAYOUTS
        dispatch("view,1")
        for layout in LAYOUTS:
            dispatch("setlayout," + layout)
            dispatch("toggleoverview")
            c = msg("get", "focusing-client")
            x, y = begin(c)
            move(x - 135, y)
            check(layout + " detaches through native drag", by_id(c["id"])["overview_drag"]["detached"])
            move(640, 360)
            release()
            check(layout + " returns to its native layout", not by_id(c["id"])["is_floating"])
            native = {w["id"]: box(w) for w in clients()}
            dispatch("leaveoverview")
            check(layout + " drag closes without a jump", native == {w["id"]: box(w) for w in clients()})

        # Early release animates back and never unmaximizes a window.
        dispatch("setoption,animations,1")
        dispatch("setoption,animation_duration_move,400")
        dispatch("setlayout,dwindle")
        dispatch("togglemaximizescreen")
        dispatch("toggleoverview")
        c = msg("get", "focusing-client")
        x, y = begin(c)
        start = c["overview_geometry"]
        move(x + 60, y)
        check("maximized window resists before detaching", by_id(c["id"])["is_maximized"]
              and not by_id(c["id"])["overview_drag"]["detached"])
        send("release 272", .03)
        check("early release renders a spring-back frame", by_id(c["id"])["overview_geometry"] != start)
        time.sleep(.5)
        check("spring-back settles at original position", by_id(c["id"])["overview_geometry"] == start)
        x, y = begin(by_id(c["id"]))
        move(x - 135, y)
        check("detaching restores a maximized window", not by_id(c["id"])["is_maximized"]
              and by_id(c["id"])["overview_drag"]["detached"])
        release()
        dispatch("leaveoverview")
        dispatch("setoption,animations,0")

        # Fill every tag. A gap must not offer or commit an overflowing shift.
        while len(clients()) < 9:
            spawn()
        ordered = [w["id"] for w in clients()]
        for tag, client_id in enumerate(ordered, 1):
            dispatch(f"client,{client_id},tagsilent,{tag}")
        dispatch("view,1")
        dispatch("setlayout,scroller")
        dispatch("toggleoverview")
        original = {w["id"]: w["tags"] for w in clients()}
        c = msg("get", "focusing-client")
        x, y = begin(c)
        move(x - 135, y)
        move(640, 558)
        check("full workspaces show no insertion highlight", by_id(c["id"])["overview_drag"]["target_tag"] == 0)
        release()
        check("full workspace insertion loses no windows", original == {w["id"]: w["tags"] for w in clients()})
        dispatch("leaveoverview")

        # Use an existing hole without shifting the workspaces below it.
        dispatch(f"client,{ordered[1]},tagsilent,9")
        dispatch("view,1")
        dispatch("toggleoverview")
        original = {w["id"]: w["tags"] for w in clients()}
        c = msg("get", "focusing-client")
        x, y = begin(c)
        move(x - 135, y)
        move(640, 558)
        check("hidden empty tag offers a gap landing", by_id(c["id"])["overview_drag"]["target_tag"] == 2)
        release()
        check("gap drop reuses empty tag without shifting others", by_id(c["id"])["tags"] == [2]
              and all(w["tags"] == original[w["id"]] for w in clients() if w["id"] != c["id"]))
        # Destruction during a held drag must clean up its grab and hint.
        c = msg("get", "focusing-client")
        x, y = begin(c)
        move(x - 135, y)
        dispatch(f"client,{c['id']},killclient")
        send("release 272")
        check("destroying dragged window clears drag state", all("overview_drag" not in w for w in clients()))
        dispatch("leaveoverview")
    finally:
        if pointer.poll() is None:
            pointer.stdin.write("quit\n")
            pointer.stdin.flush()
        pointer.wait(timeout=5)
