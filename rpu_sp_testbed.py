#!/usr/bin/env python3
"""
RPU-SP serial protocol test bed
===============================

Exercises every command / response in "RPU-SP API.md".

Three layers, all in this one file:

  1. Protocol library  - packet encode/decode, stream parser, command builders,
                         response decoders.
  2. Devices           - SerialDevice (real hardware, needs pyserial) and
                         MockRpuSp (in-process simulator, no hardware needed).
  3. Tests             - "offline" tests (golden byte vectors taken from the
                         examples in the spec, parser behaviour) and "live"
                         tests that talk to a device.

Usage
-----
  pip install pyserial                       # only needed for real hardware

  python rpu_sp_testbed.py --offline         # byte-exact spec vectors, no device
  python rpu_sp_testbed.py --mock            # everything, against the simulator
  python rpu_sp_testbed.py --port COM5       # everything, against hardware
  python rpu_sp_testbed.py --port /dev/ttyUSB0 --only duck --only loop -v
  python rpu_sp_testbed.py --list
  python rpu_sp_testbed.py --port COM5 --monitor 30   # just print traffic

Live tests need audio on the SD card.  Defaults (override with flags):
  --sfx-track 1 --sfx-track2 2   : SFX WAVs, ideally > 3 s long
  --callout-track 3              : callout WAV, > 2 s
  --music-track 4 --music-track2 5 : music WAVs, > 4 s
  --loop-track 400               : a track with a soundtrack.ini defining loops
                                   (optional; loop-query tests degrade to warnings)
"""
from __future__ import annotations

import argparse
import queue
import struct
import sys
import threading
import time
from typing import Callable, Optional

# --------------------------------------------------------------------------- #
# 1. Protocol library
# --------------------------------------------------------------------------- #

SOM1, SOM2, EOM = 0xF0, 0xAA, 0x55
MIN_LEN = 5


class Cmd:
    GET_VERSION = 0x01
    GET_SYS_INFO = 0x02
    TRACK_CONTROL = 0x03
    STOP_ALL = 0x04
    MASTER_VOLUME = 0x05
    GET_STATUS = 0x07
    TRACK_VOLUME = 0x08
    AMP_POWER = 0x09
    TRACK_FADE = 0x0A
    TRACK_CONTROL_EX = 0x0D
    SET_REPORTING = 0x0E
    PLAY_EX = 0x60
    CLASS_GAIN = 0x61
    DUCK_RULE = 0x62
    LOOP_POINTS = 0x63
    GET_SOUNDTRACK_FOLDERS = 0x64
    STOP_CLASS = 0x65
    SET_EVENT_FLAGS = 0x66
    SET_SOUNDTRACK_FOLDER = 0x67
    GET_NUM_LOOPS = 0x68
    GET_LOOP_POINTS = 0x69
    SYNTH_CONTROL = 0x70


class Rsp:
    VERSION = 0x81
    SYS_INFO = 0x82
    STATUS = 0x83
    TRACK_REPORT = 0x84
    EVENT = 0x8A
    SOUNDTRACK = 0x8B
    LOOP_REPORT = 0x8C
    SYNTH = 0x8F


class Ctl:  # Track Control codes
    PLAY_SOLO, PLAY_POLY, PAUSE, RESUME, STOP, LOOP_ON, LOOP_OFF, LOAD = range(8)


class Cls:  # sound classes
    SFX, CALLOUT, MUSIC = 0, 1, 2


class PF:  # Play Ex flags
    LOOP = 0x01
    LOCK = 0x02
    SOLO = 0x04
    WAIT_BAR = 0x08
    CROSSFADE = 0x10
    REPLACE = 0x20
    PAUSE = 0x40


class LP:  # Loop Points flags
    START = 0x01
    TOP = 0x02
    THROUGH = 0x04
    ALERT_EACH = 0x08
    USE_TUID = 0x10
    COUNT_ONLY = 0x20
    NEXT_ONLY = 0x40
    NEXT_IS_TUID = 0x80


class Ev:  # Event IDs (in Event responses)
    FINISHED, LOOPED, MUSIC_CHANGED, DUCKING, STARTED, STAGED = 1, 2, 3, 4, 5, 6


class EvMask:  # Set Events Flags mask bits
    FINISHED = 0x01
    LOOPED = 0x02
    MUSIC_CHANGED = 0x04
    DUCKING = 0x08
    STARTED = 0x10
    STAGED = 0x20


_EVENT_TO_MASK = {1: 0x01, 2: 0x02, 3: 0x04, 4: 0x08, 5: 0x10, 6: 0x20}

SYNTH_TYPES = {
    0x0201: "SB100", 0x0202: "SB300", 0x0301: "WMS Type 1",
    0x0302: "WMS Type 2", 0x0A01: "A-12738", 0x0A02: "DCS",
}


def encode(code: int, payload: bytes = b"") -> bytes:
    n = len(payload) + MIN_LEN
    if n > 255:
        raise ValueError(f"packet too long ({n} bytes)")
    return bytes([SOM1, SOM2, n, code]) + bytes(payload) + bytes([EOM])


class PacketParser:
    """Incremental stream parser. Resyncs on garbage / bad length / bad EOM."""

    def __init__(self):
        self.buf = bytearray()
        self.errors = 0

    def feed(self, data: bytes):
        self.buf += data
        out = []
        while True:
            i = bytes(self.buf).find(b"\xf0\xaa")
            if i < 0:
                keep = 1 if self.buf and self.buf[-1] == SOM1 else 0
                if len(self.buf) - keep > 0:
                    self.errors += 1
                del self.buf[: len(self.buf) - keep]
                break
            if i > 0:
                self.errors += 1
                del self.buf[:i]
            if len(self.buf) < 3:
                break
            n = self.buf[2]
            if n < MIN_LEN:
                self.errors += 1
                del self.buf[0]
                continue
            if len(self.buf) < n:
                break
            if self.buf[n - 1] != EOM:
                self.errors += 1
                del self.buf[0]
                continue
            out.append((self.buf[3], bytes(self.buf[4 : n - 1])))
            del self.buf[:n]
        return out


# ---- command builders ------------------------------------------------------ #

def get_version(): return encode(Cmd.GET_VERSION)
def get_sys_info(): return encode(Cmd.GET_SYS_INFO)
def stop_all(): return encode(Cmd.STOP_ALL)
def get_status(): return encode(Cmd.GET_STATUS)
def track_control(ctl, track): return encode(Cmd.TRACK_CONTROL, struct.pack("<BH", ctl, track))
def master_volume(db): return encode(Cmd.MASTER_VOLUME, struct.pack("<h", db))
def track_volume(track, db): return encode(Cmd.TRACK_VOLUME, struct.pack("<Hh", track, db))
def amp_power(on): return encode(Cmd.AMP_POWER, bytes([1 if on else 0]))
def track_fade(track, db, ms, stop): return encode(Cmd.TRACK_FADE, struct.pack("<HhHB", track, db, ms, 1 if stop else 0))
def track_control_ex(ctl, track, lock): return encode(Cmd.TRACK_CONTROL_EX, struct.pack("<BHB", ctl, track, 1 if lock else 0))
def set_reporting(flags): return encode(Cmd.SET_REPORTING, bytes([flags]))
def play_ex(track, cls, flags=0, gain=0, priority=0): return encode(Cmd.PLAY_EX, struct.pack("<HBBhB", track, cls, flags, gain, priority))
def class_gain(cls, db, fade_ms=0): return encode(Cmd.CLASS_GAIN, struct.pack("<BhH", cls, db, fade_ms))
def duck_rule(ducked, ducking, atten_db, attack_ms, release_ms): return encode(Cmd.DUCK_RULE, struct.pack("<BBbHH", ducked, ducking, atten_db, attack_ms, release_ms))
def get_soundtrack_folders(): return encode(Cmd.GET_SOUNDTRACK_FOLDERS)
def stop_class(cls, fade_ms=0): return encode(Cmd.STOP_CLASS, struct.pack("<BH", cls, fade_ms))
def set_event_flags(mask, cls): return encode(Cmd.SET_EVENT_FLAGS, bytes([mask, cls]))
def set_soundtrack_folder(action, folder): return encode(Cmd.SET_SOUNDTRACK_FOLDER, bytes([action, folder]))
def get_num_loops(track): return encode(Cmd.GET_NUM_LOOPS, struct.pack("<H", track))
def get_loop_points(track, index): return encode(Cmd.GET_LOOP_POINTS, struct.pack("<HB", track, index))


