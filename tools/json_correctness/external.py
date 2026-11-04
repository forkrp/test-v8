#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Fetch pinned upstream tests, prepare independent expectations, and run d8."""
import argparse
import concurrent.futures
import gzip
import hashlib
import itertools
import json
import re
import struct
import subprocess
import sys
import time
import urllib.request
import urllib.parse
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LOCK = HERE / 'external-sources.json'
DRIVER = ROOT / 'test/json/external-corpus.js'
C_STRING = r'"(?:\\.|[^"\\])*"'
NUMBER = re.compile(r'-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\Z')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=True, indent=2) + '\n')


def verify_sources(cache):
    lock = json.loads(LOCK.read_text())
    for row in lock['files']:
        path = cache / row['source'] / row['path']
        if not path.is_file() or sha(path) != row['sha256']:
            raise ValueError(f'Missing or modified upstream file: {path}')
    return lock


def fetch(cache):
    lock = json.loads(LOCK.read_text())
    def one(row):
        path = cache / row['source'] / row['path']
        if path.exists():
            if sha(path) != row['sha256']:
                raise ValueError(f'Refusing to overwrite modified source: {path}')
            return
        source = lock['sources'][row['source']]
        url = (f'https://raw.githubusercontent.com/{source["repository"]}/'
               f'{source["commit"]}/' + urllib.parse.quote(row['path']))
        data = urllib.request.urlopen(url, timeout=60).read()
        if hashlib.sha256(data).hexdigest() != row['sha256']:
            raise ValueError(f'Checksum mismatch: {url}')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(one, lock['files']))
    verify_sources(cache)
    print(f'Verified {len(lock["files"])} pinned upstream files')


def units(s):
    return s.encode('utf-16-be', errors='surrogatepass').hex()


def keys(obj):
    # ECMAScript own string key ordering; duplicate keys retain first position.
    indices = [k for k in obj if re.fullmatch(r'0|[1-9][0-9]*', k)
               and len(k) <= 10 and int(k) < 0xffffffff]
    index_set = set(indices)
    return sorted(indices, key=int) + [k for k in obj if k not in index_set]


def model(value):
    if value is None: return ['null']
    if isinstance(value, bool): return ['b', value]
    if isinstance(value, float): return ['n', struct.pack('>d', value).hex()]
    if isinstance(value, str): return ['s', value]
    if isinstance(value, list): return ['a', [model(v) for v in value]]
    if isinstance(value, dict): return ['o', [[k, model(value[k])] for k in keys(value)]]
    raise TypeError(type(value))


def wire(m):
    kind = m[0]
    if kind == 'null': return 'null'
    if kind == 'b': return 'true' if m[1] else 'false'
    if kind == 'n': return 'n' + m[1]
    if kind == 's': return 's' + units(m[1])
    if kind == 'a': return 'a[' + ','.join(wire(v) for v in m[1]) + ']'
    return 'o{' + ','.join('s' + units(k) + ':' + wire(v) for k, v in m[1]) + '}'


def parse(text):
    def constant(s): raise ValueError('Non-JSON constant: ' + s)
    return json.loads(text, parse_int=float, parse_float=float,
                      parse_constant=constant)


def c_literal(expr):
    """Decode only ordinary UTF-8 C string literals, never arbitrary C code."""
    literals = re.findall(C_STRING, expr, re.S)
    if not literals or re.sub(C_STRING, '', expr, flags=re.S).strip():
        raise ValueError('Not a sequence of ordinary string literals')
    data = bytearray()
    escapes = {'a':7, 'b':8, 'f':12, 'n':10, 'r':13, 't':9, 'v':11,
               '\\':92, '"':34, "'":39, '?':63}
    for token in literals:
        s = token[1:-1]; i = 0
        while i < len(s):
            c = s[i]; i += 1
            if c != '\\': data.extend(c.encode('utf-8')); continue
            c = s[i]; i += 1
            if c in escapes: data.append(escapes[c])
            elif c in '01234567':
                m = re.match(r'[0-7]{0,2}', s[i:]); digits = c + m[0]
                data.append(int(digits, 8)); i += len(m[0])
            elif c == 'x':
                m = re.match(r'[0-9a-fA-F]+', s[i:])
                if not m: raise ValueError('Invalid C hex escape')
                data.append(int(m[0], 16)); i += len(m[0])
            elif c == '\n': pass
            else: raise ValueError(f'Unsupported C escape: {c}')
    return data.decode('utf-8', errors='strict')


