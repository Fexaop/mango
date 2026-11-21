"""Protocol-visible workspace order must survive zoom and repeated focus changes."""
import json
from pathlib import Path
import select
import shlex
import subprocess


def run_workspace_protocol(runtime, env, msg, dispatch, clients, check):
    xml_dir = subprocess.check_output(
        ["pkg-config", "--variable=pkgdatadir", "wayland-protocols"], text=True).strip()
    xml = Path(xml_dir) / "staging/ext-workspace/ext-workspace-v1.xml"
    header, source = runtime / "workspace-client-protocol.h", runtime / "workspace-protocol.c"
    subprocess.run(["wayland-scanner", "client-header", str(xml), str(header)], check=True)
    subprocess.run(["wayland-scanner", "private-code", str(xml), str(source)], check=True)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "wayland-client"], text=True))
    binary = runtime / "workspace-events"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter",
                    "-I", str(runtime), str(Path(__file__).with_name("workspace-events.c")),
                    str(source), "-o", str(binary), *flags], check=True)
    observer = subprocess.Popen([str(binary)], env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)

    def request(command="snapshot"):
        observer.stdin.write(command + "\n")
        observer.stdin.flush()
        assert select.select([observer.stdout], [], [], 5)[0], "workspace observer timed out"
        line = observer.stdout.readline()
        assert line, "workspace observer disconnected"
        return json.loads(line)

    def identities(state):
        return [(w["name"], w["id"]) for w in state]

    try:
        dispatch("setoption,animations,0")
        dispatch("setoption,tag_gather,0")
        ids = sorted(c["id"] for c in clients())
        for tag, cid in enumerate(ids, 1):
            dispatch(f"client,{cid},tagsilent,{tag}")
        dispatch("view,3")
        dispatch("setoption,tag_gather,2")
        original = request()
        check("protocol lists numeric workspace order, not current workspace first",
              [w["name"] for w in original] == [str(i) for i in range(1, 10)]
              and [w["position"] for w in original] == list(range(9))
              and [w["name"] for w in original if w["active"]] == ["3"])
        dispatch("enteroverview")
        opened = request()
        check("overview zoom retains native protocol workspace identities",
              identities(opened) == identities(original)
              and [w["name"] for w in opened if w["active"]] == ["3"])
        for tag in (1, 3, 2, 1, 2):
            dispatch(f"view,{tag}")
        focused = request()
        check("repeated overview focus neither rotates nor duplicates workspace handles",
              identities(focused) == identities(original)
              and [w["name"] for w in focused if w["active"]] == ["2"])
        request("activate 1")
        activated = request()
        check("external workspace activation selects native overview workspace 1",
              [w["name"] for w in activated if w["active"]] == ["1"]
              and msg("get", "focusing-client")["id"] == ids[0])
        dispatch("leaveoverview")
        check("leaving overview preserves ordered unique workspace handles",
              identities(request()) == identities(original))
    finally:
        observer.stdin.close()
        if observer.poll() is None:
            observer.terminate()
        observer.wait(timeout=5)