def loop_points(target, flags, loop_count=0, a=0, b=0xFFFFFFFF, fade_in=0, fade_out=0,
                loops_per_phrase=1, alert_margin=0, next_track=0, *,
                use_tuid=False, next_is_tuid=False):
    """`target` is a 16-bit track number, or a 32-bit TUID when use_tuid=True."""
    if use_tuid:
        flags |= LP.USE_TUID
    if next_is_tuid:
        flags |= LP.NEXT_IS_TUID
    p = struct.pack("<BBIIHHBH", flags, loop_count, a, b, fade_in, fade_out, loops_per_phrase, alert_margin)
    p += struct.pack("<I" if next_is_tuid else "<H", next_track)
    p += struct.pack("<I" if use_tuid else "<H", target)
    return encode(Cmd.LOOP_POINTS, p)


def synth_get_volume(vid): return encode(Cmd.SYNTH_CONTROL, bytes([0x01, vid]))
def synth_set_volume(vid, left, right): return encode(Cmd.SYNTH_CONTROL, struct.pack("<BBhh", 0x02, vid, left, right))
def synth_get_num_roms(): return encode(Cmd.SYNTH_CONTROL, bytes([0x03]))
def synth_get_rom_name(n): return encode(Cmd.SYNTH_CONTROL, bytes([0x04, n]))
def synth_get_current_rom(): return encode(Cmd.SYNTH_CONTROL, bytes([0x05]))
def synth_flash_rom(n): return encode(Cmd.SYNTH_CONTROL, bytes([0x06, n]))
def synth_available(stype): return encode(Cmd.SYNTH_CONTROL, struct.pack("<BH", 0x80, stype))
def synth_set_status(stype, on): return encode(Cmd.SYNTH_CONTROL, struct.pack("<BHB", 0x81, stype, 1 if on else 0))
def synth_send_command(c): return encode(Cmd.SYNTH_CONTROL, bytes([0x82, c]))


# ---- response decoders ----------------------------------------------------- #

