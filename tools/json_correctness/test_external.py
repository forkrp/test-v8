#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Oracle and failure-detection checks. Set JSON_TEST_D8 to a candidate d8."""
import json
import gzip
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

import external


class OracleTest(unittest.TestCase):
    def test_numeric_answers(self):
        answers = {'-0':'8000000000000000', '9007199254740993':'4340000000000000',
                   '5e-324':'0000000000000001', '1e400':'7ff0000000000000',
                   '-1e-400':'8000000000000000',
                   '1.00000000000000011102230246251565404236316680908203125':'3ff0000000000000',
                   '1.00000000000000011102230246251565404236316680908203126':'3ff0000000000001'}
        for text, bits in answers.items():
            self.assertEqual(external.model(external.parse(text)), ['n', bits])

    def test_strict_grammar(self):
        for text in ['NaN', 'Infinity', '+1', '01', '0x10', '1.', '[1,]', 'true false', '\ufeff0']:
            with self.subTest(text=text), self.assertRaises(ValueError): external.parse(text)

    def test_units_keys_and_duplicates(self):
        value = external.parse('{"b":0,"2":2,"1":1,"b":3,"__proto__":4,"01":5}')
        self.assertEqual(external.keys(value), ['1','2','b','__proto__','01'])
        self.assertEqual(value['b'], 3.0)
        self.assertEqual(external.units(external.parse('"\\ud800"')), 'd800')
        self.assertEqual(external.units(external.parse('"\\ud834\\udd1e"')), 'd834dd1e')

    def test_c_literals(self):
        self.assertEqual(external.c_literal(r'"a\0b" "\xC2\xA2"'), 'a\0b\u00a2')
        rows = list(external.macro_inputs('  TEST_DOUBLE(fullPrecision, "1.0" /*x*/, 1.0);'))
        self.assertEqual(rows[0][2], '1.0')
        with self.assertRaises(ValueError): external.c_literal('some_expression()')
        with self.assertRaises(UnicodeDecodeError): external.c_literal(r'"\xff"')

    def test_comparison_detects_changed_and_missing_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            left, right = Path(directory)/'a.gz', Path(directory)/'b.gz'
            with gzip.open(left, 'wb') as f: f.write(b'one\ntwo\nDONE\n')
            for changed in [b'one\nTWO\nDONE\n', b'one\n', b'one\ntwo\nDONE\nextra\n']:
                with gzip.open(right, 'wb') as f: f.write(changed)
                self.assertFalse(external.equal_files(left, right)['equal'])

    def test_runner_rejects_modified_fixture_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path/'manifest.json').write_text(json.dumps({'lock_sha256':'wrong'}))
            with self.assertRaisesRegex(ValueError, 'identity mismatch'):
                external.run(SimpleNamespace(prepared=path))


class DriverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = os.environ.get('JSON_TEST_D8')
        if not cls.binary: raise RuntimeError('Set JSON_TEST_D8; driver checks must not silently skip')

    def execute(self, row, api):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'fixture.js'
            path.write_text('globalThis.externalCases = '+json.dumps([row])+';')
            return subprocess.run([self.binary, '--expose-gc', str(path), str(external.DRIVER), '--', api],
                                  capture_output=True, text=True, timeout=30)

    def test_valid_and_invalid_inputs(self):
        for api in ['parse', 'parseAsync']:
            for text, expected in [('1','n3ff0000000000000'), ('[1,]','throw:SyntaxError')]:
                result = self.execute(dict(id='control',text=text,expected=expected),api)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertTrue(result.stdout.endswith('DONE\t1\n'))

    def test_wrong_expected_is_a_failure(self):
        for api in ['parse','parseAsync']:
            result = self.execute(dict(id='injected',text='1',expected='n4000000000000000'),api)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('Parse mismatch',result.stdout)
            self.assertNotIn('DONE\t',result.stdout)

    def test_deep_reference_only_encoding(self):
        for api in ['parse','parseAsync']:
            result = self.execute(dict(id='deep',text='['*2000+'0'+']'*2000,expected=None),api)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('DONE\t1\n',result.stdout)

    def test_stringify_does_not_prepare_with_parse(self):
        for api in ['stringify','stringifyAsync']:
            value = '\ud800\x00"\\'
            row = dict(id='quote',text='intentionally invalid JSON',model=['s',value],
                       expected='s'+external.units(value))
            result = self.execute(row,api)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('DONE\t1\n',result.stdout)

    def test_bad_fixture_and_missing_api_fail(self):
        row = dict(id='bad-model',model=['s','abc'],expected='s0000')
        self.assertNotEqual(self.execute(row,'stringify').returncode,0)
        self.assertNotEqual(self.execute(row,'noSuchAPI').returncode,0)


if __name__ == '__main__': unittest.main()
