"""An idle editor is not a hang (the integration of M10 and M12; ADR-111).

In Edit mode the editor idles after 1 s without input: SDL then calls no
iterate until an event arrives, so main's iterate counter stands still by
design. The watchdog must not take that for a hang.

Runs simple_editor in a window (shown without focus, audio on the null
backend) with a script that only waits for a few frames,
and with --watchdog-period-ms 200, so that a hang would be declared after
1 s without progress (with or without crash capture: msvc-asan builds the
stub, and its watchdog declares hangs all the same). The test waits until the editor logs that it waits
for events, lets it idle for 4 s (20 watchdog periods), and checks that no
hang was declared and no dump written. Then it posts WM_CLOSE to the
editor's window (a window message, not input): the event ends the wait and
the editor quits through the two-phase quit with exit status 0.

usage:
  idle_watchdog_test.py --editor EXE --script FILE --work DIR

Windows only (the test finds the window with user32). Python 3, stdlib.
"""

import argparse
import ctypes
import ctypes.wintypes
import os
import pathlib
import shutil
import subprocess
import sys
import threading
import time

IDLE_S = 4.0
WM_CLOSE = 0x0010


class Run:
    """The editor with its merged output read line by line as it comes."""

    def __init__(self, command, env):
        self.lines = []
        self.lock = threading.Lock()
        self.process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace", env=env)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            with self.lock:
                self.lines.append(line.rstrip("\n"))

    def output(self):
        with self.lock:
            return "\n".join(self.lines)

    def wait_for(self, text, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if text in self.output():
                return True
            if self.process.poll() is not None:
                return text in self.output()
            time.sleep(0.05)
        return False


def windows_of(pid):
    """The visible top-level windows of process |pid|."""
    user32 = ctypes.windll.user32
    found = []
    callback_type = ctypes.WINFUNCTYPE(
        ctypes.wintypes.BOOL, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)

    def visit(hwnd, _):
        owner = ctypes.wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(callback_type(visit), 0)
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", type=pathlib.Path, required=True)
    parser.add_argument("--script", type=pathlib.Path, required=True)
    parser.add_argument("--work", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)
    pref = args.work / "pref"

    env = dict(os.environ)
    env["SE_NO_MESSAGE_BOX"] = "1"
    env["SE_AUDIO"] = "null"
    env.setdefault("SDL_ASSERT", "abort")
    run = Run([str(args.editor), "--pref-dir", str(pref), "--audio", "null",
               "--no-activate", "--log-level", "debug",
               "--watchdog-period-ms", "200", "--",
               "--editor-script", str(args.script.resolve())], env)
    failures = []
    waiting = run.wait_for("idle: waiting for events", 30)
    if not waiting:
        failures.append("the editor never logged that it waits for events")
    else:
        time.sleep(IDLE_S)
    idle_output = run.output()
    if run.process.poll() is not None:
        failures.append("the editor exited while it should idle")
    posted = 0
    if run.process.poll() is None:
        for hwnd in windows_of(run.process.pid):
            if ctypes.windll.user32.PostMessageW(hwnd, WM_CLOSE, 0, 0):
                posted += 1
        if posted == 0:
            failures.append("no window to post WM_CLOSE to")
    try:
        run.process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        failures.append("the editor did not quit after WM_CLOSE")
        run.process.kill()
        run.process.wait()
    run.reader.join(timeout=10)
    output = run.output()
    print("\n".join(output.splitlines()[-60:]))

    # Without crash capture (msvc-asan builds the stub) the watchdog still
    # declares hangs; only its dumps are missing.
    if "crash capture on" not in output:
        print("note: this build runs without crash capture")
    if "editor script: passed" not in output:
        failures.append("no 'editor script: passed' line")
    for text in ("requesting a dump", "hang 1:"):
        if text in output:
            failures.append(f"the watchdog declared a hang ('{text}')")
    if waiting and "idle: the wait ended" in idle_output:
        # Something woke the editor during the wait (a real event on the
        # desktop): the idle time the test asked for was not all one wait.
        print("note: an event ended an idle wait during the test")
    if "idle: the wait ended after" not in output:
        failures.append("the WM_CLOSE did not end the idle wait")
    if "shutdown (success)" not in output:
        failures.append("no clean two-phase shutdown")
    if run.process.returncode != 0:
        failures.append(f"exit status {run.process.returncode}")
    dumps = list((pref / "crashdb").rglob("*.dmp"))
    if dumps:
        failures.append(f"{len(dumps)} dump(s) written while idle")
    for failure in failures:
        print(f"FAIL: {failure}")
    if not failures:
        print(f"PASS: idle for {IDLE_S} s at a 200 ms watchdog period, no "
              "hang; WM_CLOSE quit the editor")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