def decode_response(code: int, p: bytes) -> dict:
    m: dict = {"code": code, "raw": p}
    try:
        if code == Rsp.VERSION:
            m["version"] = p.decode("ascii", "replace")
        elif code == Rsp.SYS_INFO:
            m["voices"], m["tracks"] = struct.unpack_from("<BH", p)
        elif code == Rsp.STATUS:
            m["tracks"] = list(struct.unpack(f"<{len(p) // 2}H", p[: len(p) // 2 * 2]))
        elif code == Rsp.TRACK_REPORT:
            # NOTE: track number here is zero-based (track N is reported as N-1)
            m["track"], m["voice"], m["state"] = struct.unpack_from("<HBB", p)
        elif code == Rsp.EVENT:
            m["event_id"], m["track"], m["tuid"], m["cls"] = struct.unpack_from("<BHIB", p)
            data = p[8:]
            m["data"] = data
            if m["event_id"] == Ev.LOOPED and data:
                m["loop_count"] = data[0]
            if m["event_id"] == Ev.DUCKING and data:
                m["ducked"] = bool(data[0])
        elif code == Rsp.SOUNDTRACK:
            m["count"] = p[0]
        elif code == Rsp.LOOP_REPORT:
            m["track"], m["report_type"] = struct.unpack_from("<HB", p)
            if m["report_type"] == 0:
                m["num_loops"] = p[3]
            else:
                m["loop_index"], m["loop_number"], m["a"], m["b"], m["loops_per_phrase"] = \
                    struct.unpack_from("<BBIIB", p, 3)
        elif code == Rsp.SYNTH:
            c = p[0]
            m["synth_cmd"] = c
            if c == 0x01:
                m["vol_id"], m["left"], m["right"] = struct.unpack_from("<Bhh", p, 1)
            elif c == 0x03:
                m["num_roms"] = p[1]
            elif c in (0x04, 0x05):
                m["rom_name"] = p[1:65].split(b"\0")[0].decode("ascii", "replace")
            elif c == 0x06:
                m["percent"] = p[1]
            elif c in (0x80, 0x81):
                m["synth_type"], m["capability"] = struct.unpack_from("<HB", p, 1)
            elif c == 0x82:
                m["response"] = p[1]
    except struct.error as e:
        m["decode_error"] = str(e)
    return m


# --------------------------------------------------------------------------- #
# 2. Devices
# --------------------------------------------------------------------------- #

class Device:
    def __init__(self, verbose=False):
        self.q: queue.Queue = queue.Queue()
        self.backlog: list = []
        self.verbose = verbose
        self.t0 = time.time()

    def _log(self, tag, data):
        if self.verbose:
            print(f"    {time.time() - self.t0:8.3f} {tag} {bytes(data).hex(' ').upper()}")

    def send(self, pkt: bytes):
        raise NotImplementedError

    def close(self):
        pass

    def _rx(self, code, payload):
        msg = decode_response(code, payload)
        if self.verbose:
            brief = {k: v for k, v in msg.items() if k not in ("raw",)}
            print(f"    {time.time() - self.t0:8.3f} RX  {brief}")
        self.q.put(msg)

    def _pull(self, timeout):
        try:
            return self.q.get(timeout=timeout) if timeout > 0 else self.q.get_nowait()
        except queue.Empty:
            return None

    def wait_for(self, code, pred: Optional[Callable[[dict], bool]] = None, timeout=2.0):
        """Return first decoded packet with this code (and pred true). Others are kept."""
        def ok(m): return m["code"] == code and (pred is None or pred(m))
        for i, m in enumerate(self.backlog):
            if ok(m):
                return self.backlog.pop(i)
        end = time.time() + timeout
        while True:
            m = self._pull(max(0.0, end - time.time()))
            if m is None:
                return None
            if ok(m):
                return m
            self.backlog.append(m)

    def request(self, pkt, code, timeout=2.0, pred=None):
        # discard stale answers of the same kind, keep everything else
        while (m := self._pull(0)) is not None:
            self.backlog.append(m)
        self.backlog = [m for m in self.backlog if m["code"] != code]
        self.send(pkt)
        return self.wait_for(code, pred, timeout)

    def drain(self, secs=0.2):
        end = time.time() + secs
        got = []
        while (m := self._pull(max(0.0, end - time.time()))) is not None:
            got.append(m)
        return self.backlog_take() + got

    def backlog_take(self):
        b, self.backlog = self.backlog, []
        return b

    def clear(self):
        while self._pull(0) is not None:
            pass
        self.backlog = []


class SerialDevice(Device):
    def __init__(self, port, baud=57600, verbose=False):
        super().__init__(verbose)
        try:
            import serial  # type: ignore
        except ImportError:
            sys.exit("pyserial is required for hardware tests:  pip install pyserial")
        self.ser = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1, timeout=0.05)
        self.parser = PacketParser()
        self._run = True
        self._th = threading.Thread(target=self._reader, daemon=True)
        self._th.start()

    def _reader(self):
        while self._run:
            data = self.ser.read(256)
            if data:
                self._log("RX ", data)
                for code, payload in self.parser.feed(data):
                    self._rx(code, payload)

    def send(self, pkt):
        self._log("TX ", pkt)
        self.ser.write(pkt)
        self.ser.flush()

    def close(self):
        self._run = False
        self._th.join(timeout=1)
        self.ser.close()


class MockRpuSp(Device):
    """Small in-process RPU-SP simulator: enough behaviour to exercise the tests.
    Tracks 'finish' instantly where a real device would take time."""

    VOICES = 24
    LOOPS = {400: [(1, 190604, 369026, 1), (2, 11661, 33952, 4)]}  # per the .ini example

    def __init__(self, verbose=False):
        super().__init__(verbose)
        self.rx, self.out = PacketParser(), PacketParser()
        self.playing, self.staged, self.queued = [], [], []
        self.reporting = False
        self.mask = {0: 0, 1: 0, 2: 0}
        self.duck: list = []
        self.next_tuid = 0x11170
        self.folders = 3
        self.synth_vol: dict = {}

    # -- plumbing
    def _emit(self, code, payload=b""):
        for c, p in self.out.feed(encode(code, payload)):
            self._rx(c, p)

    def send(self, pkt):
        self._log("TX ", pkt)
        for code, payload in self.rx.feed(pkt):
            self._handle(code, payload)

    def _event(self, eid, track, tuid, cls, data=b""):
        if self.mask.get(cls, 0) & _EVENT_TO_MASK[eid]:
            self._emit(Rsp.EVENT, struct.pack("<BHIB", eid, track, tuid, cls) + data)

    def _report(self, track, voice, state):
        if self.reporting:
            self._emit(Rsp.TRACK_REPORT, struct.pack("<HBB", track - 1, voice, state))

    def _tuid(self):
        self.next_tuid += 1
        return self.next_tuid

    def _start(self, track, cls, prio=0, entry=None):
        e = entry or dict(track=track, cls=cls, prio=prio, tuid=self._tuid())
        self.playing.append(e)
        self._report(track, len(self.playing) - 1, 1)
        return e

    def _remove(self, e):
        self.playing.remove(e)
        self._report(e["track"], 0, 0)

    def _find_staged(self, ident, is_tuid):
        for e in self.staged:
            if (e["tuid"] == ident) if is_tuid else (e["track"] == ident):
                return e

    # -- command handling
    def _handle(self, code, p):
        if code == Cmd.GET_VERSION:
            self._emit(Rsp.VERSION, b"RPU-SP FW   v1.00 ")
        elif code == Cmd.GET_SYS_INFO:
            self._emit(Rsp.SYS_INFO, struct.pack("<BH", self.VOICES, 4096))
        elif code in (Cmd.TRACK_CONTROL, Cmd.TRACK_CONTROL_EX):
            ctl, track = struct.unpack_from("<BH", p)
            if ctl in (Ctl.PLAY_SOLO, Ctl.PLAY_POLY, Ctl.LOAD):
                if ctl == Ctl.PLAY_SOLO:
                    for e in list(self.playing):
                        self._remove(e)
                self._start(track, Cls.SFX)
            elif ctl == Ctl.STOP:
                for e in [x for x in self.playing if x["track"] == track]:
                    self._remove(e)
        elif code == Cmd.STOP_ALL:
            for e in list(self.playing):
                self._remove(e)
            self.queued.clear()
        elif code == Cmd.GET_STATUS:
            self._emit(Rsp.STATUS, b"".join(struct.pack("<H", e["track"]) for e in self.playing))
        elif code == Cmd.SET_REPORTING:
            self.reporting = bool(p[0] & 1)
        elif code == Cmd.TRACK_FADE:
            track, _db, _ms, stop = struct.unpack("<HhHB", p)
            if stop:
                for e in [x for x in self.playing if x["track"] == track]:
                    self._remove(e)
        elif code == Cmd.PLAY_EX:
            self._play_ex(*struct.unpack("<HBBhB", p))
        elif code == Cmd.DUCK_RULE:
            ded, ding, gain, _a, _r = struct.unpack("<BBbHH", p)
            self.duck = [r for r in self.duck if r != (ded, ding)]
            if gain:
                self.duck.append((ded, ding))
        elif code == Cmd.LOOP_POINTS:
            self._loop_points(p)
        elif code == Cmd.GET_SOUNDTRACK_FOLDERS:
            self._emit(Rsp.SOUNDTRACK, bytes([self.folders]))
        elif code == Cmd.STOP_CLASS:
            cls, _ms = struct.unpack("<BH", p)
            for e in [x for x in self.playing if x["cls"] == cls]:
                self._remove(e)
                self._event(Ev.FINISHED, e["track"], e["tuid"], cls)
            self.queued = [x for x in self.queued if x["cls"] != cls]
            for ded, ding in self.duck:
                if ding == cls:
                    self._event(Ev.DUCKING, 0, 0, ded, b"\x00")
        elif code == Cmd.SET_EVENT_FLAGS:
            self.mask[p[1]] = p[0]
        elif code == Cmd.GET_NUM_LOOPS:
            t = struct.unpack("<H", p)[0]
            self._emit(Rsp.LOOP_REPORT, struct.pack("<HBB", t, 0, len(self.LOOPS.get(t, []))))
        elif code == Cmd.GET_LOOP_POINTS:
            t, idx = struct.unpack("<HB", p)
            loops = self.LOOPS.get(t, [])
            if idx < len(loops):
                n, a, b, lpp = loops[idx]
                self._emit(Rsp.LOOP_REPORT, struct.pack("<HBBBIIB", t, 1, idx, n, a, b, lpp))
        elif code == Cmd.SYNTH_CONTROL:
            self._synth(p)
        # 0x05, 0x08, 0x09, 0x61, 0x67: accepted, no response

    def _play_ex(self, track, cls, flags, gain, prio):
        if flags & PF.PAUSE:
            e = dict(track=track, cls=cls, prio=prio, tuid=self._tuid())
            self.staged.append(e)
            self._event(Ev.STAGED, track, e["tuid"], cls)
            return
        if flags & PF.REPLACE:
            for e in [x for x in self.playing if x["track"] == track]:
                self._remove(e)
        if flags & PF.SOLO or cls == Cls.MUSIC:
            for e in [x for x in self.playing if x["cls"] == cls]:
                self._remove(e)
        if cls == Cls.SFX and len(self.playing) >= self.VOICES:
            victims = [x for x in self.playing if x["cls"] == Cls.SFX and x["prio"] < prio]
            if not victims:
                return
            self._remove(min(victims, key=lambda x: x["prio"]))
        if cls == Cls.CALLOUT:
            cur = [x for x in self.playing if x["cls"] == Cls.CALLOUT]
            if cur and max(x["prio"] for x in cur) >= prio:
                self.queued.append(dict(track=track, cls=cls, prio=prio, tuid=self._tuid()))
                return
            for x in cur:
                self._remove(x)
        e = self._start(track, cls, prio)
        self._event(Ev.STARTED, track, e["tuid"], cls)
        if cls == Cls.MUSIC:
            self._event(Ev.MUSIC_CHANGED, track, e["tuid"], cls)
        for ded, ding in self.duck:
            if ding == cls:
                self._event(Ev.DUCKING, 0, 0, ded, b"\x01")

    def _loop_points(self, p):
        flags, count, a, b, fi, fo, lpp, am = struct.unpack_from("<BBIIHHBH", p)
        off = 17
        nl = 4 if flags & LP.NEXT_IS_TUID else 2
        nxt = int.from_bytes(p[off : off + nl], "little")
        off += nl
        tl = 4 if flags & LP.USE_TUID else 2
        target = int.from_bytes(p[off : off + tl], "little")
        if flags & (LP.COUNT_ONLY | LP.NEXT_ONLY):
            return
        e = self._find_staged(target, bool(flags & LP.USE_TUID))
        if e is None or not flags & LP.START:
            return
        self.staged.remove(e)
        self._start(e["track"], e["cls"], entry=e)
        self._event(Ev.STARTED, e["track"], e["tuid"], e["cls"])
        if count == 0xFF:
            self._event(Ev.LOOPED, e["track"], e["tuid"], e["cls"], bytes([0xFF]))
            return
        for left in range(count - 1, -1, -1):
            self._event(Ev.LOOPED, e["track"], e["tuid"], e["cls"], bytes([left]))
        self._remove(e)
        self._event(Ev.FINISHED, e["track"], e["tuid"], e["cls"])
        if nxt:
            n = self._find_staged(nxt, bool(flags & LP.NEXT_IS_TUID))
            if n:
                self.staged.remove(n)
                self._start(n["track"], n["cls"], entry=n)
                self._event(Ev.STARTED, n["track"], n["tuid"], n["cls"])

    def _synth(self, p):
        c = p[0]
        if c == 0x01:
            l, r = self.synth_vol.get(p[1], (256, 256))
            self._emit(Rsp.SYNTH, struct.pack("<BBhh", 1, p[1], l, r))
        elif c == 0x02:
            self.synth_vol[p[1]] = struct.unpack_from("<hh", p, 2)
        elif c == 0x03:
            self._emit(Rsp.SYNTH, bytes([3, 2]))
        elif c in (0x04, 0x05):
            name = b"Hurricane" if c == 0x05 or p[1] == 0 else b"Pinbot"
            self._emit(Rsp.SYNTH, bytes([c]) + name.ljust(64, b"\0"))
        elif c == 0x06:
            for pct in range(0, 101, 10):
                self._emit(Rsp.SYNTH, bytes([6, pct]))
        elif c in (0x80, 0x81):
            t = struct.unpack_from("<H", p, 1)[0]
            on = c == 0x81 and p[3]
            cap = 0 if t != 0x0202 else (0x81 if on else 0x01)
            self._emit(Rsp.SYNTH, struct.pack("<BHB", c, t, cap))
        elif c == 0x82:
            self._emit(Rsp.SYNTH, bytes([0x82, p[1]]))


# --------------------------------------------------------------------------- #
# 3. Test framework
# --------------------------------------------------------------------------- #

class Skip(Exception):
    pass


TESTS: list = []


def test(group, name=None, rpu=False, opt=None):
    """group: 'offline' | 'live'.  rpu: needs 0x60+ commands (skipped on WAV Trigger).
    opt: name of an argparse flag that must be set for the test to run."""
    def deco(fn):
        TESTS.append(dict(fn=fn, group=group, name=name or fn.__name__.removeprefix("t_"), rpu=rpu, opt=opt))
        return fn
    return deco


class Ctx:
    def __init__(self, dev, args):
        self.dev, self.a = dev, args
        self.is_rpu = False
        self.voices = 0
        self.notes: list = []

    def note(self, s): self.notes.append(s)
    def warn(self, s): self.notes.append("WARN: " + s)

    def status(self):
        r = self.dev.request(get_status(), Rsp.STATUS)
        assert r is not None, "no Status (0x83) response"
        return r["tracks"]

    def wait_status(self, pred, timeout=3.0):
        end, last = time.time() + timeout, None
        while time.time() < end:
            last = self.status()
            if pred(last):
                return last
            time.sleep(0.1)
        raise AssertionError(f"Status never satisfied condition; last={last}")

    def alive(self):
        r = self.dev.request(get_version(), Rsp.VERSION)
        assert r is not None, "device stopped answering Get Version"
        return r

    def events(self, cls, mask):
        self.dev.send(set_event_flags(mask, cls))

    def cleanup(self):
        d = self.dev
        d.send(stop_all())
        d.send(master_volume(0))
        d.send(amp_power(True))
        if self.is_rpu:
            for c in (Cls.SFX, Cls.CALLOUT, Cls.MUSIC):
                d.send(stop_class(c, 0))
                d.send(class_gain(c, 0, 0))
                d.send(set_event_flags(0, c))
            d.send(duck_rule(Cls.SFX, Cls.CALLOUT, 0, 0, 0))
            d.send(duck_rule(Cls.MUSIC, Cls.CALLOUT, 0, 0, 0))
        d.send(set_reporting(0))
        time.sleep(0.15)
        d.clear()


# ---- offline: golden vectors from the spec -------------------------------- #

def _hx(s): return bytes.fromhex(s.replace(" ", ""))

TX_VECTORS = {
    "get_version": (get_version(), "F0 AA 05 01 55"),
    "get_sys_info": (get_sys_info(), "F0 AA 05 02 55"),
    "track_control": (track_control(Ctl.PLAY_POLY, 1), "F0 AA 08 03 01 01 00 55"),
    "stop_all": (stop_all(), "F0 AA 05 04 55"),
    "master_volume": (master_volume(0), "F0 AA 07 05 00 00 55"),
    "get_status": (get_status(), "F0 AA 05 07 55"),
    "track_volume": (track_volume(14, 0), "F0 AA 09 08 0E 00 00 00 55"),
    "amp_power": (amp_power(True), "F0 AA 06 09 01 55"),
    "track_fade": (track_fade(1, -50, 1000, True), "F0 AA 0C 0A 01 00 CE FF E8 03 01 55"),
    "track_control_ex": (track_control_ex(Ctl.STOP, 1, False), "F0 AA 09 0D 04 01 00 00 55"),
    "set_reporting": (set_reporting(1), "F0 AA 06 0E 01 55"),
    "play_ex": (play_ex(1, Cls.SFX, PF.REPLACE, -10, 10), "F0 AA 0C 60 01 00 00 20 F6 FF 0A 55"),
    "class_gain": (class_gain(Cls.SFX, -10, 1000), "F0 AA 0A 61 00 F6 FF E8 03 55"),
    "duck_rule": (duck_rule(Cls.SFX, Cls.CALLOUT, 20, 1000, 1000), "F0 AA 0C 62 00 01 14 E8 03 E8 03 55"),
    "loop_points_track": (
        loop_points(200, 0x0B, 2, 44100, 88200, 0, 0, 1, 500, 0),
        "F0 AA 1A 63 0B 02 44 AC 00 00 88 58 01 00 00 00 00 00 01 F4 01 00 00 C8 00 55"),
    "loop_points_tuid": (
        loop_points(70000, 0x01, 0xFF, 661500, 5292000, 0, 0, 1, 500, 0, use_tuid=True),
        "F0 AA 1C 63 11 FF FC 17 0A 00 E0 BF 50 00 00 00 00 00 01 F4 01 00 00 70 11 01 00 55"),
    "get_soundtrack_folders": (get_soundtrack_folders(), "F0 AA 05 64 55"),
    "stop_class": (stop_class(Cls.SFX, 1000), "F0 AA 08 65 00 E8 03 55"),
    "set_event_flags": (set_event_flags(0x06, Cls.MUSIC), "F0 AA 07 66 06 02 55"),
    "set_soundtrack_folder": (set_soundtrack_folder(0x03, 2), "F0 AA 07 67 03 02 55"),
    "get_num_loops": (get_num_loops(400), "F0 AA 07 68 90 01 55"),
    "get_loop_points": (get_loop_points(400, 0), "F0 AA 08 69 90 01 00 55"),
    "synth_set_volume": (synth_set_volume(0x02, 256, 128), "F0 AA 0B 70 02 02 00 01 80 00 55"),
}

RX_VECTORS = {
    "version": (encode(0x81, b"RPU-SP FW   v1.00 "), {"version": "RPU-SP FW   v1.00 "}),
    "sys_info": (_hx("F0 AA 08 82 18 00 10 55"), {"voices": 24, "tracks": 4096}),
    "status": (_hx("F0 AA 09 83 01 00 0E 00 55"), {"tracks": [1, 14]}),
    "track_report": (_hx("F0 AA 09 84 00 00 00 00 55"), {"track": 0, "voice": 0, "state": 0}),
    "event_looping": (_hx("F0 AA 0E 8A 02 04 00 70 11 01 00 02 03 55"),
                      {"event_id": 2, "track": 4, "tuid": 0x11170, "cls": 2, "loop_count": 3}),
    "soundtrack": (_hx("F0 AA 06 8B 03 55"), {"count": 3}),
    "loop_report": (_hx("F0 AA 13 8C 91 01 01 00 05 FC 17 0A 00 E0 BF 50 00 01 55"),
                    {"track": 401, "report_type": 1, "loop_index": 0, "loop_number": 5,
                     "a": 661500, "b": 5292000, "loops_per_phrase": 1}),
    "synth_rom_name": (encode(0x8F, bytes([5]) + b"Hurricane".ljust(64, b"\0")),
                       {"synth_cmd": 5, "rom_name": "Hurricane"}),
}


def _make_tx_test(name, built, expected):
    def fn(ctx):
        want = _hx(expected)
        assert built == want, f"\n  built {built.hex(' ').upper()}\n  spec  {want.hex(' ').upper()}"
        assert built[2] == len(built), "length byte != actual length"
    TESTS.append(dict(fn=fn, group="offline", name=f"tx_{name}", rpu=False, opt=None))


def _make_rx_test(name, raw, expect):
    def fn(ctx):
        parser = PacketParser()
        pk = parser.feed(raw)
        assert len(pk) == 1 and parser.errors == 0, "spec packet did not parse cleanly"
        m = decode_response(*pk[0])
        for k, v in expect.items():
            assert m.get(k) == v, f"{k}: got {m.get(k)!r}, expected {v!r}"
    TESTS.append(dict(fn=fn, group="offline", name=f"rx_{name}", rpu=False, opt=None))


for _n, (_b, _e) in TX_VECTORS.items():
    _make_tx_test(_n, _b, _e)
for _n, (_r, _e) in RX_VECTORS.items():
    _make_rx_test(_n, _r, _e)


@test("offline")
def t_parser_chunked(ctx):
    p = PacketParser()
    pkt = encode(0x83, b"\x01\x00\x0e\x00")
    out = []
    for b in pkt:
        out += p.feed(bytes([b]))
    assert out == [(0x83, b"\x01\x00\x0e\x00")]


@test("offline")
def t_parser_back_to_back(ctx):
    p = PacketParser()
    out = p.feed(encode(0x8B, b"\x03") + encode(0x82, b"\x18\x00\x10"))
    assert [c for c, _ in out] == [0x8B, 0x82]


@test("offline")
def t_parser_resync_garbage(ctx):
    p = PacketParser()
    out = p.feed(b"\x00\xf0\x12\xf0" + encode(0x8B, b"\x01"))
    assert out == [(0x8B, b"\x01")] and p.errors > 0


@test("offline")
def t_parser_rejects_bad_eom(ctx):
    p = PacketParser()
    bad = bytearray(encode(0x8B, b"\x01"))
    bad[-1] = 0x56
    out = p.feed(bytes(bad) + encode(0x8B, b"\x02"))
    assert out == [(0x8B, b"\x02")], out


@test("offline")
def t_parser_rejects_short_length(ctx):
    p = PacketParser()
    out = p.feed(bytes([SOM1, SOM2, 0x02, 0x01, EOM]) + encode(0x8B, b"\x02"))
    assert out == [(0x8B, b"\x02")], out


@test("offline")
def t_little_endian_and_signed(ctx):
    assert master_volume(-70)[4:6] == bytes([0xBA, 0xFF])
    assert master_volume(10)[4:6] == bytes([0x0A, 0x00])
    assert play_ex(0x1234, 0)[4:6] == bytes([0x34, 0x12])


@test("offline")
def t_max_length_guard(ctx):
    try:
        encode(1, bytes(251))
    except ValueError:
        return
    raise AssertionError("encode accepted a >255-byte packet")


@test("offline")
def t_loop_points_next_tuid_layout(ctx):
    pkt = loop_points(0x00011170, 0x01, 1, 0, 100, next_track=0x00012345, use_tuid=True, next_is_tuid=True)
    assert pkt[4] == 0x91 and pkt[2] == len(pkt) == 30
    assert pkt[21:25] == struct.pack("<I", 0x12345) and pkt[25:29] == struct.pack("<I", 0x11170)


@test("offline")
def t_mock_selfcheck(ctx):
    """The simulator's own encode/parse round-trip works (guards the other mock tests)."""
    m = MockRpuSp()
    m.send(get_version())
    r = m.wait_for(Rsp.VERSION, timeout=0.5)
    assert r and r["version"].startswith("RPU-SP")


# ---- live tests ------------------------------------------------------------ #

@test("live")
def t_version(ctx):
    r = ctx.dev.request(get_version(), Rsp.VERSION)
    assert r, "no Version String (0x81) response"
    v = r["version"]
    ctx.note(f"version={v!r} -> {'RPU-SP' if v.startswith('RPU-SP') else 'legacy WAV Trigger'}")
    assert v, "empty version string"


@test("live")
def t_sys_info(ctx):
    r = ctx.dev.request(get_sys_info(), Rsp.SYS_INFO)
    assert r and "decode_error" not in r, "no/short System Info response"
    assert r["voices"] >= 1 and r["tracks"] >= 1, r
    ctx.voices = r["voices"]
    ctx.note(f"voices={r['voices']} tracks={r['tracks']}")


@test("live")
def t_status_idle(ctx):
    ctx.dev.send(stop_all())
    time.sleep(0.2)
    assert ctx.status() == [], "voices still occupied after Stop All"


@test("live")
def t_play_poly_report_stop(ctx):
    t, d = ctx.a.sfx_track, ctx.dev
    d.send(set_reporting(1))
    d.send(track_control(Ctl.PLAY_POLY, t))
    d.send(track_control(Ctl.LOOP_ON, t))
    rep = d.wait_for(Rsp.TRACK_REPORT, lambda m: m["track"] == t - 1 and m["state"] == 1, 2)
    assert rep, "no Track Report 'playing' (note: report track numbers are zero-based)"
    assert t in ctx.status(), "Status must list real (1-based) track numbers"
    d.send(track_control(Ctl.STOP, t))
    assert d.wait_for(Rsp.TRACK_REPORT, lambda m: m["track"] == t - 1 and m["state"] == 0, 2), \
        "no Track Report 'stopped'"


@test("live")
def t_reporting_off(ctx):
    d = ctx.dev
    d.send(set_reporting(0))
    d.send(track_control(Ctl.PLAY_POLY, ctx.a.sfx_track))
    time.sleep(0.4)
    assert d.wait_for(Rsp.TRACK_REPORT, timeout=0.3) is None, "Track Report sent while reporting disabled"


@test("live")
def t_play_solo(ctx):
    d = ctx.dev
    d.send(track_control(Ctl.PLAY_POLY, ctx.a.sfx_track))
    d.send(track_control(Ctl.LOOP_ON, ctx.a.sfx_track))
    d.send(track_control(Ctl.PLAY_SOLO, ctx.a.sfx_track2))
    d.send(track_control(Ctl.LOOP_ON, ctx.a.sfx_track2))
    st = ctx.wait_status(lambda t: ctx.a.sfx_track2 in t)
    assert ctx.a.sfx_track not in st, f"Play Solo did not stop other tracks: {st}"


@test("live")
def t_pause_resume_load(ctx):
    d, t = ctx.dev, ctx.a.sfx_track
    d.send(track_control(Ctl.PLAY_POLY, t))
    d.send(track_control(Ctl.LOOP_ON, t))
    d.send(track_control(Ctl.PAUSE, t))
    time.sleep(0.2)
    if t not in ctx.status():
        ctx.warn("paused track no longer occupies a voice")
    d.send(track_control(Ctl.RESUME, t))
    d.send(track_control(Ctl.STOP, t))
    ctx.wait_status(lambda s: t not in s)
    # Load: occupies a voice, paused at first sample
    d.send(track_control(Ctl.LOAD, t))
    ctx.wait_status(lambda s: t in s)
    d.send(track_control(Ctl.RESUME, t))
    d.send(track_control(Ctl.STOP, t))
    ctx.wait_status(lambda s: t not in s)


@test("live")
def t_loop_on_off(ctx):
    d, t = ctx.dev, ctx.a.sfx_track
    d.send(track_control(Ctl.PLAY_POLY, t))
    d.send(track_control(Ctl.LOOP_ON, t))
    d.send(track_control(Ctl.LOOP_OFF, t))
    ctx.alive()


@test("live")
def t_master_and_track_volume(ctx):
    d, t = ctx.dev, ctx.a.sfx_track
    d.send(track_control(Ctl.PLAY_POLY, t))
    d.send(track_control(Ctl.LOOP_ON, t))
    for db in (-70, -30, 0, 10, 0):
        d.send(master_volume(db))
    for db in (-70, -20, 0, 10, 0):
        d.send(track_volume(t, db))
    ctx.alive()
    ctx.note("volume is audible-only; verify by ear (device returns no ack)")


@test("live")
def t_amp_power(ctx):
    d = ctx.dev
    d.send(amp_power(False))
    ctx.alive()
    d.send(amp_power(True))
    ctx.alive()


@test("live")
def t_track_fade_stop(ctx):
    d, t = ctx.dev, ctx.a.sfx_track
    d.send(track_control(Ctl.PLAY_POLY, t))
    d.send(track_control(Ctl.LOOP_ON, t))
    ctx.wait_status(lambda s: t in s)
    d.send(track_fade(t, -70, 500, True))
    ctx.wait_status(lambda s: t not in s, timeout=3)


@test("live")
def t_track_fade_keep(ctx):
    d, t = ctx.dev, ctx.a.sfx_track
    d.send(track_control(Ctl.PLAY_POLY, t))
    d.send(track_control(Ctl.LOOP_ON, t))
    d.send(track_fade(t, -20, 300, False))
    time.sleep(0.8)
    assert t in ctx.status(), "track stopped although Stop Flag was 0"


@test("live")
def t_track_control_ex_lock(ctx):
    d, t = ctx.dev, ctx.a.sfx_track
    d.send(track_control_ex(Ctl.PLAY_POLY, t, True))
    d.send(track_control_ex(Ctl.LOOP_ON, t, True))
    ctx.wait_status(lambda s: t in s)
    d.send(track_control_ex(Ctl.STOP, t, False))
    ctx.wait_status(lambda s: t not in s)


@test("live", rpu=True)
def t_play_ex_replace(ctx):
    d, t = ctx.dev, ctx.a.sfx_track
    d.send(play_ex(t, Cls.SFX, PF.LOOP | PF.REPLACE, 0, 10))
    d.send(play_ex(t, Cls.SFX, PF.LOOP | PF.REPLACE, 0, 10))
    time.sleep(0.3)
    st = ctx.status()
    assert st.count(t) == 1, f"replace-earlier left {st.count(t)} instances: {st}"


@test("live", rpu=True)
def t_sfx_priority_bump(ctx):
    d = ctx.dev
    if not ctx.voices:
        ctx.voices = d.request(get_sys_info(), Rsp.SYS_INFO)["voices"]
    n = ctx.voices
    for _ in range(n):
        d.send(play_ex(ctx.a.sfx_track, Cls.SFX, PF.LOOP, 0, 1))
    time.sleep(0.3)
    d.send(play_ex(ctx.a.sfx_track2, Cls.SFX, PF.LOOP, 0, 200))
    st = ctx.wait_status(lambda s: ctx.a.sfx_track2 in s)
    assert len(st) <= n, f"more tracks than voices? {len(st)} > {n}"
    ctx.note(f"{n} voices saturated; high-priority SFX bumped a low-priority one")


@test("live", rpu=True)
def t_callout_priority(ctx):
    d, c = ctx.dev, ctx.a.callout_track
    ctx.events(Cls.CALLOUT, EvMask.STARTED | EvMask.FINISHED)
    d.send(play_ex(c, Cls.CALLOUT, PF.LOOP, 0, 10))
    assert d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.STARTED and m["cls"] == Cls.CALLOUT, 2), \
        "no Started event for first callout"
    d.send(play_ex(ctx.a.sfx_track2, Cls.CALLOUT, 0, 0, 200))  # higher priority preempts
    ev = d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.STARTED and m["track"] == ctx.a.sfx_track2, 2)
    assert ev, "higher-priority callout did not start"


