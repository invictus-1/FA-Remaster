"""Compare the existing FA_ORACLE Lua hook records with candidate snapshots."""
import gzip
import json
import math
from pathlib import Path

from .records import Capture, Comparison, difference
from framework.protocol import unique_object


class OriginalChunks:
    """Sequential fragments from the original logger, bounded to one record."""
    def __init__(self, run_id):
        self.run_id, self.sequence, self.parts, self.count, self.bytes = run_id, 1, [], None, 0

    def append(self, chunk):
        fields = {'run_id', 'sequence', 'part', 'parts', 'data'}
        if type(chunk) is not dict or set(chunk) != fields or type(chunk['data']) is not str:
            raise ValueError('invalid original chunk')
        if any(type(chunk[key]) is not int for key in ('sequence', 'part', 'parts')):
            raise ValueError('invalid original chunk counters')
        if (chunk['run_id'] != self.run_id or chunk['sequence'] != self.sequence or chunk['part'] != len(self.parts) + 1 or
                not 1 <= chunk['part'] <= chunk['parts'] <= 262144):
            raise ValueError('missing, duplicate or foreign original chunks')
        if self.count is not None and self.count != chunk['parts']:
            raise ValueError('original chunk count changed')
        self.count = chunk['parts']
        self.bytes += len(chunk['data'].encode('utf-8'))
        if self.bytes > 32 * 1024 * 1024:
            raise ValueError('original record exceeds the capture size bound')
        self.parts.append(chunk['data'])
        if len(self.parts) < self.count:
            return None
        row = json.loads(''.join(self.parts), object_pairs_hook=unique_object)
        if type(row) is not dict or row.get('run_id') != self.run_id:
            raise ValueError('original chunk payload belongs to another run')
        self.parts, self.count, self.bytes = [], None, 0
        self.sequence += 1
        return row


class LegacyCapture:
    def __init__(self, path):
        self.path = Path(path)

    def __enter__(self):
        self.file = gzip.open(self.path, 'rt', encoding='utf-8') if self.path.suffix == '.gz' else self.path.open(encoding='utf-8')
        try:
            self.header = self.read()
            if (set(self.header) != {'kind', 'schema_version', 'run_id', 'requested_ticks'} or
                    self.header.get('kind') != 'start' or type(self.header.get('schema_version')) is not int or self.header['schema_version'] != 1):
                raise ValueError('missing original FA_ORACLE start record')
            ticks = self.header.get('requested_ticks')
            if type(ticks) is not list or not ticks or any(type(t) is not int or not 0 <= t <= 2147483647 for t in ticks) or ticks != list(range(ticks[0], ticks[-1] + 1)):
                raise ValueError('legacy regression requires every tick in a contiguous range')
            self.run_id = self.header.get('run_id')
            if type(self.run_id) is not str or not self.run_id:
                raise ValueError('missing original run identity')
            return self
        except BaseException:
            self.file.close()
            raise

    def __exit__(self, *args):
        self.file.close()

    def read(self):
        line = self.file.readline()
        if not line:
            raise ValueError('incomplete original FA_ORACLE capture')
        def invalid(value):
            raise ValueError('nonfinite original value: ' + value)
        row = json.loads(line, object_pairs_hook=unique_object, parse_constant=invalid)
        if type(row) is not dict:
            raise ValueError('invalid FA_ORACLE record')
        return row

    def ticks(self):
        for tick in self.header['requested_ticks']:
            row = self.read()
            if (set(row) != {'kind', 'schema_version', 'run_id', 'tick', 'units', 'armies'} or
                    row.get('kind') != 'snapshot' or type(row.get('tick')) is not int or row['tick'] != tick or
                    row.get('run_id') != self.run_id or type(row.get('schema_version')) is not int or row['schema_version'] != 1):
                raise ValueError(f'invalid original snapshot at tick {tick}')
            if type(row.get('units')) is not list or type(row.get('armies')) is not list or not row['armies']:
                raise ValueError('missing original units or armies')
            ids, army_ids = set(), set()
            for army in row['armies']:
                if type(army) is not dict or set(army) != {'army', 'mass_stored', 'energy_stored'} or type(army['army']) is not int or army['army'] < 1 or army['army'] in army_ids:
                    raise ValueError('invalid original army')
                army_ids.add(army['army'])
                for key in ('mass_stored', 'energy_stored'):
                    if type(army[key]) not in (int, float) or not math.isfinite(army[key]):
                        raise ValueError('invalid original economy')
            for unit in row['units']:
                fields = {'entity_id', 'blueprint_id', 'army', 'position', 'health', 'max_health'}
                if type(unit) is not dict or set(unit) != fields or type(unit['entity_id']) is not int or not 0 <= unit['entity_id'] <= 0xffffffff or unit['entity_id'] in ids:
                    raise ValueError('invalid original unit')
                ids.add(unit['entity_id'])
                if type(unit['army']) is not int or unit['army'] not in army_ids or type(unit['blueprint_id']) is not str or not unit['blueprint_id']:
                    raise ValueError('invalid original unit identity')
                if type(unit['position']) is not list or len(unit['position']) != 3:
                    raise ValueError('invalid original position')
                if any(type(value) not in (int, float) or not math.isfinite(value) for value in [*unit['position'], unit['health'], unit['max_health']]):
                    raise ValueError('invalid original unit values')
            yield row
        footer = self.read()
        if footer != dict(kind='complete', schema_version=1, run_id=self.run_id, snapshot_count=len(self.header['requested_ticks'])):
            raise ValueError('invalid original completion')
        if type(footer['schema_version']) is not int or type(footer['snapshot_count']) is not int:
            raise ValueError('invalid original completion types')
        if self.file.read(1):
            raise ValueError('trailing original records')


