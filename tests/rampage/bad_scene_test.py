"""A Gameplay scene that does not load takes the fault path (review fix F2).

rampage.bad_scene_faults (docs/milestone-specs.md M13a: "A failed load of a
state's text is a boot or state error: the state is not created, the error
is logged with its path, line and column, and the game takes the fault
path"): rampage runs headless on a copy of the content with
--start-state gameplay and a --scene that is read but does not load:

  parse       not JSON;
  component   an entity whose overrides name an unknown component;
  prefab      an entity whose prefab file does not exist.

Each run must log "state 'gameplay' cannot be created" with the path of the
file at fault (the scene, or the missing prefab), its line and column, never enter another state (GameOver), and exit with
a failure status. No real input; the audio is off; no message box.

usage: bad_scene_test.py --rampage EXE --content ROOT [--out DIR]
Python 3, stdlib only.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

GUID = "5a4db0c5-0000-4000-8000-0000000000f2"
SCENES = {
    "parse": '{"format": "se-scene-v1", "entities": [}\n',
    "component": (
        '{"format": "se-scene-v1", "name": "bad", "entities": [\n'
        f'  {{"guid": "{GUID}", "prefab": "prefabs/enemy.prefab.json",\n'
        '   "overrides": {"NoSuchComponent": {}}}]}\n'),
    "prefab": (
        '{"format": "se-scene-v1", "name": "bad", "entities": [\n'
        f'  {{"guid": "{GUID}", "prefab": "prefabs/no_such.prefab.json"}}]}}\n'),
}

# The file each error names: the scene, or the prefab it needs (the lane
# reads it as the scene's dependency).
BLAMED = {"prefab": "no_such.prefab.json"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rampage", required=True)
    parser.add_argument("--content", required=True)
    parser.add_argument("--out")
    args = parser.parse_args()

    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    env["SE_AUDIO"] = "null"
    env.setdefault("SDL_ASSERT", "abort")
    if args.out:
        work = pathlib.Path(args.out)
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
    else:
        work = pathlib.Path(tempfile.mkdtemp(prefix="se_bad_scene_"))
    content = work / "content"
    for part in ("assets", "config"):
        shutil.copytree(pathlib.Path(args.content) / part, content / part)

    failures = []
    for name, text in SCENES.items():
        rel = f"assets/scenes/bad_{name}.scene.json"
        (content / rel).write_text(text, encoding="utf-8")
        done = subprocess.run(
            [args.rampage, "--headless", "--audio", "none",
             "--fixed-frame-dt", "8.3333", "--frames", "600",
             "--no-crash-capture", "--pref-dir", str(work / f"pref_{name}"),
             "--content-root", str(content), "--", "--start-state",
             "gameplay", "--scene", rel],
            capture_output=True, text=True, env=env, timeout=120,
            errors="replace")
        out = done.stdout + done.stderr
        print(f"--- {name}: exit {done.returncode}")
        print("\n".join(out.splitlines()[-12:]))
        if done.returncode == 0:
            failures.append(f"{name}: rampage exited 0")
        line = re.search(r"state 'gameplay' cannot be created: .*\((\S*)"
                         r":(\d+):(\d+)\)", out)
        if line is None:
            failures.append(f"{name}: no \"state 'gameplay' cannot be "
                            "created\" line with a path, line and column")
        elif not line.group(1).endswith(BLAMED.get(
                name, f"bad_{name}.scene.json")):
            failures.append(f"{name}: the error names {line.group(1)!r}")
        elif name == "parse" and int(line.group(2)) < 1:
            failures.append(f"{name}: the parse error has no line")
        if "flow: entered" in out:
            failures.append(f"{name}: a state was entered after the error")
    for failure in failures:
        print("FAIL:", failure)
    if failures:
        print(f"files kept in {work}")
        return 1
    print(f"rampage.bad_scene_faults: passed ({len(SCENES)} scenes)")
    if not args.out:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