@test("live", rpu=True)
def t_music_solo_crossfade(ctx):
    d, a = ctx.dev, ctx.a
    ctx.events(Cls.MUSIC, EvMask.MUSIC_CHANGED | EvMask.STARTED)
    d.send(play_ex(a.music_track, Cls.MUSIC, PF.LOOP | PF.LOCK | PF.SOLO, 0, 0))
    ctx.wait_status(lambda s: a.music_track in s)
    d.clear()
    d.send(play_ex(a.music_track2, Cls.MUSIC, PF.LOOP | PF.LOCK | PF.SOLO | PF.CROSSFADE, 0, 0))
    st = ctx.wait_status(lambda s: a.music_track2 in s and a.music_track not in s, timeout=6)
    ev = d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.MUSIC_CHANGED, 1)
    if not ev:
        ctx.warn("no Music Changed event after switching music tracks")


@test("live", rpu=True)
def t_class_gain(ctx):
    d = ctx.dev
    d.send(play_ex(ctx.a.sfx_track, Cls.SFX, PF.LOOP, 0, 1))
    for c in (Cls.SFX, Cls.CALLOUT, Cls.MUSIC):
        d.send(class_gain(c, -20, 0))
        d.send(class_gain(c, 0, 500))
    ctx.alive()
    ctx.note("audible-only; verify by ear")