def inspect_legacy(path):
    with LegacyCapture(path) as capture:
        for _ in capture.ticks():
            pass
        return capture.header


def compare_legacy(oracle, actual, abs_tol=0, rel_tol=0):
    if any(type(value) not in (int, float) or not math.isfinite(value) or value < 0 for value in (abs_tol, rel_tol)):
        raise ValueError('tolerances must be finite nonnegative numbers')
    count = checks = 0
    with LegacyCapture(oracle) as original, Capture(actual) as candidate:
        if candidate.header['start_tick'] != original.header['requested_ticks'][0] or candidate.header['end_tick'] != original.header['requested_ticks'][-1] or candidate.header['interval'] != 1:
            raise ValueError('original and candidate tick ranges differ')
        rows = candidate.ticks()
        for row in original.ticks():
            data = next(rows)['data']
            expected = {'units': {str(u['entity_id']): dict(u, position=dict(zip(('x', 'y', 'z'), u['position']))) for u in row['units']},
                        'armies': {str(a['army']): a for a in row['armies']}}
            if len(expected['units']) != len(row['units']) or len(expected['armies']) != len(row['armies']):
                raise ValueError('duplicate original entity or army IDs')
            projected = {'units': {}, 'armies': {}}
            for identity, unit in data['units'].items():
                projected['units'][identity] = dict(entity_id=int(identity), blueprint_id=unit['blueprint'], army=unit['army'],
                    position=unit['position'], health=unit['health'], max_health=unit['max_health'])
            for identity, army in data['armies'].items():
                projected['armies'][identity] = dict(army=int(identity), mass_stored=army['economy']['MASS']['stored'],
                                                     energy_stored=army['economy']['ENERGY']['stored'])
            found = difference(expected, projected, 'session', abs_tol, rel_tol)
            if found:
                return Comparison(False, count, checks, f'tick {row["tick"]}: {found}', row['tick'])
            count += 1
            checks += len(row['units']) * 8 + len(row['armies']) * 3
        try:
            next(rows)
        except StopIteration:
            pass
        else:
            raise ValueError('unexpected candidate snapshots')
    return Comparison(True, count, checks)
