#!/usr/bin/env python3
"""Independent stdlib-only byte verification of the isolated XNU first-entry image."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tomllib
import zlib

sys.dont_write_bytecode = True
from verify_menu_artifact import require, sha256, load_json
from verify_initrd80_probe import checked_relative, check_identity, verify_fd_provenance
from verify_first_payload import adt_read, scalar, probe_readback

KERNEL_SHA = "62215d029ca1b96f5c140abf2fb435e97da8b3ae3574ef10b568a0b8a980f3e8"
FD_SHA = "c4443748490e4b028059d9bf44dbbf45eaf4576af5e552e7a176f0bce75a78ce"
DTB_SHA = "6c87f77599a2056befac0080718d826205afed462b9abacac7aa0664e4d3e6e3"
SHIM_SHA = "72bfc9e9137a0e3ab9f7264162968c546ba63533b94d6813814fe84c831d03b4"
BASELINE_ADT_SHA = "97e98f4911923d5b4c9c2b27af582ad74c40c3db7870b35839bea9e1b091acc2"
FAT_BYTES = 83886080


def validate_menu(raw):
    cfg = tomllib.loads(raw.decode("utf-8"))
    require(set(cfg) == {"version", "boot", "ui", "kits", "entry"} and cfg["version"] == 1 and
            cfg["boot"] == {"timeout": -1, "default": "tools", "rotate": 0} and
            cfg["ui"] == {"title": "J716F XNU first entry r1"} and cfg["kits"] == {"autoload": False},
            "Unexpected candidate menu title/default/hold policy")
    xnu = {"id": "xnu", "title": "Start XNU (first entry)", "type": "xnu",
           "path": "Payloads/kernel.macho", "args": "-v serial=3"}
    actions = [("tools", "Tools"), ("exit", "Return to firmware"), ("reset", "Restart"), ("shutdown", "Power off")]
    require(cfg["entry"] == [xnu] + [{"id": a, "title": t, "type": "action", "action": a} for a, t in actions],
            "Unexpected XNU or action entry; installation and extra boot arguments prohibited")
    return cfg


def read_fat32(data):
    """Decode FAT32 independently of mtools and of the official packager."""
    require(len(data) == FAT_BYTES and data[510:512] == b"\x55\xaa", "Invalid FAT32 volume size/signature")
    bps, spc, reserved, nfats, roots, total16, media, spf16 = struct.unpack_from("<HBHBHHBH", data, 11)
    total, spf, flags, version, root_cluster, fsinfo, backup = struct.unpack_from("<IIHHIHH", data, 32)
    require(bps == 512 and spc in (1, 2, 4, 8, 16, 32, 64, 128) and reserved > 0 and nfats == 2 and
            roots == total16 == spf16 == flags == version == 0 and media == 0xf8 and spf > 0,
            "Unsupported FAT32 geometry")
    require(total * bps == len(data) and 0 < fsinfo < reserved and 0 < backup < reserved,
            "FAT32 extent/reserved sectors invalid")
    require(data[66] == 0x29 and data[71:82] == b"J716FENTRY".ljust(11,b" "), "Unexpected FAT32 volume identity")
    data_sector = reserved + nfats * spf
    clusters = (total-data_sector)//spc
    require(65525 <= clusters < 0xffffff5 and (clusters+2)*4 <= spf*bps, "Invalid FAT32 cluster count")
    fat = data[reserved*bps:(reserved+spf)*bps]
    require(fat == data[(reserved+spf)*bps:(reserved+2*spf)*bps], "FAT copies differ")
    require(struct.unpack_from("<I", fat)[0] & 0x0fffffff == 0xffffff8, "Invalid reserved FAT entry")
    require(data[backup*bps:(backup+1)*bps] == data[:bps], "Backup FAT32 boot sector differs")
    info = data[fsinfo*bps:(fsinfo+1)*bps]
    require(info[:4] == b"RRaA" and info[484:488] == b"rrAa" and info[508:512] == b"\0\0\x55\xaa", "Invalid FAT32 FSInfo")
    cluster_bytes, used, files, namespace = spc*bps, set(), {}, set()

    def chain(first):
        parts, seen = [], set()
        if first == 0:
            return b""
        current = first
        while current < 0xffffff8:
            require(2 <= current < clusters+2 and current not in seen and current not in used,
                    "Invalid, cyclic or crosslinked FAT32 chain")
            seen.add(current); used.add(current)
            pos = data_sector*bps + (current-2)*cluster_bytes
            parts.append(data[pos:pos+cluster_bytes])
            current = struct.unpack_from("<I", fat, current*4)[0] & 0xfffffff
        return b"".join(parts)

    def directory(blob, prefix="", depth=0):
        require(depth <= 4, "Unexpected FAT directory depth")
        lfn, checksum, expected_order = {}, None, None
        for pos in range(0, len(blob), 32):
            e = blob[pos:pos+32]
            require(len(e) == 32, "Truncated FAT directory entry")
            if e[0] == 0:
                require(not lfn, "Unterminated LFN chain")
                break
            if e[0] == 0xe5:
                lfn, checksum, expected_order = {}, None, None
                continue
            if e[11] == 0x0f:
                order = e[0] & 0x1f
                require(1 <= order <= 20 and not e[0] & 0xa0 and e[12] == 0 and e[26:28] == b"\0\0", "Invalid LFN entry")
                if e[0] & 0x40:
                    require(not lfn, "Unterminated prior LFN")
                    checksum, expected_order = e[13], order
                require(order == expected_order and order not in lfn and checksum == e[13], "Invalid LFN sequence")
                lfn[order] = e[1:11]+e[14:26]+e[28:32]
                expected_order -= 1
                continue
            if e[11] & 8:
                require(not lfn, "LFN on volume label")
                continue
            if lfn:
                require(expected_order == 0 and sorted(lfn) == list(range(1,max(lfn)+1)), "Incomplete LFN")
                actual = 0
                for byte in e[:11]:
                    actual = (((actual&1)<<7)+(actual>>1)+byte)&255
                require(actual == checksum, "LFN checksum mismatch")
                name = b"".join(lfn[i] for i in sorted(lfn)).decode("utf-16le").split("\0",1)[0].rstrip("\uffff")
            else:
                stem, ext = e[:8].decode("ascii").rstrip(), e[8:11].decode("ascii").rstrip()
                name = stem + ("." + ext if ext else "")
            lfn, checksum, expected_order = {}, None, None
            if name in (".", ".."):
                continue
            require(name and not any(c in name for c in "/\\:"), "Unsafe FAT filename")
            first = (struct.unpack_from("<H",e,20)[0]<<16) | struct.unpack_from("<H",e,26)[0]
            size = struct.unpack_from("<I",e,28)[0]
            path = (prefix+"/"+name).strip("/").lower()
            require(path not in namespace, "Duplicate FAT name")
            namespace.add(path)
            content = chain(first)
            if e[11] & 16:
                require(first >= 2, "Missing directory cluster")
                directory(content,path,depth+1)
            else:
                require(size <= len(content) and len(content)-size < cluster_bytes, "Invalid FAT file extent")
                require(not any(content[size:]), "Nonzero file slack")
                files[path] = content[:size]
    directory(chain(root_cluster))
    for number in range(2,clusters+2):
        require(number in used or (struct.unpack_from("<I",fat,number*4)[0] & 0xfffffff) == 0,
                "Orphaned allocated/bad FAT cluster")
    return files, {"bytes_per_sector":bps,"sectors_per_cluster":spc,"clusters":clusters,"used_clusters":len(used),"type":"FAT32"}


def validate_adt(adt):
    nodes = adt_read(adt)
    chosen = nodes["/chosen"]
    require(scalar(chosen,"unique-chip-id",8) != 0 and scalar(chosen,"chip-id",4) == 0x8011, "Invalid private/OS identity")
    require(len(chosen.get("random-seed",b"")) == 256 and any(chosen["random-seed"]), "Invalid random seed")
    require(scalar(chosen,"dram-base",8) == 0xc0000000 and scalar(chosen,"dram-size",8) == 0x140000000, "Unexpected DRAM inventory")
    require(scalar(chosen,"darwinboot-memory-size",8) == 0x10000000, "Unexpected 256 MiB ownership request")
    for name in ("darwinboot-memory-excluded","darwinboot-memory-outside","darwinboot-memory-outside-kept","root-matching","ephemeral-storage"):
        require(name not in chosen, "Unexpected memory/root/Data property")
    loader = chosen.get("darwinboot-loader-inputs")
    require(loader is None or len(loader) == 64 and not any(loader), "Invalid loader-input placeholder")
    require(set(nodes["/chosen/memory-map"]) == {"name"}, "Unexpected root/Data/trust placeholder")
    require([p for p in nodes if p.startswith("/cpus/")] == ["/cpus/cpu0"] and "/psci" not in nodes, "Unexpected secondary CPU/PSCI")
    cpu = nodes["/cpus/cpu0"]
    require(scalar(cpu,"reg",4) == 0 and cpu.get("state") == b"running\0" and cpu.get("compatible") == b"arm,cortex-a55\0", "Invalid boot CPU")
    require(scalar(cpu,"timebase-frequency",4) == 19200000, "Invalid CPU timebase")
    timer,gic = nodes["/arm-io/timer"], nodes["/arm-io/intc"]
    require(scalar(timer,"clock-frequency",4) == 19200000 and timer.get("compatible") == b"arm,armv8-timer\0", "Incorrect timer")
    require(timer.get("interrupts") == struct.pack("<12I",1,13,0xff08,1,14,0xff08,1,11,0xff08,1,12,0xff08), "Incorrect timer PPIs")
    require("reg" not in timer and gic.get("compatible") == b"arm,gic-v3\0", "Invalid timer/GIC provider")
    require(scalar(gic,"redistributor-stride",8) == 0x20000 and scalar(gic,"#interrupt-cells",4) == 3 and gic.get("interrupt-controller") == b"", "Invalid GIC properties")
    require(scalar(timer,"interrupt-parent",4) == scalar(gic,"phandle",4) != 0, "Missing interrupt provider")
    require(nodes["/arm-io"].get("ranges") == struct.pack("<3Q",0,0x17000000,0x1000000) and gic.get("reg") == struct.pack("<4Q",0xa00000,0x10000,0xa60000,0x100000), "Invalid GIC ranges")
    require(nodes["/"].get("model") == b"iPad7,1\0" and nodes["/"].get("compatible") == b"J120AP\0lenovo,j716f\0qcom,sm8250\0", "Invalid model")
    return {"node_paths":sorted(nodes),"cpu_count":1,"requested_owned_bytes":0x10000000,"timebase_hz":19200000,"identity_value_disclosed":False,"actual_ownership_verified":False}


def compare_adt_baseline(baseline, candidate):
    """Allow only the generated random seed to differ; never disclose property values."""
    require(sha256(baseline) == BASELINE_ADT_SHA, "Unrecognized retained ADT baseline")
    before, after = adt_read(baseline), adt_read(candidate)
    require(set(before) == set(after), "Candidate ADT node set differs from baseline")
    properties = 0
    for node, old in before.items():
        new = after[node]
        require(set(old) == set(new), "Candidate ADT property set differs from baseline")
        for name, value in old.items():
            properties += 1
            if (node, name) == ("/chosen", "random-seed"):
                require(len(value) == len(new[name]) == 256 and any(value) and any(new[name]),
                        "ADT random seed must remain 256 nonzero bytes")
            else:
                require(value == new[name], "Candidate ADT property value differs from baseline")
    return {"result": "passed", "baseline_sha256": BASELINE_ADT_SHA,
            "nodes_compared": len(before), "properties_compared": properties,
            "only_allowed_difference": "/chosen/random-seed", "identity_value_disclosed": False,
            "actual_memory_ownership_verified": False, "hardware_tested": False}


def verify(root):
    root = root.resolve(strict=True)
    manifest = load_json(root/"manifest.json")
    required = {"boot.img","boot.json","payload.img","payload.json","payload.official.json","unixnu.toml","original-kernel.macho","baseline.adt",
                "SILICIUM_UEFI.fd","firmware.dtb","shim.bin","board.toml","fd-provenance.json","fd-source.json","fd-archive-manifest.json","build-result.json"}
    require(required <= manifest["outputs"].keys(), "Incomplete artifact manifest")
    require(manifest.get("hardware_tested") is False and manifest.get("deployment_ready") is False, "Host result cannot claim deployment qualification")
    for name,spec in manifest["outputs"].items():
        path = (root/checked_relative(name)).resolve(strict=True)
        require(path.is_relative_to(root), "Manifest escapes artifact")
        check_identity(path,spec)
    data, ramdisk, fd, dtb, shim, original = [(root/name).read_bytes() for name in ("boot.img","payload.img","SILICIUM_UEFI.fd","firmware.dtb","shim.bin","original-kernel.macho")]
    for raw,digest in ((fd,FD_SHA),(dtb,DTB_SHA),(shim,SHIM_SHA),(original,KERNEL_SHA)):
        require(sha256(raw) == digest, "A fixed candidate input changed")
    firmware = verify_fd_provenance(root,fd)
    require(data[:8] == b"ANDROID!" and 1660 < len(data) <= 100663296, "Invalid Android header/capacity")
    ks,ka,rs,ra,ss,sa,tags,page,version,osver = struct.unpack_from("<10I",data,8)
    recovery,roff,hsize = struct.unpack_from("<IQI",data,1632)
    ds,da = struct.unpack_from("<IQ",data,1648)
    require((ka,rs,ra,ss,sa,tags,page,version,recovery,roff,hsize,ds,da) == (0x8000,FAT_BYTES,0x1000000,0,0,0x100,4096,2,0,0,1660,len(dtb),0x1f00000), "Unexpected boot offsets/size")
    aligned = lambda n:(n+page-1)//page*page
    rp = page+aligned(ks)
    dp = rp+aligned(rs)
    require(ks > 0 and len(data) == dp+aligned(ds), "Invalid boot extent")
    require(not any(data[hsize:page]) and not any(data[page+ks:rp]) and not any(data[rp+rs:dp]) and not any(data[dp+ds:]), "Nonzero boot padding")
    kernel = data[page:page+ks]
    decoder = zlib.decompressobj(16+zlib.MAX_WBITS)
    decoded = decoder.decompress(kernel,len(shim)+len(fd)+1)
    require(decoded == shim+fd and decoder.eof and not decoder.unconsumed_tail and decoder.unused_data == dtb, "Invalid shim/FD gzip or DTB tail")
    require(data[rp:rp+rs] == ramdisk and data[dp:dp+ds] == dtb, "Payload/DTB does not match boot image")
    digest = hashlib.sha1()
    for raw in (kernel,ramdisk,b"",b"",dtb):
        digest.update(raw);digest.update(struct.pack("<I",len(raw)))
    require(data[576:608] == digest.digest()+b"\0"*12,"Android image ID mismatch")
    files,geometry = read_fat32(ramdisk)
    require(set(files) == {"efi/unixnu/unixnu.toml","efi/unixnu/payloads/kernel.macho","efi/unixnu/payloads/kernel.macho.adt"}, "Unexpected payload files")
    require(files["efi/unixnu/unixnu.toml"] == (root/"unixnu.toml").read_bytes(), "Menu bytes differ")
    cfg = validate_menu(files["efi/unixnu/unixnu.toml"])
    xnu,adt = files["efi/unixnu/payloads/kernel.macho"],files["efi/unixnu/payloads/kernel.macho.adt"]
    diagnostic = probe_readback(original,xnu)
    adt_report = validate_adt(adt)
    adt_comparison = compare_adt_baseline((root/"baseline.adt").read_bytes(), adt)
    payload = load_json(root/"payload.json")
    official = load_json(root/"payload.official.json")
    for name in ("kernel_sha256","payload_kernel_sha256","adt_sha256","kernel_args","panic_screen","trust_cache_sha256","gpu_addon_sha256","data_ramdisk_sha256","hardware_tested","skip_setup"):
        require(payload[name] == official[name], "Menu adaptation changed official payload metadata: "+name)
    require(payload["kernel_sha256"] == KERNEL_SHA and payload["payload_kernel_sha256"] == sha256(xnu) and payload["adt_sha256"] == sha256(adt), "Kernel/ADT report mismatch")
    require(payload["payload_sha256"] == sha256(ramdisk) and payload["payload_bytes"] == len(ramdisk) == FAT_BYTES and payload["kernel_args"] == "-v serial=3" and payload["panic_screen"] is True and payload["skip_setup"] is False and payload["hardware_tested"] is False, "Payload metadata mismatch")
    require(all(payload[n] is None for n in ("trust_cache_sha256","gpu_addon_sha256","data_ramdisk_sha256")), "Unexpected optional input")
    build = load_json(root/"build-result.json")
    require(build["status"] == "host-generation-passed" and build["hardware_tested"] is False and build["device_actions"] is False, "Build result not host-success")
    expected = {"image_sha256":sha256(data),"image_bytes":len(data),"ramdisk_bytes":len(ramdisk),"header_version":2,"kernel_address":hex(ka),"tags_address":hex(tags),"dtb_address":hex(da),"dtb_file_offset":dp,"kernel_sha256":sha256(kernel),"fd_sha256":sha256(fd),"dtb_sha256":sha256(dtb),"dtb_bytes":len(dtb),"shim_bytes":len(shim),"hardware_tested":False}
    boot = load_json(root/"boot.json")
    for key,value in expected.items():
        require(boot.get(key) == value,"boot.json mismatch: "+key)
    return {"result":"passed","scope":"Static first-XNU-entry candidate verification", "image":expected,
            "payload_sha256":sha256(ramdisk),"payload_crc32":f"{zlib.crc32(ramdisk):08x}","payload_bytes":len(ramdisk),
            "menu_title":cfg["ui"]["title"],"menu_default":"tools","menu_entry_ids":[e["id"] for e in cfg["entry"]],
            "fat":geometry,"adt":adt_report,"adt_baseline_comparison":adt_comparison,"diagnostic_kernel":diagnostic,"firmware":firmware,
            "manifest_entries_verified":len(manifest["outputs"]),"hardware_tested":False,"deployment_ready":False,
            "files":{name:{"bytes":len(raw),"sha256":sha256(raw)} for name,raw in files.items()},
            "limits":["Runtime CRC must match this exact new payload; prior non-XNU probe does not qualify it.","256 MiB is a loader allocation request, not an allocated/owned range.","No DarwinBoot, EBS, XNU, root or userland execution is inferred from host bytes."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact",type=Path)
    parser.add_argument("--report",type=Path,required=True)
    args = parser.parse_args()
    require(not args.report.exists(),"Refusing to overwrite a report")
    result = verify(args.artifact)
    args.report.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(result,indent=2))
