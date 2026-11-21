"""Occupied-workspace numbering regressions; run via overview.py --compaction-only."""
import time


def run_compaction(msg, dispatch, clients, selected, check, spawn):
    def geometry(c):
        return tuple(c[key] for key in ("x", "y", "width", "height"))

    def by_id():
        return {c["id"]: c for c in clients()}

    def monitor():
        return msg("get", "all-monitors")["monitors"][0]

    def active():
        return [t["index"] for t in monitor()["tags"] if t["is_active"]]

    def tags():
        return {c["id"]: c["tags"] for c in clients()}

    def close_selected():
        target = selected()["id"]
        msg("dispatch", "killclient")
        for _ in range(100):
            if target not in by_id():
                break
            time.sleep(.02)
        check("closing removes the selected native window", target not in by_id())
        time.sleep(.2)

    ids = sorted(by_id())
    dispatch("setoption,animations,0")

    def seed(numbers, current):
        dispatch("setoption,tag_gather,0")
        for cid, number, layout in zip(ids, numbers, ("grid", "dwindle", "scroller")):
            dispatch(f"client,{cid},tagsilent,{number}")
            dispatch(f"view,{number}")
            dispatch("setlayout," + layout)
        dispatch(f"view,{current}")

    seed((1, 3, 9), 9)
    native = {cid: geometry(c) for cid, c in by_id().items()}
    layouts = [monitor()["tags"][number - 1]["layout"] for number in (1, 3, 9)]
    dispatch("setoption,tag_gather,2")
    check("sparse occupied workspaces become 1, 2, 3",
          tags() == dict(zip(ids, ([1], [2], [3]))))
    check("active occupied workspace follows its original window",
          active() == [3] and selected().get("id") == ids[2])
    check("compaction preserves occupied workspace layouts",
          [t["layout"] for t in monitor()["tags"][:3]] == layouts)
    check("compaction preserves native window geometry",
          {cid: geometry(c) for cid, c in by_id().items()} == native)

    # Deliberately put client IDs out of workspace order: numbering follows
    # native workspace identity, not focus history or client iteration order.
    sparse_numbers = (8, 2, 5)
    packed = dict(zip(ids, ([3], [1], [2])))
    for current in (2, 5, 8):
        seed(sparse_numbers, current)
        check(f"mode 0 preserves sparse identities while viewing {current}",
              tags() == {cid: [number] for cid, number in zip(ids, sparse_numbers)}
              and active() == [current])
        selected_id = selected().get("id")
        expected_active = packed[ids[sparse_numbers.index(current)]]
        dispatch("setoption,tag_gather,2")
        check(f"workspace order is independent of current workspace {current}",
              tags() == packed and active() == expected_active)
        check(f"compacting current workspace {current} preserves selection",
              selected().get("id") == selected_id)

    packed_layouts = [t["layout"] for t in monitor()["tags"][:3]]
    in_overview = False
    for overview in (False, True, False):
        if overview != in_overview:
            dispatch("toggleoverview" if overview else "leaveoverview")
            in_overview = overview
        context = "overview" if overview else "native"
        for target in (3, 1, 2, 3, 2, 1):
            dispatch(f"view,{target}")
            check(f"{context} view {target} does not reorder occupied identities",
                  tags() == packed and active() == [target]
                  and selected().get("id") == next(
                      cid for cid, numbers in packed.items() if numbers == [target]))
            check(f"{context} view {target} keeps layouts with their workspace",
                  [t["layout"] for t in monitor()["tags"][:3]] == packed_layouts)

    seed((1, 3, 9), 9)
    dispatch("setoption,tag_gather,2")

    dispatch("view,1")
    dispatch("view_insert,next")
    check("explicit insertion retains its active empty workspace",
          tags() == dict(zip(ids, ([1], [3], [4]))) and active() == [2])

    seed((1, 3, 9), 9)
    dispatch("toggleoverview")
    dispatch("setoption,tag_num,3")
    dispatch("setoption,tag_gather,2")
    check("shrinking tag count in overview preserves and compacts live windows",
          tags() == dict(zip(ids, ([1], [2], [2]))))
    dispatch("leaveoverview")
    dispatch("setoption,tag_num,9")

    seed((2, 4, 8), 1)
    dispatch("setoption,tag_gather,2")
    check("active empty workspace remains in its relative position",
          tags() == dict(zip(ids, ([2], [3], [4]))) and active() == [1])
    check("empty active workspace has no application selection",
          selected().get("id") is None)

    seed((2, 4, 8), 1)
    dispatch("setoption,tag_gather,1")
    check("legacy gathering retains an active empty leading workspace",
          tags() == dict(zip(ids, ([2], [3], [4]))) and active() == [1])
    seed((1, 2, 3), 2)
    dispatch("setoption,tag_gather,2")
    dispatch("toggleoverview")
    remaining = {cid: geometry(c) for cid, c in by_id().items() if cid != ids[1]}
    close_selected()
    check("overview retains an empty middle workspace while it remains active",
          tags() == {ids[0]: [1], ids[2]: [3]} and active() == [2])
    check("overview keeps previews attached to the remaining native windows",
          all("overview_geometry" in c for c in clients()))
    dispatch("leaveoverview")
    check("leaving overview does not remove the still-active empty workspace",
          tags() == {ids[0]: [1], ids[2]: [3]} and active() == [2])
    dispatch("view,3")
    check("leaving the empty slot compacts around the destination identity",
          tags() == {ids[0]: [1], ids[2]: [2]} and active() == [2]
          and selected().get("id") == ids[2])
    check("overview compaction preserves native layout on exit",
          {cid: geometry(c) for cid, c in by_id().items()} == remaining)

    dispatch("view,1")
    close_selected()
    check("closing workspace 1 retains it while the remaining workspace stays 2",
          tags() == {ids[2]: [2]} and active() == [1])
    spawn()
    check("new windows refill the retained active empty workspace",
          sorted(c["tags"] for c in clients()) == [[1], [2]])
    while clients():
        dispatch(f"view,{min(c['tags'][0] for c in clients())}")
        close_selected()
    check("closing all windows leaves workspace 1 available", active() == [1])

    dispatch("setoption,tag_num,3")
    full = {}
    for number in (1, 2, 3):
        dispatch(f"view,{number}")
        spawn()
        full[selected()["id"]] = [number]
    check("a full tag set keeps all occupied workspaces distinct",
          tags() == full and active() == [3])
    close_selected()
    check("closing the last bounded workspace leaves a valid trailing slot",
          sorted(c["tags"] for c in clients()) == [[1], [2]] and active() == [3])
    while clients():
        dispatch(f"view,{min(c['tags'][0] for c in clients())}")
        close_selected()
    dispatch("setoption,tag_num,9")

    dispatch("setoption,tag_gather,0")
    spawn()
    original = selected()["id"]
    dispatch("tagsilent,7")
    check("disabled gathering keeps explicitly assigned workspace numbers",
          tags() == {original: [7]} and active() == [1])
