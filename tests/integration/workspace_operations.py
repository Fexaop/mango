"""Native target-workspace operations during camera travel and overview zoom."""
import shlex
import subprocess
import time

from PIL import Image, ImageChops


def run_operations(runtime, env, msg, dispatch, clients, selected, check, spawn):
    def command(value):
        return msg("dispatch", value)

    def option(key, value):
        command(f"setoption,{key},{value}")

    def wait_for(predicate, timeout=3):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if predicate():
                return True
            time.sleep(.02)
        return False

    def by_id(cid):
        return next(c for c in clients() if c["id"] == cid)

    def dimensions(c):
        return c["width"], c["height"]

    def close(cid):
        command(f"client,{cid},killclient")
        check("closing removes the requested native terminal",
              wait_for(lambda: all(c["id"] != cid for c in clients())))
        time.sleep(.06)

    def desktop(tag=3):
        option("animations", 0)
        command("leaveoverview")
        command(f"view,{tag}")
        time.sleep(.06)

    option("animations", 0)
    option("tag_gather", 0)
    option("animation_duration_tag", 1600)
    option("animation_curve_tag", "0,0,1,1")
    option("tag_animation_continuous", 1)
    option("tag_animation_direction", 0)
    option("animation_type_close", "none")
    ids = sorted(c["id"] for c in clients())
    for tag, cid in enumerate(ids, 1):
        command(f"client,{cid},tagsilent,{tag}")
    command("view,3")
    option("tag_gather", 2)

    for layout in ("scroller", "vertical_scroller", "dwindle", "tile"):
        desktop()
        command("setlayout," + layout)
        spawn("native-control-" + layout)
        control = selected()["id"]
        expected = dimensions(by_id(control))
        check(layout + " control is a native tiled terminal", not by_id(control)["is_floating"])
        close(control)

        for stage in ("switching", "zooming-in", "zooming-out"):
            desktop(1)
            option("animations", 1)
            command("view,3")
            time.sleep(.12)
            if stage != "switching":
                command("enteroverview")
                time.sleep(.16)
            if stage == "zooming-out":
                command("leaveoverview")
            title = layout + "-" + stage
            spawn(title)
            c = next(c for c in clients() if c["title"] == title)
            check(title + " opens on logical destination workspace 3", c["tags"] == [3])
            check(title + " uses normal native tiling and size",
                  not c["is_floating"] and dimensions(c) == expected)
            if stage == "zooming-out":
                check(title + " receives application keyboard focus before zoom finishes",
                      c["has_keyboard_focus"] and "overview_geometry" in c)
            desktop()
            check(title + " retains native size after presentation settles",
                  dimensions(by_id(c["id"])) == expected and not by_id(c["id"])["is_floating"])
            close(c["id"])

    desktop()
    marker = runtime / "typed-during-exit"
    reader = ("IFS= read -r first; IFS= read -r second; "
              "printf '%s\\n%s\\n' \"$first\" \"$second\" > " + shlex.quote(str(marker)) +
              "; sleep 300")
    spawn("zoom-input-reader", ["sh", "-c", reader])
    reader_id = selected()["id"]
    command("enteroverview")
    time.sleep(.08)
    check("open overview blocks application keyboard input",
          not by_id(reader_id)["has_keyboard_focus"])
    option("animations", 1)
    began = time.monotonic()
    command("leaveoverview")
    check("leaveoverview focuses the target before its exit animation finishes",
          wait_for(lambda: by_id(reader_id)["has_keyboard_focus"], .25)
          and time.monotonic() - began < .45
          and "overview_geometry" in by_id(reader_id))
    subprocess.run(["wtype", "early-first", "-k", "Return"], env=env, check=True, timeout=5)
    command("toggleoverview")
    check("reopening overview immediately blocks application input again",
          not by_id(reader_id)["has_keyboard_focus"])
    command("toggleoverview")
    check("toggle-close restores application input without waiting for zoom",
          wait_for(lambda: by_id(reader_id)["has_keyboard_focus"], .25)
          and "overview_geometry" in by_id(reader_id))
    subprocess.run(["wtype", "early-second", "-k", "Return"], env=env, check=True, timeout=5)
    check("terminal receives actual typed input during exit zoom",
          wait_for(marker.exists, .5) and marker.read_text() == "early-first\nearly-second\n")
    command("setlayout,monocle")
    current = msg("get", "all-monitors")["monitors"][0]
    check("native layout commands execute while overview is closing",
          current["tags"][2]["layout"] == "M")
    desktop()
    close(reader_id)

    # With close fading disabled, an empty workspace must render the same
    # background before/after a last-window close, not replay a historical tag.
    desktop(4)
    reference = runtime / "empty-reference.png"
    subprocess.run(["grim", str(reference)], env=env, check=True, timeout=5)
    command("view,2")
    time.sleep(.06)
    option("animation_duration_tag", 400)
    option("animations", 1)
    command("view,3")
    time.sleep(.50)
    option("animation_duration_tag", 1600)
    close(ids[2])
    actual = runtime / "empty-after-close.png"
    subprocess.run(["grim", str(actual)], env=env, check=True, timeout=5)
    check("closing the last terminal does not animate the previous workspace",
          ImageChops.difference(Image.open(reference).convert("RGB"),
                                Image.open(actual).convert("RGB")).getbbox() is None)
    check("closing the last terminal keeps the empty destination workspace selected",
          [t["index"] for t in msg("get", "all-monitors")["monitors"][0]["tags"]
           if t["is_active"]] == [3])
