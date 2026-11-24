#!/usr/bin/env python3
"""Host diagnostic CLI and GSC1 screen-capture decoder for TB-J716F USB CDC-ACM.

Connects to the bare-metal SM8250 DWC3 CDC-ACM terminal (VID 0x1209 / PID 0x0001,
serial J716FDBG01) over Windows usbser.sys or Linux cdc_acm without custom drivers.

Subcommands:
  monitor   Stream live multi-core kernel logs (with optional --log capture file)
  exec      Send one or more diagnostic commands (e.g. status, cpufreq, smp) and print replies
  shell     Interactive command terminal + live log output
  snapshot  Capture a GSC1 RGB565 frame from the tablet framebuffer and save as PNG
  probe     Wait for PONG and dump status + cpufreq + smp + display + touch summary
"""
from __future__ import annotations

import argparse
import binascii
import struct
import sys
import threading
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

USB_VID = 0x1209
USB_PID = 0x0001
USB_SERIAL_PREFIX = "J716FDBG"

GSC1_MAGIC = b"\x00GSC1\r\n\x1a"
GSC1_HEADER_FMT = "<8sIIHHIIIIII"
GSC1_HEADER_SIZE = struct.calcsize(GSC1_HEADER_FMT)
assert GSC1_HEADER_SIZE == 44

GSC1_FLAG_SNAPSHOT = 1 << 0
GSC1_FLAG_STOP = 1 << 1


def crc32_ieee(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def rgb565_to_png(rgb565: bytes, width: int, height: int) -> bytes:
    """Encode little-endian RGB565 pixels to a valid 24-bit RGB PNG using stdlib only."""
    if width <= 0 or height <= 0:
        raise ValueError(f"invalid dimensions: {width}x{height}")
    expected = width * height * 2
    if len(rgb565) != expected:
        raise ValueError(f"rgb565 size mismatch: got {len(rgb565)}, expected {expected}")

    raw_rows = bytearray()
    row_stride = width * 2
    for y in range(height):
        raw_rows.append(0)  # PNG filter type 0 (None)
        base = y * row_stride
        row = rgb565[base : base + row_stride]
        for x in range(0, row_stride, 2):
            px = row[x] | (row[x + 1] << 8)
            r5 = (px >> 11) & 0x1F
            g6 = (px >> 5) & 0x3F
            b5 = px & 0x1F
            raw_rows.append((r5 << 3) | (r5 >> 2))
            raw_rows.append((g6 << 2) | (g6 >> 4))
            raw_rows.append((b5 << 3) | (b5 >> 2))

    def png_chunk(tag: bytes, payload: bytes) -> bytes:
        crc = crc32_ieee(tag + payload)
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(raw_rows), level=6)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", idat)
        + png_chunk(b"IEND", b"")
    )


@dataclass
class Gsc1Frame:
    frame_id: int
    flags: int
    width: int
    height: int
    rgb565: bytes
    frame_crc: int


@dataclass
class _PartialFrame:
    frame_id: int
    flags: int
    width: int
    height: int
    total: int
    frame_crc: int
    buf: bytearray
    received: set[tuple[int, int]] = field(default_factory=set)
    received_bytes: int = 0


