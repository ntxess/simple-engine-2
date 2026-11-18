"""Reads a minidump written by the crash handler and decodes the engine's
first-party streams (docs/milestone-specs.md, M4b).

It parses MINIDUMP_HEADER and the stream directory, lists the threads (with
their names), the exception record, the system and process information and
crashpad's annotations, and decodes every first-party stream (type
0x5345xxxx) by the fixed header it starts with (docs/milestone-specs.md,
"Dump stream layout"):

  kind 1  breadcrumbs: one record per ThreadRegistry slot, with the packed
          breadcrumb split into role, phase, system, chunk and tick
  kind 2  a log ring: the records still in the ring, oldest first, as text;
          sequence numbers must increase through the ring
  kind 3  the InputFrame ring: the slots oldest first, with their seqlock
          stamps, which must say "frame n, complete" (2n + 2,
          se_core/input_frame_ring.h); an odd stamp means the slot was
          being written (torn)

A ring whose writer was running when the dump was taken may show one
record in progress: the oldest slot of a full ring, which the next write
reuses. It is marked (torn, or overwritten by the next record) and not
decoded as its own index, but it is not a problem. Any other record that
does not fit its index (a stale or torn slot inside the ring, a sequence
number out of order) is listed under "problems".
  kind 4  the SessionLog tail and kind 5 the flight recorder: listed, not
          decoded (their record formats come with later milestones)
  kind 6  the session header (M12): module, scene and its hash, seed, tick
          rate, build, start tick, the ring index of its first frame and
          the newest published tick hash; its seqlock stamp is odd while
          main rewrites it (marked, not a problem)
  kind 7  bytes (M12: the editor's Play-start bytes while Play runs): their
          size, FNV-1a 64 hash and, in --json, base64

It is both a diagnostic tool and the oracle of the crash tests. Python 3
standard library only; it never needs the build.

usage:
  read_dump.py DUMP [--json] [--records N] [--strict]

  --json      print one JSON object instead of text
  --records   show at most N records per stream in text mode (default 16;
              0 shows all); --json always has every record
  --strict    exit 1 when anything was malformed (listed under "problems")

Exit status: 0 decoded, 1 problems found with --strict, 2 not a readable
minidump.
"""

import argparse
import base64
import json
import pathlib
import struct
import sys

MINIDUMP_SIGNATURE = 0x504D444D  # "MDMP"
MINIDUMP_VERSION = 0xA793

STREAM_NAMES = {
    3: "ThreadList",
    4: "ModuleList",
    5: "MemoryList",
    6: "Exception",
    7: "SystemInfo",
    9: "Memory64List",
    12: "HandleData",
    14: "UnloadedModuleList",
    15: "MiscInfo",
    16: "MemoryInfoList",
    24: "ThreadNames",
    0x43500001: "CrashpadInfo",
    0x47670001: "LinuxCpuInfo",
    0x47670003: "LinuxLsbRelease",
    0x47670004: "LinuxProcStatus",
    0x47670005: "LinuxEnviron",
    0x47670007: "LinuxCmdLine",
    0x47670009: "LinuxMaps",
}

EXCEPTION_NAMES = {
    0x0517A7ED: "crashpad DumpWithoutCrash (simulated)",
    0x40000015: "STATUS_FATAL_APP_EXIT (abort)",
    0x80000003: "EXCEPTION_BREAKPOINT",
    0xC0000005: "EXCEPTION_ACCESS_VIOLATION",
    0xC000001D: "EXCEPTION_ILLEGAL_INSTRUCTION",
    0xC0000094: "EXCEPTION_INT_DIVIDE_BY_ZERO",
    0xC00000FD: "EXCEPTION_STACK_OVERFLOW",
    0xC0000374: "STATUS_HEAP_CORRUPTION",
    0xC0000409: "STATUS_STACK_BUFFER_OVERRUN (fast fail)",
    0xE06D7363: "unhandled C++ exception (MSVC)",
}

# The first-party stream types (se_crash/crash.h).
SE_FIRST = 0x53450001
SE_LAST = 0x5345FFFF
SE_LOG_RING_BASE = 0x53450100
SE_LOG_RING_SLOTS = 256
SE_LIBRARY_LOG_SLOT = 64
SE_STREAM_TYPES = {
    0x53450001: ("breadcrumbs", 1),
    0x53450002: ("input frames", 3),
    0x53450003: ("session log tail", 4),
    0x53450004: ("flight recorder", 5),
    0x53450005: ("session header", 6),
    0x53450006: ("play-start bytes", 7),
}

