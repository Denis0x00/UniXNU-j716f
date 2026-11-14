"""Prepare source-only inputs and officially build the independent r31 Power-only target."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.parse

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from verify_host_early_log import effective_config, check_panic_compile_record


def copy_sparse_source_cache(source, destination, paths, git, digest):
    """Copy only Git metadata and present tracked sparse sources, without upload-pack.

    Local clones of shallow promisor repositories can invoke upload-pack and
    fail when lazy object fetching is disabled. This copies the already checked
    out source cache; no old build tree, ignored outputs or SDK is transferred.
    """
    source = source.resolve(strict=True)
    assert not destination.exists(), 'Refusing to replace an existing source cache'
    metadata = source/'.git'
    assert metadata.is_dir() and not metadata.is_symlink()
    assert Path(git(source, 'rev-parse', '--absolute-git-dir')).resolve() == metadata.resolve()
    assert git(source, 'rev-parse', '--is-bare-repository') == 'false'
    assert not (metadata/'commondir').exists(), 'Linked Git metadata is not supported'
    assert not any(line.startswith('core.worktree=') for line in git(source, 'config', '--local', '--list').splitlines()), 'Source cache has a bound worktree'
    assert not any(path.is_symlink() for path in metadata.rglob('*')), 'Symlink in Git metadata'
    assert not (metadata/'objects/info/alternates').exists(), 'Source cache has external Git object alternates'
    allowed = [Path(path) for path in paths]
    parents = {parent for path in allowed for parent in path.parents}
    present = {}
    for name in git(source, 'ls-files', '-z').split('\0'):
        if not name:
            continue
        relative = Path(name)
        assert not relative.is_absolute() and '..' not in relative.parts
        path = source/relative
        assert not path.is_symlink(), 'Symlink in sparse source checkout'
        if not path.exists():
            continue  # skip-worktree entries have no checked-out source bytes
        assert path.is_file() and (relative.parent in parents or any(relative.is_relative_to(prefix) for prefix in allowed)), name
        assert path.suffix not in ('.o', '.obj', '.a', '.so', '.dylib', '.dll', '.exe') and not name.endswith(('.o.json','.obj.json')), name
        assert not any(part.lower().endswith('.sdk') or part in ('build','__pycache__') for part in relative.parts), name
        present[name] = {'bytes': path.stat().st_size, 'sha256': digest(path)}
    assert present, 'Sparse source cache has no checked-out tracked files'
    destination.mkdir(parents=True)
    shutil.copytree(metadata, destination/'.git')
    for name, spec in present.items():
        target = destination/name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source/name, target)
        assert target.stat().st_size == spec['bytes'] and digest(target) == spec['sha256']
        assert digest(source/name) == spec['sha256'], 'Source changed during copy'
    assert git(destination, 'rev-parse', 'HEAD') == git(source, 'rev-parse', 'HEAD')
    assert not git(destination, 'status', '--porcelain') and not git(source, 'status', '--porcelain')
    assert git(destination, 'remote', 'get-url', 'origin') == git(source, 'remote', 'get-url', 'origin')
    return {'method': 'copy-git-metadata-and-present-tracked-sparse-sources', 'source': str(source),
            'tracked_files': present, 'compiled_outputs_copied': False, 'network_used': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=('prepare', 'kernel'))
    p.add_argument('--root', required=True)
    p.add_argument('--proxy', required=True)
    p.add_argument('--attempt', type=int, default=1)
    a = p.parse_args()
    if sys.platform != 'linux' or not __debug__:
        p.error('Run on the prepared Linux host with normal assertion checks enabled.')
    if a.attempt < 1:
        p.error('--attempt must be positive')
    root = Path(a.root).resolve()
    main_repo = root / 'UniXNU'
    device = root / 'work/UniXNU-j716f-host-ufs-power-r31'
    output = main_repo / 'out-host-ufs-power-r31-20261114/j716f-host-ufs-power-r31/ipados-17.7.11-21H461/j716f_host_ufs_power_r31'
    tree = output / 'tree'
    report_dir = root / 'analysis/r31-ufs-power-20261114/host-build'
    report_dir.mkdir(parents=True, exist_ok=True)
    report = report_dir / f'{a.mode}-{a.attempt:02d}.json'
    if report.exists():
        raise SystemExit(f'Refusing to overwrite an earlier run: {report}')
    data = {'mode': a.mode, 'attempt': a.attempt, 'started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'status': 'running', 'commands': [], 'output': str(output), 'device_actions': False,
            'qemu': False, 'target': 'j716f-host-ufs-power-r31', 'dt_panic_region': True}
    env = dict(os.environ, GIT_CONFIG_COUNT='3', GIT_CONFIG_KEY_0='http.proxy',
               GIT_CONFIG_VALUE_0=a.proxy, GIT_CONFIG_KEY_1='pack.threads', GIT_CONFIG_VALUE_1='1',
               GIT_CONFIG_KEY_2='http.version', GIT_CONFIG_VALUE_2='HTTP/1.1', PYTHONUNBUFFERED='1',
               PYTHONDONTWRITEBYTECODE='1', GIT_OPTIONAL_LOCKS='0')
    env.pop('PYTHONOPTIMIZE', None)

    def save():
        temp = report.with_suffix('.tmp')
        temp.write_text(json.dumps(data, indent=2) + '\n')
        temp.replace(report)

    def run(argv, label, cwd=root, capture=False, timeout=None):
        argv = [str(x) for x in argv]
        logfile = report_dir / f'{a.mode}-{a.attempt:02d}-{label}.log'
        entry = {'argv': argv, 'cwd': str(cwd), 'log': str(logfile), 'started': datetime.datetime.now(datetime.timezone.utc).isoformat()}
        data['commands'].append(entry)
        save()
        print(f'Running {label}', flush=True)
        if capture:
            result = subprocess.run(argv, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, timeout=timeout)
            logfile.write_text(result.stdout + result.stderr)
        else:
            with logfile.open('w') as stream:
                result = subprocess.run(argv, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        entry['exit_code'] = result.returncode
        entry['finished'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save()
        if result.returncode:
            raise RuntimeError(f'{label} failed with {result.returncode}; see {logfile}')
        return result.stdout if capture else None

    def git(directory, *args):
        result = subprocess.run(['git', '-C', str(directory), *args], env=env, capture_output=True, text=True, check=True)
        return result.stdout.strip()

    def digest(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()

    try:
        save()
        assert git(main_repo, 'rev-parse', 'HEAD') == '7b730b231746667d8b7f54746c2c1ce45852611c'
        cache = main_repo / 'out/ipsw/21H461/kernelcache.decompressed'
        assert digest(cache) == 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
        data['upstream_revision'] = git(main_repo, 'rev-parse', 'HEAD')
        data['kernelcache_sha256'] = digest(cache)
        source_manifest = device/'docs/host-source-manifest.json'
        source_inputs = json.loads(source_manifest.read_text())
        assert source_inputs['upstream_revision'] == data['upstream_revision']
        physical_path = root/'analysis/r31-ufs-power-20261114/vm-source-identities.json'
        physical = json.loads(physical_path.read_text())
        assert physical['source_manifest_sha256'] == digest(source_manifest)
        assert set(physical['inputs']) == set(source_inputs['inputs'])
        source_inputs['inputs'] = physical['inputs']
        data['physical_source_manifest'] = {'sha256': digest(physical_path), 'crlf_lf_only': len(physical['crlf_lf_only'])}
        for relative, spec in source_inputs['inputs'].items():
            path = (root/relative).resolve(strict=True)
            assert path.is_relative_to(root), 'Target source input escapes workspace'
            assert path.stat().st_size == spec['bytes'] and digest(path) == spec['sha256'], relative
        data['target_source_manifest'] = {'path': str(source_manifest), 'sha256': digest(source_manifest)}
        protected = [root/'artifacts/j716f-host-kernel-20261112'/name for name in ('native.macho','kernel.macho')]
        protected += [main_repo/'out-host/j716f-host/ipados-17.7.11-21H461/j716f_host'/name
                      for name in ('.config','kernel/kernel.macho')]
        protected += [root/'artifacts/j716f-host-dt-panic-20261113'/name for name in ('native.macho','kernel.macho')]
        protected += [root/'artifacts/j716f-host-early-log-20261114'/name for name in ('native.macho','kernel.macho')]
        protected += [root/'artifacts/j716f-host-iokit-cpu0-20261114'/name for name in ('native.macho','kernel.macho')]
        protected += [root/'artifacts/j716f-xnu-bsd-pre-root-probe-20261114-r30/boot.img']
        protected += [root/'artifacts/j716f-host-storage-20261114-r30'/name for name in ('native.macho','kernel.macho')]
        data['retained_inputs'] = {str(path): digest(path) for path in protected}
        host = [sys.executable, device / 'scripts/host.py']
        if a.mode == 'prepare':
            # Independent feed checkouts retain the official source metadata and remotes.
            manifest = json.loads((root / 'analysis/vm/offline-inputs-manifest-20261112.json').read_text())
            data['feed_seeds'] = []
            for entry in manifest['source_heads']:
                if entry['name'] == 'edk2':
                    continue  # Not a dependency of build kernel.
                src = Path(entry['path'])
                dst = main_repo / 'out-host-ufs-power-r31-20261114/feeds' / src.name
                assert git(src, 'rev-parse', 'HEAD') == entry['commit']
                assert not git(src, 'status', '--porcelain'), src
                dst.parent.mkdir(parents=True, exist_ok=True)
                if not dst.exists():
                    run(['git', 'clone', '--quiet', '--shared', '--no-checkout', src, dst], 'clone-' + entry['name'])
                    run(['git', '-C', dst, 'remote', 'set-url', 'origin', git(src, 'remote', 'get-url', 'origin')], 'remote-' + entry['name'])
                    run(['git', '-C', dst, 'checkout', '--quiet', '--detach', entry['commit']], 'checkout-' + entry['name'])
                assert git(dst, 'rev-parse', 'HEAD') == entry['commit']
                assert not git(dst, 'status', '--porcelain'), dst
                data['feed_seeds'].append({'name': entry['name'], 'source': str(src), 'path': str(dst), 'commit': entry['commit']})
            for action in ('defconfig', 'fetch', 'tree'):
                run([*host, action], action)
            status = json.loads(run([*host, 'status'], 'status', capture=True))
            (report_dir / 'status.json').write_text(json.dumps(status, indent=2) + '\n')
            assert Path(status['output']) == output
            assert status['name'] == 'j716f-host-ufs-power-r31'
            expected = json.loads((root / 'analysis/host-iokit-cpu0-20261114/status.json').read_text())
            assert len(status['patch_order']) == 63 and len(expected['patch_order']) == 60
            assert [digest(x) for x in status['patch_order'][:60]] == [digest(x) for x in expected['patch_order']]
            assert [digest(x) for x in status['patch_order'][60:]] == [digest(device/'patches/xnu'/name) for name in ('j716f-storage-init.patch', 'j716f-storage-dependency.patch', 'j716f-power-probe-call.patch')]
            assert status['board']['kernel']['archives'][:8] == expected['board']['kernel']['archives']
            assert status['board']['kernel']['archives'][8:] == ['build-j716f-ufs-power-r31.py']
            assert [x['id'] for x in status['components']] == [x['id'] for x in expected['components']]
            current_config = effective_config(Path(status['config_path']).read_text())
            old_config = effective_config(Path(expected['config_path']).read_text())
            storage_archive = current_config.pop('XNU_EXTRA_LD_ARCHIVES')
            assert storage_archive == '-Wl,-force_load,{tree}/build/j716f-ufs-power-r31/libJ716fStorageBringup.a'
            old_archive = old_config.pop('XNU_EXTRA_LD_ARCHIVES', '')
            assert old_archive == '' and current_config == old_config
            assert current_config['XNU_DT_PANIC_REGION'] is True and current_config.get('XNU_NATIVE_UFS') is False
            data['resolved'] = {'patches': 63, 'archive_recipes': 9, 'components': [x['id'] for x in status['components']],
                                'effective_config_delta': {'XNU_EXTRA_LD_ARCHIVES': storage_archive}}
            data['component_heads'] = {c['id']: ('local-cpu0-separator-source-manifest' if c['id'] == 'ArmCpu' else git(main_repo / c['path'], 'rev-parse', 'HEAD')) for c in status['components']}
            assert (tree/'drivers/ArmCpu').resolve() == (root/'work/ArmCpu-j716f-iokit-cpu0-20261114').resolve()
            # Copy only directory metadata. The official build will restore headers and create its own stamps.
            headers = root / 'validation/offline-headers-20261112/.cache/sources'
            destination = tree / '.cache/sources'
            destination.mkdir(parents=True, exist_ok=True)
            data['header_sources'] = []
            for name in ('xnu-driverkit-10063.121.3.json', 'xnu-driverkit-crypto-10063.121.3.json', 'xnu-trustcache-8796.101.5.json'):
                entries = json.loads((headers / name).read_text())
                for entry in entries:
                    if entry['type'] != 'file' or not entry['name'].endswith('.h'):
                        continue
                    url = urllib.parse.urlsplit(entry['download_url'])
                    assert url.scheme == 'file' and not url.netloc
                    source = Path(urllib.parse.unquote(url.path)).resolve()
                    assert source.is_relative_to(root / 'validation/offline-headers-20261112/header-sources')
                    content = source.read_bytes()
                    blob = hashlib.sha1(b'blob ' + str(len(content)).encode() + b'\0' + content).hexdigest()
                    assert blob == entry['sha'], source
                    data['header_sources'].append({'path': str(source), 'git_blob': blob, 'sha256': digest(source)})
                shutil.copyfile(headers / name, destination / name)
            assert len(data['header_sources']) == 26
            data['linux_sources'] = []
            for name, url, commit, paths in (
                ('llvm-project', 'https://github.com/llvm/llvm-project', '58c46bae21181fb4240cf964461bea738d96d50e', ['compiler-rt/lib/profile', 'compiler-rt/include/profile']),
                ('Libc', 'https://github.com/apple-oss-distributions/Libc.git', '899a3b2d52d95d75e05fb286a5e64975ec3de757', ['include'])):
                dst = tree / '.cache/linux' / name
                old_cache = main_repo/'out-host/j716f-host/ipados-17.7.11-21H461/j716f_host/tree/.cache/linux'/name
                seeded_from = None
                seed_record = None
                if not dst.exists():
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    if old_cache.is_dir():
                        assert git(old_cache, 'rev-parse', 'HEAD') == commit
                        assert not git(old_cache, 'status', '--porcelain'), old_cache
                        seed_record = copy_sparse_source_cache(old_cache, dst, paths, git, digest)
                        seeded_from = str(old_cache)
                    else:
                        dst.mkdir()
                        run(['git', 'init', '--quiet', dst], 'init-' + name)
                        run(['git', '-C', dst, 'remote', 'add', 'origin', url], 'origin-' + name)
                        run(['git', '-C', dst, 'sparse-checkout', 'set', '--cone', *paths], 'sparse-' + name)
                        run(['git', '-C', dst, 'fetch', '--quiet', '--depth', '1', '--filter=blob:none', 'origin', commit], 'fetch-' + name, timeout=600)
                        run(['git', '-C', dst, 'checkout', '--quiet', '--detach', 'FETCH_HEAD'], 'checkout-' + name, timeout=600)
                assert git(dst, 'rev-parse', 'HEAD') == commit
                assert not git(dst, 'status', '--porcelain'), dst
                files = {str(f.relative_to(dst)): digest(f) for path in paths for f in (dst / path).rglob('*') if f.is_file()}
                assert files
                if name == 'llvm-project':
                    for base in ('InstrProfiling', 'InstrProfilingBuffer', 'InstrProfilingPlatformDarwin', 'InstrProfilingWriter', 'InstrProfilingInternal', 'InstrProfilingVersionVar'):
                        assert (dst / f'compiler-rt/lib/profile/{base}.c').is_file()
                data['linux_sources'].append({'name': name, 'commit': commit, 'files': files,
                                              'source_only_seed': seeded_from, 'seed_record': seed_record,
                                              'compiled_outputs_copied': False})
                save()
            run([sys.executable, main_repo / 'scripts/porting-record.py', 'check', '--device', device], 'porting-record')
            assert not (output / 'xnu-inputs.json').exists()
            assert not (tree / '.cache/xnu-work').exists()
            assert not any(path.is_file() for path in (tree/'build').rglob('*')), 'Preparation unexpectedly copied or generated compilation outputs'
            data['formal_xnu_source_not_precreated'] = True
        else:
            preparations = [(path, json.loads(path.read_text())) for path in sorted(report_dir.glob('prepare-*.json'))]
            completed = [(path, result) for path, result in preparations if result.get('status') == 'passed']
            assert completed, 'No successful host preparation report'
            ready_path, ready = completed[-1]
            assert ready['output'] == str(output)
            data['preparation_report'] = str(ready_path)
            if (output / 'logs').exists():
                shutil.copytree(output / 'logs', report_dir / f'logs-before-kernel-{a.attempt:02d}')
            run([*host, 'kernel'], 'official-build')
            native = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
            candidate = output / 'kernel/kernel.macho'
            data['artifacts'] = [{'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest(path)} for path in (native, candidate)]
            data['dt_panic_compile'] = check_panic_compile_record(
                tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_init.o.json', tree)
        for path, before in data['retained_inputs'].items():
            assert digest(path) == before, 'Retained original host input changed: ' + path
        assert digest(source_manifest) == data['target_source_manifest']['sha256'], 'Target source manifest changed during run'
        for relative, spec in source_inputs['inputs'].items():
            assert digest(root/relative) == spec['sha256'], 'Target source input changed during run: ' + relative
        data['retained_inputs_unchanged'] = True
        data['status'] = 'passed'
    except Exception as exc:
        data['status'] = 'failed'
        data['error'] = str(exc)
        print(str(exc), file=sys.stderr, flush=True)
    finally:
        data['finished'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save()
    return 0 if data['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