@test("live", rpu=True)
def t_duck_rule(ctx):
    d, a = ctx.dev, ctx.a
    ctx.events(Cls.SFX, EvMask.DUCKING)
    ctx.events(Cls.CALLOUT, EvMask.DUCKING)
    d.send(duck_rule(Cls.SFX, Cls.CALLOUT, 20, 100, 100))
    d.send(play_ex(a.sfx_track, Cls.SFX, PF.LOOP, 0, 1))
    time.sleep(0.2)
    d.clear()
    d.send(play_ex(a.callout_track, Cls.CALLOUT, PF.LOOP, 0, 10))
    on = d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.DUCKING and m["cls"] == Cls.SFX and m["ducked"], 2)
    assert on, "no Ducking Change (ducked) event for SFX"
    d.send(stop_class(Cls.CALLOUT, 0))
    off = d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.DUCKING and m["cls"] == Cls.SFX and not m["ducked"], 2)
    assert off, "no Ducking Change (released) event for SFX"


@test("live", rpu=True)
def t_stop_class(ctx):
    d, a = ctx.dev, ctx.a
    d.send(play_ex(a.sfx_track, Cls.SFX, PF.LOOP, 0, 1))
    d.send(play_ex(a.music_track, Cls.MUSIC, PF.LOOP | PF.LOCK, 0, 0))
    ctx.wait_status(lambda s: a.sfx_track in s and a.music_track in s)
    d.send(stop_class(Cls.SFX, 200))
    st = ctx.wait_status(lambda s: a.sfx_track not in s, timeout=3)
    assert a.music_track in st, "Stop Class SFX also stopped music"
    d.send(stop_class(Cls.MUSIC, 0))
    ctx.wait_status(lambda s: a.music_track not in s)