def macro_inputs(text):
    # Preserve line numbers, literals and inactive #if branches. The latter are
    # extra inputs, not claims that RapidJSON executes those tests upstream.
    text = re.sub(C_STRING + r'|//[^\n]*|/\*.*?\*/',
                  lambda m: m[0] if m[0].startswith('"') else
                  ''.join('\n' if c == '\n' else ' ' for c in m[0]), text, flags=re.S)
    pattern = r'^\s*(TEST_DOUBLE|TEST_INTEGER|TEST_NUMBER_ERROR|TEST_STRING_ERROR|TEST_ERROR|TEST_STRING|TEST_ROUNDTRIP)\('
    for match in re.finditer(pattern, text, re.M):
        start = match.end(); i = start; depth = 1; args = []
        while depth:
            if text[i] == '"':
                token = re.match(C_STRING, text[i:], re.S); i += len(token[0]); continue
            c = text[i]
            if c == '(': depth += 1
            if c == ')': depth -= 1
            if (c == ',' and depth == 1) or depth == 0:
                args.append(text[start:i].strip()); start = i + 1
            i += 1
        kind = match[1]
        arg = 0 if kind == 'TEST_ROUNDTRIP' else 2 if kind == 'TEST_STRING' else 1
        if kind == 'TEST_STRING' and args[0] != 'UTF8<>': continue
        # Nonliteral arguments are loops/macros requiring upstream C++ context.
        if not args[arg].startswith('"'): continue
        yield text.count('\n', 0, match.start()) + 1, kind, c_literal(args[arg]), args


def metadata(text, field):
    header = re.search(r'/\*---(.*?)---\*/', text, re.S)
    if not header: raise ValueError('Missing Test262 metadata')
    match = re.search(r'^' + field + r':\s*\[([^\]]*)\]', header[1], re.M)
    if not match:
        if re.search(r'^' + field + ':', header[1], re.M):
            raise ValueError(f'Unsupported Test262 metadata: {field}')
        return []
    return [v.strip() for v in match[1].split(',') if v.strip()]


