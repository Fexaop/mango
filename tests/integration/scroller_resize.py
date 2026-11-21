"""Rendered scrolling resize regressions, invoked by --scroller-resize-only."""
import math
from pathlib import Path
import shutil
import subprocess
import sys
import time

from PIL import Image, ImageChops


def run_scroller_resize(runtime, env, msg, clients, selected, check, spawn):
    frames = []
    destination = None
    if "--screenshots" in sys.argv:
        destination = Path(sys.argv[sys.argv.index("--screenshots") + 1])
        destination.mkdir(parents=True, exist_ok=True)

    def command(value):
        return msg("dispatch", value)

    def option(key, value):
        command(f"setoption,{key},{value}")

    def geometry(client):
        return tuple(client[key] for key in ("x", "y", "width", "height"))

    def until(deadline):
        time.sleep(max(0, deadline - time.monotonic()))

    def capture(name):
        frame = {"path": runtime / (name + ".png"), "started": time.monotonic()}
        subprocess.run(["grim", str(frame["path"])], env=env, check=True, timeout=5)
        frames.append(frame)
        return frame

    def pixels(frame, color=(255, 0, 255), *, allow_hidden=False):
        with Image.open(frame["path"]) as source:
            image = source.convert("RGB")
        difference = ImageChops.difference(
            image, Image.new("RGB", image.size, color))
        red, green, blue = difference.split()
        distance = ImageChops.lighter(ImageChops.lighter(red, green), blue)
        bounds = distance.point(lambda value: 255 if value == 0 else 0).getbbox()
        if bounds is None and not allow_hidden:
            raise AssertionError(f"no visible window border in {frame['path'].name}")
        return image, bounds

    def patch(image, bounds):
        x, y = bounds[:2]
        return image.crop((x + 4, y + 4, x + 164, y + 68))

    def check_frame(frame, target, reference=None):
        image, bounds = pixels(frame)
        x, y, right, bottom = bounds
        actual = right - x, bottom - y
        expected = (min(target["width"], image.width - x),
                    min(target["height"], image.height - y))
        label = frame["path"].stem
        check(f"{label} snaps visible dimensions: {actual} vs {expected}",
              all(abs(a - b) <= 1 for a, b in zip(actual, expected)))
        if reference is not None:
            check(label + " preserves glyph pixels",
                  ImageChops.difference(patch(image, bounds), reference).getbbox() is None)
        return bounds

    def check_adjacent_frame(frame, targets, references, focused):
        positions = {}
        for cid, target in targets.items():
            color = (255, 0, 255) if cid == focused else (0, 255, 255)
            image, bounds = pixels(frame, color, allow_hidden=cid != focused)
            if bounds is None:
                continue
            left, top, right, bottom = bounds
            middle_x, middle_y = (left + right - 1) // 2, (top + bottom - 1) // 2
            left_edge = left > 0 and image.getpixel((left, middle_y)) == color
            right_edge = image.getpixel((right - 1, middle_y)) == color
            top_edge = top > 0 and image.getpixel((middle_x, top)) == color
            bottom_edge = image.getpixel((middle_x, bottom - 1)) == color
            # A maximized foreground window can occlude both native edges of
            # its neighbor. Its remaining border sliver cannot locate content.
            if not (left_edge or right_edge) or not (top_edge or bottom_edge):
                continue
            x = left if left_edge else right - target["width"]
            y = top if top_edge else bottom - target["height"]
            positions[cid] = (x, y)
            label = frame["path"].stem + (" focused" if cid == focused else " neighbor")
            if left_edge:
                expected = min(target["width"], image.width - left)
                check(label + " snaps visible width", abs(right - left - expected) <= 1)
            if top_edge:
                expected = min(target["height"], image.height - top)
                check(label + " snaps visible height", abs(bottom - top - expected) <= 1)
            reference = references[cid]
            local_left, local_top = max(4, -x), max(4, -y)
            local_right = min(reference.width - 4, target["width"] - 4, image.width - x)
            local_bottom = min(reference.height - 4, target["height"] - 4, image.height - y)
            if local_left < local_right and local_top < local_bottom:
                actual = image.crop((x + local_left, y + local_top,
                                     x + local_right, y + local_bottom))
                native = reference.crop((local_left, local_top, local_right, local_bottom))
                check(label + " preserves native glyphs through edge clipping",
                      ImageChops.difference(actual, native).getbbox() is None)
        return positions

    try:
        for key, value in (
            ("animations", 0), ("tag_gather", 0),
            ("scroller_ignore_proportion_single", 1), ("scroller_focus_center", 1),
            ("scroller_prefer_overspread", 0), ("border_px", 2), ("border_radius", 0),
            ("no_border_when_single", 0), ("focused_opacity", 1), ("unfocused_opacity", 1),
            ("border_color", "0xff00ffff"), ("focus_color", "0xff00ffff"),
            ("maximized_screen_color", "0xff00ffff"),
        ):
            option(key, value)
        for client in clients():
            command(f"client,{client['id']},tagsilent,2")
        command("view,1")
        command("setlayout,scroller")
        pattern = ("printf '\\033[?25l'; "
                   + "printf '0123456789 | ABCDEFGHIJ\\n'; " * 6 + "sleep 300")
        spawn("scroller-resize-pattern", ["sh", "-c", pattern],
              foot_args=("--config=/dev/null", "--font=monospace:size=12"))
        cid = next(c["id"] for c in clients() if c["title"] == "scroller-resize-pattern")

        # Mapping with animations disabled leaves an opening pending. Finish it
        # before measuring ordinary MOVE reflows, which intentionally exclude OPEN.
        option("animations", 1)
        command("set_proportion,1.0")
        time.sleep(.65)
        option("border_color", "0x00ffffff")
        for layout in ("scroller", "vertical_scroller"):
            option("animations", 0)
            command("setlayout," + layout)
            command("set_proportion,0.4")
            command("enteroverview")
            time.sleep(.1)
            previous = capture(layout + "-overview-baseline")
            option("animation_duration_move", 1200)
            option("animation_curve_move", "0,0,1,1")
            option("animations", 1)
            for action in ("maximize", "restore"):
                label = layout + "-overview-" + action
                started = time.monotonic()
                command("togglemaximizescreen")
                target = selected()
                samples = []
                for delay in (.08, .24, .40):
                    until(started + delay)
                    samples.append(capture(f"{label}-{round(delay * 1000):03}ms"))
                until(started + 1.4)
                settled = capture(label + "-settled")
                final_image, final_bounds = pixels(settled)
                reference = patch(final_image, final_bounds)
                projected = {key: target[key] * .5 for key in ("width", "height")}
                sample_bounds = [check_frame(frame, projected, reference) for frame in samples]
                check_frame(settled, projected)
                _, before_bounds = pixels(previous)
                moving = [axis for axis in (0, 1)
                          if before_bounds[axis] != final_bounds[axis]]
                check(label + " animates position while snapping size", bool(moving) and any(
                    min(before_bounds[axis], final_bounds[axis]) < bounds[axis]
                    < max(before_bounds[axis], final_bounds[axis])
                    for bounds in sample_bounds for axis in moving))
                check(label + " preserves native maximize state and size",
                      selected()["is_maximized"] == (action == "maximize")
                      and geometry(selected()) == geometry(target))
                previous = settled
            option("animations", 0)
            command("leaveoverview")
        if "--overview-resize-only" in sys.argv:
            return
        option("border_color", "0xff00ffff")

        for layout, dimension in (("scroller", "width"), ("vertical_scroller", "height")):
            option("animations", 0)
            command("setlayout," + layout)
            command("set_proportion,1.0")
            time.sleep(.1)
            full = selected()
            previous = capture(layout + "-baseline")
            option("animation_duration_move", 1200)
            option("animation_curve_move", "0,0,1,1")
            option("animations", 1)
            half_geometry = None

            for name, action in (
                ("shrink", "set_proportion,0.5"),
                ("grow", "set_proportion,1.0"),
                ("before-maximize", "set_proportion,0.5"),
                ("maximize", "togglemaximizescreen"),
                ("restore", "togglemaximizescreen"),
            ):
                label = layout + "-" + name
                started = time.monotonic()
                command(action)
                target = selected()
                samples = []
                for delay in (.08, .24, .40):
                    until(started + delay)
                    samples.append(capture(f"{label}-{round(delay * 1000):03}ms"))
                until(started + 1.4)
                settled = capture(label + "-settled")
                actual = selected()
                final_image, final_bounds = pixels(settled)
                reference = patch(final_image, final_bounds)
                sample_bounds = [check_frame(frame, target, reference) for frame in samples]
                check_frame(settled, target)
                check(label + " settles at the target position",
                      final_bounds[:2] == geometry(target)[:2])
                check(label + " converges without changing focus",
                      actual["id"] == cid and geometry(actual) == geometry(target))
                _, before_bounds = pixels(previous)
                moving = [axis for axis in (0, 1)
                          if before_bounds[axis] != geometry(target)[axis]]
                if moving:
                    check(label + " retains positional animation", any(
                        min(before_bounds[axis], geometry(target)[axis]) < bounds[axis]
                        < max(before_bounds[axis], geometry(target)[axis])
                        for bounds in sample_bounds for axis in moving))
                if name in ("shrink", "before-maximize"):
                    check(label + " reaches half proportion",
                          abs(target[dimension] - full[dimension] * .5) <= 1)
                    half_geometry = geometry(target)
                elif name == "grow":
                    check(label + " restores full proportion", geometry(target) == geometry(full))
                elif name == "maximize":
                    check(label + " maximizes the tiled client",
                          target["is_maximized"] and target[dimension] >= full[dimension])
                else:
                    check(label + " restores the pre-maximize size and position",
                          not target["is_maximized"] and geometry(target) == half_geometry)
                previous = settled

            command("set_proportion,1.0")
            time.sleep(1.4)
            before = capture(layout + "-interrupt-before-0")
            retargets = []
            previous_start = None
            for index, proportion in enumerate((.5, 1.0, .5)):
                if previous_start is not None:
                    until(previous_start + .18)
                    before = capture(f"{layout}-interrupt-before-{index}")
                    until(previous_start + .2)
                started = time.monotonic()
                command(f"set_proportion,{proportion}")
                dispatched = time.monotonic()
                target = selected()
                until(started + .08)
                early = capture(f"{layout}-interrupt-{index}-080ms")
                retargets.append((before, early, target, dispatched))
                previous_start = started
            until(previous_start + 1.4)
            settled = capture(layout + "-interrupt-settled")
            actual = selected()
            final_image, final_bounds = pixels(settled)
            reference = patch(final_image, final_bounds)
            for before, early, target, dispatched in retargets:
                bounds = check_frame(early, target, reference)
                _, before_bounds = pixels(before)
                gap = max(0, dispatched - before["started"])
                for axis, output_size in enumerate(final_image.size):
                    tolerance = math.ceil(output_size * (gap + 1 / 60) / 1.2) + 2
                    endpoint = geometry(target)[axis]
                    low = min(before_bounds[axis], endpoint) - tolerance
                    high = max(before_bounds[axis], endpoint) + tolerance
                    check(early["path"].stem + f" retargets continuously on axis {axis}",
                          low <= bounds[axis] <= high)
            check_frame(settled, retargets[-1][2])
            check(layout + " interruptions converge to half size without changing focus",
                  actual["id"] == cid and geometry(actual) == half_geometry
                  and final_bounds[:2] == half_geometry[:2])

        # Two half-size windows fit together; maximizing either pushes its
        # neighbor through an output edge. Test both focus orders and both axes.
        command(f"client,{cid},tagsilent,2")
        option("animations", 0)
        option("border_color", "0x00ffffff")
        option("scroller_focus_center", 0)
        option("scroller_prefer_center", 0)
        option("scroller_prefer_overspread", 1)
        command("setlayout,scroller")
        grid = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ | 0123456789ABCDEFGHIJ"
        grid_command = "printf '\\033[?25l'; " + f"printf '{grid}\\n'; " * 60 + "sleep 300"
        adjacent_ids = []
        for index in range(2):
            title = f"scroller-resize-adjacent-{index}"
            spawn(title, ["sh", "-c", grid_command],
                  foot_args=("--config=/dev/null", "--font=monospace:size=12"))
            adjacent_ids.append(next(c["id"] for c in clients() if c["title"] == title))
            command("set_proportion,0.5")
        option("animations", 1)
        command("set_proportion,0.5")
        time.sleep(.65)

        for layout in ("scroller", "vertical_scroller"):
            option("animations", 0)
            command("setlayout," + layout)
            command("set_proportion,0.5")
            time.sleep(.1)
            native_targets = {c["id"]: c for c in clients() if c["id"] in adjacent_ids}
            baseline = capture(layout + "-adjacent-native")
            focused = selected()["id"]
            references = {}
            for window_id, target in native_targets.items():
                color = (255, 0, 255) if window_id == focused else (0, 255, 255)
                image, bounds = pixels(baseline, color)
                if (bounds[2] - bounds[0], bounds[3] - bounds[1]) != (
                        target["width"], target["height"]):
                    raise AssertionError("adjacent baseline must show both complete native windows")
                references[window_id] = image.crop(bounds)

            for index, focused in enumerate(adjacent_ids):
                option("animations", 0)
                if selected()["id"] != focused:
                    command("focusstack,next")
                if selected()["id"] != focused:
                    raise AssertionError("adjacent fixture did not focus the requested terminal")
                time.sleep(.1)
                previous = {c["id"]: c for c in clients() if c["id"] in adjacent_ids}
                option("animations", 1)
                for action in ("maximize", "restore"):
                    label = f"{layout}-adjacent-{index}-{action}"
                    started = time.monotonic()
                    command("togglemaximizescreen")
                    targets = {c["id"]: c for c in clients() if c["id"] in adjacent_ids}
                    samples = []
                    for delay in (.08, .24, .40):
                        until(started + delay)
                        samples.append(capture(f"{label}-{round(delay * 1000):03}ms"))
                    until(started + 1.4)
                    settled = capture(label + "-settled")
                    final_image, final_bounds = pixels(settled)
                    # A terminal can change cell-grid padding at a new size.
                    # Compare the resized client at its destination size, while
                    # its unchanged-size neighbor keeps the unclipped baseline.
                    transition_references = references | {focused: final_image.crop(final_bounds)}
                    positions = [check_adjacent_frame(frame, targets, transition_references, focused)
                                 for frame in samples]
                    final_target = targets[focused]
                    check(label + " settles at the target geometry",
                          final_bounds == (final_target["x"], final_target["y"],
                                           final_target["x"] + final_target["width"],
                                           final_target["y"] + final_target["height"]))
                    check(label + " keeps focus and completes maximize/restore",
                          selected()["id"] == focused
                          and selected()["is_maximized"] == (action == "maximize"))
                    for window_id, target in targets.items():
                        moving = [axis for axis in (0, 1)
                                  if geometry(previous[window_id])[axis] != geometry(target)[axis]]
                        if moving and (window_id == focused or action == "restore"):
                            check(label + f" keeps window {window_id} moving", any(
                                min(geometry(previous[window_id])[axis], geometry(target)[axis])
                                < frame[window_id][axis]
                                < max(geometry(previous[window_id])[axis], geometry(target)[axis])
                                for frame in positions if window_id in frame for axis in moving))
                    previous = targets
    finally:
        if destination is not None:
            for frame in frames:
                shutil.copy2(frame["path"], destination / frame["path"].name)