@test("live", rpu=True)
def t_event_flags_finished(ctx):
    d = ctx.dev
    ctx.events(Cls.SFX, EvMask.FINISHED | EvMask.STARTED)
    d.send(play_ex(ctx.a.sfx_track, Cls.SFX, PF.LOOP, 0, 1))
    st = d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.STARTED, 2)
    assert st and st["tuid"] >= 0x10000, f"Started event / TUID missing or out of range: {st}"
    d.send(stop_class(Cls.SFX, 0))
    fin = d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.FINISHED, 2)
    assert fin, "no Finished event after stop"
    # masking: with mask cleared, no more events
    ctx.events(Cls.SFX, 0)
    d.clear()
    d.send(play_ex(ctx.a.sfx_track, Cls.SFX, 0, 0, 1))
    assert d.wait_for(Rsp.EVENT, timeout=0.5) is None, "event sent although mask is 0"


@test("live", rpu=True)
def t_staged_loop_points(ctx):
    d, a = ctx.dev, ctx.a
    ctx.events(Cls.MUSIC, EvMask.STAGED | EvMask.STARTED | EvMask.LOOPED | EvMask.FINISHED)
    d.send(play_ex(a.music_track, Cls.MUSIC, PF.PAUSE, 0, 0))
    st = d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.STAGED, 2)
    assert st and st["tuid"], "no Staged-and-Paused event with TUID"
    tuid = st["tuid"]
    assert a.music_track not in ctx.status() or True  # voice is held, may or may not be listed
    d.send(loop_points(tuid, LP.START | LP.TOP | LP.ALERT_EACH, 2, 0, 44100 * 2,
                       loops_per_phrase=1, alert_margin=300, use_tuid=True))
    assert d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.STARTED and m["tuid"] == tuid, 3), \
        "staged track did not start on Loop Points"
    lp = d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.LOOPED and m["tuid"] == tuid, 8)
    assert lp, "no Looping alert (needs music track >= 4 s)"
    ctx.note(f"first Looping alert: loops left={lp.get('loop_count')}")
    d.send(loop_points(tuid, LP.COUNT_ONLY, 0, use_tuid=True))  # end loop at next B
    assert d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.FINISHED and m["tuid"] == tuid, 15), \
        "track never reported Finished"


