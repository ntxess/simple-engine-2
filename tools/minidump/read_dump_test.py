"""Tests read_dump.py on synthetic minidumps built here.

The synthetic dumps carry what the crash handler writes for the engine:
the standard thread, thread-name, module, exception and misc streams,
crashpad's annotations, and first-party streams in the layout of
docs/milestone-specs.md ("Dump stream layout"), including a wrapped log ring
and an InputFrame ring with a torn slot, with the stamps the engine's
InputFrameRing writes (2n + 1 while frame n is written, 2n + 2 after).
Malformed inputs must be reported, not decoded; a record that does not fit
its index is a problem unless it is the one a running writer was
overwriting. CTest runs this as minidump.read_dump; the crash tests
(tests/crash/crash_test.py) check read_dump.py on real dumps.

usage:
  read_dump_test.py
"""

import contextlib
import io
import json
import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import read_dump  # noqa: E402  (imported from this directory)

MAGIC = 0x31304553
HEADER_FORMAT = "<IHHIIQQQB23s"


class DumpBuilder:
    """Lays out a minidump: header, directory, then 4-aligned blobs."""

    def __init__(self, stream_count):
        self.stream_count = stream_count
        self.data = bytearray(32 + 12 * stream_count)
        self.directory = []

    def put(self, blob):
        while len(self.data) % 4:
            self.data.append(0)
        rva = len(self.data)
        self.data += blob
        return rva

    def utf16(self, text):
        encoded = text.encode("utf-16-le")
        return self.put(struct.pack("<I", len(encoded)) + encoded + b"\0\0")

    def utf8(self, text):
        encoded = text.encode("utf-8")
        return self.put(struct.pack("<I", len(encoded)) + encoded + b"\0")

    def stream(self, stream_type, blob):
        self.directory.append((stream_type, len(blob), self.put(blob)))

    def build(self, signature=0x504D444D, version=0xA793):
        assert len(self.directory) <= self.stream_count
        struct.pack_into("<IIIIIIQ", self.data, 0, signature, version,
                         self.stream_count, 32, 0, 1700000000, 0)
        for i, (stream_type, size, rva) in enumerate(self.directory):
            struct.pack_into("<III", self.data, 32 + 12 * i, stream_type, size,
                             rva)
        return bytes(self.data)


def se_header(kind, record_size, capacity, head, dropped=0, tid=0, role=0,
              name=b"", magic=MAGIC, version=1):
    return struct.pack(HEADER_FORMAT, magic, version, kind, record_size,
                       capacity, head, dropped, tid, role, name)


def breadcrumb(role, phase, system, chunk, tick):
    return ((role << 60) | (phase << 56) | (system << 44) | (chunk << 28)
            | tick)


def breadcrumb_stream():
    records = bytearray(64 * 8)
    entries = [
        (0, breadcrumb(1, 2, 5, 0, 240), 100, 1, 1, b"main"),
        (1, breadcrumb(6, 0, 0, 0, 240), 200, 6, 1, b"worker"),
        (2, breadcrumb(3, 4, 7, 3, 239), 0x1234, 3, 2, b"fork-worker-3"),
    ]
    for slot, packed, tid, role, state, name in entries:
        struct.pack_into("<QQBB46s", records, 64 * slot, packed, tid, role,
                         state, name)
    return se_header(1, 64, 8, 3, name=b"threads") + bytes(records)


def log_record(i):
    message = f"log {i}".encode()
    return struct.pack("<QQIIBBH36s448s", 1_000_000_000 + i, 1000 + i, 200 + i,
                       100 + i, i % 5, 1, len(message), b"probe.cc", message)


def log_stream(capacity=4, head=6, kind=2, slots_override=None):
    slots = [b"\0" * 512] * capacity
    for i in range(head):
        slots[i % capacity] = log_record(i)
    for slot, record in (slots_override or {}).items():
        slots[slot] = record
    return (se_header(kind, 512, capacity, head, dropped=2, tid=100, role=1,
                      name=b"main") + b"".join(slots))


