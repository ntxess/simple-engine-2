"""rampage's game flow through the executable (docs/milestone-specs.md M13b).

The keys are pushed with the --push-key test hook (SDL events, never OS
input) at fixed iterates, on the doomed scene (the player dies about a
second into the Gameplay): Enter at the Title, Escape (Pause), Escape
(Gameplay again), the player's death (GameOver), Escape (the Title).

--mode headless (rampage.headless_flow): rampage --headless records the
  session (--record); the log must show the states in that order, and
  se_replay must reproduce every recorded hash from the replay's content
  bundle (its start bytes, M13a), starting at the Title. The run uses a
  content root of its own whose assets/scenes/sandbox.scene.json is the
  doomed scene: se_replay's bundle run uses rampage's default Gameplay
  scene path (it takes no --scene for the flow).
--mode desktop (rampage.desktop.flow): the same in an unfocused window
  (--no-activate), with a --readback of the composite in each state: the
  Title's, the Pause's and the GameOver's images differ from the
  Gameplay's and from each other.

usage:
  flow_app_test.py --rampage EXE --mode headless|desktop --out DIR
                   [--se-replay EXE] [--timeout S]

Python 3, stdlib only. The children run with SE_NO_MESSAGE_BOX=1 and
SE_AUDIO=null (nothing is ever heard).
"""

import argparse
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import zlib

REPO = pathlib.Path(__file__).resolve().parents[2]
DOOMED = "tests/fixtures/flow/doomed.scene.json"

# Headless iterates take microseconds while the lane takes a fraction of a
# millisecond to load a state's texts (the ticks hold meanwhile), so the
# keys are far apart, except Enter and the first Escape: the doomed player
# dies about 128 Gameplay ticks in, and the Pause must come before that
# (an Escape that arrives while the Gameplay waits at the gate lands on its
# first tick). Iterates at which a key goes down (it comes up 4 later).
HEADLESS_KEYS = [(5000, "enter"), (5040, "esc"), (10000, "esc"),
                 (15000, "esc")]
HEADLESS_FRAMES = 20000
# In a window the iterates follow the display, so the holds are a frame or
# two: Enter, Escape (Pause), Escape (Gameplay again), the death about 90
# Gameplay ticks later, Escape at the GameOver (the Title).
DESKTOP_KEYS = [(60, "enter"), (100, "esc"), (200, "esc"), (400, "esc")]
DESKTOP_FRAMES = 520
# The state on top at each readback (desktop), by iterate.
READBACKS = [(40, "title"), (85, "gameplay"), (150, "pause"),
             (360, "game_over")]
# The flow's log lines, in order.
SEQUENCE = [
    "flow: entered 'title'",
    "flow: entered 'gameplay'",
    "flow: entered 'pause'",
    "flow: resumed 'gameplay'",
    "flow: entered 'game_over'",
    "flow: entered 'title'",
]


def headless_content_root(out):
    """A content root whose default Gameplay scene is the doomed scene.

    se_replay replays a bundle with rampage's default flow configuration,
    whose Gameplay scene is assets/scenes/sandbox.scene.json: the session
    must load the doomed scene under that path for its replay to find it.
    """
    root = out / "content"
    if root.exists():
        shutil.rmtree(root)
    (root / "config").mkdir(parents=True)
    for name in ("texture.json", "config.json", "sound.json"):
        shutil.copy(REPO / "config" / name, root / "config" / name)
    shutil.copytree(REPO / "assets" / "prefabs", root / "assets" / "prefabs")
    scenes = root / "assets" / "scenes"
    scenes.mkdir(parents=True)
    for name in ("title", "pause", "game_over"):
        shutil.copy(REPO / "assets" / "scenes" / f"{name}.scene.json",
                    scenes / f"{name}.scene.json")
    shutil.copy(REPO / DOOMED, scenes / "sandbox.scene.json")
    return root


def run(command, timeout):
    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    env["SE_AUDIO"] = "null"
    print("running:", " ".join(str(c) for c in command))
    result = subprocess.run([str(c) for c in command], capture_output=True,
                            text=True, errors="replace", timeout=timeout,
                            env=env, check=False)
    output = (result.stdout or "") + (result.stderr or "")
    print(output)
    return result.returncode, output


def png_pixels(path):
    """The decompressed image data of a PNG (its IDAT chunks)."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")
    pos = 8
    idat = b""
    header = None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            header = body
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    return header, zlib.decompress(idat)


def check_sequence(output, failures):
    lines = [line for line in output.splitlines() if "flow: " in line]
    found = []
    for line in lines:
        match = re.search(r"flow: (entered|resumed) '([a-z_]+)'", line)
        if match:
            found.append(f"flow: {match.group(1)} '{match.group(2)}'")
    print("state sequence:", found)
    if found != SEQUENCE:
        failures.append(f"state sequence {found}, expected {SEQUENCE}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rampage", required=True)
    parser.add_argument("--se-replay")
    parser.add_argument("--mode", choices=["headless", "desktop"],
                        required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()
    args.rampage = str(pathlib.Path(args.rampage).resolve())
    if args.se_replay is not None:
        args.se_replay = str(pathlib.Path(args.se_replay).resolve())

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    replay = out / "flow.sereplay"
    shots = {state: out / f"{frame}_{state}.png"
             for frame, state in READBACKS}
    for stale in [replay, *shots.values()]:
        if stale.exists():
            stale.unlink()

    if args.mode == "headless":
        keys, frames = HEADLESS_KEYS, HEADLESS_FRAMES
        command = [args.rampage, "--headless", "--audio", "none",
                   "--content-root", headless_content_root(out),
                   "--record", replay]
    else:
        keys, frames = DESKTOP_KEYS, DESKTOP_FRAMES
        command = [args.rampage, "--no-activate", "--audio", "null"]
        for frame, state in READBACKS:
            command += ["--readback", f"{frame}:{shots[state]}"]
    command += ["--frames", str(frames), "--fixed-frame-dt", "8.3333",
                "--return-on-quit", "--no-crash-capture", "--pref-dir",
                out / "pref"]
    for frame, key in keys:
        command += ["--push-key", f"{frame}:2:{key}",
                    "--push-key", f"{frame + 4}:2:{key}:up"]
    if args.mode == "desktop":
        command += ["--", "--scene", DOOMED]
    status, output = run(command, args.timeout)

    failures = []
    if status != 0:
        failures.append(f"rampage exited with {status}")
    check_sequence(output, failures)

    if args.mode == "headless":
        if "replay of " not in output or not replay.exists():
            failures.append("rampage wrote no replay")
        elif args.se_replay is not None:
            status, replayed = run([args.se_replay, "--module", "rampage",
                                    "--inputs", replay], args.timeout)
            if status != 0:
                failures.append(f"se_replay exited with {status} (a hash "
                                f"mismatch prints the tick)")
            elif not re.search(r"from state 'title' on the replay's "
                               r"content bundle", replayed):
                failures.append("se_replay did not run the bundle from "
                                "the Title")
            else:
                check_sequence(replayed, failures)
    else:
        images = {}
        for state, path in shots.items():
            if not path.exists():
                failures.append(f"no readback of the {state} ({path})")
                continue
            images[state] = png_pixels(path)
        if len(images) == len(shots):
            states = list(images)
            for i, a in enumerate(states):
                for b in states[i + 1:]:
                    if images[a][1] == images[b][1]:
                        failures.append(f"the {a} and the {b} look the same")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print(f"ok: the flow ran {' -> '.join(SEQUENCE)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
