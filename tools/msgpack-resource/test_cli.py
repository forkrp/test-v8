#!/usr/bin/env python3
"""CLI behavior, independent decoding, and exact public-API compatibility."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--d8', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binary, d8 = args.binary.resolve(), args.d8.resolve()
    peer = load('resource_peer', ROOT / 'tools/binary_serialization/resource_peer.py')
    fixtures = load('resource_corpus', ROOT / 'tools/binary_serialization/run.py')
    corpus = fixtures.corpus()
    corpus['protocol_fixture'] = json.loads((ROOT / 'include/js_protocol-1.3.json').read_text())
    for name, text in [('cjk_unique', '中文序列化配置性能测试'),
                       ('greek_unique', 'Ελληνικάκείμενα'), ('emoji_unique', '🌏😀🚀🧪')]:
        corpus[name] = [{'id': i, 'body': f'{i}:' + text * 40} for i in range(2500)]
    sources = {name: json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode()
               for name, value in corpus.items()}
    sources.update({
        'null': b'null', 'boolean': b'true', 'empty': b'{}',
        'scalar_string': '"hello 🌏"'.encode(),
        'boundary_numbers': b'[0,-0,127,128,255,256,65535,65536,4294967295,4294967296,'
                            b'-32,-33,-128,-129,-32768,-32769,-2147483648,-2147483649,'
                            b'9007199254740991,-9007199254740991,9007199254740992,'
                            b'18446744073709551615,0.1,0.5,5e-324,1.7976931348623157e308]',
        'negative_zero_block': b'[' + b','.join([b'-0'] * 16) + b']',
        'nonfinite_block': b'[' + b','.join([b'1e400', b'-1e400'] * 16) + b']',
        'indexed_and_proto': json.dumps([
            {'10': i, '2': i + 1, '__proto__': {'kept': i},
             'long_property_name': 'repeated 🌏' * 20} for i in range(100)
        ], ensure_ascii=False).encode(),
        'bom': b'\xef\xbb\xbf{"title":"bom"}',
        'depth_256': b'[' * 256 + b'0' + b']' * 256,
        'block_boundaries': json.dumps([[i / 10 for i in range(n)]
                                      for n in [15, 16, 255, 256, 257, 512, 513]]).encode(),
    })
    checks, cases = 0, []

    def check(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(message)

    def run(*arguments, data=None):
        return subprocess.run([str(binary), *map(str, arguments)], input=data,
                              capture_output=True, timeout=120)

    with tempfile.TemporaryDirectory(prefix='msgpack-resource-cli-') as directory:
        folder = Path(directory)
        for name, source in sources.items():
            input_file = folder / (name + '.json')
            input_file.write_bytes(source)
            outputs = []
            for standard in [False, True]:
                output_file = folder / (name + ('.msgpack' if standard else '.v8mr'))
                command = [input_file, '-o', output_file, '--stats']
                if standard:
                    command.append('--standard')
                result = run(*command)
                check(result.returncode == 0, result.stderr.decode(errors='replace'))
                check(result.stdout == b'', 'File conversion polluted stdout')
                wire = output_file.read_bytes()
                stats = json.loads(result.stderr)
                check(stats['outputBytes'] == len(wire), 'Incorrect output length')
                check(stats['inputBytes'] == len(source), 'Incorrect input length')
                expected = json.loads(source.decode('utf-8-sig'), parse_int=float)
                check(peer.equivalent(expected, peer.decode(wire)), 'Independent decode mismatch: ' + name)
                outputs.append((wire, stats))
                cases.append({'name': name, 'input': str(input_file),
                              'wire': str(output_file), 'standard': standard})
            resource, stats = outputs[0]
            standard = outputs[1][0]
            check(len(resource) <= len(standard), 'Resource is larger than standard')
            check(stats['standardBytes'] == len(standard), 'Incorrect standard size')
            check(stats['bytesSaved'] == len(standard) - len(resource), 'Incorrect savings')
            if stats['format'] == 'msgpack':
                check(resource == standard, 'Fallback differs from exact standard bytes')
        manifest = folder / 'cases.json'
        manifest.write_text(json.dumps(cases))
        result = subprocess.run([str(d8), str(Path(__file__).with_name('verify.js')),
                                 '--', str(manifest)], capture_output=True, text=True, timeout=120)
        check(result.returncode == 0, result.stdout + result.stderr)

        sample = folder / 'sample.json'
        sample.write_text('{"value":42}')
        existing = folder / 'existing.v8mr'
        existing.write_bytes(b'KEEP')
        check(run(sample, '-o', existing).returncode == 2, 'Overwrite was not refused')
        check(existing.read_bytes() == b'KEEP', 'Existing file changed')
        check(run(sample, '-o', existing, '--force').returncode == 0, 'Force failed')
        check(peer.decode(existing.read_bytes()) == {'value': 42}, 'Force output mismatch')
        check(run(sample, '-o', sample, '--force').returncode == 2, 'Same-file output accepted')
        alias = folder / 'alias.json'
        alias.hardlink_to(sample)
        check(run(sample, '-o', alias, '--force').returncode == 2, 'Input hardlink output accepted')
        for source in [b'', b'{', b'{} {}', b'{"x":NaN}', b'"\xff"',
                       b'"\xc0\xaf"', b'"\xed\xa0\x80"', b'"\\ud800"',
                       b'[' * 257 + b'0' + b']' * 257]:
            check(run('-', '-o', existing, '--force', data=source).returncode == 1,
                  'Malformed/unsupported input succeeded: ' + repr(source[:30]))
            check(peer.decode(existing.read_bytes()) == {'value': 42}, 'Bad input changed output')
        streamed = run('-', '-o', '-', '--stats', data=sample.read_bytes())
        check(streamed.returncode == 0, streamed.stderr.decode())
        check(peer.decode(streamed.stdout) == {'value': 42}, 'Binary stdout mismatch')
        check(json.loads(streamed.stderr)['outputBytes'] == len(streamed.stdout), 'Stderr stats mismatch')
        check(run('--help').returncode == 0, 'Help failed')
        check(b'V8MR v1' in run('--version').stdout, 'Version failed')
        check(run().returncode == 2, 'Missing arguments accepted')
        check(run(sample, '-o', '-', '--unknown').returncode == 2, 'Unknown option accepted')
        check(run(folder / 'missing.json', '-o', '-').returncode == 1, 'Missing input accepted')
        check(run(sample, '-o', folder / 'missing' / 'out').returncode == 1, 'Output error accepted')
        unicode_input = folder / '配置 🌏.json'
        unicode_input.write_bytes(sample.read_bytes())
        check(run(unicode_input, '-o', folder / '资源 🌏.v8mr').returncode == 0,
              'Unicode paths failed')
        report = {'checks': checks, 'fixtures': len(sources), 'publicApiWireChecks': len(cases),
                  'binary': str(binary), 'binarySha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                  'd8': str(d8), 'd8Sha256': hashlib.sha256(d8.read_bytes()).hexdigest(),
                  'result': 'passed'}
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')
        print(result.stdout.strip() if result.stdout else '')
        print(json.dumps(report))


if __name__ == '__main__':
    main()
