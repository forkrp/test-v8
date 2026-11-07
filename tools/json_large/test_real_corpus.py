# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Check expansion invariants which affect benchmark representativeness."""
import copy
import unittest

from real_corpus import encode, expand, strict_load


class RealCorpusTest(unittest.TestCase):
    def check_expansion(self, root, recipe):
        saved = copy.deepcopy(root)
        raw, info = expand(root, recipe, 4096, 42)
        self.assertEqual(root, saved, 'The original snapshot must not be modified')
        self.assertEqual(raw, expand(root, recipe, 4096, 42)[0])
        self.assertNotEqual(raw, expand(root, recipe, 4096, 43)[0])
        self.assertGreater(info['added_records'], 0)
        self.assertLess(abs(len(raw) / 4096 - 1), .1)
        return strict_load(raw), info

    def test_geojson_envelope_and_metadata(self):
        features = [{'type': 'Feature', 'id': 'a', 'properties': {'mag': 1.2},
                     'geometry': {'coordinates': [-1.3, 2, 8]}},
                    {'type': 'Feature', 'id': 'b', 'properties': {'mag': None},
                     'geometry': {'coordinates': [-2.5, 3, 9]}}]
        root = dict(type='FeatureCollection', metadata=dict(count=2),
                    features=features, bbox=[-2.5, 2, 8, -1.3, 3, 9])
        result, info = self.check_expansion(root, 'geojson-features')
        self.assertEqual(list(result), list(root))
        self.assertEqual(result['bbox'], root['bbox'])
        self.assertEqual(result['metadata']['count'], info['total_records'])
        self.assertEqual(result['features'][:2], features)
        self.assertEqual(len({r['id'] for r in result['features']}), info['total_records'])
        originals = [{k: v for k, v in r.items() if k != 'id'} for r in features]
        for record in result['features']:
            self.assertIn({k: v for k, v in record.items() if k != 'id'}, originals)

    def test_station_array_keeps_unicode_and_numeric_types(self):
        root = [dict(sno='500100001', sna='臺北', available=4, lat=25.03),
                dict(sno='500100002', sna='中山', available=0, lat=25.08)]
        result, info = self.check_expansion(root, 'station-array')
        self.assertEqual(result[:2], root)
        self.assertEqual(len({r['sno'] for r in result}), info['total_records'])
        for record in result:
            self.assertEqual(len(record['sno']), 9)
            self.assertTrue(record['sno'].isdigit())
            self.assertIn(record['sna'], ['臺北', '中山'])
            self.assertIs(type(record['available']), int)
            self.assertIs(type(record['lat']), float)

    def test_npm_keeps_dictionary_tags_and_version_references(self):
        root = {'name': 'pkg', 'versions': {
            '1.0.0': {'name': 'pkg', 'version': '1.0.0', '_id': 'pkg@1.0.0'},
            '2.0.0': {'name': 'pkg', 'version': '2.0.0', '_id': 'pkg@2.0.0',
                      'optional': True}},
            'dist-tags': {'latest': '2.0.0'},
            'time': {'1.0.0': '2000-01-01', '2.0.0': '2000-02-01'}}
        result, info = self.check_expansion(root, 'npm-versions')
        self.assertEqual(list(result), list(root))
        self.assertEqual(result['dist-tags'], root['dist-tags'])
        self.assertEqual(len(result['versions']), info['total_records'])
        for version, record in result['versions'].items():
            self.assertEqual(record['version'], version)
            self.assertEqual(record['_id'], 'pkg@' + version)
            self.assertIn(version, result['time'])

    def test_refuse_lossy_recipe_inputs(self):
        for raw in [b'{"a":1,"a":2}', b'[NaN]', b'[Infinity]']:
            with self.assertRaises(ValueError):
                strict_load(raw)
        value = strict_load(b'{"a":-0.0,"b":1e100,"c":"\\ud83d\\ude00"}')
        self.assertEqual(encode(value), '{"a":-0.0,"b":1e+100,"c":"😀"}'.encode())


if __name__ == '__main__':
    unittest.main()
