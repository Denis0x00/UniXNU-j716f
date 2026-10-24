#!/usr/bin/env python3
"""Encrypt source build outputs for local bring-up; never upload plaintext binaries."""
import argparse
import hashlib
import io
import json
import os
import struct
import zipfile
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

MAGIC = b"J716ENC1"
OUTPUTS = (
    "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf",
    "build/libpthread-kernel/libpthread_kernel.a",
    "build/corecrypto-2023/libcorecrypto_kernel_noasm.a",
    "build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a",
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def public_der(key):
    return key.public_bytes(serialization.Encoding.DER,
                            serialization.PublicFormat.SubjectPublicKeyInfo)


def oaep():
    return padding.OAEP(mgf=padding.MGF1(hashes.SHA256()),
                        algorithm=hashes.SHA256(), label=MAGIC)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    keygen = sub.add_parser("keygen")
    keygen.add_argument("--private-key", type=Path, required=True)
    keygen.add_argument("--public-key", type=Path, required=True)
    seal = sub.add_parser("seal")
    seal.add_argument("--root", type=Path, required=True)
    seal.add_argument("--report", type=Path, required=True)
    seal.add_argument("--public-key", type=Path, required=True)
    seal.add_argument("--output", type=Path, required=True)
    decrypt = sub.add_parser("open")
    decrypt.add_argument("--private-key", type=Path, required=True)
    decrypt.add_argument("--input", type=Path, required=True)
    decrypt.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "keygen":
        if args.private_key.exists() or args.public_key.exists():
            raise SystemExit("Refusing to replace an existing transfer key")
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        args.private_key.parent.mkdir(parents=True, exist_ok=True)
        args.public_key.parent.mkdir(parents=True, exist_ok=True)
        with args.private_key.open("xb") as stream:
            os.chmod(args.private_key, 0o600)
            stream.write(key.private_bytes(serialization.Encoding.PEM,
                         serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        args.public_key.write_bytes(key.public_key().public_bytes(
            serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
        print("Public key SHA256:", sha(public_der(key.public_key())))
        return
    if args.output.exists():
        raise SystemExit("Refusing to overwrite an existing output")
    if args.command == "seal":
        report_data = args.report.read_bytes()
        report = json.loads(report_data)
        if report["status"] != "compiled" or report["prelink_performed"]:
            raise SystemExit("Expected a successful source-only build report")
        records = {entry["path"]: entry for entry in report["artifacts"]}
        if set(records) != set(OUTPUTS):
            raise SystemExit("Unexpected build output manifest")
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as target:
            target.writestr("source-build-report.json", report_data)
            for name in OUTPUTS:
                content = (args.root / name).read_bytes()
                if sha(content) != records[name]["sha256"] or len(content) != records[name]["bytes"]:
                    raise SystemExit("Build output differs from its report: " + name)
                target.writestr(name, content)
        key = serialization.load_pem_public_key(args.public_key.read_bytes())
        secret = AESGCM.generate_key(bit_length=256)
        wrapped = key.encrypt(secret, oaep())
        nonce = os.urandom(12)
        header = MAGIC + struct.pack(">H", len(wrapped)) + bytes.fromhex(sha(public_der(key))) + nonce + wrapped
        content = header + AESGCM(secret).encrypt(nonce, archive.getvalue(), header)
    else:
        data = args.input.read_bytes()
        if data[:8] != MAGIC or len(data) < 70:
            raise SystemExit("Not a J716F encrypted artifact")
        size = struct.unpack_from(">H", data, 8)[0]
        end = 54 + size
        key = serialization.load_pem_private_key(args.private_key.read_bytes(), password=None)
        if data[10:42] != bytes.fromhex(sha(public_der(key.public_key()))):
            raise SystemExit("Artifact was encrypted for a different local key")
        secret = key.decrypt(data[54:end], oaep())
        content = AESGCM(secret).decrypt(data[42:54], data[end:], data[:end])
        with zipfile.ZipFile(io.BytesIO(content)) as archive:
            if set(archive.namelist()) != set(OUTPUTS) | {"source-build-report.json"}:
                raise SystemExit("Unexpected decrypted archive contents")
            report = json.loads(archive.read("source-build-report.json"))
            for entry in report["artifacts"]:
                raw = archive.read(entry["path"])
                if sha(raw) != entry["sha256"] or len(raw) != entry["bytes"]:
                    raise SystemExit("Decrypted output differs from its build report")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(content)
    print(json.dumps({"file": args.output.name, "bytes": len(content),
                      "sha256": sha(content)}, indent=2))


if __name__ == "__main__":
    main()