class Gsc1StreamDemuxer:
    """Demultiplexes interleaved plain-text kernel logs and binary GSC1 packets."""

    def __init__(
        self,
        on_text: Callable[[bytes], None] | None = None,
        on_frame: Callable[[Gsc1Frame], None] | None = None,
        on_stop: Callable[[int], None] | None = None,
    ) -> None:
        self._buf = bytearray()
        self._on_text = on_text
        self._on_frame = on_frame
        self._on_stop = on_stop
        self._partial: _PartialFrame | None = None
        self.bad_headers = 0
        self.bad_chunks = 0
        self.bad_frames = 0
        self.frames_ok = 0

    def _emit_text(self, data: bytes) -> None:
        if data and self._on_text is not None:
            self._on_text(data)

    def feed(self, data: bytes) -> None:
        if not data:
            return
        self._buf.extend(data)
        while self._buf:
            idx = self._buf.find(GSC1_MAGIC)
            if idx < 0:
                # Keep up to len(GSC1_MAGIC)-1 trailing bytes in case magic spans reads
                keep = 0
                max_tail = min(len(self._buf), len(GSC1_MAGIC) - 1)
                for n in range(max_tail, 0, -1):
                    if GSC1_MAGIC.startswith(bytes(self._buf[-n:])):
                        keep = n
                        break
                emit_len = len(self._buf) - keep
                if emit_len > 0:
                    self._emit_text(bytes(self._buf[:emit_len]))
                    del self._buf[:emit_len]
                return

            if idx > 0:
                self._emit_text(bytes(self._buf[:idx]))
                del self._buf[:idx]

            if len(self._buf) < GSC1_HEADER_SIZE:
                return

            hdr_bytes = bytes(self._buf[:GSC1_HEADER_SIZE])
            (
                _magic,
                frame_id,
                flags,
                width,
                height,
                total,
                offset,
                size,
                chunk_crc,
                frame_crc,
                header_crc,
            ) = struct.unpack(GSC1_HEADER_FMT, hdr_bytes)

            if crc32_ieee(hdr_bytes[8:40]) != header_crc:
                self.bad_headers += 1
                self._emit_text(bytes(self._buf[:1]))
                del self._buf[:1]
                continue

            if flags & GSC1_FLAG_STOP:
                del self._buf[:GSC1_HEADER_SIZE]
                self._partial = None
                if self._on_stop is not None:
                    self._on_stop(frame_id)
                continue

            if (
                frame_id == 0
                or width == 0
                or width > 1264
                or height == 0
                or height > 2780
                or total != width * height * 2
                or size == 0
                or size > 1024
                or offset + size > total
            ):
                self.bad_headers += 1
                del self._buf[:GSC1_HEADER_SIZE]
                continue

            if len(self._buf) < GSC1_HEADER_SIZE + size:
                return

            payload = bytes(self._buf[GSC1_HEADER_SIZE : GSC1_HEADER_SIZE + size])
            del self._buf[: GSC1_HEADER_SIZE + size]

            if crc32_ieee(payload) != chunk_crc:
                self.bad_chunks += 1
                continue

            if self._partial is None or self._partial.frame_id != frame_id:
                self._partial = _PartialFrame(
                    frame_id=frame_id,
                    flags=flags,
                    width=width,
                    height=height,
                    total=total,
                    frame_crc=frame_crc,
                    buf=bytearray(total),
                )

            p = self._partial
            span = (offset, size)
            if span not in p.received:
                p.buf[offset : offset + size] = payload
                p.received.add(span)
                p.received_bytes += size

            if p.received_bytes == p.total:
                raw = bytes(p.buf)
                self._partial = None
                if crc32_ieee(raw) != p.frame_crc:
                    self.bad_frames += 1
                    continue
                self.frames_ok += 1
                if self._on_frame is not None:
                    self._on_frame(
                        Gsc1Frame(
                            frame_id=p.frame_id,
                            flags=p.flags,
                            width=p.width,
                            height=p.height,
                            rgb565=raw,
                            frame_crc=p.frame_crc,
                        )
                    )


def find_j716f_port() -> tuple[str | None, str | None, str | None]:
    import serial.tools.list_ports

    for port in serial.tools.list_ports.comports():
        if port.vid == USB_VID and port.pid == USB_PID:
            return port.device, port.description, port.serial_number
        if port.serial_number and USB_SERIAL_PREFIX in port.serial_number.upper():
            return port.device, port.description, port.serial_number
    return None, None, None


def wait_for_port(explicit_port: str | None, timeout_s: float) -> str:
    if explicit_port:
        return explicit_port
    deadline = time.monotonic() + timeout_s
    while True:
        dev, desc, sn = find_j716f_port()
        if dev:
            print(f"[host] Connected candidate: {dev} ({desc}, sn={sn})", file=sys.stderr, flush=True)
            return dev
        if time.monotonic() >= deadline:
            raise SystemExit(
                f"Timed out after {timeout_s:.1f}s waiting for TB-J716F USB CDC-ACM "
                f"(VID {USB_VID:04x}:PID {USB_PID:04x})"
            )
        time.sleep(0.3)


