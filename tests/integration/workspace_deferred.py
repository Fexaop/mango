"""Empty workspaces survive until departure settles; compaction anchors destination."""
import time


def run_deferred(msg, dispatch, clients, selected, check, spawn):
    duration = 1.0
    ids = sorted(c["id"] for c in clients())

    def command(value):
        msg("dispatch", value)

    def option(key, value):
        dispatch(f"setoption,{key},{value}")

    def state():
        return {c["id"]: c for c in clients()}

    def tags():
        return {cid: c["tags"] for cid, c in state().items()}

    def native(c):
        return tuple(c[key] for key in ("x", "y", "width", "height"))

    def preview(c):
        p = c["overview_geometry"]
        return tuple(p[key] for key in ("x", "y", "width", "height"))

    def active():
        return [t["index"] for t in msg("get", "all-monitors")["monitors"][0]["tags"]
                if t["is_active"]]

    def until(predicate, label, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(.01)
        raise AssertionError(label)

    def at(start, seconds):
        time.sleep(max(0, start + seconds - time.monotonic()))

    option("animations", 0)
    option("tag_gather", 0)
    option("animation_type_close", "none")
    option("tag_animation_continuous", 1)
    option("tag_animation_direction", 0)
    option("animation_duration_tag", 1000)
    option("animation_curve_tag", "0,0,1,1")
    for tag, cid in enumerate(ids, 1):
        dispatch(f"client,{cid},tagsilent,{tag}")
        dispatch(f"view,{tag}")
        dispatch("setlayout,scroller")
    dispatch("view,2")
    option("tag_gather", 2)
    dispatch("enteroverview")
    before = state()
    before_preview = {cid: preview(c) for cid, c in before.items()}
    before_native = {cid: native(c) for cid, c in before.items()}
    option("animations", 1)
    command("killclient")
    until(lambda: ids[1] not in state(), "middle terminal did not close")
    time.sleep(.15)
    current = state()
    check("closing active workspace 2 keeps occupied workspace 3 numbered 3",
          tags() == {ids[0]: [1], ids[2]: [3]} and active() == [2])
    check("empty active workspace keeps every remaining overview card stationary",
          all(preview(c) == before_preview[cid] for cid, c in current.items()))
    time.sleep(.25)
    check("remaining on the empty workspace does not trigger delayed compaction",
          tags() == {ids[0]: [1], ids[2]: [3]} and active() == [2])

    # Refill and empty the same workspace: it remains a normal native workspace.
    spawn("retained-empty-refill")
    replacement = selected()["id"]
    check("refilling an active empty workspace keeps its native identity",
          state()[replacement]["tags"] == [2] and active() == [2])
    command("killclient")
    until(lambda: replacement not in state(), "refill terminal did not close")

    # Reverse before reaching 3: empty 2 must survive cancellation/retargeting.
    began = time.monotonic()
    command("view,3")
    at(began, .35)
    check("departing empty workspace survives intermediate switch frames",
          tags() == {ids[0]: [1], ids[2]: [3]} and active() == [3])
    command("view,2")
    time.sleep(duration + .10)
    check("returning to the empty workspace cancels its removal",
          tags() == {ids[0]: [1], ids[2]: [3]} and active() == [2])

    # Camera finishes first; a subsequently started zoom animation still owns
    # the coordinate system, so compaction must wait for that too.
    began = time.monotonic()
    command("view,3")
    at(began, .20)
    command("leaveoverview")
    at(began, .45)
    command("enteroverview")
    at(began, 1.12)
    check("workspace compaction waits for overview zoom after travel finishes",
          tags() == {ids[0]: [1], ids[2]: [3]})
    until(lambda: tags().get(ids[2]) == [2], "empty workspace never compacted")
    until(lambda: preview(state()[ids[0]])[1] == before_preview[ids[0]][1],
          "workspace above the removed gap did not close toward the anchor")
    after = state()
    check("settled departure removes the old empty slot and follows destination identity",
          tags() == {ids[0]: [1], ids[2]: [2]} and active() == [2]
          and selected()["id"] == ids[2])
    monitor = msg("get", "all-monitors")["monitors"][0]
    target = after[ids[2]]
    expected = (round(monitor["x"] + monitor["width"] * .25 +
                      (target["x"] - monitor["x"]) * .5),
                round(monitor["y"] + monitor["height"] * .25 +
                      (target["y"] - monitor["y"]) * .5),
                round(target["width"] * .5), round(target["height"] * .5))
    check("compaction leaves the focused destination centered at its settled preview",
          all(abs(a - b) <= 1 for a, b in zip(preview(target), expected)))
    check("the workspace above the removed gap moves toward the anchored destination",
          preview(after[ids[0]])[1] == before_preview[ids[0]][1])
    check("deferred compaction preserves native window geometry",
          all(native(c) == before_native[cid] for cid, c in after.items()))
    time.sleep(.15)
    check("compaction does not start a second animation on the focused window",
          preview(state()[ids[2]]) == preview(target))

    # Repeat in the ordinary desktop: target tags stay stable for the whole
    # switch and renumber only at completion, without replaying a traversal.
    option("animations", 0)
    dispatch("leaveoverview")
    dispatch("view,1")
    option("animations", 1)
    command("killclient")
    until(lambda: ids[0] not in state(), "native terminal did not close")
    check("native empty workspace 1 stays until departure",
          tags() == {ids[2]: [2]} and active() == [1])
    began = time.monotonic()
    command("view,2")
    at(began, .65)
    check("native departure does not renumber the target mid-animation",
          tags() == {ids[2]: [2]} and active() == [2])
    until(lambda: tags().get(ids[2]) == [1], "native compaction was not retried at completion")
    check("native destination follows its identity after compaction",
          active() == [1] and selected()["id"] == ids[2]
          and native(state()[ids[2]]) == before_native[ids[2]])
