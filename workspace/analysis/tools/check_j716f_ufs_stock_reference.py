"""Read-only stock boot/DTBO resource audit; writes only two new report files.

No original script imports, subprocesses, device/VM access, mounts, extraction
files or firmware writes. Parsing is bounded to the two original image buffers.
"""
from pathlib import Path
import datetime
import hashlib
import json
import re
import struct

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "analysis/ufs-adapter-20261113"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_record(path):
    data = path.read_bytes()
    return {"path": path.relative_to(ROOT).as_posix(), "bytes": len(data), "sha256": digest(data)}


def cells(data):
    if len(data) % 4:
        raise ValueError("unaligned cell property")
    return list(struct.unpack(">" + "I" * (len(data) // 4), data))


def strings(data):
    if not data or data[-1] != 0:
        raise ValueError("nonterminated FDT string property")
    return data[:-1].decode("ascii").split("\0")


def parse_fdt(data):
    if len(data) < 40:
        raise ValueError("short FDT header")
    magic, total, off_struct, off_strings, _, version, compat, _, n_strings, n_struct = struct.unpack_from(">10I", data)
    if magic != 0xD00DFEED or total != len(data) or version < 17 or compat > 17:
        raise ValueError("FDT header/extent mismatch")
    if off_struct > total or n_struct > total - off_struct or off_strings > total or n_strings > total - off_strings:
        raise ValueError("FDT section bounds")
    string_table = data[off_strings:off_strings + n_strings]
    end_struct = off_struct + n_struct
    at, stack, nodes = off_struct, [], {}
    while at + 4 <= end_struct:
        token = struct.unpack_from(">I", data, at)[0]
        at += 4
        if token == 1:
            end = data.index(0, at, end_struct)
            name = data[at:end].decode("ascii")
            if (not stack and name) or "/" in name:
                raise ValueError("invalid node name")
            stack.append(name)
            path = "/".join(stack) or "/"
            if path in nodes:
                raise ValueError("duplicate FDT node")
            nodes[path] = {}
            at = (end + 4) & ~3
        elif token == 2:
            if not stack:
                raise ValueError("unbalanced FDT node")
            stack.pop()
        elif token == 3:
            if not stack or at + 8 > end_struct:
                raise ValueError("invalid property header")
            length, name_offset = struct.unpack_from(">2I", data, at)
            at += 8
            if length > end_struct - at or name_offset >= len(string_table):
                raise ValueError("property bounds")
            end = string_table.index(0, name_offset)
            name = string_table[name_offset:end].decode("ascii")
            props = nodes["/".join(stack) or "/"]
            if name in props:
                raise ValueError("duplicate property")
            props[name] = data[at:at + length]
            at = (at + length + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack:
                raise ValueError("FDT end with open node")
            return nodes
        else:
            raise ValueError(f"unknown FDT token {token}")
    raise ValueError("missing FDT end")


def display(data):
    result = {"bytes": len(data), "hex": data.hex()}
    if len(data) % 4 == 0:
        result["cells"] = [hex(v) for v in cells(data)]
    if data and data[-1] == 0 and all(x == 0 or 32 <= x < 127 for x in data):
        result["strings"] = strings(data)
    return result


def parent(path):
    return path.rsplit("/", 1)[0] or "/"


def reg_ranges(nodes, path):
    props = nodes[parent(path)]
    ac = cells(props.get("#address-cells", struct.pack(">I", 2)))[0]
    sc = cells(props.get("#size-cells", struct.pack(">I", 1)))[0]
    if ac not in (1, 2) or sc not in (1, 2):
        raise ValueError("unsupported reg cell widths")
    raw = cells(nodes[path]["reg"])
    if len(raw) % (ac + sc):
        raise ValueError("bad reg tuple length")
    names = strings(nodes[path]["reg-names"]) if "reg-names" in nodes[path] else []
    out = []
    for pos in range(0, len(raw), ac + sc):
        addr = size = 0
        for value in raw[pos:pos + ac]:
            addr = (addr << 32) | value
        for value in raw[pos + ac:pos + ac + sc]:
            size = (size << 32) | value
        n = len(out)
        out.append({"address": hex(addr), "bytes": hex(size), "name": names[n] if n < len(names) else None})
    return out


def resources(nodes):
    host = "/soc/ufshc@1d84000"
    gcc = "/soc/qcom,gcc@100000"
    phy_handle = cells(nodes[host]["phys"])[0]
    phys = [path for path, props in nodes.items() if props.get("phandle") == struct.pack(">I", phy_handle)]
    assert len(phys) == 1
    smmus = [path for path, props in nodes.items() if "reg" in props and
             props.get("compatible") and "qcom,qsmmu-v500" in strings(props["compatible"]) and
             reg_ranges(nodes, path)[0]["address"] == "0x15000000"]
    assert len(smmus) == 1
    selected = {"host": host, "phy": phys[0], "smmu": smmus[0], "gcc": gcc}
    keep = ("compatible", "reg", "reg-names", "interrupts", "interrupt-parent", "phys", "phy-names",
            "lanes-per-direction", "clock-names", "clocks", "status", "#iommu-cells", "qcom,skip-init",
            "dma-coherent", "dma-noncoherent", "iommus")
    out = {}
    for kind, path in selected.items():
        props = nodes[path]
        out[kind] = {"path": path, "reg": reg_ranges(nodes, path),
                     "properties": {key: display(props[key]) for key in keep if key in props},
                     "present": {key: key in props for key in ("dma-coherent", "dma-noncoherent", "iommus")}}
    irq = cells(nodes[host]["interrupts"])
    assert irq == [0, 265, 4]
    out["irq"] = {"raw_cells": irq, "type": "GIC SPI", "spi": 265, "intid": 297, "flags": 4,
                  "trigger": "level high", "adapter_mode": "polling; no IRQ registration"}
    out["host_ancestry"] = {path: {key: display(value) for key, value in nodes[path].items()
                                      if key in ("#address-cells", "#size-cells", "ranges", "dma-ranges", "dma-coherent", "dma-noncoherent", "interrupt-parent")}
                              for path in ("/", "/soc", host)}
    # This stock tree uses an explicit 32-bit child -> 64-bit parent identity
    # window, not an empty ranges property. Preserve and check that mapping.
    assert cells(nodes["/soc"]["#address-cells"]) == [1]
    assert cells(nodes["/soc"]["#size-cells"]) == [1]
    assert cells(nodes["/"]["#address-cells"]) == [2]
    assert cells(nodes["/soc"]["ranges"]) == [0, 0, 0, 0xffffffff]
    return out


def overlay_targets(base, overlay, wanted):
    # Resolve only symbolic references to relevant targets; never apply an
    # overlay or change retained trees. Empty results explicitly remain empty.
    symbols = base.get("/__symbols__", {})
    result = []
    for symbol, locations in overlay.get("/__fixups__", {}).items():
        if symbol not in symbols:
            continue
        target = strings(symbols[symbol])[0]
        if target not in wanted:
            continue
        for location in strings(locations):
            node, prop, byte_offset = location.rsplit(":", 2)
            item = {"symbol": symbol, "target": target, "fixup": location}
            fragment = node + "/__overlay__"
            if prop == "target" and byte_offset == "0" and fragment in overlay:
                item["overlay_properties"] = {k: display(v) for k, v in overlay[fragment].items()}
            result.append(item)
    return result


def main():
    original = ROOT / "TB_J716F_ZUI_15.0.485/image"
    retained = ROOT / "analysis/firmware"
    inputs = [original / "boot.img", original / "dtbo.img", retained / "boot-base-00.dtb",
              retained / "dtbo-overlay-01.dtbo", retained / "j716f-stock-base00-overlay01.dtb",
              retained / "original-file-sha256.json"]
    before = [file_record(path) for path in inputs]
    expected = {item["path"].replace("\\", "/"): item for item in json.loads(inputs[-1].read_text(encoding="utf-8-sig"))}
    checks = []

    def check(name, value):
        checks.append({"name": name, "passed": bool(value)})
        if not value:
            raise ValueError(name)

    for item in before[:2]:
        reference = expected["image/" + Path(item["path"]).name]
        check(item["path"] + " original manifest", item["sha256"] == reference["sha256"] and item["bytes"] == reference["bytes"])
    boot, dtbo = inputs[0].read_bytes(), inputs[1].read_bytes()
    check("Android boot magic", boot[:8] == b"ANDROID!")
    u32 = lambda off: struct.unpack_from("<I", boot, off)[0]
    page = u32(36)
    check("v2 boot header", u32(40) == 2 and page == 4096)
    aligned = lambda value: (value + page - 1) // page * page
    dtb_offset = page + sum(aligned(u32(off)) for off in (8, 16, 24, 1632))
    check("recomputed boot DTB offset", dtb_offset == 40689664)
    base_size = struct.unpack_from(">I", boot, dtb_offset + 4)[0]
    check("base DTB inside declared boot DTB area", base_size <= u32(1648) and dtb_offset + u32(1648) <= len(boot))
    base_blob = boot[dtb_offset:dtb_offset + base_size]
    check("direct boot first-DTB equals retained base00", base_blob == inputs[2].read_bytes())
    magic, total, header_size, entry_size, count, entries, dtbo_page, version = struct.unpack_from(">8I", dtbo)
    check("DTBO table extent", magic == 0xD7B7AB1E and header_size >= 32 and entry_size >= 32 and
          count > 1 and entries >= header_size and entries + count * entry_size <= total <= len(dtbo))
    size, offset, ident, revision, *custom = struct.unpack_from(">8I", dtbo, entries + entry_size)
    check("DTBO entry1 extent", offset >= entries + count * entry_size and offset + size <= total)
    overlay_blob = dtbo[offset:offset + size]
    check("direct DTBO entry1 equals retained overlay01", overlay_blob == inputs[3].read_bytes())
    merged_blob = inputs[4].read_bytes()
    merge_record = json.loads((retained / "firmware-dtb-candidate-review.json").read_text(encoding="utf-8-sig"))
    check("retained merged DTB matches independent historical review", digest(merged_blob) == merge_record["files"]["candidate"]["sha256"])
    base, overlay, merged = parse_fdt(base_blob), parse_fdt(overlay_blob), parse_fdt(merged_blob)
    base_resources, merged_resources = resources(base), resources(merged)
    expected_ranges = {"host": [("0x1d84000", "0x3000"), ("0x1d90000", "0x8000")],
                       "phy": [("0x1d87000", "0xe00"), ("0x1d90000", "0x8000")],
                       "smmu": [("0x15000000", "0x100000"), ("0x15182000", "0x20")],
                       "gcc": [("0x100000", "0x1f0000")]}
    for name, ranges in expected_ranges.items():
        for label, facts in (("raw base", base_resources), ("retained merged", merged_resources)):
            check(label + " " + name + " resource extent", [(v["address"], v["bytes"]) for v in facts[name]["reg"]] == ranges)
    for label, nodes in (("raw base", base), ("retained merged", merged)):
        props = nodes["/soc/ufshc@1d84000"]
        check(label + " host compatible", strings(props["compatible"]) == ["qcom,ufshc"])
        check(label + " host lacks iommus/dma-coherent", "iommus" not in props and "dma-coherent" not in props)
        check(label + " two UFS lanes", cells(props["lanes-per-direction"]) == [2])
    phy_path = base_resources["phy"]["path"]
    check("base PHY binding absent until overlay", "compatible" not in base[phy_path])
    check("overlay adds PHY v4 binding", strings(merged[phy_path]["compatible"]) == ["qcom,ufs-phy-qmp-v4"])
    profile_path = ROOT / "work/ShengUfs-sm8250/UfsPlatform.h"
    profile = profile_path.read_text(encoding="utf-8-sig")
    check("current profile host/SMMU matches stock", bool(re.search(r"\{0x01d84000,\s*0x3000\},\s*\{0x15000000,\s*0x100000\}", profile)))
    check("current profile GCC/IRQ matches stock", "0x00100000, 0x77004, 0x77000" in profile and "{0xe0, 0x4e0}, 297" in profile)
    gcc_offsets = [("AXI", "0x77010", True), ("AHB", "0x77018", True), ("ICE", "0x77064", True),
                   ("AUX", "0x7709c", True), ("RX0", "0x77020", False), ("RX1", "0x770b8", False),
                   ("TX0", "0x7701c", False), ("UniPro", "0x7705c", True), ("AGGRE AXI", "0x770cc", True)]
    parsed_clocks = [(v, flag == "true") for v, flag in re.findall(r"\{(0x[0-9a-f]+),\s*(true|false)\}", profile)]
    check("current profile GCC branch table matches fixed reference plan", parsed_clocks == [(offset, gated) for _, offset, gated in gcc_offsets])
    linux_index = json.loads((ROOT / "analysis/driver-adaptation-20261113/ufs-linux-source-index.json").read_text())
    linux_sources = []
    for item in linux_index["files"]:
        current = file_record(ROOT / item["path"])
        check("fixed Linux source " + Path(item["path"]).name, current["sha256"] == item["sha256"])
        linux_sources.append({**current, "url": item["url"]})
    linux_dts = (ROOT / linux_index["files"][0]["path"]).read_text()
    check("fixed Linux dual-SID source", bool(re.search(r"iommus\s*=\s*<&apps_smmu\s+0x0?e0\s+(?:0x0|0)>,\s*<&apps_smmu\s+0x4e0\s+(?:0x0|0)>", linux_dts)))
    linux_gcc = (ROOT / linux_index["files"][1]["path"]).read_text()
    check("GCC planned offsets present in fixed Linux source", all(offset in linux_gcc for _, offset, _ in gcc_offsets) and "0x77004" in linux_gcc and "0x77000" in linux_gcc)
    loose_sources = [p.relative_to(ROOT).as_posix() for p in original.parent.rglob("*")
                     if p.is_file() and p.suffix.lower() in (".c", ".h", ".dts", ".dtsi")]
    after = [file_record(path) for path in inputs]
    check("all six original/retained input files unchanged", before == after)
    report = {
        "schema_version": 1, "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "scope": "Read-only ROM binary resource recheck; no original tools, device/VM, image output or source disassembly",
        "package_directory": "TB_J716F_ZUI_15.0.485", "input_hash_scope": "boot.img/dtbo.img plus four named retained inputs, not every package file",
        "inputs_before": before, "inputs_after": after, "checks": checks, "all_passed": True,
        "boot": {"header_version": u32(40), "page_size": page, "dtb_offset": dtb_offset,
                 "declared_dtb_bytes": u32(1648), "first_dtb_bytes": base_size, "sha256": digest(base_blob)},
        "dtbo": {"total_size": total, "header_size": header_size, "entry_size": entry_size, "entry_count": count,
                 "entries_offset": entries, "page_size": dtbo_page, "version": version,
                 "selected_index": 1, "offset": offset, "bytes": size, "id": ident, "revision": revision,
                 "custom": custom, "sha256": digest(overlay_blob)},
        "trees": {"base_node_count": len(base), "overlay_node_count": len(overlay), "merged_node_count": len(merged),
                  "merge_recomputed": False, "merge_provenance": "analysis/vm/firmware-dtb-merge-command.sh; existing DTC 1.7.0 raw output hash rechecked",
                  "selection_scope": "DTB0/DTBO1 comes from retained Android observation 20261111-231708, not inferred from package entry order"},
        "raw_base_resources": base_resources, "merged_resources": merged_resources,
        "overlay_related_targets": overlay_targets(base, overlay, {base_resources[k]["path"] for k in expected_ranges}),
        "current_profile": file_record(profile_path), "fixed_linux_sources": linux_sources,
        "linux_reference_gcc_offsets": [{"clock": name, "offset": offset, "hardware_gating": gated} for name, offset, gated in gcc_offsets],
        "source_provenance": {"stock_facts": "HCI/ICE/PHY/APPS SMMU/TCU/GCC extents, compatible strings, host SPI265/INTID297 and two lanes; exact host properties lack iommus/dma-coherent",
            "linux_reference_only": "SIDs 0xe0/0x4e0 and GCC branch/GDSC/reset offsets; they are not stock owner measurements",
            "not_proven_by_rom": "Current powered/readable MMIO, CAP/IDR geometry, secure visibility, firmware/TZ exclusivity, queue drain, SMMU assignments or an owned DMA grant",
            "loose_source_files": loose_sources,
            "source_limit": "No full vendor kernel driver source was identified by this loose source-file inventory; boot.img contains binary kernel/DTBs. No claim of complete decompilation or source recovery."},
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "stock-reference.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    rows = "\n".join(f"| {name} | " + "; ".join(f"{v['address']} / {v['bytes']}" for v in merged_resources[name]["reg"]) + " |" for name in expected_ranges)
    md = f"""# TB-J716F ZUI 15.0.485：UFS 适配原厂资源复核

本轮直接只读核对 `TB_J716F_ZUI_15.0.485/image/boot.img` 和 `dtbo.img`，共 **{len(checks)} 项检查通过**。两个原镜像的字节数和 SHA-256 与历史 `analysis/firmware/original-file-sha256.json` 一致；本轮前后再次哈希，两个原镜像及四个指定历史输入均未改变。没有执行原厂工具、脚本、设备、VM、挂载或写镜像操作。没有声称重新哈希整个 ROM 包。

| 原镜像 | 字节数 | SHA-256 |
| --- | ---: | --- |
| boot.img | {before[0]['bytes']} | `{before[0]['sha256']}` |
| dtbo.img | {before[1]['bytes']} | `{before[1]['sha256']}` |

从 Android v2 header 重新计算 DTB 起点 **{dtb_offset}**，直接切取第一棵树，与保留的 `boot-base-00.dtb` 字节完全相同（{base_size} 字节，`{digest(base_blob)}`）。从 DTBO 表直接读取 entry 1 的偏移 **{offset}**、长度 **{size}**，与 `dtbo-overlay-01.dtbo` 字节完全相同（`{digest(overlay_blob)}`）。这些切片仅在内存中使用，未另写提取文件。

已有合成树 `j716f-stock-base00-overlay01.dtb` 的 {len(merged_blob)} 字节及 SHA-256 `{digest(merged_blob)}` 与历史独立审查吻合。此次重新解析原始切片及已有合成树，未重新运行 fdtoverlay。DTB0/DTBO1 的选择依据仍是 `20261111-231708` Android 观察，包内顺序本身不证明本机选择。

| 原厂资源 | 起点 / 长度 |
| --- | --- |
{rows}

host 是 `/soc/ufshc@1d84000`，compatible 为 `qcom,ufshc`；两条 lane。IRQ 原始三元组 `<0 0x109 4>`，对应 SPI 265 / GIC INTID 297、level-high。本版仍只轮询，不注册该 IRQ。host 和 PHY 节点都在第二段重复引用 ICE，SMMU 的第二段是 TCU；当前受限适配不访问 PHY、ICE 或 TCU。

PHY 的 `qcom,ufs-phy-qmp-v4` binding **不在原始 base00 中**，由 overlay01 的 `fragment@68` 添加，同时设 `status=ok`；`fragment@69` 启用 host 并补电源属性。`/soc/ranges` 是显式 `<0 0 0 0xffffffff>` 恒等映射：子总线 1 address cell、父总线 2 address cells、1 size cell，不能简单当作空 `ranges`。

原始 base 与合成树的 UFS 节点均没有 `iommus`、`dma-coherent` 属性。这不证明 stream bypass、DMA 一致性或运行时拥有权；当前显式 cache maintenance 保持必要。JSON 保留节点原始属性字节、cell、phandle 解析、父总线 cell/ranges 和相关 overlay fixup，避免只按字符串猜资源。

当前 `UfsPlatform.h` 的 HCI `0x1d84000/0x3000`、APPS SMMU `0x15000000/0x100000`、GCC base `0x100000`、INTID 297 与本包一致。**双 SID `0xe0/0x4e0`、GCC 分支/GDSC/reset 偏移来自固定 Linux v6.12 源码**，不是本包 UFS `iommus` 属性或本机 owner 测量。原有三个 Linux 快照的 SHA-256 也已复核；索引及 URL 保留在 JSON。Linux PHY 长度 `0x1000` 不覆盖本包 `0xe00`。

ROM 不能提供当前 HCE/HCS/CAP/IDR、时钟供电可读性、firmware/TZ 独占、完整可见 SMMU、旧 DMA 排空或本次 allocation 的授权。真实启用仍要求外部板级 handoff 事实；不得从节点存在、空闲 CB 或缺少 `iommus` 自动生成 owner。

本包松散 `.c/.h/.dts/.dtsi` 文件盘点为 **{len(loose_sources)}**。这次没有得到完整厂商 kernel 驱动源码包，不能把 boot.img 的二进制内核/DTB 当作源码；没有做或声称完成整内核反编译。此次依据是**原厂二进制中的硬件描述 + 已有 Android 观察 + 单独标识的 Linux/UniXNU 源码参考**。

复核脚本：`analysis/tools/check_j716f_ufs_stock_reference.py`。机器结果：`analysis/ufs-adapter-20261113/stock-reference.json`。没有改写历史 `analysis/firmware` 证据。
"""
    (OUT / "stock-reference.md").write_text(md, encoding="utf-8")
    print(json.dumps({"passed": True, "checks": len(checks), "boot_sha256": before[0]["sha256"],
                      "dtbo_sha256": before[1]["sha256"], "unchanged_inputs": len(inputs)}))


if __name__ == "__main__":
    main()
