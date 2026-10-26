#!/usr/bin/env python3
"""Read existing J716F Recovery ramoops evidence; only host files are written."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import shlex


def remote_capture(adb, serial, command):
    """Frame remote status outside stdout: exec-out may hide the shell exit code."""
    tag = b'__J716F_CAPTURE_STATUS__='
    wrapped = ('sh -c ' + shlex.quote(command) +
               '; capture_status=$?; printf "\\n__J716F_CAPTURE_STATUS__=%s\\n" "$capture_status" >&2')
    r = subprocess.run([adb, '-s', serial, 'exec-out', wrapped],
                       capture_output=True, timeout=30)
    stdout, stderr = r.stdout, r.stderr
    # Legacy adb/Recovery may merge stderr into stdout; split only a final frame.
    status = None
    for stream in ('stderr', 'stdout'):
        data = stderr if stream == 'stderr' else stdout
        match = re.search(rb'\n' + tag + rb'(\d+)\r?\n?\Z', data)
        if match:
            status = int(match[1])
            if stream == 'stderr': stderr = data[:match.start()]
            else: stdout = data[:match.start()]
            break
    return r.returncode, status, stdout, stderr


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--adb', default=shutil.which('adb'))
    p.add_argument('--serial', required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if not a.adb or not re.fullmatch(r'[A-Za-z0-9_.:-]+', a.serial):
        p.error('Provide adb and a valid exact device serial')
    a.output.mkdir(parents=True, exist_ok=False)
    report = {'scope': 'Read-only Recovery commands, host files only; private raw evidence',
              'captured_at': datetime.now().astimezone().isoformat(),
              'serial': a.serial, 'captures': [], 'writes_to_device': False}

    def save():
        (a.output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')

    def capture(name, command, required=False):
        rc, remote_status, stdout, stderr = remote_capture(a.adb, a.serial, command)
        ok = rc == 0 and remote_status == 0
        saved_name = name if ok else name + '.failed-output'
        (a.output / saved_name).write_bytes(stdout)
        (a.output / (name + '.stderr')).write_bytes(stderr)
        report['captures'].append({'file': saved_name, 'requested_file': name,
                                   'command': command, 'returncode': rc,
                                   'remote_status': remote_status, 'accepted': ok,
                                   'bytes': len(stdout), 'sha256': hashlib.sha256(stdout).hexdigest()})
        save()
        print(name, 'accepted', ok, 'remote_status', remote_status, 'bytes', len(stdout), flush=True)
        if required and (not ok or not stdout):
            raise RuntimeError('Required capture failed: ' + name)
        return stdout if ok else None

    try:
        devices = subprocess.run([a.adb, 'devices'], capture_output=True, text=True, timeout=15)
        rows = [s.split() for s in devices.stdout.splitlines()[1:] if s.strip()]
        if devices.returncode or len(rows) != 1 or rows[0][0] != a.serial or rows[0][1] not in ('recovery', 'device'):
            raise RuntimeError('Require exactly the selected ADB device')
        identity = capture('identity.txt', 'id; getprop ro.product.model; getprop ro.product.device; getprop ro.twrp.version; uname -r', True)
        if b'uid=0(root)' not in identity or b'J716F' not in identity or b'3.7.1' not in identity:
            raise RuntimeError('Expected the known root J716F Recovery')
        params = {}
        for name in ['mem_address', 'mem_size', 'record_size', 'console_size', 'ftrace_size', 'pmsg_size', 'mem_type', 'ecc', 'dump_oops']:
            raw = capture('param-' + name + '.txt', 'cat /sys/module/ramoops/parameters/' + name, True)
            params[name] = int(raw.strip(), 0)
        report['module_parameters'] = params
        save()
        for name, cmd in [
            ('iomem.txt', 'cat /proc/iomem'), ('dmesg.txt', 'dmesg'),
            ('proc-version.txt', 'cat /proc/version'),
            ('kernel-config.gz', 'cat /proc/config.gz'),
            ('fdt.dtb', 'cat /sys/firmware/fdt'),
            ('device-tree.tar', 'tar -C /sys/firmware/devicetree/base -cf - .'),
            ('pstore-list.txt', 'ls -l /sys/fs/pstore'),
            ('mounts.txt', 'cat /proc/mounts'),
            ('cmdline.txt', 'cat /proc/cmdline'),
            ('pstore-exists.txt', 'if [ -r /sys/fs/pstore/console-ramoops-0 ]; then echo PRESENT; else echo ABSENT; fi'),
            ('console-ramoops-0', 'cat /sys/fs/pstore/console-ramoops-0'),
            ('ramoops-uevent.txt', 'cat /sys/bus/platform/devices/ramoops/uevent'),
            ('memory-device-status.txt', 'ls -l /dev/mem /dev/kmem'),
            ('boot-log-driver.txt', 'ls -l /sys/bus/platform/drivers/*boot*log*'),
        ]:
            capture(name, cmd)
        report['status'] = 'captured; layout and console offset require independent verification'
    except Exception as error:
        report['stopped_error'] = str(error)
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
