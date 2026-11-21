"""Trailing empty workspace numbers alias one native workspace before travel."""
import time


def run_aliases(msg, dispatch, clients, selected, check, spawn):
    def command(value):
        return msg("dispatch", value)

    def active():
        return [t["index"] for t in msg("get", "all-monitors")["monitors"][0]["tags"]
                if t["is_active"]]

    def tags():
        return {c["id"]: c["tags"] for c in clients()}

    def at(start, seconds):
        time.sleep(max(0, start + seconds - time.monotonic()))

    def camera():
        windows = {c["tags"][0]: c for c in clients()}
        first, last = windows[1], windows[4]
        a, b = first["overview_geometry"], last["overview_geometry"]
        monitor = msg("get", "all-monitors")["monitors"][0]
        zoom = a["width"] / first["width"]
        origin = a["y"] - (first["y"] - monitor["y"]) * zoom
        end = b["y"] - (last["y"] - monitor["y"]) * zoom
        stride = (end - origin) / 3
        center = monitor["y"] + monitor["height"] * (1 - zoom) / 2
        return (center - origin) / stride

    dispatch("setoption,animations,0")
    dispatch("setoption,tag_gather,0")
    spawn("fourth-occupied-workspace")
    ids = sorted(c["id"] for c in clients())
    for tag, cid in enumerate(ids, 1):
        dispatch(f"client,{cid},tagsilent,{tag}")
        dispatch(f"view,{tag}")
        dispatch("setlayout,scroller")
    dispatch("view,1")
    dispatch("setoption,tag_gather,2")
    original = tags()
    dispatch("setoption,animation_duration_tag,1200")
    dispatch("setoption,animation_curve_tag,0,0,1,1")
    dispatch("setoption,tag_animation_direction,0")
    dispatch("setoption,tag_animation_continuous,1")
    dispatch("setoption,animations,1")
    began = time.monotonic()
    command("view,9")
    check("requesting empty workspace 9 immediately targets first empty workspace 5",
          active() == [5])
    at(began, .25)
    command("view,6")
    check("native aliases keep the same logical empty destination", active() == [5])
    command("enteroverview")
    at(began, .55)
    check("repeated native alias requests neither snap nor restart camera travel",
          abs(camera() - 4 * (time.monotonic() - began) / 1.2) < .15)
    command("view,7")
    at(began, .83)
    check("overview aliases preserve ongoing camera travel",
          active() == [5] and abs(camera() - 4 * (time.monotonic() - began) / 1.2) < .15)
    at(began, 1.32)
    check("aliased travel retains its original deadline and occupied identities",
          abs(camera() - 4) < .04 and tags() == original)

    dispatch("setoption,animations,0")
    for target in (5, 6, 9):
        dispatch(f"view,{target}")
        check(f"overview request {target} selects the same empty workspace 5",
              active() == [5])
    dispatch("leaveoverview")
    dispatch("viewtoright,0")
    check("native next-workspace cannot go beyond the empty trailing slot", active() == [5])
    spawn("fills-workspace-five")
    fifth = selected()["id"]
    check("new windows fill the aliased workspace 5", tags()[fifth] == [5])
    dispatch("view,9")
    check("workspace 6 becomes available once workspace 5 is occupied", active() == [6])
    dispatch("enteroverview")
    spawn("fills-workspace-six")
    sixth = selected()["id"]
    check("overview creation also fills the resolved native workspace", tags()[sixth] == [6])
    dispatch("view,9")
    check("the next empty alias advances as occupied workspaces grow", active() == [7])
    dispatch("leaveoverview")
    dispatch("setoption,tag_num,6")
    dispatch("view,6")
    check("a fully occupied tag range keeps its last workspace distinct",
          active() == [6] and tags()[sixth] == [6])
    dispatch("setoption,tag_num,9")

    # An empty slot retained in the middle is still a distinct workspace.
    dispatch("view,2")
    command("killclient")
    until = time.monotonic() + 3
    while ids[1] in tags() and time.monotonic() < until:
        time.sleep(.02)
    check("trailing aliases do not replace a retained empty middle workspace",
          ids[1] not in tags() and active() == [2])
    spawn("refills-middle-workspace")
    check("refilling a middle empty workspace keeps its original identity",
          selected()["tags"] == [2])

    dispatch("setoption,tag_gather,0")
    dispatch("setoption,animations,1")
    command("view,9")
    check("disabled gathering retains fixed numeric request semantics", active() == [9])
    dispatch("setoption,animations,0")
    dispatch("view,1")

    for cid in tags():
        command(f"client,{cid},killclient")
    until = time.monotonic() + 3
    while clients() and time.monotonic() < until:
        time.sleep(.02)
    dispatch("setoption,tag_gather,2")
    command("view,9")
    check("with no windows every trailing number aliases empty workspace 1",
          not clients() and active() == [1])