def input_slot(stamp, tick):
    frame = bytearray(960)
    struct.pack_into("<QQ", frame, 0, tick, 1 | (1 << (tick % 64)))
    frame[16 + tick % 64] = 1
    struct.pack_into("<f", frame, 144, tick * 0.25)
    frame[176] = 1
    struct.pack_into("<BBxxxxxxQBB", frame, 184, tick % 64, 1,
                     tick * 1000, 0, 4)
    frame[952] = 1
    return struct.pack("<Q", stamp) + bytes(frame)


def input_stream(record_size=968, slots_override=None):
    # Ticks 1..5 in a 4-slot ring: frame i holds tick i + 1 with stamp
    # 2i + 2, so the ring holds frames 1..4 (ticks 2..5); frame 5 (tick 6)
    # is being written over slot 1 (frame 1), whose stamp is 2 * 5 + 1.
    slots = [input_slot(10, 5), input_slot(11, 6), input_slot(6, 3),
             input_slot(8, 4)]
    for slot, (stamp, tick) in (slots_override or {}).items():
        slots[slot] = input_slot(stamp, tick)
    if record_size != 968:
        slots = [s[:record_size].ljust(record_size, b"\0") for s in slots]
    return (se_header(3, record_size, 4, 5, tid=100, role=1, name=b"input")
            + b"".join(slots))


def flight_stream():
    return se_header(5, 8, 2, 2, name=b"flight") + bytes(range(16))


def crashpad_info(builder):
    process = [(builder.utf8("se.probe"), builder.utf8("synthetic"))]
    simple = builder.put(struct.pack("<I", len(process)) + b"".join(
        struct.pack("<II", k, v) for k, v in process))
    value = b"std::terminate with exception: boom"
    value_rva = builder.put(struct.pack("<I", len(value)) + value)
    objects = builder.put(struct.pack("<I", 1) + struct.pack(
        "<IHHI", builder.utf8("se.terminate"), 1, 0, value_rva))
    module_info = builder.put(struct.pack("<IIIIIII", 1, 0, 0, 0, 0, 16,
                                          objects))
    module_list = builder.put(struct.pack("<I", 1) + struct.pack(
        "<III", 0, 28, module_info))
    return (struct.pack("<I", 1) + bytes(range(16)) + bytes(16)
            + struct.pack("<IIIIIQ", 12, simple, 16, module_list, 0, 0))


def full_dump(se_streams=None):
    se_streams = se_streams if se_streams is not None else [
        (0x53450001, breadcrumb_stream()),
        (0x53450100, log_stream()),
        (0x53450002, input_stream()),
        (0x53450004, flight_stream()),
    ]
    builder = DumpBuilder(8 + len(se_streams))
    threads = struct.pack("<I", 2) + b"".join(
        struct.pack("<IIIIQQIIII", tid, 1, 32, 0, 0, 0x1000, 4096, 0, 0, 0)
        for tid in (100, 200))
    builder.stream(3, threads)
    names = [(100, builder.utf16("se-probe-main")),
             (200, builder.utf16("se-probe-worker"))]
    builder.stream(24, struct.pack("<I", 2) + b"".join(
        struct.pack("<IQ", tid, rva) for tid, rva in names))
    module_name = builder.utf16("C:\\probe\\se_crash_probe.exe")
    module = bytearray(108)
    struct.pack_into("<QIIII", module, 0, 0x140000000, 0x10000, 0, 0,
                     module_name)
    builder.stream(4, struct.pack("<I", 1) + bytes(module))
    exception = struct.pack("<II", 100, 0) + struct.pack(
        "<IIQQII", 0xC0000005, 0, 0, 0x140001234, 2, 0) + struct.pack(
            "<15Q", 1, 0, *([0] * 13)) + struct.pack("<II", 0, 0)
    builder.stream(6, exception)
    builder.stream(15, struct.pack("<III", 24, 1, 4321) + bytes(12))
    builder.stream(7, struct.pack("<HHHBBIIII", 9, 25, 0x7402, 16, 1, 10, 0,
                                  26200, 2) + bytes(24))
    builder.stream(0x43500001, crashpad_info(builder))
    for stream_type, blob in se_streams:
        builder.stream(stream_type, blob)
    return builder.build()


def se_stream(dump, stream_type):
    for stream in dump["se_streams"]:
        if stream["type"] == stream_type:
            return stream
    raise AssertionError(f"stream {stream_type:#x} missing")