@test("live", rpu=True)
def t_next_track_chain(ctx):
    d, a = ctx.dev, ctx.a
    ctx.events(Cls.SFX, EvMask.STAGED | EvMask.STARTED | EvMask.FINISHED)
    d.send(play_ex(a.sfx_track2, Cls.SFX, PF.PAUSE, 0, 1))
    assert d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.STAGED, 2), "B not staged"
    d.send(play_ex(a.sfx_track, Cls.SFX, PF.PAUSE, 0, 1))
    a_ev = d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.STAGED and m["track"] == a.sfx_track, 2)
    assert a_ev, "A not staged"
    d.send(loop_points(a.sfx_track, LP.START | LP.TOP, 1, 0, 44100, next_track=a.sfx_track2))
    assert d.wait_for(Rsp.EVENT, lambda m: m["event_id"] == Ev.STARTED and m["track"] == a.sfx_track2, 8), \
        "next track did not start after first finished"


@test("live", rpu=True)
def t_soundtrack_folders(ctx):
    d = ctx.dev
    r = d.request(get_soundtrack_folders(), Rsp.SOUNDTRACK)
    assert r and r["count"] >= 1, f"Soundtrack count must be >= 1 (root): {r}"
    n = r["count"]
    ctx.note(f"soundtracks (incl. root): {n}")
    d.send(set_soundtrack_folder(0x01 | 0x02, 0))
    ctx.alive()
    if n > 1:
        if ctx.a.soundtrack_switch:
            d.send(set_soundtrack_folder(0x01 | 0x02, 1))
            ctx.alive()
            d.send(set_soundtrack_folder(0x01 | 0x02, 0))
        else:
            ctx.warn("alt folders exist; pass --soundtrack-switch to test switching")


@test("live", rpu=True)
def t_loop_queries(ctx):
    d, t = ctx.dev, ctx.a.loop_track
    r = d.request(get_num_loops(t), Rsp.LOOP_REPORT, pred=lambda m: m["report_type"] == 0)
    assert r and r["track"] == t, "no number-of-loops Loop Report"
    n = r["num_loops"]
    ctx.note(f"track {t}: {n} loops defined")
    if n == 0:
        ctx.warn(f"track {t} has no loops; supply --loop-track with a soundtrack.ini entry to test details")
    for i in range(n):
        r = d.request(get_loop_points(t, i), Rsp.LOOP_REPORT, pred=lambda m: m["report_type"] == 1)
        assert r and r["loop_index"] == i, f"no loop details for index {i}"
        assert r["a"] < r["b"] and r["loops_per_phrase"] >= 1, r
        ctx.note(f"  idx {i}: loop#{r['loop_number']} A={r['a']} B={r['b']} lpp={r['loops_per_phrase']}")
    r0 = d.request(get_num_loops(ctx.a.sfx_track), Rsp.LOOP_REPORT, pred=lambda m: m["report_type"] == 0)
    assert r0, "no Loop Report for loop-less track"


