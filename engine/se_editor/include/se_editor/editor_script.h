// Copyright 2026 The simple-engine-2 Authors.
//
// The editor's test script (--editor-script FILE, test builds;
// docs/milestone-specs.md M10a, apps/editor; ADR-099). One action per
// line, run at frame boundaries (the editor's frame start); '#' starts a
// comment. Every desktop test drives the editor through a script, never
// through real input.
//
//   open FILE                      the scene file as the document
//   select GUID...                 the selection
//   set GUID COMPONENT JSON        set a component (the rest of the line)
//   add GUID COMPONENT JSON        add a component
//   remove GUID COMPONENT          remove a component
//   create GUID COMPONENTS         create an entity: a JSON object of
//                                  {"<component>": <value>, ...}
//   create-copy GUID FROM          create an entity with FROM's components
//   destroy GUID                   destroy an entity
//   path-insert GUID INDEX X Y     path-point commands on "Path"
//   path-move GUID INDEX X Y
//   path-delete GUID INDEX
//   coalesce-begin LABEL           a drag: commands until coalesce-end are
//   coalesce-end                   one undo step
//   undo | redo
//   save [FILE]                    save the document (through the lane)
//   play | play-game | pause | resume | stop
//   step [N]                       N single ticks (default 1), one a frame
//   inject-throw tick|render       the next play tick or render build throws
//   wait-ticks N                   until the play session ran N more ticks
//   wait-frames N                  N frames
//   dialog-open open|save          a dialog opens (no OS dialog: the script
//                                  answers it)
//   dialog-callback PATHS [from-thread]
//                                  the oldest open dialog's callback (one is
//                                  opened first when none is), with PATHS
//                                  (';'-separated, or "cancel"), on main or
//                                  on another thread
//   quit [save|discard|cancel]     the two-phase quit; the prompt about a
//                                  dirty document takes the answer (save);
//                                  the script goes on while it drains
//   terminate                      SDL_EVENT_TERMINATING (recovery files are
//                                  written synchronously, then the exit)
//   wait-ms N                      N milliseconds of real time
//   key CHORD                      a key press for the UI's hotkeys, fed to
//                                  ImGui as key events (never OS input):
//                                  "ctrl+z", "shift+f5", "f10"; pressed
//                                  for one frame, released in the next
//   push-key KEY [up]              an SDL key event (SDL_PushEvent, as the
//                                  OS sends one; never OS input) for the
//                                  game: KEY as config/input.json names it
//                                  ("d", "space"); down unless "up"
//   expect-game-keyboard [yes|no]  'Play game' and the view has the focus:
//                                  the keyboard and the gamepad are the
//                                  game's InputService's (ADR-113)
//   recovery accept|discard        the answer to the recovery offer
//   answer-open save|discard|cancel
//                                  the answer to the prompt about a dirty
//                                  document that a dialog's open waits on
//                                  (or to the next one)
//   expect-open-prompt [yes|no]    an open waits for that prompt
//   wait-saves                     until no save or autosave is in the lane
//   wait-autosave                  until the dirty document's autosave of
//                                  its revision was submitted
//   expect-hash document|play V    V: 16 hex digits, "initial" (the
//                                  document's at the script's start) or
//                                  "play-start" (the document's at Play)
//   expect-faulted [yes|no]
//   expect-dirty [yes|no]
//   expect-state edit|playing|paused|stepping|faulted
//   expect-entities document|play N
//   expect-recovery [yes|no]       the document's recovery file exists
//   expect-quit-pending [yes|no]
//   show-panel NAME                brings the panel's window (its dock tab)
//                                  to the front; NAME is the rest of the
//                                  line ("Stress generator")
//   expect-panel NAME              the panel drew in the last frame
//   expect-viewport [yes|no|window]
//                                  a panel asked for an offscreen viewport
//                                  in the last frame (the World is drawn
//                                  into it, not into the composite);
//                                  "window": exactly when the editor has a
//                                  window (headless has no viewports)
//   expect-fork-sites N            the frame's stats list at least N fork
//                                  sites, none enabled, with no worker
//   expect-dump-stream [yes|no]    the Play-start dump stream is registered
//   dump-hash FILE                 writes "document H", "play H", "tick T"
//   throttle-disk MS               every file write step takes MS ms more
//                                  (se::fs's fault injector, test builds)
//   disk-busy MS                   renames meet a sharing violation for MS
//                                  ms (over 2 s, a save fails)
//
// A failed action or expectation logs "editor script: line N: FAILED ..."
// and the editor exits with a failure after the script.

#ifndef SE_EDITOR_EDITOR_SCRIPT_H_
#define SE_EDITOR_EDITOR_SCRIPT_H_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "se_core/error.h"

namespace se::editor {

struct ScriptAction {
  std::uint32_t line = 0;
  // The first word ("set", "wait-ticks", ...).
  std::string verb;
  // The words after it; for set, add and create the JSON is the last one,
  // the rest of the line as written.
  std::vector<std::string> args;
};

// Parses a script; an unknown verb or a missing argument is an error
// naming the line.
[[nodiscard]] std::expected<std::vector<ScriptAction>, Error> ParseScript(
    std::string_view text);

}  // namespace se::editor

#endif  // SE_EDITOR_EDITOR_SCRIPT_H_