def run_cli(data, *args):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "synthetic.dmp"
        path.write_bytes(data)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = read_dump.main([str(path), *args])
        return status, out.getvalue(), err.getvalue()


class StandardStreams(unittest.TestCase):

    def setUp(self):
        self.dump = read_dump.read_dump(full_dump())

    def test_no_problems(self):
        self.assertEqual(self.dump["problems"], [])

    def test_threads_and_names(self):
        threads = {t["id"]: t["name"] for t in self.dump["threads"]}
        self.assertEqual(threads, {100: "se-probe-main",
                                   200: "se-probe-worker"})

    def test_exception(self):
        exception = self.dump["exception"]
        self.assertEqual(exception["thread_id"], 100)
        self.assertEqual(exception["code"], 0xC0000005)
        self.assertEqual(exception["code_name"], "EXCEPTION_ACCESS_VIOLATION")
        self.assertEqual(exception["address"], 0x140001234)
        self.assertEqual(exception["parameters"], [1, 0])

    def test_process_and_modules(self):
        self.assertEqual(self.dump["process_id"], 4321)
        self.assertEqual(self.dump["system"]["os_version"], "10.0.26200")
        self.assertEqual(self.dump["modules"][0]["name"],
                         "C:\\probe\\se_crash_probe.exe")

    def test_annotations(self):
        self.assertEqual(self.dump["annotations"]["process"],
                         {"se.probe": "synthetic"})
        self.assertEqual(self.dump["annotation_objects"], {
            "se.terminate": "std::terminate with exception: boom"})
        self.assertEqual(self.dump["annotations"]["modules"][0]["module"],
                         "C:\\probe\\se_crash_probe.exe")


class FirstPartyStreams(unittest.TestCase):

    def setUp(self):
        self.dump = read_dump.read_dump(full_dump())

    def test_breadcrumbs(self):
        stream = se_stream(self.dump, 0x53450001)
        self.assertEqual(stream["kind_name"], "breadcrumbs")
        self.assertEqual(stream["name"], "threads")
        slots = {r["slot"]: r for r in stream["records"]}
        self.assertEqual(sorted(slots), [0, 1, 2])
        main = slots[0]
        self.assertEqual((main["state"], main["name"], main["os_thread_id"]),
                         ("live", "main", 100))
        self.assertEqual(main["breadcrumb"], {
            "role": 1, "role_name": "main", "phase": 2,
            "phase_name": "Movement", "system": 5, "chunk": 0, "tick": 240})
        worker = slots[2]
        self.assertEqual(worker["state"], "exited")
        self.assertEqual(worker["breadcrumb"]["chunk"], 3)
        self.assertEqual(worker["breadcrumb"]["phase_name"], "Effects")

    def test_log_ring_wraps_oldest_first(self):
        stream = se_stream(self.dump, 0x53450100)
        self.assertEqual(stream["type_name"], "log ring (slot 0)")
        self.assertEqual((stream["head"], stream["dropped"]), (6, 2))
        messages = [r["message"] for r in stream["records"]]
        self.assertEqual(messages, ["log 2", "log 3", "log 4", "log 5"])
        first = stream["records"][0]
        self.assertEqual((first["slot"], first["sequence"], first["tick"],
                          first["line"], first["level_letter"], first["file"]),
                         (2, 1002, 202, 102, "I", "probe.cc"))

    def test_input_ring_marks_the_torn_slot(self):
        stream = se_stream(self.dump, 0x53450002)
        records = stream["records"]
        self.assertEqual([r["slot"] for r in records], [1, 2, 3, 0])
        self.assertTrue(records[0]["torn"])
        self.assertEqual((records[0]["status"], records[0]["holds"]),
                         ("torn", 5))
        self.assertNotIn("frame", records[0])
        self.assertEqual([r["status"] for r in records[1:]], ["ok"] * 3)
        self.assertEqual(self.dump["problems"], [])
        ticks = [r["frame"]["tick"] for r in records[1:]]
        self.assertEqual(ticks, [3, 4, 5])
        frame = records[3]["frame"]
        self.assertEqual(frame["held"], f"{1 | (1 << 5):#018x}")
        self.assertEqual(frame["axes"][0], 1.25)
        self.assertEqual(frame["pressed"], {5: 1})
        self.assertEqual(frame["edges"], [{
            "action": 5, "down": True, "stamp_ns": 5000, "order": 0,
            "source": "replay"}])
        self.assertTrue(frame["platform_live"])

    def test_later_kinds_are_listed_raw(self):
        stream = se_stream(self.dump, 0x53450004)
        self.assertFalse(stream["decoded"])
        self.assertEqual([r["bytes"] for r in stream["records"]],
                         [bytes(range(8)).hex(), bytes(range(8, 16)).hex()])

    def test_json_output(self):
        status, out, _ = run_cli(full_dump(), "--json")
        self.assertEqual(status, 0)
        decoded = json.loads(out)
        self.assertEqual(len(decoded["se_streams"]), 4)

    def test_text_output(self):
        status, out, _ = run_cli(full_dump(), "--records", "2")
        self.assertEqual(status, 0)
        self.assertIn("exception: 0xc0000005 EXCEPTION_ACCESS_VIOLATION on "
                      "thread 100", out)
        self.assertIn("... 2 older records", out)
        self.assertIn("T main t=205 probe.cc:105 log 5", out)
        self.assertNotIn("TORN", out)  # the oldest slot is not shown
        status, out, _ = run_cli(full_dump())
        self.assertEqual(status, 0)
        self.assertIn("[1] slot 1 stamp 11 TORN (frame 5 being written)",
                      out)
        self.assertIn("se.terminate = 'std::terminate with exception: boom'",
                      out)


