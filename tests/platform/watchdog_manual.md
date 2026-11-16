# Watchdog: sleep/resume and a real debugger (manual procedure)

The M12 hang detection (docs/milestone-specs.md "M12 Watchdog"; ADR-104 to
ADR-106) is tested automatically with a fake unbiased clock and a faked
debugger (`crash_unit.*`, `platform.*hang*`, `platform.*heartbeat*`) and
through the real stack with an injected hang (`crash.rampage_hang`,
`crash.rampage_driver_stall`). Two cases need a person at the machine: a
real system sleep and resume, and a real debugger attached to a running
or stopped process. docs/gates.md lists this procedure as the row "Sleep/
resume and debugger do not trigger the watchdog".

Build msvc-dev first (`scripts\with-msvc.cmd cmake --workflow --preset
msvc-dev`). Every run below uses `--audio null`, so nothing is heard, and a
fresh `--pref-dir`, so the dumps it writes are easy to find:

```bat
set SE_NO_MESSAGE_BOX=1
set PREF=%TEMP%\se-watchdog-manual
rmdir /s /q %PREF%
out\build\msvc-dev\bin\rampage.exe --audio null --pref-dir %PREF% --log-level debug
```

The log goes to the console and to `%PREF%\logs\rampage.log`. Dumps go to
`%PREF%\crashdb\reports\*.dmp`; `python tools\minidump\read_dump.py
<dump>` decodes one. A hang shows in the log as an `E watchdog` line
starting `hang 1:`, followed by `dump written (hang 1: ...)`.

## 1. System sleep and resume

1. Start rampage as above and let it run for 10 s (the title's tick
   advances).
2. Put the machine to sleep (Start > Power > Sleep, or `rundll32.exe
   powrprof.dll,SetSuspendState 0,1,0` from a second console; with
   hibernation enabled that call hibernates instead, which is as good a
   test). Wait at least 30 s before waking it.
3. Wake the machine, log in, and let rampage run for another 10 s, then
   close its window.

Expected:
- no `hang` line and no `.dmp` file in `%PREF%\crashdb`: the watchdog's
  periods are measured on QueryUnbiasedInterruptTimePrecise, which does not
  advance while the system sleeps;
- the ticks resume without a burst of catch-up ticks (the progress lines
  every 2 s keep their tick spacing; the frame clock is reset when the
  lifecycle leaves Suspended);
- the shutdown ends with `shutdown (success)` and `teardown took` under
  2000 ms.

Repeat once with the window minimized before the sleep (Suspended through
the window as well as the power state).

## 2. A debugger attached while rampage runs

Use any debugger that attaches to a running process: Visual Studio
(Debug > Attach to Process), WinDbg (`windbg -p <pid>`), or `cdb -p <pid>`
from the Windows SDK's Debugging Tools.

1. Start rampage as above. Find its process id in the log's first lines or
   in Task Manager.
2. Attach the debugger. Let rampage run for 10 s attached.
3. Break into the process (Debug > Break All, or Ctrl+Break in WinDbg).
   Stay stopped for at least 20 s, so far longer than the 5 s hang rule.
4. Continue (F5, or `g`). Let it run 10 s, detach the debugger, and close
   the window.

Expected:
- no `hang` line and no `.dmp` file: while stopped, the watchdog thread is
  stopped too; once it runs, `IsDebuggerPresent` excuses every period
  without progress, and the count starts over after the debugger detaches;
- the log may show one `W watchdog` line "the watchdog took no sample for
  N s (the process was stopped ...)": time during which the watchdog itself
  did not run is not counted towards a hang (ADR-104);
- the watchdog never breaks into the debugger itself and never ends the
  process (no `DebugBreak`, checked by `checks.planted.debugbreak`).

Repeat steps 1 to 3 and then detach while still stopped (Debug > Detach
All; `qd` in WinDbg or cdb) instead of continuing: the process resumes
with no debugger attached and 20 s gone from the unbiased clock. Expected:
no `hang` line and no dump; the log shows the "took no sample for 2x.x s"
line once.

Variation: start rampage with `--hang-test after-ticks:2400` (test builds:
main loops forever in the HangTest system 20 s in), attach the debugger
within the first 15 s, and leave it attached: no hang dump is written while
the debugger is attached. Detach it: about 5 s later the log shows `hang
1: main made no progress for N s at tick 2400` (N counts from the last
progress, so it includes the attached time) and one dump is written.
End the process from Task Manager afterwards (main never returns from the
hang test).

## 3. A debugger attached to a hung process

1. Start rampage with `--hang-test after-ticks:600`.
2. Wait for `dump written (hang 1: ...)` (about 10 s in: 5 s of ticks, then
   5 s of hang).
3. Attach the debugger and look at the main thread: it is in the HangTest
   system (`se::...HangTestSystem::Run`), called from `se::Schedule::Step`.
   Detach.
4. End the process from Task Manager.

Expected: exactly one dump for the episode (attaching does not start a new
one), and `se_replay --from-dump <dump>` replays the session to tick 600
("reached tick 600 (main's breadcrumb), inside which main stopped") with
"tick N's hash ... matches the dump".

## Recording the result

Write the date, the build (`git rev-parse --short HEAD`), the debugger
used and what each step showed into the docs/gates.md row "Sleep/resume
and debugger do not trigger the watchdog", and set its status to passed or
to what failed.
