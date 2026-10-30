#!/usr/bin/env python3
"""Decode an 8-frame FAST-V1 session QR export (or raw 8 KiB session.bin / video / images).

Supports:
  1. Text file containing J716FS1/... lines
  2. Directory of photos (.jpg/.png/.webp) or video (.mp4/.mov/.mkv) via zxingcpp
  3. Raw 8,192-byte session.bin or 256 KiB console.bin

Zero git dependencies.
"""
from __future__ import annotations

import argparse
import binascii
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from typing import Dict, List, Optional

JL_MAGIC = 0x31474F4C4E36314A
JL_RECORD = 0x31524C4A
JL_ARMED = 0x414C4E4A
JL_USED = 0x554C4E4A
JL_COMMIT = 0xC04D17ED
JL_SESSION_BYTES = 8192
JL_SLOTS = 63
JLF_FRAMES = 8
JLF_CHUNK = 1024

SLOT_LABELS: Dict[int, str] = {
    0: "start_begin",
    1: "boot_pt_map",
    2: "arm_init",
    3: "vm_entry",
    4: "vm_perm_map",
    5: "vm_ttbr_sw",
    6: "pmap_boot",
    7: "vm_done",
    8: "ArmDT_cfg",
    9: "ArmDT_iter",
    10: "ArmDT_reg",
    11: "CPU_prepGic",
    12: "CPU_bootSym",
    13: "CPU_reg",
    14: "exec_launchd",
    15: "load_init",
    16: "pid1_create",
    17: "mountroot_ok",
    18: "proc_ro_ok",
    19: "vfs_ok",
    20: "iokit_lck",
    21: "bsd_init",
    22: "kext_sub_ok",
    23: "screen_ok",
    24: "comm_init_ok",
    25: "comm_txt_ok",
    26: "mac_ok",
    27: "trust_ok",
    28: "all_sub_ok",
    29: "IRQ2_eoi",
    30: "IRQ8_enter",
    31: "IRQ8_rtclk",
    32: "IRQ8_eoi",
    33: "serial_ok",
    34: "pe_plat",
    35: "cpu_tb",
    36: "pe_fiq",
    37: "early_rng",
    38: "kboot_ent",
    39: "kdbg_1st",
    40: "EARLY_FAULT",
    41: "EL1_SYNC_FLT",
    42: "ANY_SYNC_FLT",
    43: "BOOT_MAP_ERR",
    44: "PERM_MAP_ERR",
    45: "PANIC_CALL",
    46: "PANIC_STR",
    47: "KDBG_LAST",
    48: "SERR_FIQ",
    49: "LOAD_CTX",
    50: "sched_pre1",
    51: "sched_post1",
    52: "sched_pre8",
    53: "sched_post8",
    54: "kboot_thr",
    55: "iokit_pre",
    56: "iokit_post",
    57: "vm_mem_ok",
    58: "con_ok",
    59: "sched_ok",
    60: "mach_ok",
    61: "clock_ok",
    62: "thread_ok",
}