class RecordsThatDoNotFitTheirIndex(unittest.TestCase):

    def test_input_frame_written_but_not_published(self):
        # Frame 5 is complete in slot 1 but the head still says 5: slot 1
        # no longer holds frame 1. Marked, not decoded, not a problem.
        data = full_dump([
            (0x53450002, input_stream(slots_override={1: (12, 6)}))])
        dump = read_dump.read_dump(data)
        first = se_stream(dump, 0x53450002)["records"][0]
        self.assertEqual((first["index"], first["status"], first["holds"],
                          first["torn"]), (1, "overwritten", 5, False))
        self.assertNotIn("frame", first)
        self.assertEqual(dump["problems"], [])
        status, out, _ = run_cli(data)
        self.assertEqual(status, 0)
        self.assertIn("[1] slot 1 stamp 12 OVERWRITTEN (holds frame 5)", out)

    def test_stale_or_torn_input_slots_inside_the_ring_are_problems(self):
        # Slot 2 still holds frame 0 (stamp 2), not frame 2; slot 3 says
        # frame 3 is being written although the head says it was published.
        data = full_dump([
            (0x53450002, input_stream(slots_override={2: (2, 1),
                                                      3: (7, 4)}))])
        dump = read_dump.read_dump(data)
        records = se_stream(dump, 0x53450002)["records"]
        self.assertEqual([r["status"] for r in records],
                         ["torn", "not written", "torn", "ok"])
        self.assertEqual([r["slot"] for r in records if "frame" in r], [0])
        self.assertEqual(dump["problems"], [
            "stream 0x53450002: InputFrame slot 2 has stamp 2 where frame 2 "
            "needs 6 (not written)",
            "stream 0x53450002: InputFrame slot 3 has stamp 7 where frame 3 "
            "needs 8 (torn)"])
        status, _, _ = run_cli(data, "--strict")
        self.assertEqual(status, 1)

    def test_overwritten_input_slot_inside_the_ring_is_a_problem(self):
        # Slot 3 holds frame 7 (a later frame) where frame 3 should be.
        dump = read_dump.read_dump(full_dump([
            (0x53450002, input_stream(slots_override={3: (16, 8)}))]))
        records = se_stream(dump, 0x53450002)["records"]
        self.assertEqual((records[2]["status"], records[2]["holds"]),
                         ("overwritten", 7))
        self.assertIn("InputFrame slot 3 has stamp 16 where frame 3 needs 8 "
                      "(overwritten)", dump["problems"][0])

    def test_log_record_half_overwritten_by_the_next_push(self):
        # Record 6 has written its header over record 2 (the oldest, in
        # slot 2), whose message is still there.
        torn = log_record(6)[:28] + log_record(2)[28:]
        data = full_dump([(0x53450100, log_stream(slots_override={2: torn}))])
        dump = read_dump.read_dump(data)
        records = se_stream(dump, 0x53450100)["records"]
        self.assertEqual([(r["index"], r["torn"], r["may_be_torn"])
                          for r in records],
                         [(2, True, True), (3, False, False),
                          (4, False, False), (5, False, False)])
        self.assertEqual(dump["problems"], [])
        status, out, _ = run_cli(data)
        self.assertEqual(status, 0)
        self.assertIn("[2] slot 2 TORN (overwritten by record 6 while the "
                      "dump was written)", out)
        self.assertNotIn("log 2", out)

    def test_log_ring_out_of_order_inside_is_a_problem(self):
        # Slots 3 and 0 (records 3 and 4) hold each other's record.
        dump = read_dump.read_dump(full_dump([
            (0x53450100, log_stream(slots_override={3: log_record(4),
                                                    0: log_record(3)}))]))
        self.assertEqual(dump["problems"], [
            "stream 0x53450100: log record 4 (slot 0) has sequence 1003, "
            "not after 1004 of record 3"])

    def test_a_ring_that_has_not_wrapped_has_no_suspect_slot(self):
        dump = read_dump.read_dump(full_dump([
            (0x53450100, log_stream(capacity=8, head=6))]))
        records = se_stream(dump, 0x53450100)["records"]
        self.assertEqual([r["may_be_torn"] for r in records], [False] * 6)
        self.assertEqual(dump["problems"], [])


