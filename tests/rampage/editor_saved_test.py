"""rampage plays a scene the editor saved (M11/M13 integration).

rampage.plays_an_editor_saved_scene (docs/milestone-specs.md "Integration
(M11a + M11b + M13a + M13b)"): simple_editor runs
tests/editor/scripts/saved_for_rampage.editor headless on a copy of the
content: a Stress generator press of 20 enemy-prefab instances, the scene
saved with prefab references, a short Play. Then

  1. se_replay writes the Play session's start bytes (the document as Play
     serialized it, flattened) from the editor's replay and runs the
     "gameplay" state on them for 600 ticks without input;
  2. rampage runs headless on the saved file (--start-state gameplay
     --scene), its scene and prefabs through the background lane, and
     records its session (--record);
  3. se_replay replays that recording from its content bundle (the
     recorded hashes are checked tick by tick) and writes its hashes.

The first 600 tick hashes of 1 and 3 must be equal: the runtime's World
from the prefab references is the editor's from the same document. No
real input; the audio is off.

usage: editor_saved_test.py --editor EXE --rampage EXE --se-replay EXE
                            --content ROOT --script FILE [--out DIR]
Python 3, stdlib only.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

TICKS = 600


def run(cmd, env, timeout=180):
    done = subprocess.run(cmd, capture_output=True, text=True, env=env,
                          timeout=timeout, errors="replace")
    return done.returncode, done.stdout + done.stderr


def read_hashes(path):
    hashes = []
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2:
            hashes.append(parts[1])
    return hashes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--rampage", required=True)
    parser.add_argument("--se-replay", required=True)
    parser.add_argument("--content", required=True)
    parser.add_argument("--script", required=True)
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
        work = pathlib.Path(tempfile.mkdtemp(prefix="se_saved_"))
    content = work / "content"
    for part in ("assets", "config"):
        shutil.copytree(pathlib.Path(args.content) / part, content / part)
    scene_rel = "assets/scenes/sandbox.scene.json"
    failures = []
    outputs = []
    instances = 0

    # The editor: the press, the save (kPrefabs), a short Play.
    code, out = run([args.editor, "--pref-dir", str(work / "editor-pref"),
                     "--no-crash-capture", "--audio", "null",
                     "--content-root", str(content), "--headless",
                     "--fixed-frame-dt", "8.3333", "--",
                     "--editor-script",
                     str(pathlib.Path(args.script).resolve())], env)
    outputs.append(out)
    if code != 0 or "editor script: passed" not in out:
        failures.append(f"simple_editor: exit {code}, script not passed")
    saved = (content / scene_rel).read_text(encoding="utf-8")
    instances = saved.count("prefabs/enemy.prefab.json")
    if instances < 36:
        failures.append(f"the saved scene has {instances} enemy-prefab "
                        "references, expected 36 or more")
    if '"components"' in saved:
        failures.append("the saved scene writes whole components")
    replay = work / "editor-pref" / "sessions" / "last-play.sereplay"
    if not replay.exists():
        failures.append("the editor wrote no last-play.sereplay")

    editor_hashes = []
    if not failures:
        # 1. The Play-start bytes, then the gameplay state on them.
        start = work / "play-start.scene.json"
        code, out = run([args.se_replay, "--module", "rampage", "--inputs",
                         str(replay), "--start-scene", str(start)], env)
        outputs.append(out)
        if code != 0 or not start.exists():
            failures.append(f"se_replay --start-scene: exit {code}")
        else:
            hashes = work / "editor.hashes"
            code, out = run([args.se_replay, "--module", "rampage",
                             "--start-state", "gameplay", "--scene",
                             str(start), "--root", str(content), "--seed",
                             "1", "--ticks", str(TICKS), "--hash-out",
                             str(hashes)], env)
            outputs.append(out)
            if code != 0:
                failures.append("se_replay on the Play-start bytes: exit "
                                f"{code}")
            else:
                editor_hashes = read_hashes(hashes)

    runtime_hashes = []
    if not failures:
        # 2. rampage on the saved file, recorded.
        record = work / "rampage.sereplay"
        code, out = run([args.rampage, "--headless", "--audio", "none",
                         "--fixed-frame-dt", "8.3333", "--frames",
                         str(TICKS + 200), "--no-crash-capture",
                         "--pref-dir", str(work / "rampage-pref"),
                         "--content-root", str(content), "--record",
                         str(record), "--", "--start-state", "gameplay",
                         "--scene", scene_rel], env)
        outputs.append(out)
        if code != 0 or not record.exists():
            failures.append(f"rampage: exit {code}, recorded "
                            f"{record.exists()}")
        if "cannot be created" in out:
            failures.append("rampage could not create the gameplay state")
        # 3. Its recording replayed from its content bundle.
        if not failures:
            hashes = work / "runtime.hashes"
            code, out = run([args.se_replay, "--module", "rampage",
                             "--inputs", str(record), "--scene", scene_rel,
                             "--hash-out", str(hashes)], env)
            outputs.append(out)
            if code != 0 or "diverges" in out:
                failures.append("se_replay of rampage's recording: exit "
                                f"{code}")
            elif "on the replay's content bundle" not in out:
                failures.append("rampage's recording is not a content "
                                "bundle")
            else:
                runtime_hashes = read_hashes(hashes)

    if not failures:
        if len(editor_hashes) < TICKS or len(runtime_hashes) < TICKS:
            failures.append(f"{len(editor_hashes)} and "
                            f"{len(runtime_hashes)} hashes, expected "
                            f"{TICKS} each")
        else:
            for tick in range(TICKS):
                if editor_hashes[tick] != runtime_hashes[tick]:
                    failures.append(f"tick {tick + 1}: the editor's "
                                    f"{editor_hashes[tick]}, rampage's "
                                    f"{runtime_hashes[tick]}")
                    break
    for out in outputs:
        lines = out.splitlines()
        sys.stdout.write("\n".join(lines[-25:]) + "\n")
    for failure in failures:
        print("FAIL:", failure)
    if failures:
        print(f"files kept in {work}")
        return 1
    print(f"rampage.plays_an_editor_saved_scene: passed ({TICKS} equal tick "
          f"hashes, {instances} prefab references)")
    if not args.out:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