def open_serial(port: str):
    import serial

    ser = serial.Serial(port, 115200, timeout=0.15)
    ser.dtr = True
    ser.rts = True
    return ser


def drain_initial(ser, demux: Gsc1StreamDemuxer, max_s: float = 3.5, idle_s: float = 0.6) -> None:
    t0 = time.monotonic()
    last_rx = t0
    while time.monotonic() - t0 < max_s:
        chunk = ser.read(4096)
        if chunk:
            demux.feed(chunk)
            last_rx = time.monotonic()
        elif time.monotonic() - last_rx >= idle_s:
            break


def run_commands(
    ser,
    demux: Gsc1StreamDemuxer,
    commands: list[str],
    per_cmd_max_s: float = 3.0,
    idle_s: float = 0.5,
) -> None:
    for cmd in commands:
        ser.write((cmd.strip() + "\r\n").encode("utf-8"))
        ser.flush()
        t0 = time.monotonic()
        last_rx = t0
        while time.monotonic() - t0 < per_cmd_max_s:
            chunk = ser.read(4096)
            if chunk:
                demux.feed(chunk)
                last_rx = time.monotonic()
            elif time.monotonic() - last_rx >= idle_s:
                break


def cmd_monitor(args: argparse.Namespace) -> int:
    port = wait_for_port(args.port, args.wait)
    log_fp = open(args.log, "ab") if args.log else None

    def on_text(b: bytes) -> None:
        sys.stdout.buffer.write(b)
        sys.stdout.buffer.flush()
        if log_fp:
            log_fp.write(b)
            log_fp.flush()

    demux = Gsc1StreamDemuxer(on_text=on_text)
    try:
        with open_serial(port) as ser:
            t0 = time.monotonic()
            while True:
                chunk = ser.read(4096)
                if chunk:
                    demux.feed(chunk)
                if args.duration > 0 and time.monotonic() - t0 >= args.duration:
                    break
    except KeyboardInterrupt:
        pass
    finally:
        if log_fp:
            log_fp.close()
    return 0


def cmd_exec(args: argparse.Namespace) -> int:
    port = wait_for_port(args.port, args.wait)
    log_fp = open(args.log, "ab") if args.log else None

    def on_text(b: bytes) -> None:
        sys.stdout.buffer.write(b)
        sys.stdout.buffer.flush()
        if log_fp:
            log_fp.write(b)
            log_fp.flush()

    demux = Gsc1StreamDemuxer(on_text=on_text)
    try:
        with open_serial(port) as ser:
            if not args.no_replay:
                drain_initial(ser, demux, max_s=3.5, idle_s=0.6)
            run_commands(ser, demux, args.commands, per_cmd_max_s=args.cmd_timeout)
    finally:
        if log_fp:
            log_fp.close()
    return 0


def cmd_probe(args: argparse.Namespace) -> int:
    args.commands = ["ping", "status", "cpufreq", "battery", "smp", "display", "touch", "usb"]
    args.no_replay = False
    args.cmd_timeout = 2.0
    return cmd_exec(args)


def cmd_snapshot(args: argparse.Namespace) -> int:
    port = wait_for_port(args.port, args.wait)
    out_path = Path(args.output)
    saved: list[Gsc1Frame] = []

    def on_text(b: bytes) -> None:
        if args.verbose:
            sys.stderr.buffer.write(b)
            sys.stderr.buffer.flush()

    def on_frame(frame: Gsc1Frame) -> None:
        saved.append(frame)

    demux = Gsc1StreamDemuxer(on_text=on_text, on_frame=on_frame)
    with open_serial(port) as ser:
        drain_initial(ser, demux, max_s=2.0, idle_s=0.4)
        if args.max_edge:
            ser.write(f"screen.configure {args.max_edge} 2\r\n".encode("utf-8"))
            ser.flush()
            time.sleep(0.2)
        ser.write(b"screen.snapshot\r\n")
        ser.flush()
        deadline = time.monotonic() + args.timeout
        while not saved and time.monotonic() < deadline:
            chunk = ser.read(8192)
            if chunk:
                demux.feed(chunk)

    if not saved:
        print(
            f"[host] Snapshot timed out (bad_hdr={demux.bad_headers} bad_chk={demux.bad_chunks} bad_frm={demux.bad_frames})",
            file=sys.stderr,
        )
        return 1

    frame = saved[0]
    png = rgb565_to_png(frame.rgb565, frame.width, frame.height)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(png)
    print(
        f"[host] Saved snapshot id={frame.frame_id} {frame.width}x{frame.height} "
        f"({len(png)} B PNG, crc=0x{frame.frame_crc:08x}) -> {out_path}"
    )
    return 0