class MalformedInput(unittest.TestCase):

    def test_not_a_minidump(self):
        with self.assertRaisesRegex(read_dump.DumpError, "not a minidump"):
            read_dump.read_dump(full_dump()[:4].replace(b"M", b"X") +
                                full_dump()[4:])
        status, _, err = run_cli(b"garbage, not a dump at all......")
        self.assertEqual(status, 2)
        self.assertIn("not a minidump", err)

    def test_truncated_directory(self):
        with self.assertRaisesRegex(read_dump.DumpError, "outside"):
            read_dump.read_dump(full_dump()[:40])

    def test_bad_magic_is_a_problem(self):
        bad = se_header(2, 512, 1, 1, magic=0x12345678) + log_record(0)
        dump = read_dump.read_dump(full_dump([(0x53450100, bad),
                                              (0x53450001,
                                               breadcrumb_stream())]))
        self.assertEqual(se_stream(dump, 0x53450100)["error"], "bad magic")
        self.assertEqual(len(se_stream(dump, 0x53450001)["records"]), 3)
        self.assertEqual(len(dump["problems"]), 1)
        status, _, _ = run_cli(full_dump([(0x53450100, bad)]), "--strict")
        self.assertEqual(status, 1)

    def test_truncated_records(self):
        dump = read_dump.read_dump(full_dump([
            (0x53450100, log_stream()[:64 + 512])]))
        self.assertEqual(se_stream(dump, 0x53450100)["error"],
                         "truncated records")
        self.assertIn("describes 2112", dump["problems"][0])

    def test_kind_must_match_the_stream_type(self):
        dump = read_dump.read_dump(full_dump([
            (0x53450101, log_stream(kind=1))]))
        self.assertIn("kind 1 where the stream type says 2",
                      dump["problems"][0])

    def test_unknown_input_layout_keeps_the_stamps(self):
        dump = read_dump.read_dump(full_dump([
            (0x53450002, input_stream(record_size=100))]))
        stream = se_stream(dump, 0x53450002)
        self.assertEqual([r["stamp"] for r in stream["records"]],
                         [11, 6, 8, 10])
        self.assertNotIn("frame", stream["records"][1])
        self.assertIn("unknown InputFrame layout", dump["problems"][0])

    def test_unknown_version(self):
        bad = se_header(1, 64, 1, 1, version=2) + bytes(64)
        dump = read_dump.read_dump(full_dump([(0x53450001, bad)]))
        self.assertEqual(se_stream(dump, 0x53450001)["error"],
                         "unknown version")


if __name__ == "__main__":
    unittest.main(verbosity=2)
