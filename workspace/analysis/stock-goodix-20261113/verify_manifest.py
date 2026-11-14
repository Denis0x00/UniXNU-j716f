"""Read-only verifier for the stock Goodix evidence/source/host-result manifest."""
from pathlib import Path
import argparse
import hashlib
import json
import struct

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent


def need(ok, why):
    if not ok:
        raise ValueError(why)


def sha(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        while part := stream.read(1024 * 1024):
            value.update(part)
    return value.hexdigest()


def verify_file(item):
    path = (ROOT / item['path']).resolve()
    need(path.is_relative_to(ROOT.resolve()), 'path outside workspace')
    need(path.stat().st_size == item['bytes'], 'size mismatch: ' + item['path'])
    need(sha(path) == item['sha256'], 'hash mismatch: ' + item['path'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true', help='Read-only verification (also the default).')
    parser.parse_args()
    manifest = json.loads((OUT / 'source-manifest.json').read_text())
    all_files = manifest['files'] + manifest['references'] + manifest['validation']['artifacts']
    for entry in all_files:
        verify_file(entry)
    validation = manifest['validation']
    need(validation['status'] == 'passed' and validation['checks'] == 6293, 'validation summary')
    for path, expected in validation['inputs'].items():
        need(sha(ROOT / path) == expected, 'tested source mismatch: ' + path)
    obj = ROOT / validation['object']['path']
    need(struct.unpack_from('<III', obj.read_bytes()) == (0xfeedfacf, 0x100000c, 0), 'Mach-O architecture')
    result_path = obj.parent / 'result.json'
    result = json.loads(result_path.read_text())
    need(result['status'] == 'passed' and result['inputs'] == validation['inputs'], 'result input/status mismatch')
    need(all(command['exit_code'] == 0 for command in result['commands']), 'failed command')
    need('-fsanitize=address,undefined' in result['commands'][0]['argv'], 'sanitizers missing')
    need('--target=arm64-apple-ios17.0' in result['commands'][2]['argv'], 'cross target missing')
    need((obj.parent / 'host-test.log').read_text() == 'PASS 6293 stock Goodix core checks\n', 'host output')
    exports = sorted(line.split()[-1] for line in (obj.parent / 'arm64-exports.log').read_text().splitlines())
    expected = sorted('_jg_' + name for name in ['decode', 'expire', 'fault', 'finish', 'identity_matches', 'init', 'inspect', 'stop', 'submit'])
    need(exports == expected, 'exports')
    need(not (obj.parent / 'arm64-undefined.log').read_text().strip(), 'undefined symbols')
    kernel = manifest['kernel_slice']
    with (ROOT / kernel['boot_path']).open('rb') as stream:
        stream.seek(kernel['offset'])
        data = stream.read(kernel['bytes'])
    need(len(data) == kernel['bytes'] and hashlib.sha256(data).hexdigest() == kernel['sha256'], 'kernel slice')
    evidence = json.loads((OUT / 'stock-protocol-evidence.json').read_text())
    need(len(evidence['facts']) == 19, 'instruction evidence count')
    for name, fact in evidence['facts'].items():
        offset = int(fact['kernel_image_offset'], 16)
        need(data[offset:offset + 4].hex() == fact['bytes'], 'instruction mismatch: ' + name)
    need(evidence['config']['registers']['coor']['address'] == '0x824d', 'stock coordinate register')
    need(not manifest['scope']['runtime_qualified'] and not manifest['scope']['raw_touch_capture_available'], 'scope drift')
    print(json.dumps({'status': 'passed', 'verified_files': len(all_files), 'stock_instruction_assertions': 19, 'host_checks': 6293, 'arm64_exports': len(exports), 'undefined_symbols': 0, 'runtime_qualified': False}))


if __name__ == '__main__':
    main()