def cmd_shell(args: argparse.Namespace) -> int:
    port = wait_for_port(args.port, args.wait)
    log_fp = open(args.log, "ab") if args.log else None
    stop_evt = threading.Event()

    def on_text(b: bytes) -> None:
        sys.stdout.buffer.write(b)
        sys.stdout.buffer.flush()
        if log_fp:
            log_fp.write(b)
            log_fp.flush()

    demux = Gsc1StreamDemuxer(on_text=on_text)
    try:
        with open_serial(port) as ser:
            def reader() -> None:
                while not stop_evt.is_set():
                    try:
                        chunk = ser.read(4096)
                        if chunk:
                            demux.feed(chunk)
                    except Exception:
                        break

            th = threading.Thread(target=reader, daemon=True)
            th.start()
            print("[host] Interactive TB-J716F shell ready (Ctrl+C or 'exit' to quit)", file=sys.stderr)
            while True:
                line = sys.stdin.readline()
                if not line:
                    break
                if line.strip().lower() in ("exit", "quit"):
                    break
                ser.write((line.rstrip("\r\n") + "\r\n").encode("utf-8"))
                ser.flush()
            stop_evt.set()
            th.join(timeout=1.0)
    except KeyboardInterrupt:
        stop_evt.set()
    finally:
        if log_fp:
            log_fp.close()
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="TB-J716F USB Debug CDC-ACM host tool")
    p.add_argument("--port", default=None, help="Explicit COM/tty port (default: auto-detect 1209:0001)")
    p.add_argument("--wait", type=float, default=30.0, help="Seconds to wait for device enumeration")
    sub = p.add_subparsers(dest="subcmd", required=True)

    pm = sub.add_parser("monitor", help="Stream live multi-core kernel logs")
    pm.add_argument("--log", default=None, help="Save raw log stream to file")
    pm.add_argument("--duration", type=float, default=0.0, help="Stop after N seconds (0 = forever)")
    pm.set_defaults(func=cmd_monitor)

    pe = sub.add_parser("exec", help="Execute one or more commands and print responses")
    pe.add_argument("commands", nargs="+", help="Commands to run (e.g. status cpufreq smp)")
    pe.add_argument("--log", default=None, help="Save captured output to file")
    pe.add_argument("--no-replay", action="store_true", help="Skip draining initial log replay")
    pe.add_argument("--cmd-timeout", type=float, default=3.0, help="Max wait seconds per command")
    pe.set_defaults(func=cmd_exec)

    pp = sub.add_parser("probe", help="Run full TB-J716F health check (ping/status/cpufreq/smp/display/touch/usb)")
    pp.add_argument("--log", default=None, help="Save captured output to file")
    pp.set_defaults(func=cmd_probe)

    ps = sub.add_parser("snapshot", help="Capture GSC1 framebuffer screenshot as PNG")
    ps.add_argument("output", help="Output PNG path")
    ps.add_argument("--max-edge", type=int, default=960, help="Longest output edge (160..1264)")
    ps.add_argument("--timeout", type=float, default=15.0, help="Capture timeout seconds")
    ps.add_argument("-v", "--verbose", action="store_true", help="Print interleaved log text to stderr")
    ps.set_defaults(func=cmd_snapshot)

    psh = sub.add_parser("shell", help="Interactive command terminal")
    psh.add_argument("--log", default=None, help="Save session output to file")
    psh.set_defaults(func=cmd_shell)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