def prepare(cache, out):
    lock = verify_sources(cache)
    out.mkdir(parents=True, exist_ok=False)
    cases = []; seen = {}; skipped = []; adaptations = []; differential_only = []; occurrences = 0
    def add(text, origin, note=None):
        nonlocal occurrences
        occurrences += 1
        digest = hashlib.sha256(text.encode('utf-16-be', errors='surrogatepass')).hexdigest()
        if note: adaptations.append(dict(origin=origin, note=note))
        if digest in seen:
            seen[digest]['origins'].append(origin); return
        row = dict(id=digest, text=text, origins=[origin])
        try:
            row['model'] = model(parse(text)); row['expected'] = wire(row['model'])
        except (json.JSONDecodeError, ValueError): row['expected'] = 'throw:SyntaxError'
        except RecursionError:
            row.pop('model', None)
            row['expected'] = None
            differential_only.append(dict(origin=origin, reason='Python oracle recursion limit; compare parse APIs with original V8'))
        cases.append(row); seen[digest] = row

    for path in sorted((cache/'rapidjson/test/unittest').glob('*test.cpp')):
        if path.name not in ['readertest.cpp', 'writertest.cpp']: continue
        for line, kind, text, args in macro_inputs(path.read_text()):
            origin = f'rapidjson/test/unittest/{path.name}:{line}:{kind}'
            note = None
            if kind.endswith('ERROR') and ('TooBig' in args[0] or 'SurrogateInvalid' in args[0]):
                note = 'ECMAScript permits numeric overflow / escaped unpaired UTF-16 units'
            if kind == 'TEST_ROUNDTRIP':
                note = 'Reuse input; ECMAScript number formatting need not preserve upstream spelling'
            add(text, origin, note)
            if kind == 'TEST_DOUBLE' and NUMBER.fullmatch(args[2]):
                if struct.pack('>d', float(text)) != struct.pack('>d', float(args[2])):
                    raise ValueError(f'Upstream double expectation disagrees: {origin}')
            if kind == 'TEST_STRING':
                if units(parse(text)) != units(c_literal(args[1])):
                    raise ValueError(f'Upstream string expectation disagrees: {origin}')
        # The scanner regression supplies a variable to TEST_ROUNDTRIP.
        if path.name == 'writertest.cpp':
            part = path.read_text().split('TEST(Writer, ScanWriteUnescapedString)')[1]
            literal = re.search(r'const char json\[\] = (' + C_STRING + ');', part)[1]
            add(c_literal(literal), 'rapidjson/Writer.ScanWriteUnescapedString')

    for path in sorted((cache/'yyjson/test/data/num').glob('*.txt')):
        for line, text in enumerate(path.read_text().splitlines(), 1):
            if not text or text.startswith('#'): continue
            add(text, f'yyjson/test/data/num/{path.name}:{line}',
                'Use strict ECMAScript grammar and binary64, not yyjson extension/integer modes'
                if '(ext)' in path.name or '(inf)' in path.name else None)

    for source, prefix in [('simdjson_data', 'jsonchecker'), ('nlohmann_data', 'json_roundtrip'),
                           ('nlohmann_data', 'json_tests')]:
        for path in sorted((cache/source/prefix).rglob('*.json')):
            origin = source + '/' + path.relative_to(cache/source).as_posix()
            try: text = path.read_bytes().decode('utf-8', errors='strict')
            except UnicodeDecodeError:
                skipped.append(dict(origin=origin, reason='Invalid UTF-8 bytes; JSON APIs take UTF-16 strings')); continue
            add(text, origin, 'Classify by ECMAScript-compatible oracle, not upstream filename')

    blns = cache/'nlohmann_data/big-list-of-naughty-strings/blns.json'
    strings = json.loads(blns.read_text())
    for i, s in enumerate(strings):
        add(json.dumps(s, ensure_ascii=True), f'nlohmann_data/blns:{i}')
    # Reproducible lane/length mutations of imported string seeds. These are
    # logical string offsets, not a claim about actual allocation alignment.
    seeds = [c for c in cases if c.get('model', [None])[0] == 's']
    for row in seeds:
        value = row['model'][1]
        if len(value) > 128: continue
        for width in [15,16,17,31,32,33,63,64,65]:
            add(json.dumps('a'*width + value + '\u6f22', ensure_ascii=True),
                f'derived/lanes:{row["id"]}:{width}')
    # Truncated escape/token prefixes with independent accept/reject outcomes.
    for row in list(cases):
        if not row['origins'][0].startswith('rapidjson/') or len(row['text']) > 128: continue
        for end in sorted({0, 1, len(row['text'])//2, len(row['text'])-1}):
            if end >= 0: add(row['text'][:end], f'derived/truncate:{row["id"]}:{end}')

    scanner = next(c for c in cases if 'rapidjson/Writer.ScanWriteUnescapedString' in c['origins'])
    seed = scanner['model'][1][0][1]
    for size in [4*1024*1024-32, 4*1024*1024+32, 16*1024*1024+32]:
        for invalid in [False, True]:
            identity = f'derived/large:{scanner["id"]}:{size}:{invalid}'
            cases.append(dict(id=hashlib.sha256(identity.encode()).hexdigest(),
                              origins=[identity], large=dict(size=size, seed=seed, invalid=invalid)))

    fixtures = out/'test262'; fixtures.mkdir()
    test_rows = []
    for path in sorted((cache/'test262/test/built-ins/JSON').rglob('*.js')):
        text = path.read_text(); flags = metadata(text, 'flags')
        features = metadata(text, 'features'); includes = metadata(text, 'includes')
        if set(flags) - {'onlyStrict', 'noStrict'} or re.search(r'^negative:', text, re.M):
            raise ValueError(f'Unhandled Test262 execution semantics: {path}')
        harness = '\n'.join((cache/'test262/harness'/p).read_text()
                            for p in ['sta.js', 'assert.js', *includes])
        host = ('var $262 = {createRealm: function() { return {global: '
                'Realm.global(Realm.createAllowCrossRealmAccess())}; }};\n')
        for strict in ([True] if 'onlyStrict' in flags else [False] if 'noStrict' in flags else [False, True]):
            name = path.relative_to(cache/'test262').as_posix()
            key = name + (':strict' if strict else ':sloppy')
            target = fixtures/(hashlib.sha256(key.encode()).hexdigest() + '.js')
            target.write_text(('"use strict";\n' if strict else '') + harness + '\n' + host + text +
                              '\nprint("EXTERNAL_TEST262_DONE");\n')
            test_rows.append(dict(id=key, file=target.relative_to(out).as_posix(),
                                  features=features, sha256=sha(target)))
    # JS literal transport avoids parsing fixture JSON using the API under test.
    corpus = out/'corpus.js'
    corpus.write_text('globalThis.externalCases = ' + json.dumps(cases, ensure_ascii=True,
                      separators=(',', ':')) + ';\n')
    save(out/'manifest.json', dict(lock_sha256=sha(LOCK), preparer_sha256=sha(Path(__file__)),
         corpus_sha256=sha(corpus), test262=test_rows, cases=len(cases),
         valid=sum('model' in c or 'large' in c and not c['large']['invalid'] for c in cases),
         occurrences=occurrences+6, large_cases=6,
         skipped=skipped, adaptations=adaptations, differential_only=differential_only,
         oracle='CPython json strict syntax, float conversion to binary64; JS property ordering'))
    print('Prepared', len(cases), 'unique cases and', len(test_rows), 'Test262 executions', flush=True)


def execute(command, stdout, stderr, timeout):
    start = time.monotonic(); timed_out = False
    with stdout.open('wb') as output, stderr.open('wb') as errors:
        try:
            result = subprocess.run(command, stdout=output, stderr=errors, cwd=ROOT, timeout=timeout)
            status = result.returncode
        except subprocess.TimeoutExpired:
            status = None; timed_out = True
    return dict(command=command, returncode=status, timed_out=timed_out,
                seconds=time.monotonic()-start, stdout_sha256=sha(stdout),
                stderr_sha256=sha(stderr))


def equal_files(a, b):
    with gzip.open(a, 'rb') as left, gzip.open(b, 'rb') as right:
        for i, (x, y) in enumerate(itertools.zip_longest(left, right), 1):
            if x != y: return dict(equal=False, line=i, expected=repr(x)[:500], actual=repr(y)[:500])
    return dict(equal=True)


def run(args):
    prepared = args.prepared.resolve(); manifest = json.loads((prepared/'manifest.json').read_text())
    if manifest['lock_sha256'] != sha(LOCK) or sha(prepared/'corpus.js') != manifest['corpus_sha256']:
        raise ValueError('Prepared fixture identity mismatch')
    for case in manifest['test262']:
        if sha(prepared/case['file']) != case['sha256']: raise ValueError(case['id'])
    args.out.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve(); binary_sha = sha(binary)
    reference = json.loads(args.reference_results.read_text()) if args.reference_results else None
    if reference and (not reference['all_passed'] or reference['fixture_sha256'] != sha(prepared/'manifest.json')):
        raise ValueError('Reference failed or fixture manifest differs')
    if reference and not reference['reference']: raise ValueError('Expected an original-reference run')
    if reference and reference['driver_sha256'] != sha(DRIVER):
        raise ValueError('Reference used a different JS driver')
    report = dict(label=args.label, binary=str(binary), binary_sha256=binary_sha,
                  reference=reference is None, reference_results=str(args.reference_results),
                  fixture_sha256=sha(prepared/'manifest.json'), lock_sha256=sha(LOCK),
                  driver_sha256=sha(DRIVER), runner_sha256=sha(Path(__file__)),
                  flags=args.flag, test262=[], corpus=[], completed=False, all_passed=False)
    def checkpoint(): save(args.out/'results.json', report)
    checkpoint()
    for index, case in enumerate(manifest['test262']):
        log = args.out/f'test262-{index:03}.stdout'; err = log.with_suffix('.stderr')
        result = execute([str(binary), *args.flag, str(prepared/case['file'])], log, err, 60)
        result.update(id=case['id'], passed=result['returncode'] == 0 and
                      log.read_bytes().endswith(b'EXTERNAL_TEST262_DONE\n'))
        report['test262'].append(result)
        if not result['passed']: print('FAIL',case['id'],log,flush=True)
        if (index + 1) % 50 == 0:
            checkpoint(); print(args.label, 'Test262', index+1, flush=True)
    apis = ['parse','stringify'] if reference is None else ['parse','parseAsync','stringify','stringifyAsync']
    for api in apis:
        log = args.out/(api+'.stdout'); err = args.out/(api+'.stderr')
        command = [str(binary), '--expose-gc', '--thread-pool-size=4', *args.flag,
                   str(prepared/'corpus.js'), str(DRIVER), '--', api]
        result = execute(command, log, err, 1800)
        final = b''; rows = 0; failures = []
        with log.open('rb') as f, gzip.open(log.with_suffix('.gz'), 'wb') as compressed:
            for line in f:
                final = line; compressed.write(line)
                if line.startswith(b'CASE\t'): rows += 1
                elif not line.startswith(b'DONE\t') and len(failures) < 20:
                    failures.append(line[:1000].decode(errors='replace'))
        expected = manifest['cases'] if api.startswith('parse') else manifest['valid']
        result.update(api=api, rows=rows, failures=failures[:20], output=str(log.with_suffix('.gz').resolve()),
                      passed=result['returncode'] == 0 and not failures and rows == expected and
                      final == f'DONE\t{expected}\n'.encode())
        if reference:
            base = next(x for x in reference['corpus'] if x['api'] == ('parse' if api.startswith('parse') else 'stringify'))
            result['comparison'] = equal_files(Path(base['output']), log.with_suffix('.gz'))
            result['passed'] &= result['comparison']['equal']
        report['corpus'].append(result); checkpoint()
        print(args.label, api, rows, 'PASS' if result['passed'] else 'FAIL', flush=True)
    if sha(binary) != binary_sha: raise ValueError('Binary changed during run')
    report['completed'] = True
    report['all_passed'] = all(r['passed'] for group in ['test262','corpus'] for r in report[group])
    checkpoint()
    if not report['all_passed']: raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    f = sub.add_parser('fetch'); f.add_argument('--cache', type=Path, required=True)
    p = sub.add_parser('prepare'); p.add_argument('--cache', type=Path, required=True); p.add_argument('--out', type=Path, required=True)
    r = sub.add_parser('run')
    for option in ['prepared','binary','out']: r.add_argument('--'+option, type=Path, required=True)
    r.add_argument('--label', required=True); r.add_argument('--reference-results', type=Path)
    r.add_argument('--flag', action='append', default=[])
    args = parser.parse_args()
    if args.action == 'fetch': fetch(args.cache)
    elif args.action == 'prepare': prepare(args.cache, args.out)
    else: run(args)


if __name__ == '__main__': main()