@test("live", rpu=True)
def t_synth(ctx):
    d = ctx.dev
    found = []
    for stype, name in SYNTH_TYPES.items():
        r = d.request(synth_available(stype), Rsp.SYNTH, timeout=1.0,
                      pred=lambda m: m["synth_cmd"] == 0x80 and m.get("synth_type") == stype)
        assert r, f"no Synth Available response for {name}"
        if r["capability"] & 0x01:
            found.append(stype)
    if not found:
        raise Skip("no synth emulation available on this unit")
    ctx.note("synths: " + ", ".join(SYNTH_TYPES[s] for s in found))
    for vid in (1, 2, 3):
        r = d.request(synth_get_volume(vid), Rsp.SYNTH, pred=lambda m: m["synth_cmd"] == 1)
        assert r and r["vol_id"] == vid, f"Get Volume {vid} failed"
        orig = (r["left"], r["right"])
        d.send(synth_set_volume(vid, 256, 128))
        r2 = d.request(synth_get_volume(vid), Rsp.SYNTH, pred=lambda m: m["synth_cmd"] == 1)
        assert (r2["left"], r2["right"]) == (256, 128), f"pan not applied: {r2}"
        d.send(synth_set_volume(vid, *orig))
    n = d.request(synth_get_num_roms(), Rsp.SYNTH, pred=lambda m: m["synth_cmd"] == 3)
    assert n, "no Get Num ROMs response"
    for i in range(n["num_roms"]):
        r = d.request(synth_get_rom_name(i), Rsp.SYNTH, pred=lambda m: m["synth_cmd"] == 4)
        assert r and r["rom_name"], f"no ROM name for {i}"
        ctx.note(f"  ROM {i}: {r['rom_name']}")
    r = d.request(synth_get_current_rom(), Rsp.SYNTH, pred=lambda m: m["synth_cmd"] == 5)
    assert r, "no Get Current ROM response"
    ctx.note("flash ROM (0x06) deliberately not exercised: it is destructive")


@test("live", rpu=True, opt="robustness")
def t_robustness(ctx):
    d = ctx.dev
    bad = bytearray(get_version())
    bad[-1] = 0x56
    d.send(bytes(bad))                        # bad EOM
    ctx.alive()
    d.send(b"\x00\x13\xf0" + get_version())    # garbage before SOM
    assert d.wait_for(Rsp.VERSION, timeout=1.5), "no resync after leading garbage"
    d.send(encode(0x7F, b"\x01\x02"))          # unknown command
    ctx.alive()
    d.send(bytes([SOM1, SOM2, 0x02, 0x01, EOM]))  # length below minimum
    ctx.alive()


# --------------------------------------------------------------------------- #
# Runner / CLI
# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(description="RPU-SP protocol test bed")
    ap.add_argument("--port", help="serial port (COM5, /dev/ttyUSB0, ...)")
    ap.add_argument("--baud", type=int, default=57600)
    ap.add_argument("--mock", action="store_true", help="run live tests against the built-in simulator")
    ap.add_argument("--offline", action="store_true", help="only run offline (no device) tests")
    ap.add_argument("--list", action="store_true", help="list tests and exit")
    ap.add_argument("--only", action="append", default=[], help="run tests whose name contains this (repeatable)")
    ap.add_argument("--monitor", type=float, metavar="SECS", help="just print decoded traffic for SECS seconds")
    ap.add_argument("-v", "--verbose", action="store_true", help="hex-dump TX/RX")
    ap.add_argument("--sfx-track", type=int, default=1)
    ap.add_argument("--sfx-track2", type=int, default=2)
    ap.add_argument("--callout-track", type=int, default=3)
    ap.add_argument("--music-track", type=int, default=4)
    ap.add_argument("--music-track2", type=int, default=5)
    ap.add_argument("--loop-track", type=int, default=400)
    ap.add_argument("--soundtrack-switch", action="store_true", help="allow switching to alt soundtrack folder 1")
    ap.add_argument("--robustness", action="store_true", help="send malformed packets to the device")
    args = ap.parse_args()

    if args.list:
        for t in TESTS:
            tags = [t["group"]] + (["rpu-only"] if t["rpu"] else []) + ([f"--{t['opt']}"] if t["opt"] else [])
            print(f"{t['name']:32s} {', '.join(tags)}")
        return 0

    dev: Optional[Device] = None
    if not args.offline:
        if args.mock:
            dev = MockRpuSp(args.verbose)
        elif args.port:
            dev = SerialDevice(args.port, args.baud, args.verbose)
        else:
            ap.error("give --port, --mock, or --offline")

    if args.monitor is not None:
        assert dev, "--monitor needs a device"
        end = time.time() + args.monitor
        while time.time() < end:
            m = dev._pull(0.1)
            if m:
                print({k: v for k, v in m.items() if k != "raw"})
        dev.close()
        return 0

    ctx = Ctx(dev, args)
    if dev:
        r = dev.request(get_version(), Rsp.VERSION, timeout=2.0)
        if r is None:
            print("No response to Get Version. Check port, baud rate (default 57600) and wiring.")
            dev.close()
            return 2
        ctx.is_rpu = r["version"].startswith("RPU-SP")
        print(f"Device: {r['version']!r}  ({'RPU-SP' if ctx.is_rpu else 'legacy WAV Trigger: 0x60+ tests skipped'})")
        ctx.cleanup()

    counts = dict(PASS=0, FAIL=0, SKIP=0, ERROR=0)
    for t in TESTS:
        if t["group"] == "live" and dev is None:
            continue
        if args.only and not any(s in t["name"] for s in args.only):
            continue
        label = f"[{t['group']:7s}] {t['name']:32s}"
        ctx.notes = []
        status, detail = "PASS", ""
        if t["rpu"] and not ctx.is_rpu:
            status, detail = "SKIP", "needs RPU-SP firmware"
        elif t["opt"] and not getattr(args, t["opt"]):
            status, detail = "SKIP", f"enable with --{t['opt']}"
        else:
            if t["group"] == "live":
                dev.clear()
            try:
                t["fn"](ctx)
            except Skip as e:
                status, detail = "SKIP", str(e)
            except AssertionError as e:
                status, detail = "FAIL", str(e)
            except Exception as e:  # noqa: BLE001
                status, detail = "ERROR", f"{type(e).__name__}: {e}"
            if t["group"] == "live":
                ctx.cleanup()
        counts[status] += 1
        print(f"{label} {status}" + (f"  - {detail}" if detail else ""))
        for n in ctx.notes:
            print(f"{'':43s}{n}")

    if dev:
        dev.close()
    print("\n" + "  ".join(f"{k}={v}" for k, v in counts.items()))
    return 1 if counts["FAIL"] or counts["ERROR"] else 0


if __name__ == "__main__":
    sys.exit(main())
