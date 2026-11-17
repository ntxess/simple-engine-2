"""Runs simple_editor with an editor script and checks the run (M10a).

The editor's script tests: simple_editor runs a script of editor actions
(--editor-script, se_editor/editor_script.h) at frame boundaries, headless
(SDL's dummy video, a synthetic 120 Hz frame clock) or in a window that is
shown without taking focus; no test uses real input. The run passes when
the editor exits with status 0 and logged "editor script: passed".

usage:
  run_editor.py --editor EXE --script FILE [--headless] [--scene-copy SRC]
                [--then-script FILE] [--replay SE_REPLAY]
                [--expect TEXT]... [--expect-scene-contains TEXT]
                [--expect-scene-unchanged] [--expect-recovery-file]
                [--expect-fault-files] [--readback FRAME] [--timeout S]
                [--expect-edges N] [--expect-script-failure]
                [--content-copy ROOT] [--expect-file-contains REL TEXT]...
                [--expect-file-lacks REL TEXT]...
                [--expect-file-count REL TEXT N]...
                [--readback-frames A B]
                [--expect-files-equal REL REL]...
                [--then-expect-files-equal REL REL]...
                [--expect-order TEXT...]...

  --scene-copy SRC       the editor opens a copy of SRC (saves go there)
  --content-copy ROOT    the editor runs on a copy of ROOT's assets/ and
                         config/ (--content-root): the scripts may save and
                         write-file its scenes and prefabs (M11b)
  --expect-file-contains REL TEXT, --expect-file-lacks REL TEXT
                         the content copy's file REL has (lacks) TEXT
  --expect-file-count REL TEXT N
                         the content copy's file REL has TEXT N times or more
  --readback-frames A B  read the Viewport back at frames A and B (windowed)
                         and check the two images differ
  --expect-files-equal REL REL
                         after the first run, the content copy's two files
                         hold the same bytes (a document's file and what
                         the script's write-document wrote for it)
  --then-expect-files-equal REL REL
                         the same after the second run (--then-script)
  --expect-order TEXT... the run's output has the texts in this order
  --then-script FILE     a second run on the same pref directory
  --replay SE_REPLAY     se_replay replays the last Play session's replay
                         (<pref>/sessions/last-play.sereplay) from its start
                         bytes; its recorded per-tick hashes must match
  --expect-edges N       with --replay: the replay records exactly N input
                         edges (se_replay --edges-out)
  --expect-script-failure
                         the script fails: the editor logs a FAILED line
                         and exits with a non-zero status
  --readback TICK        write the composite of the first frame after tick
                         TICK and check it is not one colour (windowed)
Python 3, stdlib only.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zlib


def run(cmd, timeout, env):
    started = subprocess.run(cmd, capture_output=True, text=True,
                             timeout=timeout, env=env, errors="replace")
    return started.returncode, started.stdout + started.stderr


def png_colours(path):
    """The number of distinct RGBA pixels of an 8-bit RGBA PNG (stdlib)."""
    data = path.read_bytes()
    pos = 8
    width = height = 0
    idat = b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    colours = set()
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for i in range(stride):
            left = line[i - 4] if i >= 4 else 0
            up = previous[i]
            corner = previous[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                pred = left if pa <= pb and pa <= pc else (
                    up if pb <= pc else corner)
                line[i] = (line[i] + pred) & 0xFF
        for x in range(0, stride, 4 * 7):
            colours.add(bytes(line[x:x + 4]))
        previous = line
    return len(colours)


def editor_command(args, pref, script, scene, workdir):
    cmd = [args.editor, "--pref-dir", str(pref), "--no-crash-capture",
           "--audio", "null"]
    if args.content_copy:
        cmd += ["--content-root", str(workdir / "content")]
    if args.headless:
        cmd += ["--headless", "--fixed-frame-dt", "8.3333"]
    else:
        cmd += ["--no-activate"]
        if args.readback:
            cmd += ["--readback-tick",
                    f"{args.readback}:{workdir / 'readback.png'}"]
        if args.readback_frames:
            for i, frame in enumerate(args.readback_frames):
                cmd += ["--readback",
                        f"{frame}:{workdir / f'readback-{i}.png'}"]
    cmd += ["--", "--editor-script", str(pathlib.Path(script).resolve())]
    if scene is not None:
        cmd += ["--scene", str(scene)]
    return cmd


def check_run(name, code, output, failures, expect_failure=False):
    if expect_failure:
        if code == 0:
            failures.append(f"{name}: exit status 0 after a failed script")
        if "editor script: FAILED" not in output:
            failures.append(f"{name}: no 'editor script: FAILED' line")
        return
    if code != 0:
        failures.append(f"{name}: exit status {code}")
    if "editor script: passed" not in output:
        failures.append(f"{name}: no 'editor script: passed' line")
    if "editor script: FAILED" in output or "FAILED " in output:
        failures.append(f"{name}: the script reported a failure")


def files_equal(content, pairs, name, failures):
    for first, second in pairs:
        a, b = content / first, content / second
        if not a.exists() or not b.exists():
            failures.append(f"{name}: {first} or {second} is missing")
        elif a.read_bytes() != b.read_bytes():
            failures.append(f"{name}: {first} and {second} differ")


def in_order(output, orders, name, failures):
    for texts in orders:
        position = 0
        for text in texts:
            found = output.find(text, position)
            if found < 0:
                failures.append(f"{name}: '{text}' is missing, or not after "
                                f"the texts before it in {texts}")
                break
            position = found + len(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--script", required=True)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--scene-copy")
    parser.add_argument("--then-script")
    parser.add_argument("--replay")
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument("--expect-scene-contains")
    parser.add_argument("--expect-scene-unchanged", action="store_true")
    parser.add_argument("--expect-recovery-file", action="store_true")
    parser.add_argument("--expect-fault-files", action="store_true")
    parser.add_argument("--readback", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--expect-edges", type=int)
    parser.add_argument("--expect-script-failure", action="store_true")
    parser.add_argument("--content-copy")
    parser.add_argument("--expect-file-contains", nargs=2, action="append",
                        default=[])
    parser.add_argument("--expect-file-lacks", nargs=2, action="append",
                        default=[])
    parser.add_argument("--expect-file-count", nargs=3, action="append",
                        default=[])
    parser.add_argument("--readback-frames", nargs=2, type=int)
    parser.add_argument("--expect-files-equal", nargs=2, action="append",
                        default=[])
    parser.add_argument("--then-expect-files-equal", nargs=2,
                        action="append", default=[])
    parser.add_argument("--expect-order", nargs="+", action="append",
                        default=[])
    args = parser.parse_args()

    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    env["SE_AUDIO"] = "null"
    env.setdefault("SDL_ASSERT", "abort")

    workdir = pathlib.Path(tempfile.mkdtemp(prefix="se_editor_"))
    pref = workdir / "pref"
    scene = None
    original = None
    if args.scene_copy:
        scene = workdir / "document.scene.json"
        shutil.copyfile(args.scene_copy, scene)
        original = scene.read_bytes()
    content = workdir / "content"
    if args.content_copy:
        # The editor's content root: a copy, so saves and write-file never
        # touch the repository's files.
        for part in ("assets", "config"):
            shutil.copytree(pathlib.Path(args.content_copy) / part,
                            content / part)
    failures = []
    outputs = []
    try:
        code, output = run(editor_command(args, pref, args.script, scene,
                                          workdir), args.timeout, env)
        outputs.append(output)
        check_run("run", code, output, failures, args.expect_script_failure)
        files_equal(content, args.expect_files_equal, "run", failures)
        in_order(output, args.expect_order, "run", failures)
        if args.then_script:
            code, output = run(editor_command(args, pref, args.then_script,
                                              scene, workdir),
                               args.timeout, env)
            outputs.append(output)
            check_run("second run", code, output, failures)
            files_equal(content, args.then_expect_files_equal, "second run",
                        failures)
        everything = "\n".join(outputs)
        for text in args.expect:
            if text not in everything:
                failures.append(f"the output lacks '{text}'")
        if args.expect_scene_contains is not None:
            text = scene.read_text(encoding="utf-8")
            if args.expect_scene_contains not in text:
                failures.append("the saved scene lacks "
                                f"'{args.expect_scene_contains}'")
        if args.expect_scene_unchanged and scene.read_bytes() != original:
            failures.append("the scene file changed")
        for rel, text in args.expect_file_contains:
            body = (content / rel).read_text(encoding="utf-8")
            if text not in body:
                failures.append(f"{rel} lacks '{text}'")
        for rel, text in args.expect_file_lacks:
            body = (content / rel).read_text(encoding="utf-8")
            if text in body:
                failures.append(f"{rel} has '{text}'")
        for rel, text, count in args.expect_file_count:
            body = (content / rel).read_text(encoding="utf-8")
            if body.count(text) < int(count):
                failures.append(f"{rel} has '{text}' {body.count(text)} "
                                f"times, expected {count} or more")
        if args.readback_frames:
            images = [workdir / f"readback-{i}.png" for i in range(2)]
            if not all(image.exists() for image in images):
                failures.append("the two readback images were not written")
            elif images[0].read_bytes() == images[1].read_bytes():
                failures.append("the readbacks before and after are equal")
        if args.expect_recovery_file:
            files = list((pref / "recovery").glob("*.serecovery"))
            if not files:
                failures.append("no recovery file under pref/recovery")
        if args.expect_fault_files:
            found = list((pref / "faults").glob("*/replay.sereplay"))
            if not found:
                failures.append("no fault files under pref/faults")
            for directory in (pref / "faults").glob("*"):
                for name in ("replay.sereplay", "session.selog",
                             "breadcrumbs.bin", "fault.txt"):
                    if not (directory / name).exists():
                        failures.append(f"{directory.name} lacks {name}")
        if args.readback:
            image = workdir / "readback.png"
            if not image.exists():
                failures.append("no readback image")
            elif png_colours(image) < 3:
                failures.append("the readback is (nearly) one colour")
        if args.replay:
            replay = pref / "sessions" / "last-play.sereplay"
            if not replay.exists():
                failures.append("no last-play.sereplay")
            else:
                edges = workdir / "edges.txt"
                cmd = [args.replay, "--module", "rampage", "--inputs",
                       str(replay), "--start-scene",
                       str(workdir / "start.scene.json")]
                # 'Play game' (ADR-129): the replay starts from a content
                # bundle, whose Gameplay scene is the editor's Play-start
                # file under the pref directory.
                play_game = pref / "play-game.scene.json"
                if play_game.exists():
                    cmd += ["--scene", play_game.resolve().as_posix()]
                if args.expect_edges is not None:
                    cmd += ["--edges-out", str(edges)]
                code, output = run(cmd, args.timeout, env)
                outputs.append(output)
                if code != 0:
                    failures.append(f"se_replay: exit status {code}")
                if "diverges" in output:
                    failures.append("se_replay: the replay diverged")
                if args.expect_edges is not None:
                    lines = []
                    if edges.exists():
                        lines = [line for line in
                                 edges.read_text(encoding="utf-8").splitlines()
                                 if line.strip()]
                    outputs.append("edges:\n" + "\n".join(lines))
                    if len(lines) != args.expect_edges:
                        failures.append(f"the replay has {len(lines)} input "
                                        f"edges, expected {args.expect_edges}")
    except subprocess.TimeoutExpired:
        failures.append(f"timed out after {args.timeout} s")
    for output in outputs:
        lines = output.splitlines()
        sys.stdout.write("\n".join(lines[-80:]) + "\n")
    for failure in failures:
        print(f"FAIL: {failure}")
    if not failures:
        print("run_editor: passed")
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print(f"run_editor: files kept in {workdir}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