DUMP_STREAM_MAGIC = 0x31304553  # "SE01"
DUMP_STREAM_VERSION = 1
DUMP_STREAM_HEADER_SIZE = 64
KIND_NAMES = {
    1: "breadcrumbs",
    2: "log ring",
    3: "input frames",
    4: "session log tail",
    5: "flight recorder",
    6: "session header",
    7: "bytes",
}

# se::ThreadRole, se::Phase and se::LogLevel, in declaration order.
ROLE_NAMES = ["lib", "main", "sim", "fork", "bg", "log", "watchdog", "input",
              "vblank"]
PHASE_NAMES = ["Input", "Intent", "Movement", "Collision", "Effects",
               "GameRules", "Cleanup"]
LEVEL_LETTERS = "TDIWEF"
SLOT_STATES = {0: "free", 1: "live", 2: "exited"}
EDGE_SOURCES = ["raw-keyboard", "win-message", "gamepad-sampler", "mouse",
                "replay", "cleared"]

BREADCRUMB_RECORD_SIZE = 64
LOG_RECORD_SIZE = 512
LOG_FILE_CHARS = 36
LOG_MESSAGE_CHARS = 448
# InputFrameSlot (tests/crash/probe_dump_stream.h pins these offsets).
INPUT_SLOT_SIZE = 968
INPUT_FRAME_OFFSET = 8
INPUT_MAX_ACTIONS = 64
INPUT_MAX_AXES = 8
INPUT_MAX_EDGES = 32
INPUT_EDGE_SIZE = 24
# SessionHeaderRecord (se_core/dump_stream.h pins these offsets).
SESSION_RECORD_SIZE = 512
SESSION_TEXT_FIELDS = (("module", 88, 40), ("build", 128, 64),
                       ("platform", 192, 32), ("compiler", 224, 64),
                       ("scene", 288, 224))
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


class DumpError(Exception):
    """The file is not a minidump this tool can read."""


