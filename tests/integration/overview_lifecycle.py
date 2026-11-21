"""Window lifetime and selection regressions for the zoomed overview."""
import time


def run_lifecycle(msg, dispatch, clients, selected, check, spawn, layouts):
    def close_selected():
        before = {c["id"] for c in clients()}
        target = selected().get("id")
        check("overview selection names a live window", target in before)
        msg("dispatch", "killclient")
        for _ in range(50):
            if target not in {c["id"] for c in clients()}:
                break
            time.sleep(.02)
        check("close removes the selected window", target not in {c["id"] for c in clients()})
        time.sleep(.2)

    for layout in layouts:
        while len(clients()) < 3:
            spawn()
        for c in clients():
            dispatch(f"client,{c['id']},tagsilent,3")
        dispatch("view,3")
        dispatch("setlayout," + layout)
        # Reproduce entry on an empty tag, then closing every window on
        # another tag. Native unmap must agree with overview selection.
        dispatch("view,1")
        dispatch("toggleoverview")
        dispatch("view,3")
        for _ in range(3):
            close_selected()
        check(layout + " closes through the last window", not clients()
              and selected().get("id") is None)
        spawn()
        check(layout + " opens on the active empty overview workspace",
              selected().get("tags") == [3] and "overview_geometry" in selected())
        close_selected()
        dispatch("leaveoverview")

    dispatch("view,3")
    spawn()
    original = selected()["id"]
    background = spawn()
    dispatch("toggleoverview")
    dispatch("tagsilent,5")
    check("silent transfer selects a remaining native window", selected()["id"] == original)
    background.terminate()
    background.wait(timeout=3)
    time.sleep(.3)
    check("background close preserves active overview selection", selected()["id"] == original)
    dispatch("tagsilent,4")
    check("silent transfer leaves an empty workspace unselected", selected().get("id") is None)
    dispatch("view,4")
    check("transferred last window is selectable on destination", selected()["id"] == original)

    dispatch("minimized")
    check("minimizing last window clears selection", selected().get("id") is None
          and clients()[0]["is_minimized"])
    dispatch("view,6")
    dispatch("restore_minimized")
    check("restore follows current native workspace", selected()["id"] == original
          and selected()["tags"] == [6] and "overview_geometry" in selected())
    dispatch("leaveoverview")
    dispatch("minimized")
    dispatch("toggleoverview")
    dispatch("restore_minimized")
    check("window minimized before entry gets a live preview", selected()["id"] == original
          and "overview_geometry" in selected())
    dispatch("tag,7")
    check("normal tag transfer follows the selected window", selected()["id"] == original
          and selected()["tags"] == [7])
    check("lifecycle commands keep application input blocked", all(
        not c["has_keyboard_focus"] and not c["has_pointer_focus"] for c in clients()))
    close_selected()
    spawn("overview-rule-test")
    check("new preview preserves native window tag rules", clients()[0]["tags"] == [8]
          and "overview_geometry" in clients()[0] and selected().get("id") is None)
    dispatch("view,8")
    close_selected()

    spawn()
    spawn()
    dispatch("setlayout,monocle")
    dispatch("groupjoin,right")
    # IPC includes parked members; only the active group member gets a card.
    check("native window group replaces previews", len(clients()) == 2
          and selected()["is_group"] and "overview_geometry" in selected()
          and sum("overview_geometry" in c for c in clients()) == 1)
    grouped = selected()["id"]
    dispatch("groupfocus,next")
    check("group focus resolves a live replacement window", selected()["id"] != grouped
          and "overview_geometry" in selected())
    dispatch("groupleave")
    check("leaving group restores both live previews", len(clients()) == 2
          and all("overview_geometry" in c for c in clients()))
    dispatch("groupjoin,right")
    close_selected()
    check("closing group member selects its live replacement", len(clients()) == 1
          and selected().get("id") == clients()[0]["id"]
          and "overview_geometry" in selected())
    close_selected()
    dispatch("leaveoverview")
