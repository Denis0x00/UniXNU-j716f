#!/usr/bin/env python3
"""Build freestanding libJ716fAth11kWifi.a (Linux v7.0.14 ath11k + mac80211/cfg80211) for TB-J716F.

Uses an upstream UniXNU drivers/Ath12kWifi workspace (holding the chip-agnostic Linux/FreeBSD
shim, mac80211/cfg80211/MHI/QMI/crypto prep scripts, and locked deps/), overlays the J716F
ath11k QCA6390 adapters from package/wireless/ath11k/, compiles all 110 objects for
arm64-apple-ios17.0, namespaces internal symbols with _nwk, and emits libJ716fAth11kWifi.a.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import shutil
import subprocess
import sys

DEVICE_ROOT = Path(__file__).resolve().parents[1]
ADAPTER_DIR = DEVICE_ROOT / 'package/wireless/ath11k'

KEEP_SYMBOLS = {
    '_nw_wifi_start', '_nw_wifi_step', '_nw_wifi_stop', '_nw_wifi_stats', '_nw_wifi_set_events',
    '_nw_wifi_scan', '_nw_wifi_scan_frequencies', '_nw_wifi_bss_count', '_nw_wifi_bss_get',
    '_nw_wifi_connect', '_nw_wifi_disconnect', '_nw_wifi_tx', '_nw_wifi_status_get',
    '_nw_wifi_where', '_nw_wifi_power', '_nw_wifi_tx_packet', '_nw_wifi_link_state',
    '_nw_wifi_tx_reasons', '_nw_wifi_buffer_stats_get', '_nw_wifi_buffer_pool_configure',
    '_nw_wifi_buffer_pool_trim', '_nw_wifi_irq_mode', '_nw_wifi_irq_pending',
    '_nw_wifi_wait_ms', '_nw_wifi_poll', '_nw_wifi_set_platform',
    '_nw_wifi_control_transport_set', '_nw_wifi_control_attach', '_nw_wifi_control_detach',
    '_nw_wifi_control_receive', '_nw_wifi_control_writable', '_nw_wifi_control_stats_get',
}

STUB_HEADERS = {
    'linux/suspend.h': '#ifndef LX_SUSPEND_H\n#define LX_SUSPEND_H\ntypedef int suspend_state_t;\n#define PM_SUSPEND_ON 0\n#endif\n',
    'linux/msi.h': '#ifndef LX_MSI_H\n#define LX_MSI_H\n#endif\n',
    'linux/of_device.h': '#ifndef LX_OF_DEVICE_H\n#define LX_OF_DEVICE_H\n#endif\n',
    'linux/of_platform.h': '#ifndef LX_OF_PLATFORM_H\n#define LX_OF_PLATFORM_H\n#endif\n',
    'linux/of_irq.h': '#ifndef LX_OF_IRQ_H\n#define LX_OF_IRQ_H\n#endif\n',
    'linux/pm_runtime.h': '#ifndef LX_PM_RUNTIME_H\n#define LX_PM_RUNTIME_H\n#endif\n',
    'linux/reset.h': '#ifndef LX_RESET_H\n#define LX_RESET_H\n#endif\n',
    'linux/regulator/consumer.h': '#ifndef LX_REGULATOR_CONSUMER_H\n#define LX_REGULATOR_CONSUMER_H\n#endif\n',
}


def find_bin(name: str, extra_dirs: list[Path]) -> str:
    for d in (*extra_dirs, Path('/opt/unixnu-linux/llvm/bin'), Path('/opt/unixnu-linux/CommandLineTools/usr/bin'), Path('/usr/bin')):
        for cand in (name, f'{name}-24', f'{name}-19', f'{name}-18'):
            p = d / cand
            if p.is_file():
                return str(p)
    found = shutil.which(name)
    if found:
        return found
    raise SystemExit(f'missing required LLVM tool: {name}')


def prepare_ath11k_tree(work: Path) -> None:
    linux_ath11k = work / 'deps/linux/drivers/net/wireless/ath/ath11k'
    if not linux_ath11k.is_dir():
        raise SystemExit(f'missing Linux ath11k sources: {linux_ath11k} (run tools/fetch-sources.py first)')

    for rel, body in STUB_HEADERS.items():
        dst = work / 'shim/include' / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(body, encoding='utf-8')

    shutil.copyfile(ADAPTER_DIR / 'lx_ath11k_shims.h', work / 'shim/include/lx_ath11k_shims.h')
    shutil.copyfile(ADAPTER_DIR / 'nw_wifi_ath11k.c', work / 'src/nw_wifi_ath11k.c')
    shutil.copyfile(ADAPTER_DIR / 'nw_debug_ath11k.c', work / 'src/nw_debug_ath11k.c')
    shutil.copyfile(ADAPTER_DIR / 'sources-ath11k.txt', work / 'tools/sources-ath11k.txt')

    lx_pci = work / 'shim/include/lx_pci.h'
    txt = lx_pci.read_text(encoding='utf-8')
    if 'lx_ath11k_shims.h' not in txt:
        txt = txt.replace('#include <linux/bitmap.h>', '#include <linux/bitmap.h>\n#include "lx_ath11k_shims.h"', 1)
        lx_pci.write_text(txt, encoding='utf-8')

    # Direct cfg80211 scan fallback when no userspace wpa_supplicant owns org.unixnu.wifi.control yet.
    ctrl_h = work / 'src/nw_cfg80211_control.h'
    hs = ctrl_h.read_text(encoding='utf-8')
    if 'nw_cfg80211_direct_scan' not in hs:
        hs = hs.replace(
            'int nw_cfg80211_set_power(int enabled);',
            'int nw_cfg80211_set_power(int enabled);\n'
            'int nw_cfg80211_direct_scan(const u8 *ssid, size_t ssid_len, const u32 *freqs, unsigned freq_count);',
            1,
        )
        ctrl_h.write_text(hs, encoding='utf-8')

    ctrl_c = work / 'src/nw_cfg80211_control.c'
    cs = ctrl_c.read_text(encoding='utf-8')
    if 'nw_cfg80211_direct_scan' not in cs:
        cs += (
            '\nint nw_cfg80211_direct_scan(const u8 *ssid, size_t ssid_len, const u32 *freqs, unsigned freq_count) {\n'
            '  if (owner) return -EBUSY;\n'
            '  if (!primary || !primary->registered) return -ENODEV;\n'
            '  if (!radio_enabled) return -ERFKILL;\n'
            '  struct cfg80211_registered_device *rdev = wiphy_to_rdev(primary->ieee80211_ptr->wiphy);\n'
            '  u8 buf[384]; struct nw_control_builder b;\n'
            '  if (nw_control_begin(&b, buf, sizeof(buf), NW_CONTROL_REQUEST, NW_CONTROL_SCAN, 1, 0, generation, 0)) return -EINVAL;\n'
            '  u8 ssid_entry[33]; memset(ssid_entry, 0, sizeof(ssid_entry));\n'
            '  if (ssid && ssid_len > 0 && ssid_len <= 32) { ssid_entry[0] = (u8)ssid_len; memcpy(ssid_entry + 1, ssid, ssid_len); }\n'
            '  if (nw_control_put(&b, NW_CONTROL_ATTR_SCAN_SSIDS, ssid_entry, sizeof(ssid_entry))) return -EINVAL;\n'
            '  if (freqs && freq_count > 0 && freq_count <= 32) {\n'
            '    u8 fbytes[32 * 4];\n'
            '    for (unsigned i = 0; i < freq_count; i++) put_unaligned_le32(freqs[i], fbytes + i * 4);\n'
            '    if (nw_control_put(&b, NW_CONTROL_ATTR_SCAN_FREQUENCIES, fbytes, freq_count * 4u)) return -EINVAL;\n'
            '  }\n'
            '  struct nw_control_view view;\n'
            '  if (nw_control_decode(b.bytes, b.length, &view)) return -EINVAL;\n'
            '  rtnl_lock();\n'
            '  int err = dev_open(primary, NULL);\n'
            '  if (!err) { wiphy_lock(&rdev->wiphy); err = scan(rdev, &view); wiphy_unlock(&rdev->wiphy); }\n'
            '  rtnl_unlock();\n'
            '  return err;\n'
            '}\n'
        )
        ctrl_c.write_text(cs, encoding='utf-8')

    mainline_c = work / 'src/nw_mainline.c'
    ms = mainline_c.read_text(encoding='utf-8')
    if 'nw_cfg80211_direct_scan' not in ms:
        anchor = '    if (!result) result = nw_cfg80211_event_send(&b);\n'
        if ms.count(anchor) != 1:
            raise SystemExit('unexpected nw_mainline.c event_send anchor')
        ms = ms.replace(
            anchor,
            anchor
            + '    if (result == NW_CONTROL_NOT_READY && pending.operation == NW_CONTROL_SCAN_REQUEST) {\n'
            + '      int dr = nw_cfg80211_direct_scan(pending.ssid_length ? pending.ssid : NULL, pending.ssid_length,\n'
            + '                                       pending.scan_frequency_count ? pending.scan_frequencies : NULL, pending.scan_frequency_count);\n'
            + '      if (!dr) result = 0;\n'
            + '    }\n',
            1,
        )
        mainline_c.write_text(ms, encoding='utf-8')

    # Prepare chip-agnostic generated trees + MHI + ath11k-src
    subprocess.run(['bash', 'tools/pin-headers.sh'], cwd=work, check=True)
    mhi_src = work / 'deps/linux/drivers/bus/mhi/host'
    mhi_out = work / 'out/mhi-src'
    if mhi_out.exists():
        shutil.rmtree(mhi_out)
    mhi_out.mkdir(parents=True)
    for f in (*mhi_src.glob('*.c'), mhi_src / 'internal.h'):
        (mhi_out / f.name).symlink_to(f)
    shutil.copyfile(work / 'shim/mhi/trace.h', mhi_out / 'trace.h')
    (mhi_out / 'boot.c').unlink()
    boot_txt = (mhi_src / 'boot.c').read_text(encoding='utf-8')
    mark = '\tret = request_firmware(&firmware, fw_name, dev);'
    if mark not in boot_txt:
        raise SystemExit('MHI boot.c request_firmware anchor missing')
    boot_txt = boot_txt.replace(mark, '\t{ extern void nw_mhi_pre_load(struct mhi_controller *); nw_mhi_pre_load(mhi_cntrl); }\n' + mark, 1)
    (mhi_out / 'boot.c').write_text(boot_txt, encoding='utf-8')

    for link_name, target in (
        ('common.h', work / 'deps/linux/drivers/bus/mhi/common.h'),
        ('spectral_common.h', work / 'deps/linux/drivers/net/wireless/ath/spectral_common.h'),
    ):
        lp = work / 'out' / link_name
        if lp.is_symlink() or lp.exists():
            lp.unlink()
        lp.symlink_to(target)

    for cmd in (
        ['python3', 'tools/apply-os-patches.py'],
        ['python3', 'tools/prepare-mac80211.py'],
        ['python3', 'tools/prepare-mac80211.py', '--subsystem', 'cfg80211'],
        ['python3', 'tools/prepare-crypto.py'],
        ['python3', 'tools/prepare-cfg80211-native.py'],
        ['python3', 'tools/prepare-cfg80211-events.py'],
    ):
        subprocess.run(cmd, cwd=work, check=True)

    ath_out = work / 'out/ath11k-src'
    if ath_out.exists():
        shutil.rmtree(ath_out)
    ath_out.mkdir(parents=True)
    for f in (*linux_ath11k.glob('*.c'), *linux_ath11k.glob('*.h')):
        (ath_out / f.name).symlink_to(f)

    traces = sorted(set(re.findall(r'trace_ath11k_[a-z_0-9]+', '\n'.join(
        p.read_text(encoding='utf-8', errors='ignore') for p in (*linux_ath11k.glob('*.c'), *linux_ath11k.glob('*.h'))
    ))))
    (ath_out / 'trace.h').unlink()
    (ath_out / 'trace.h').write_text(
        '#ifndef LX_ATH11K_TRACE_H\n#define LX_ATH11K_TRACE_H\n'
        + ''.join(f'#define {t}(...) ((void)0)\n' for t in traces)
        + '#define trace_ath11k_log_dbg_enabled() 0\n#endif\n',
        encoding='utf-8',
    )

    # Keep stock DP_RXDMA_BUF_RING_SIZE (4096) and DP_RXDMA_REFILL_RING_SIZE (2048):
    # QCA6390 has num_rxdma_per_pdev=2 with rx_mac_buf_ring[0..1] of 1024 entries each
    # (5 GHz MAC0 + 2.4 GHz MAC1). Shrinking rx_refill_buf_ring below 2048 starves MAC1
    # (2.4 GHz) of DP RX buffers so 2.4 GHz associations never receive EAPOL M1.

    (ath_out / 'dp_rx.c').unlink()
    dprx_txt = (linux_ath11k / 'dp_rx.c').read_text(encoding='utf-8')
    vht_mcs_mark = '\t\tif (rate_mcs > ATH11K_VHT_MCS_MAX) {\n'
    if dprx_txt.count(vht_mcs_mark) == 1:
        dprx_txt = dprx_txt.replace(
            vht_mcs_mark,
            '\t\t/* mac80211 accepts vendor VHT MCS 10/11; retain their NSS/BW. */\n'
            '\t\tif (rate_mcs > 11) {\n',
            1,
        )
    (ath_out / 'dp_rx.c').write_text(dprx_txt, encoding='utf-8')

    (ath_out / 'wmi.c').unlink()
    wmi_txt = (linux_ath11k / 'wmi.c').read_text(encoding='utf-8')
    wmi_txt = re.sub(
        r'(^int ath11k_wmi_cmd_send\([\s\S]*?^\})',
        lambda m: m.group(1).replace('wait_event_timeout(', 'wait_event_timeout_once('),
        wmi_txt,
        count=1,
        flags=re.MULTILINE,
    )
    (ath_out / 'wmi.c').write_text(wmi_txt, encoding='utf-8')

    (ath_out / 'pci.c').unlink()
    pci_txt = (linux_ath11k / 'pci.c').read_text(encoding='utf-8')
    pci_txt = pci_txt.replace('static int ath11k_pci_init(void)', 'int ath11k_pci_init(void)', 1)
    pci_txt = pci_txt.replace('static void ath11k_pci_exit(void)', 'void ath11k_pci_exit(void)', 1)
    (ath_out / 'pci.c').write_text(pci_txt, encoding='utf-8')

    (ath_out / 'mac.c').unlink()
    mac_txt = (linux_ath11k / 'mac.c').read_text(encoding='utf-8')
    mac_mark = '\tSET_IEEE80211_PERM_ADDR(ar->hw, ar->mac_addr);\n'
    if mac_txt.count(mac_mark) != 1:
        raise SystemExit('ath11k mac.c SET_IEEE80211_PERM_ADDR anchor missing')
    mac_txt = mac_txt.replace('#include "mac.h"\n', '#include "mac.h"\nvoid nw_board_mac(u8 *a);\n', 1)
    mac_txt = mac_txt.replace(
        mac_mark,
        '\tif (!is_valid_ether_addr(ar->mac_addr) || (!ar->mac_addr[0] && ar->mac_addr[1] == 0x03 && ar->mac_addr[2] == 0x7f)) {\n'
        '\t\tnw_board_mac(ar->mac_addr);\n\t}\n' + mac_mark,
        1,
    )
    # In the single-executor Linux shim (lx_rt.c), ordinary workqueues are deferred while
    # rtnl_lock/wiphy_lock are held (e.g. during dev_close / network switch) unless explicitly
    # flushed. Flushing wmi_mgmt_tx_work here (ar->conf_mutex is not held) lets the queued
    # Deauth transmit immediately instead of hitting three 5-second flush timeouts.
    flush_mark = (
        'static void ath11k_mac_op_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,\n'
        '\t\t\t\tu32 queues, bool drop)\n'
        '{\n'
        '\tstruct ath11k *ar = hw->priv;\n'
        '\n'
        '\tif (drop)\n'
        '\t\treturn;\n'
        '\n'
        '\tath11k_mac_flush_tx_complete(ar);\n'
        '}\n'
    )
    if mac_txt.count(flush_mark) != 1:
        raise SystemExit('ath11k mac.c ath11k_mac_op_flush anchor missing')
    mac_txt = mac_txt.replace(
        flush_mark,
        'static void ath11k_mac_op_flush(struct ieee80211_hw *hw, struct ieee80211_vif *vif,\n'
        '\t\t\t\tu32 queues, bool drop)\n'
        '{\n'
        '\tstruct ath11k *ar = hw->priv;\n'
        '\n'
        '\tif (drop)\n'
        '\t\treturn;\n'
        '\n'
        '\tif (!mutex_is_locked(&ar->conf_mutex))\n'
        '\t\tflush_work(&ar->wmi_mgmt_tx_work);\n'
        '\tath11k_mac_flush_tx_complete(ar);\n'
        '}\n',
        1,
    )
    (ath_out / 'mac.c').write_text(mac_txt, encoding='utf-8')


def build_archive(work: Path, out_archive: Path, target: str, jobs_count: int) -> None:
    clang = find_bin('clang', [])
    llvm_nm = find_bin('llvm-nm', [Path(clang).parent])
    llvm_objcopy = find_bin('llvm-objcopy', [Path(clang).parent])
    llvm_ar = find_bin('llvm-ar', [Path(clang).parent])
    resource = subprocess.run([clang, '-print-resource-dir'], check=True, capture_output=True, text=True).stdout.strip() + '/include'

    out = out_archive.parent
    raw = out / 'raw'
    out.mkdir(parents=True, exist_ok=True)
    raw.mkdir(parents=True, exist_ok=True)

    flags = [
        '-target', target, '-mkernel', '-O2', '-g0',
        '-ffreestanding', '-fno-builtin', '-fno-common', '-fno-strict-aliasing', '-fwrapv',
        '-fno-stack-protector', '-Wno-incompatible-pointer-types', '-Wno-error',
        '-fno-delete-null-pointer-checks', '-mgeneral-regs-only', '-momit-leaf-frame-pointer',
        '-std=gnu11', '-w',
        '-DCONFIG_ATH11K_DEBUG', '-DCONFIG_CFG80211=1', '-DCONFIG_MAC80211=1',
        '-DCONFIG_MAC80211_STA_HASH_MAX_SIZE=0', '-DCONFIG_MAC80211_RC_DEFAULT=""',
        '-nostdinc', '-isystem', str(work / 'shim/libc'), '-isystem', resource,
        '-include', 'lx_pci.h',
        '-I', str(work / 'shim/include'), '-I', str(work / 'include'),
        '-I', str(work / 'out/pinned'), '-I', str(work / 'src'), '-I', str(work / 'out'),
    ]
    trees = {
        'ath11k': ['-I', str(work / 'out/ath11k-src')],
        'mhi': ['-I', str(work / 'out/mhi-src')],
        'qmi': ['-I', str(work / 'out/os-src')],
        'mac80211': ['-I', str(work / 'out/mac80211')],
        'cfg80211': ['-I', str(work / 'out/cfg80211')],
        'generated': ['-I', str(work / 'out/cfg80211')],
        'crypto': ['-I', str(work / 'shim/crypto'), '-I', str(work / 'out/crypto')],
    }

    jobs: list[tuple[Path, Path, list[str]]] = []
    for line in (work / 'tools/sources-ath11k.txt').read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        tree, _, name = line.partition(':') if ':' in line else ('', '', line)
        if tree == 'ath11k':
            src = work / 'out/ath11k-src' / name
        elif tree == 'mhi':
            src = work / 'out/mhi-src' / name
        elif tree == 'qmi':
            src = work / 'out/os-src' / name
        elif tree in ('mac80211', 'cfg80211', 'crypto'):
            src = work / 'out' / tree / name
        elif tree == 'generated':
            src = work / 'out' / name
        else:
            src = work / name
        obj = out / ((tree + '_' if tree else '') + src.stem + '.o')
        extra = trees.get(tree, trees['ath11k'] if name.startswith('src/') else [])
        if name.startswith('src/nw_cfg80211_'):
            extra = trees['cfg80211']
        if name == 'src/nw_fils.c':
            extra = trees['mac80211']
        if name == 'src/nw_crypto_mac80211.c':
            extra = trees['crypto'] + trees['mac80211']
        if tree == 'crypto' and name == 'aes-siv.c':
            extra = [*extra, '-Daes_siv_encrypt=nw_aes_siv_encrypt', '-Daes_siv_decrypt=nw_aes_siv_decrypt']
        jobs.append((src, obj, extra))

    def compile_one(job: tuple[Path, Path, list[str]]) -> tuple[Path, str | None]:
        src, obj, extra = job
        raw_obj = raw / obj.name
        r = subprocess.run([clang, '-c', *extra, *flags, str(src), '-o', str(raw_obj)], capture_output=True, text=True)
        return (raw_obj, None if r.returncode == 0 else r.stderr)

    with ThreadPoolExecutor(jobs_count) as pool:
        results = list(pool.map(compile_one, jobs))

    errors = [(src, err) for (src, _, _), (_, err) in zip(jobs, results) if err is not None]
    if errors:
        for src, err in errors[:8]:
            sys.stderr.write(f'=== {src} ===\n{err[:1500]}\n')
        raise SystemExit(f'ath11k compile failed ({len(errors)}/{len(jobs)})')

    defined: set[str] = set()
    for raw_obj, _ in results:
        r = subprocess.run([llvm_nm, '-g', '--defined-only', str(raw_obj)], check=True, capture_output=True, text=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3:
                defined.add(parts[2])

    mapping = out / 'rename.txt'
    mapping.write_text(''.join(f'{n} _nwk{n}\n' for n in sorted(defined - KEEP_SYMBOLS)), encoding='utf-8')
    objs = [obj for _, obj, _ in jobs]
    for obj in objs:
        shutil.copyfile(raw / obj.name, obj)
        subprocess.run([llvm_objcopy, f'--redefine-syms={mapping}', str(obj)], check=True)

    out_archive.unlink(missing_ok=True)
    subprocess.run([llvm_ar, 'rcs', str(out_archive), *[str(o) for o in objs]], check=True)
    print(f'j716f-ath11k: PASS ({len(objs)} objs, {out_archive.stat().st_size} B -> {out_archive})')


def main() -> int:
    ap = argparse.ArgumentParser(description='Build freestanding libJ716fAth11kWifi.a for TB-J716F')
    ap.add_argument('--workspace', required=True, help='Ath12kWifi template workspace with fetched deps/')
    ap.add_argument('--out', required=True, help='Output archive path (e.g. libJ716fAth11kWifi.a)')
    ap.add_argument('--target', default='arm64-apple-ios17.0')
    ap.add_argument('--jobs', type=int, default=8)
    args = ap.parse_args()
    work = Path(args.workspace).resolve()
    prepare_ath11k_tree(work)
    build_archive(work, Path(args.out).resolve(), args.target, args.jobs)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