class Blob:
    """Bounds-checked little-endian reads from the dump."""

    def __init__(self, data):
        self.data = data

    def unpack(self, fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(self.data):
            raise DumpError(f"read of {size} bytes at {offset:#x} is outside "
                            f"the {len(self.data)}-byte file")
        return struct.unpack_from(fmt, self.data, offset)

    def u32(self, offset):
        return self.unpack("<I", offset)[0]

    def slice(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise DumpError(f"{size} bytes at {offset:#x} are outside the "
                            f"{len(self.data)}-byte file")
        return self.data[offset:offset + size]

    def utf16_string(self, rva):
        """A MINIDUMP_STRING: byte length, then UTF-16LE."""
        length = self.u32(rva)
        return self.slice(rva + 4, length).decode("utf-16-le", "replace")

    def utf8_string(self, rva):
        """crashpad's MinidumpUTF8String: byte length, then UTF-8."""
        length = self.u32(rva)
        return self.slice(rva + 4, length).decode("utf-8", "replace")


def c_string(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def stream_name(stream_type):
    if stream_type in STREAM_NAMES:
        return STREAM_NAMES[stream_type]
    if SE_FIRST <= stream_type <= SE_LAST:
        return "se:" + se_type_name(stream_type)
    return "unknown"


def se_type_name(stream_type):
    if stream_type in SE_STREAM_TYPES:
        return SE_STREAM_TYPES[stream_type][0]
    slot = stream_type - SE_LOG_RING_BASE
    if 0 <= slot < SE_LOG_RING_SLOTS:
        if slot == SE_LIBRARY_LOG_SLOT:
            return "log ring (library)"
        return f"log ring (slot {slot})"
    return "first-party (unassigned)"


def se_expected_kind(stream_type):
    if stream_type in SE_STREAM_TYPES:
        return SE_STREAM_TYPES[stream_type][1]
    if 0 <= stream_type - SE_LOG_RING_BASE < SE_LOG_RING_SLOTS:
        return 2
    return None


def role_name(role):
    return ROLE_NAMES[role] if role < len(ROLE_NAMES) else f"role{role}"


def phase_name(phase):
    return PHASE_NAMES[phase] if phase < len(PHASE_NAMES) else f"phase{phase}"


def unpack_breadcrumb(packed):
    """se_core/breadcrumb.h: role:4 phase:4 system:12 chunk:16 tick:28."""
    role = (packed >> 60) & 0xF
    phase = (packed >> 56) & 0xF
    return {
        "role": role,
        "role_name": role_name(role),
        "phase": phase,
        "phase_name": phase_name(phase),
        "system": (packed >> 44) & 0xFFF,
        "chunk": (packed >> 28) & 0xFFFF,
        "tick": packed & 0xFFFFFFF,
    }


# ----------------------------------------------------------------------------
# Standard streams.

def read_header(blob):
    signature, version, count, directory_rva, checksum, timestamp, flags = (
        blob.unpack("<IIIIIIQ", 0))
    if signature != MINIDUMP_SIGNATURE:
        raise DumpError(f"not a minidump (signature {signature:#010x})")
    if version & 0xFFFF != MINIDUMP_VERSION:
        raise DumpError(f"unknown minidump version {version:#x}")
    return {
        "version": version,
        "stream_count": count,
        "directory_rva": directory_rva,
        "checksum": checksum,
        "timestamp": timestamp,
        "flags": flags,
    }


def read_directory(blob, header):
    streams = []
    for i in range(header["stream_count"]):
        stream_type, size, rva = blob.unpack(
            "<III", header["directory_rva"] + 12 * i)
        if stream_type == 0:  # UnusedStream
            continue
        streams.append({
            "type": stream_type,
            "type_hex": f"{stream_type:#010x}",
            "name": stream_name(stream_type),
            "size": size,
            "rva": rva,
        })
    return streams


def read_threads(blob, stream):
    count = blob.u32(stream["rva"])
    threads = []
    for i in range(count):
        (thread_id, suspend_count, priority_class, priority, teb, stack_start,
         stack_size, _stack_rva, _context_size, _context_rva) = blob.unpack(
             "<IIIIQQIIII", stream["rva"] + 4 + 48 * i)
        threads.append({
            "id": thread_id,
            "suspend_count": suspend_count,
            "priority_class": priority_class,
            "priority": priority,
            "teb": teb,
            "stack_start": stack_start,
            "stack_size": stack_size,
        })
    return threads


def read_thread_names(blob, stream):
    count = blob.u32(stream["rva"])
    names = {}
    for i in range(count):
        thread_id, name_rva = blob.unpack("<IQ", stream["rva"] + 4 + 12 * i)
        names[thread_id] = blob.utf16_string(name_rva)
    return names


def read_exception(blob, stream):
    base = stream["rva"]
    thread_id, = blob.unpack("<I", base)
    code, flags, _record, address, count = blob.unpack("<IIQQI", base + 8)
    parameters = list(blob.unpack("<15Q", base + 40))[:min(count, 15)]
    return {
        "thread_id": thread_id,
        "code": code,
        "code_hex": f"{code:#010x}",
        "code_name": EXCEPTION_NAMES.get(code, "unknown"),
        "flags": flags,
        "address": address,
        "parameters": parameters,
    }


def read_modules(blob, stream):
    count = blob.u32(stream["rva"])
    modules = []
    for i in range(count):
        entry = stream["rva"] + 4 + 108 * i
        base, size = blob.unpack("<QI", entry)
        name_rva = blob.u32(entry + 20)
        modules.append({"base": base, "size": size,
                        "name": blob.utf16_string(name_rva)})
    return modules


def read_system_info(blob, stream):
    (architecture, _level, _revision, processors, _product, major, minor,
     build, platform) = blob.unpack("<HHHBBIIII", stream["rva"])
    return {"architecture": architecture, "processors": processors,
            "os_version": f"{major}.{minor}.{build}", "platform_id": platform}


def read_misc_info(blob, stream):
    if stream["size"] < 12:
        return None
    _size, flags, process_id = blob.unpack("<III", stream["rva"])
    return process_id if flags & 0x1 else None


def read_string_dictionary(blob, rva):
    count = blob.u32(rva)
    result = {}
    for i in range(count):
        key_rva, value_rva = blob.unpack("<II", rva + 4 + 8 * i)
        result[blob.utf8_string(key_rva)] = blob.utf8_string(value_rva)
    return result


def read_crashpad_info(blob, stream, module_names):
    """crashpad's MinidumpCrashpadInfo: process and module annotations."""
    base = stream["rva"]
    version, = blob.unpack("<I", base)
    report_id = blob.slice(base + 4, 16).hex()
    dict_size, dict_rva = blob.unpack("<II", base + 36)
    list_size, list_rva = blob.unpack("<II", base + 44)
    info = {"version": version, "report_id": report_id, "process": {},
            "modules": []}
    if dict_size:
        info["process"] = read_string_dictionary(blob, dict_rva)
    if not list_size:
        return info
    count = blob.u32(list_rva)
    for i in range(count):
        index, _size, rva = blob.unpack("<III", list_rva + 4 + 12 * i)
        _version, list_annotations, simple, objects = (
            blob.unpack("<I", rva)[0], blob.unpack("<II", rva + 4),
            blob.unpack("<II", rva + 12), blob.unpack("<II", rva + 20))
        module = {
            "module": (module_names[index] if index < len(module_names)
                       else f"module {index}"),
            "list": [],
            "simple": {},
            "objects": {},
        }
        if list_annotations[0]:
            entries = blob.u32(list_annotations[1])
            module["list"] = [
                blob.utf8_string(blob.u32(list_annotations[1] + 4 + 4 * k))
                for k in range(entries)]
        if simple[0]:
            module["simple"] = read_string_dictionary(blob, simple[1])
        if objects[0]:
            entries = blob.u32(objects[1])
            for k in range(entries):
                name_rva, kind, _reserved, value_rva = blob.unpack(
                    "<IHHI", objects[1] + 4 + 12 * k)
                length = blob.u32(value_rva)
                value = blob.slice(value_rva + 4, length)
                module["objects"][blob.utf8_string(name_rva)] = (
                    value.decode("utf-8", "replace") if kind == 1
                    else value.hex())
        info["modules"].append(module)
    return info


# ----------------------------------------------------------------------------
# First-party streams.

def decode_breadcrumbs(data, header, out):
    for slot in range(header["capacity"]):
        offset = DUMP_STREAM_HEADER_SIZE + slot * header["record_size"]
        packed, thread_id, role, state = struct.unpack_from("<QQBB", data,
                                                             offset)
        if state == 0 and packed == 0 and thread_id == 0:
            continue
        record = {
            "slot": slot,
            "state": SLOT_STATES.get(state, f"state{state}"),
            "os_thread_id": thread_id,
            "role": role,
            "role_name": role_name(role),
            "name": c_string(data[offset + 18:offset + 64]),
            "breadcrumb": unpack_breadcrumb(packed),
            "packed": f"{packed:#018x}",
        }
        out.append(record)


def ring_order(header):
    """Record indices still in a ring, oldest first, with their slots."""
    count = min(header["head"], header["capacity"])
    first = header["head"] - count
    return [(index, index % header["capacity"])
            for index in range(first, header["head"])]


def next_write_slot(header, index):
    """True for the oldest record of a full ring: the slot the producer
    writes next, which a write in progress may have overwritten in part."""
    return (header["head"] >= header["capacity"]
            and index == header["head"] - header["capacity"])


def decode_log_ring(data, header, out, problems, label):
    for index, slot in ring_order(header):
        offset = DUMP_STREAM_HEADER_SIZE + slot * header["record_size"]
        (timestamp, sequence, tick, line, level, role, length) = (
            struct.unpack_from("<QQIIBBH", data, offset))
        file_name = c_string(data[offset + 28:offset + 28 + LOG_FILE_CHARS])
        message_start = offset + 28 + LOG_FILE_CHARS
        message = data[message_start:message_start +
                       min(length, LOG_MESSAGE_CHARS)]
        out.append({
            "index": index,
            "slot": slot,
            "timestamp_ns": timestamp,
            "sequence": sequence,
            "tick": tick,
            "line": line,
            "level": level,
            "level_letter": (LEVEL_LETTERS[level]
                             if level < len(LEVEL_LETTERS) else "?"),
            "role": role,
            "role_name": role_name(role),
            "file": file_name,
            "message": message.decode("utf-8", "replace"),
            "may_be_torn": next_write_slot(header, index),
            "torn": False,
        })
    # Each ring has one producer at a time and its records take their
    # sequence numbers in push order (the library ring under its lock).
    for previous, record in zip(out, out[1:]):
        if record["sequence"] > previous["sequence"]:
            continue
        if previous is out[0] and previous["may_be_torn"]:
            # The record in progress (number head) has written its header
            # over the oldest one.
            previous["torn"] = True
            continue
        problems.append(
            f"{label}: log record {record['index']} (slot {record['slot']}) "
            f"has sequence {record['sequence']}, not after "
            f"{previous['sequence']} of record {previous['index']}")


def decode_input_frame(data, offset):
    tick, held = struct.unpack_from("<QQ", data, offset)
    pressed = data[offset + 16:offset + 16 + INPUT_MAX_ACTIONS]
    released = data[offset + 80:offset + 80 + INPUT_MAX_ACTIONS]
    axes = list(struct.unpack_from(f"<{INPUT_MAX_AXES}f", data, offset + 144))
    edge_count = data[offset + 176]
    edges = []
    for k in range(min(edge_count, INPUT_MAX_EDGES)):
        edge = offset + 184 + k * INPUT_EDGE_SIZE
        action, down = data[edge], data[edge + 1]
        stamp_ns, = struct.unpack_from("<Q", data, edge + 8)
        order, source = data[edge + 16], data[edge + 17]
        edges.append({
            "action": action,
            "down": bool(down),
            "stamp_ns": stamp_ns,
            "order": order,
            "source": (EDGE_SOURCES[source] if source < len(EDGE_SOURCES)
                       else f"source{source}"),
        })
    return {
        "tick": tick,
        "held": f"{held:#018x}",
        "pressed": {i: n for i, n in enumerate(pressed) if n},
        "released": {i: n for i, n in enumerate(released) if n},
        "axes": axes,
        "edge_count": edge_count,
        "edges": edges,
        "platform_live": bool(data[offset + 952]),
    }


def decode_input_ring(data, header, out, problems, label):
    known_layout = header["record_size"] == INPUT_SLOT_SIZE
    if not known_layout:
        problems.append(f"{label}: unknown InputFrame layout (record size "
                        f"{header['record_size']}, expected "
                        f"{INPUT_SLOT_SIZE}); frames not decoded")
    head = header["head"]
    for index, slot in ring_order(header):
        offset = DUMP_STREAM_HEADER_SIZE + slot * header["record_size"]
        stamp, = struct.unpack_from("<Q", data, offset)
        complete = 2 * index + 2
        # The frame the stamp names: 2n + 1 while frame n is written, 2n + 2
        # once it is complete; 0 for a slot never written.
        holds = (stamp - 1) // 2 if stamp else None
        if stamp == complete:
            status = "ok"
        elif stamp % 2 == 1:
            status = "torn"
        elif stamp > complete:
            status = "overwritten"
        else:
            status = "not written"
        record = {"index": index, "slot": slot, "stamp": stamp,
                  "status": status, "holds": holds,
                  "torn": stamp % 2 == 1}
        # Frame |head| in progress over the oldest slot: being written, or
        # written while the head is not yet published.
        in_progress = (next_write_slot(header, index)
                       and stamp in (2 * head + 1, 2 * head + 2))
        if status != "ok" and not in_progress:
            problems.append(
                f"{label}: InputFrame slot {slot} has stamp {stamp} where "
                f"frame {index} needs {complete} ({status})")
        if known_layout and status == "ok":
            record["frame"] = decode_input_frame(data,
                                                 offset + INPUT_FRAME_OFFSET)
        out.append(record)


def decode_session_header(data, header, out, problems, label):
    if header["record_size"] < SESSION_RECORD_SIZE:
        problems.append(f"{label}: session header records of "
                        f"{header['record_size']} bytes, expected "
                        f"{SESSION_RECORD_SIZE}")
        return
    if header["head"] == 0:
        out.append({"index": 0, "begun": False})
        return
    offset = DUMP_STREAM_HEADER_SIZE
    (stamp, session, seed, scene_hash, math_hash, cpu_features, start_tick,
     first_frame, last_tick, last_hash, tick_rate,
     flags) = struct.unpack_from("<10Q2I", data, offset)
    record = {
        "index": 0,
        "begun": True,
        "stamp": stamp,
        # Main was rewriting the record when the dump was taken.
        "torn": stamp % 2 == 1,
        "session": session,
        "seed": seed,
        "scene_hash": f"{scene_hash:016x}",
        "math_hash": f"{math_hash:016x}",
        "cpu_features": f"{cpu_features:#x}",
        "start_tick": start_tick,
        "first_frame": first_frame,
        "last_tick": last_tick,
        "last_hash": f"{last_hash:016x}",
        "tick_rate": tick_rate,
        "flags": flags,
    }
    for name, field_offset, size in SESSION_TEXT_FIELDS:
        record[name] = c_string(data[offset + field_offset:
                                     offset + field_offset + size])
    out.append(record)


def fnv1a64(raw):
    value = FNV_OFFSET
    for byte in raw:
        value = ((value ^ byte) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def decode_bytes(data, header, out, problems, label):
    if header["record_size"] != 1:
        problems.append(f"{label}: a byte stream with records of "
                        f"{header['record_size']} bytes")
        return
    size = min(header["head"], header["capacity"])
    raw = data[DUMP_STREAM_HEADER_SIZE:DUMP_STREAM_HEADER_SIZE + size]
    out.append({
        "index": 0,
        "size": len(raw),
        "fnv1a64": f"{fnv1a64(raw):016x}",
        "base64": base64.b64encode(raw).decode("ascii"),
        "preview": raw[:64].decode("utf-8", "replace"),
    })


def decode_raw(data, header, out):
    for index, slot in ring_order(header):
        offset = DUMP_STREAM_HEADER_SIZE + slot * header["record_size"]
        record = data[offset:offset + header["record_size"]]
        out.append({"index": index, "slot": slot, "bytes": record.hex()})


def decode_se_stream(stream_type, data, problems):
    label = f"stream {stream_type:#010x}"
    result = {
        "type": stream_type,
        "type_hex": f"{stream_type:#010x}",
        "type_name": se_type_name(stream_type),
        "size": len(data),
        "records": [],
    }
    if len(data) < DUMP_STREAM_HEADER_SIZE:
        problems.append(f"{label}: {len(data)} bytes, shorter than the "
                        f"{DUMP_STREAM_HEADER_SIZE}-byte stream header")
        result["error"] = "truncated header"
        return result
    (magic, version, kind, record_size, capacity, head, dropped, thread_id,
     role) = struct.unpack_from("<IHHIIQQQB", data, 0)
    result.update({
        "magic": f"{magic:#010x}",
        "version": version,
        "kind": kind,
        "kind_name": KIND_NAMES.get(kind, f"kind{kind}"),
        "record_size": record_size,
        "capacity": capacity,
        "head": head,
        "dropped": dropped,
        "os_thread_id": thread_id,
        "role": role,
        "role_name": role_name(role),
        "name": c_string(data[41:64]),
    })
    if magic != DUMP_STREAM_MAGIC:
        problems.append(f"{label}: bad magic {magic:#010x}")
        result["error"] = "bad magic"
        return result
    if version != DUMP_STREAM_VERSION:
        problems.append(f"{label}: unknown stream version {version}")
        result["error"] = "unknown version"
        return result
    expected_kind = se_expected_kind(stream_type)
    if expected_kind is not None and kind != expected_kind:
        problems.append(f"{label}: kind {kind} where the stream type says "
                        f"{expected_kind}")
    needed = DUMP_STREAM_HEADER_SIZE + capacity * record_size
    if capacity == 0 or record_size == 0 or len(data) < needed:
        problems.append(f"{label}: {len(data)} bytes, but the header "
                        f"describes {needed} ({capacity} x {record_size})")
        result["error"] = "truncated records"
        return result
    if kind == 1:
        if record_size < BREADCRUMB_RECORD_SIZE:
            problems.append(f"{label}: breadcrumb records of {record_size} "
                            f"bytes, expected {BREADCRUMB_RECORD_SIZE}")
        else:
            decode_breadcrumbs(data, result, result["records"])
    elif kind == 2:
        if record_size < LOG_RECORD_SIZE:
            problems.append(f"{label}: log records of {record_size} bytes, "
                            f"expected {LOG_RECORD_SIZE}")
        else:
            decode_log_ring(data, result, result["records"], problems, label)
    elif kind == 3:
        decode_input_ring(data, result, result["records"], problems, label)
    elif kind == 6:
        decode_session_header(data, result, result["records"], problems,
                              label)
    elif kind == 7:
        decode_bytes(data, result, result["records"], problems, label)
    else:
        result["decoded"] = False
        decode_raw(data, result, result["records"])
    return result


# ----------------------------------------------------------------------------

def read_dump(data):
    """Decodes a minidump given as bytes; raises DumpError when unreadable."""
    blob = Blob(data)
    header = read_header(blob)
    streams = read_directory(blob, header)
    result = {
        "size": len(data),
        "header": header,
        "streams": streams,
        "process_id": None,
        "system": None,
        "modules": [],
        "threads": [],
        "exception": None,
        "annotations": {"process": {}, "modules": []},
        "annotation_objects": {},
        "se_streams": [],
        "problems": [],
    }
    problems = result["problems"]
    by_type = {}
    for stream in streams:
        by_type.setdefault(stream["type"], stream)

    def guarded(name, function, *args):
        try:
            return function(*args)
        except DumpError as error:
            problems.append(f"{name}: {error}")
            return None

    if 4 in by_type:
        result["modules"] = guarded("ModuleList", read_modules, blob,
                                    by_type[4]) or []
    if 3 in by_type:
        result["threads"] = guarded("ThreadList", read_threads, blob,
                                    by_type[3]) or []
    names = {}
    if 24 in by_type:
        names = guarded("ThreadNames", read_thread_names, blob,
                        by_type[24]) or {}
    for thread in result["threads"]:
        thread["name"] = names.get(thread["id"], "")
    if 6 in by_type:
        result["exception"] = guarded("Exception", read_exception, blob,
                                      by_type[6])
    if 7 in by_type:
        result["system"] = guarded("SystemInfo", read_system_info, blob,
                                   by_type[7])
    if 15 in by_type:
        result["process_id"] = guarded("MiscInfo", read_misc_info, blob,
                                       by_type[15])
    if 0x43500001 in by_type:
        module_names = [m["name"] for m in result["modules"]]
        info = guarded("CrashpadInfo", read_crashpad_info, blob,
                       by_type[0x43500001], module_names)
        if info:
            result["crashpad"] = {"version": info["version"],
                                  "report_id": info["report_id"]}
            result["annotations"] = {"process": info["process"],
                                     "modules": info["modules"]}
            for module in info["modules"]:
                result["annotation_objects"].update(module["objects"])

    seen = set()
    for stream in streams:
        stream_type = stream["type"]
        if not SE_FIRST <= stream_type <= SE_LAST:
            continue
        if stream_type in seen:
            problems.append(f"stream {stream_type:#010x} appears twice")
            continue
        seen.add(stream_type)
        try:
            data_bytes = blob.slice(stream["rva"], stream["size"])
        except DumpError as error:
            problems.append(f"stream {stream_type:#010x}: {error}")
            continue
        result["se_streams"].append(
            decode_se_stream(stream_type, data_bytes, problems))
    return result


# ----------------------------------------------------------------------------
# Text output.

def shown(records, limit):
    if limit and len(records) > limit:
        return records[-limit:], len(records) - limit
    return records, 0


def format_record(stream, record):
    kind = stream.get("kind")
    if kind == 1:
        crumb = record["breadcrumb"]
        return (f"slot {record['slot']:2} {record['state']:6} "
                f"{record['name']!r} tid {record['os_thread_id']} role "
                f"{record['role_name']}: {crumb['role_name']} "
                f"{crumb['phase_name']} system {crumb['system']} chunk "
                f"{crumb['chunk']} tick {crumb['tick']}")
    if kind == 2:
        if record["torn"]:
            return (f"[{record['index']}] slot {record['slot']} TORN "
                    f"(overwritten by record {stream['head']} while the "
                    f"dump was written)")
        seconds = record["timestamp_ns"] / 1e9
        note = (" (may be torn: the next write reuses this slot)"
                if record["may_be_torn"] else "")
        return (f"[{record['index']}] {seconds:.6f} {record['level_letter']} "
                f"{record['role_name']} t={record['tick']} "
                f"{record['file']}:{record['line']} {record['message']}"
                f"{note}")
    if kind == 3:
        status = record.get("status", "ok")
        if status == "torn":
            return (f"[{record['index']}] slot {record['slot']} stamp "
                    f"{record['stamp']} TORN (frame {record['holds']} being "
                    f"written)")
        if status == "overwritten":
            return (f"[{record['index']}] slot {record['slot']} stamp "
                    f"{record['stamp']} OVERWRITTEN (holds frame "
                    f"{record['holds']})")
        if status == "not written":
            return (f"[{record['index']}] slot {record['slot']} stamp "
                    f"{record['stamp']} NOT WRITTEN")
        frame = record.get("frame")
        if frame is None:
            return (f"[{record['index']}] slot {record['slot']} stamp "
                    f"{record['stamp']}")
        return (f"[{record['index']}] slot {record['slot']} stamp "
                f"{record['stamp']} tick {frame['tick']} held "
                f"{frame['held']} edges {frame['edge_count']} axes0 "
                f"{frame['axes'][0]:g}")
    if kind == 6:
        if not record["begun"]:
            return "no session begun"
        torn = " (TORN: being rewritten)" if record["torn"] else ""
        return (f"session {record['session']} of {record['module']!r}: scene "
                f"{record['scene']!r} (hash {record['scene_hash']}), seed "
                f"{record['seed']}, {record['tick_rate']} Hz, from tick "
                f"{record['start_tick']} (ring frame "
                f"{record['first_frame']}), last tick {record['last_tick']} "
                f"hash {record['last_hash']}; build {record['build']!r}, "
                f"{record['platform']}, {record['compiler']}, math "
                f"{record['math_hash']}{torn}")
    if kind == 7:
        return (f"{record['size']} bytes, fnv1a64 {record['fnv1a64']}: "
                f"{record['preview']!r}")
    return f"[{record['index']}] {record['bytes'][:64]}"


def print_text(path, dump, limit):
    header = dump["header"]
    print(f"minidump {path}: {dump['size']} bytes, "
          f"{header['stream_count']} streams, timestamp {header['timestamp']}")
    if dump["process_id"] is not None:
        print(f"process id {dump['process_id']}")
    if dump["system"]:
        system = dump["system"]
        print(f"system: os {system['os_version']}, {system['processors']} "
              f"processors, architecture {system['architecture']}")
    print("streams:")
    for stream in dump["streams"]:
        print(f"  {stream['type_hex']} {stream['name']:24} "
              f"{stream['size']:9} bytes @ {stream['rva']:#x}")
    print(f"threads ({len(dump['threads'])}):")
    for thread in dump["threads"]:
        print(f"  tid {thread['id']:6} {thread['name']!r} suspend "
              f"{thread['suspend_count']} stack {thread['stack_size']} bytes")
    exception = dump["exception"]
    if exception:
        print(f"exception: {exception['code_hex']} {exception['code_name']} "
              f"on thread {exception['thread_id']} at "
              f"{exception['address']:#x}")
    else:
        print("exception: none")
    annotations = dump["annotations"]
    if annotations["process"] or annotations["modules"]:
        print("annotations:")
        for key, value in sorted(annotations["process"].items()):
            print(f"  process {key} = {value!r}")
        for module in annotations["modules"]:
            for key, value in sorted(module["objects"].items()):
                print(f"  {module['module']}: {key} = {value!r}")
            for key, value in sorted(module["simple"].items()):
                print(f"  {module['module']}: {key} = {value!r}")
    print(f"first-party streams ({len(dump['se_streams'])}):")
    for stream in dump["se_streams"]:
        if "error" in stream:
            print(f"  {stream['type_hex']} {stream['type_name']}: "
                  f"{stream['error']}")
            continue
        print(f"  {stream['type_hex']} {stream['type_name']} "
              f"{stream['name']!r}: {stream['kind_name']}, "
              f"{stream['capacity']} x {stream['record_size']} bytes, head "
              f"{stream['head']}, dropped {stream['dropped']}, tid "
              f"{stream['os_thread_id']}, role {stream['role_name']}")
        records, hidden = shown(stream["records"], limit)
        if hidden:
            print(f"    ... {hidden} older records")
        for record in records:
            print("    " + format_record(stream, record))
    if dump["problems"]:
        print("problems:")
        for problem in dump["problems"]:
            print(f"  {problem}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("dump")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--records", type=int, default=16)
    parser.add_argument("--strict", action="store_true")
    args = parser.parse_args(argv)
    path = pathlib.Path(args.dump)
    try:
        dump = read_dump(path.read_bytes())
    except (OSError, DumpError) as error:
        print(f"read_dump: {path}: {error}", file=sys.stderr)
        return 2
    if args.json:
        dump["file"] = str(path)
        json.dump(dump, sys.stdout, indent=1, sort_keys=True)
        print()
    else:
        print_text(path, dump, args.records)
    return 1 if args.strict and dump["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
