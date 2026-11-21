"""Workspace camera continuity across desktop/overview presentation changes."""
import time


def run_viewport(msg, dispatch, clients, selected, check):
    duration = 1.4
    ids = sorted(c["id"] for c in clients())
    dispatch("setoption,animations,0")
    dispatch("setoption,tag_gather,0")
    for number, cid in enumerate(ids, 1):
        dispatch(f"client,{cid},tagsilent,{number}")
        dispatch(f"view,{number}")
        dispatch("setlayout,scroller")
    dispatch("setoption,tag_gather,2")
    dispatch("setoption,tag_animation_continuous,1")
    dispatch("setoption,animation_duration_tag,1400")
    dispatch("setoption,animation_curve_tag,0,0,1,1")

    def native():
        return {c["id"]: tuple(c[key] for key in ("x", "y", "width", "height"))
                for c in clients()}

    def reset(horizontal=False):
        dispatch("setoption,animations,0")
        dispatch("leaveoverview")
        dispatch("view,1")
        dispatch(f"setoption,tag_animation_direction,{int(horizontal)}")
        before = native()
        dispatch("setoption,animations,1")
        return before

    def at(start, fraction):
        time.sleep(max(0, start + duration * fraction - time.monotonic()))

    def camera():
        windows = {c["tags"][0]: c for c in clients()}
        first, last = windows[1], windows[3]
        a, b = first["overview_geometry"], last["overview_geometry"]
        monitor = msg("get", "all-monitors")["monitors"][0]
        zoom = a["width"] / first["width"]
        origins = []
        for c, preview in ((first, a), (last, b)):
            origins.append(tuple(preview[key] - (c[key] - monitor[key]) * zoom
                                 for key in ("x", "y")))
        stride = tuple((end - start) / 2 for start, end in zip(*origins))
        center = tuple(monitor[key] + monitor[size] * (1 - zoom) / 2
                       for key, size in (("x", "width"), ("y", "height")))
        distance = sum((base - origin) * step
                       for base, origin, step in zip(center, origins[0], stride))
        position = distance / sum(step * step for step in stride)
        return position, zoom

    before = reset()
    started = time.monotonic()
    msg("dispatch", "view,3")
    at(started, .25)
    msg("dispatch", "enteroverview")
    at(started, .34)
    position, zoom = camera()
    check("overview opens at the ongoing workspace camera position",
          abs(position - 2 * (time.monotonic() - started) / duration) < .13
          and .4 < position < 1.2)
    check("overview entry changes zoom without changing native geometry",
          .5 < zoom < 1 and native() == before)
    at(started, .68)
    later, smaller = camera()
    check("workspace travel continues independently while overview zooms out",
          later > position + .35 and smaller < zoom
          and abs(later - 2 * (time.monotonic() - started) / duration) < .13)
    at(started, 1.10)
    settled, _ = camera()
    check("entering overview does not restart the workspace switch deadline",
          abs(settled - 2) < .035)
    check("workspace travel preserves occupied workspace identity",
          {c["id"]: c["tags"] for c in clients()} == dict(zip(ids, ([1], [2], [3]))))

    before = reset(horizontal=True)
    started = time.monotonic()
    msg("dispatch", "view,3")
    at(started, .20)
    msg("dispatch", "enteroverview")
    at(started, .31)
    first, _ = camera()
    check("horizontal desktop travel shares the overview camera",
          abs(first - 2 * (time.monotonic() - started) / duration) < .13)
    at(started, .40)
    msg("dispatch", "leaveoverview")
    at(started, .51)
    msg("dispatch", "enteroverview")
    at(started, .65)
    reopened, _ = camera()
    check("closing and reopening overview do not reset workspace progress",
          reopened > first + .4
          and abs(reopened - 2 * (time.monotonic() - started) / duration) < .15)
    at(started, 1.10)
    check("reopened overview retains the original workspace end time",
          abs(camera()[0] - 2) < .035 and native() == before)

    before = reset()
    started = time.monotonic()
    msg("dispatch", "view,3")
    at(started, .20)
    msg("dispatch", "enteroverview")
    at(started, .45)
    previous, _ = camera()
    redirected = time.monotonic()
    msg("dispatch", "view,1")
    at(redirected, .30)
    reversed_position, _ = camera()
    check("retargeting during overview zoom reverses from the current position",
          .2 < reversed_position < previous - .1
          and abs(reversed_position - previous * .70) < .15)
    dispatch("setoption,animations,0")
    check("disabling animations settles the shared camera and zoom",
          abs(camera()[0]) < .035 and abs(camera()[1] - .5) < .01
          and selected()["tags"] == [1] and native() == before)

    dispatch("setoption,tag_animation_continuous,0")
    dispatch("setoption,animations,1")
    started = time.monotonic()
    msg("dispatch", "view,3")
    at(started, .50)
    halfway, _ = camera()
    middle = next(c for c in clients() if c["tags"] == [2])
    check("overview still traverses intermediate workspaces with direct desktop jumps",
          abs(halfway - 1.0) < .13
          and abs(middle["overview_geometry"]["y"] -
                  (180 + middle["y"] * .5)) < 65)
    dispatch("setoption,animations,0")