def jl_crc(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def parse_frame_string(text: str) -> Optional[dict]:
    s = text.strip()
    if not s.startswith("J716FS1/"):
        return None
    parts = s.split("/")
    if len(parts) != 9:
        raise ValueError(f"Malformed J716FS1 frame (expected 9 fields, got {len(parts)})")
    prefix, cand_hex, sess_hex, sha_hex, crc_hex, idx_hex, total_hex, data_hex, fcrc_hex = parts
    body_with_slash = "/".join(parts[:8]) + "/"
    expected_fcrc = jl_crc(body_with_slash.encode("ascii"))
    actual_fcrc = int(fcrc_hex, 16)
    if expected_fcrc != actual_fcrc:
        raise ValueError(f"Frame CRC mismatch on index {idx_hex}: expected {expected_fcrc:08X}, got {actual_fcrc:08X}")
    idx = int(idx_hex, 16)
    total = int(total_hex, 16)
    chunk = bytes.fromhex(data_hex)
    if total != JLF_FRAMES or not (0 <= idx < JLF_FRAMES) or len(chunk) != JLF_CHUNK:
        raise ValueError(f"Invalid frame geometry idx={idx} total={total} len={len(chunk)}")
    return {
        "candidate_hex": cand_hex.lower(),
        "session": int(sess_hex, 16),
        "sha256": sha_hex.lower(),
        "session_crc": int(crc_hex, 16),
        "index": idx,
        "chunk": chunk,
    }


def extract_qr_strings_from_media(path: Path) -> List[str]:
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "unixnu_workspace/checks/native-export-python"))
    import zxingcpp  # type: ignore
    found: List[str] = []
    if path.is_dir():
        from PIL import Image  # type: ignore
        images = sorted(p for p in path.iterdir() if p.suffix.lower() in {".png", ".jpg", ".jpeg", ".webp", ".bmp"})
        for img_path in images:
            img = Image.open(img_path)
            for res in zxingcpp.read_barcodes(img):
                if res.text and res.text.startswith("J716FS1/"):
                    found.append(res.text)
        return found

    if path.suffix.lower() in {".png", ".jpg", ".jpeg", ".webp", ".bmp"}:
        from PIL import Image  # type: ignore
        img = Image.open(path)
        for res in zxingcpp.read_barcodes(img):
            if res.text and res.text.startswith("J716FS1/"):
                found.append(res.text)
        return found

    # Video file: sample via cv2 with automatic step=1 + Otsu fallback
    try:
        import cv2  # type: ignore

        for pass_idx in (1, 2):
            found = []
            cap = cv2.VideoCapture(str(path))
            step = 1 if pass_idx == 2 else 2
            fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
            if pass_idx == 1 and fps >= 24.0:
                step = max(1, int(fps // 12))
            frame_no = 0
            while True:
                ok, frame = cap.read()
                if not ok:
                    break
                if frame_no % step == 0:
                    for res in zxingcpp.read_barcodes(frame):
                        if res.text and res.text.startswith("J716FS1/"):
                            found.append(res.text)
                    if pass_idx == 2:
                        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                        _, otsu = cv2.threshold(gray, 0, 255, cv2.THRESH_BINARY | cv2.THRESH_OTSU)
                        for res in zxingcpp.read_barcodes(otsu):
                            if res.text and res.text.startswith("J716FS1/"):
                                found.append(res.text)
                frame_no += 1
            cap.release()
            unique_indices = set()
            for s in found:
                p = parse_frame_string(s)
                if p:
                    unique_indices.add(p["index"])
            if len(unique_indices) == JLF_FRAMES or pass_idx == 2:
                return found
        return found
    except ImportError:
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(
                ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(path), "-vf", "fps=6", f"{tmp}/f_%04d.png"],
                check=True,
            )
            return extract_qr_strings_from_media(Path(tmp))


def reassemble_frames(strings: List[str]) -> bytes:
    parsed = [parse_frame_string(line) for line in strings]
    frames = [p for p in parsed if p is not None]
    if not frames:
        raise ValueError("No valid J716FS1/ QR frames found")
    sessions = {f["session"] for f in frames}
    if len(sessions) != 1:
        raise ValueError(f"Multiple session IDs mixed in input: {[hex(s) for s in sessions]}")
    by_idx = {f["index"]: f for f in frames}
    missing = [i for i in range(JLF_FRAMES) if i not in by_idx]
    if missing:
        raise ValueError(f"Missing QR frame indices (0..7): {missing} (have {sorted(by_idx)})")
    raw = b"".join(by_idx[i]["chunk"] for i in range(JLF_FRAMES))
    ref = by_idx[0]
    actual_sha = hashlib.sha256(raw).hexdigest()
    actual_crc = jl_crc(raw)
    if actual_sha != ref["sha256"]:
        raise ValueError(f"Session SHA-256 mismatch: expected {ref['sha256']}, got {actual_sha}")
    if actual_crc != ref["session_crc"]:
        raise ValueError(f"Session CRC32 mismatch: expected {ref['session_crc']:08X}, got {actual_crc:08X}")
    return raw


def decode_session_bytes(raw: bytes) -> dict:
    if len(raw) != JL_SESSION_BYTES:
        raise ValueError(f"Expected {JL_SESSION_BYTES} bytes, got {len(raw)}")
    magic, version, total_bytes = struct.unpack_from("<QII", raw, 0)
    cand = struct.unpack_from("<4Q", raw, 16)
    session, offset, hdr_crc, state = struct.unpack_from("<QIII", raw, 48)
    reserved = raw[68:128]
    if magic != JL_MAGIC or version != 1 or total_bytes != JL_SESSION_BYTES:
        raise ValueError(f"Invalid JLHeader magic={magic:#x} version={version} bytes={total_bytes}")
    header_crc_ok = hdr_crc == jl_crc(raw[:60]) and not any(reserved)
    cand_bytes = b"".join(struct.pack("<Q", w) for w in cand)
    state_name = "ARMED" if state == JL_ARMED else ("USED" if state == JL_USED else f"UNKNOWN({state:#x})")

    records = []
    faults = []
    for slot in range(JL_SLOTS):
        rec = raw[128 + slot * 128 : 128 + (slot + 1) * 128]
        if not any(rec):
            continue
        rmagic, event, rsession, ordinal, counter = struct.unpack_from("<IIQQQ", rec, 0)
        data = struct.unpack_from("<10Q", rec, 32)
        rcrc, commit, rres = struct.unpack_from("<IIQ", rec, 112)
        valid = (
            state == JL_USED
            and commit == JL_COMMIT
            and rmagic == JL_RECORD
            and event == slot
            and ordinal == slot + 1
            and rsession == session
            and rres == 0
            and rcrc == jl_crc(rec[:112])
        )
        label = SLOT_LABELS.get(slot, f"slot_{slot}")
        entry = {
            "slot": slot,
            "label": label,
            "valid": valid,
            "counter": f"0x{counter:016x}",
            "data": [f"0x{x:016x}" for x in data],
        }
        if slot in (40, 41, 42):
            esr = data[0]
            ec = (esr >> 26) & 0x3F
            iss = esr & 0x1FFFFFF
            entry["fault_decode"] = {
                "esr_el1": f"0x{esr:016x}",
                "ec": f"0x{ec:02x}",
                "iss": f"0x{iss:07x}",
                "far_el1": f"0x{data[1]:016x}",
                "elr_el1": f"0x{data[2]:016x}",
                "sctlr_el1": f"0x{data[3]:016x}",
                "ttbr1_el1": f"0x{data[4]:016x}",
                "sp": f"0x{data[5]:016x}",
            }
            faults.append(entry)
        elif 24 <= slot <= 32:
            entry["irq_decode"] = {
                "intid": f"0x{data[0]:x}",
                "timer_irqs": data[1],
                "trace7": f"0x{data[2]:016x}",
                "trace8": f"0x{data[3]:016x}",
            }
        else:
            entry["reg_decode"] = {
                "arg0": f"0x{data[0]:016x}",
                "arg1": f"0x{data[1]:016x}",
                "arg2": f"0x{data[2]:016x}",
                "sctlr_el1": f"0x{data[3]:016x}",
                "ttbr1_el1": f"0x{data[4]:016x}",
                "sp": f"0x{data[5]:016x}",
                "cntfrq_el0": data[6],
                "mpidr_el1": f"0x{data[7]:016x}",
                "window_va": f"0x{data[8]:016x}",
                "daif": f"0x{data[9]:016x}",
            }
            if slot in (39, 47):
                raw_str = struct.pack("<QQ", data[1], data[2]).split(b"\x00", 1)[0]
                entry["kdbg_text"] = raw_str.decode("ascii", errors="replace")
            elif slot == 46:
                raw_str = struct.pack("<QQ", data[0], data[1]).split(b"\x00", 1)[0]
                entry["panic_text_prefix"] = raw_str.decode("ascii", errors="replace")
        records.append(entry)

    return {
        "session_id": f"0x{session:016x}",
        "state": state_name,
        "offset": offset,
        "header_crc_ok": header_crc_ok,
        "candidate_sha256": cand_bytes.hex(),
        "session_sha256": hashlib.sha256(raw).hexdigest(),
        "session_crc32": f"0x{jl_crc(raw):08x}",
        "non_empty_slots": len(records),
        "valid_slots": sum(1 for r in records if r["valid"]),
        "stage_progression": [f"{r['slot']}:{r['label']}" for r in records],
        "faults": faults,
        "records": records,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="QR text file, media file/folder, or 8 KiB session.bin")
    parser.add_argument("--out-dir", type=Path, help="Directory to write session.bin and session.json")
    args = parser.parse_args()

    inp = args.input
    if inp.is_file() and inp.stat().st_size == JL_SESSION_BYTES:
        raw = inp.read_bytes()
    elif inp.is_file() and inp.suffix.lower() in {".txt", ".log", ".qr"}:
        lines = [ln for ln in inp.read_text(encoding="utf-8", errors="replace").splitlines() if "J716FS1/" in ln]
        raw = reassemble_frames(lines)
    else:
        raw = reassemble_frames(extract_qr_strings_from_media(inp))

    summary = decode_session_bytes(raw)
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "session.bin").write_bytes(raw)
        (args.out_dir / "session.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    print(f"Session {summary['session_id']}  State={summary['state']}  Offset=0x{summary['offset']:x}  "
          f"Valid={summary['valid_slots']}/{summary['non_empty_slots']}")
    print("Progression:", " -> ".join(summary["stage_progression"]) if summary["stage_progression"] else "(none)")
    if summary["faults"]:
        for flt in summary["faults"]:
            fd = flt["fault_decode"]
            print(f"FAULT [{flt['slot']}:{flt['label']}] ESR={fd['esr_el1']} (EC={fd['ec']} ISS={fd['iss']}) "
                  f"FAR={fd['far_el1']} ELR={fd['elr_el1']}")
    else:
        print("Faults: none")


if __name__ == "__main__":
    main()
