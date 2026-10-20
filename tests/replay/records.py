"""Streaming, strict capture validation and field comparisons."""
from dataclasses import dataclass
import gzip
import json
import math
from pathlib import Path
import re

from framework.protocol import unique_object


def decode(line):
    def invalid(value):
        raise ValueError(f'invalid JSON number: {value}')
    value = json.loads(line, object_pairs_hook=unique_object, parse_constant=invalid)
    def finite(item):
        if type(item) is float and not math.isfinite(item):
            raise ValueError('nonfinite capture value')
        if type(item) is dict:
            for child in item.values():
                finite(child)
        elif type(item) is list:
            raise ValueError('capture tables must be JSON objects')
    finite(value)
    return value


def integer(value, name, minimum=0):
    if type(value) is not int or not minimum <= value <= 2147483647:
        raise ValueError(f'{name} must be an integer in {minimum}..2147483647')


def validate_header(record):
    fields = {'version', 'event', 'capture', 'case', 'role', 'collector', 'start_tick', 'end_tick', 'interval', 'session'}
    if type(record) is not dict or set(record) != fields or type(record['version']) is not int or record['version'] != 1:
        raise ValueError('invalid capture header or version')
    if record['event'] != 'header' or record['role'] not in ('original', 'candidate'):
        raise ValueError('invalid capture header event or role')
    for key in ('capture', 'case', 'collector'):
        if type(record[key]) is not str or not record[key]:
            raise ValueError(f'missing capture {key}')
    if not re.fullmatch(r'[0-9a-f]{64}', record['collector']):
        raise ValueError('invalid collector digest')
    for key in ('start_tick', 'end_tick', 'interval'):
        integer(record[key], key, 1 if key == 'interval' else 0)
    if record['end_tick'] < record['start_tick'] or (record['end_tick'] - record['start_tick']) % record['interval']:
        raise ValueError('capture end tick must be on the sampling interval')
    if type(record['session']) is not dict or set(record['session']) != {'map', 'options', 'mods'}:
        raise ValueError('invalid session metadata')
    if type(record['session']['map']) is not str or not record['session']['map']:
        raise ValueError('missing session map')
    if any(type(record['session'][key]) is not dict for key in ('options', 'mods')):
        raise ValueError('session options and mods must be objects')


def fields(value, expected, label):
    if type(value) is not dict or set(value) != set(expected):
        raise ValueError(f'invalid {label} fields')


def number(value, label):
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError(f'invalid {label} number')


def validate_data(data):
    fields(data, ('armies', 'units'), 'tick data')
    if any(type(value) is not dict for value in data.values()) or not data['armies']:
        raise ValueError('tick data needs armies and units objects')
    for index, army in data['armies'].items():
        if not re.fullmatch(r'[1-9][0-9]*', index):
            raise ValueError('invalid army index')
        fields(army, ('name', 'defeated', 'economy'), 'army')
        if type(army['name']) is not str or type(army['defeated']) is not bool:
            raise ValueError('invalid army identity or defeat state')
        fields(army['economy'], ('MASS', 'ENERGY'), 'economy')
        for resource in army['economy'].values():
            fields(resource, ('stored', 'ratio', 'income', 'requested', 'usage'), 'resource')
            for value in resource.values():
                number(value, 'economy')
    unit_fields = ('army', 'blueprint', 'dead', 'position', 'velocity', 'orientation', 'health', 'max_health',
                   'fraction_complete', 'work_progress', 'fuel', 'shield', 'layer', 'states', 'orders', 'queue_length')
    for identity, unit in data['units'].items():
        if not re.fullmatch(r'0|[1-9][0-9]*', identity):
            raise ValueError('invalid unit id')
        fields(unit, unit_fields, 'unit')
        integer(unit['army'], 'unit army', 1)
        if str(unit['army']) not in data['armies']:
            raise ValueError('unit belongs to an unknown army')
        if type(unit['dead']) is not bool or any(type(unit[key]) is not str for key in ('blueprint', 'layer')):
            raise ValueError('invalid unit identity or state')
        for key in ('position', 'velocity', 'orientation'):
            fields(unit[key], ('x', 'y', 'z', 'w') if key == 'orientation' else ('x', 'y', 'z'), key)
            for value in unit[key].values():
                number(value, key)
        for key in ('health', 'max_health', 'fraction_complete', 'work_progress', 'fuel', 'shield'):
            number(unit[key], key)
        if type(unit['states']) is not dict or any(value is not True for value in unit['states'].values()):
            raise ValueError('unit states must map names to true')
        integer(unit['queue_length'], 'queue length')
        queue = unit['orders']
        if type(queue) is not dict or set(queue) != {str(n) for n in range(1, unit['queue_length'] + 1)}:
            raise ValueError('unit queue length does not match orders')
        for command in queue.values():
            required = {'type', 'x', 'y', 'z'}
            if type(command) is not dict or not required <= set(command) <= required | {'target_id', 'blueprint'}:
                raise ValueError('invalid command fields')
            integer(command['type'], 'command type')
            for key in ('x', 'y', 'z'):
                number(command[key], 'command position')
            if 'target_id' in command:
                # FAF returns entity target IDs as decimal strings.
                value = command['target_id']
                if not ((type(value) is str and re.fullmatch(r'[0-9]+', value)) or
                        (type(value) is int and 0 <= value <= 0xffffffff)):
                    raise ValueError('invalid command target id')
            if 'blueprint' in command and type(command['blueprint']) is not str:
                raise ValueError('invalid command blueprint')


class Capture:
    """Retains one tick at a time; completion and EOF are both required."""
    def __init__(self, path):
        self.path = Path(path)
        self.file = None

    def __enter__(self):
        self.file = gzip.open(self.path, 'rt', encoding='utf-8') if self.path.suffix == '.gz' else self.path.open(encoding='utf-8')
        try:
            self.header = self.read()
            validate_header(self.header)
            return self
        except BaseException:
            self.file.close()
            raise

    def __exit__(self, *args):
        self.file.close()

    def read(self):
        line = self.file.readline()
        if not line:
            raise ValueError(f'{self.path}: incomplete capture')
        return decode(line)

    def ticks(self):
        header = self.header
        total = (header['end_tick'] - header['start_tick']) // header['interval'] + 1
        for sequence in range(1, total + 1):
            record = self.read()
            if type(record) is not dict or set(record) != {'version', 'event', 'capture', 'sequence', 'tick', 'data'}:
                raise ValueError(f'{self.path}: invalid tick record')
            tick = header['start_tick'] + (sequence - 1) * header['interval']
            if (type(record['version']) is not int or record['version'] != 1 or record['event'] != 'tick' or
                    record['capture'] != header['capture'] or type(record['sequence']) is not int or
                    record['sequence'] != sequence or type(record['tick']) is not int or record['tick'] != tick):
                raise ValueError(f'{self.path}: missing, duplicate or foreign tick at {tick}')
            validate_data(record['data'])
            yield record
        footer = self.read()
        if footer != {'version': 1, 'event': 'complete', 'capture': header['capture'], 'samples': total,
                      'last_tick': header['end_tick']} or any(type(footer.get(k)) is not int for k in ('version', 'samples', 'last_tick')):
            raise ValueError(f'{self.path}: invalid capture completion')
        if self.file.read(1):
            raise ValueError(f'{self.path}: trailing records after completion')


def inspect(path, role=None):
    with Capture(path) as capture:
        if role and capture.header['role'] != role:
            raise ValueError(f'{path}: expected a {role} capture')
        for _ in capture.ticks():
            pass
        return capture.header


def collect(log, output):
    """Extract marked records from an engine log without changing an existing baseline."""
    output = Path(output)
    created = False
    try:
        target_file = gzip.open(output, 'xt', encoding='utf-8') if output.suffix == '.gz' else output.open('x', encoding='utf-8')
        with target_file as target:
            created = True
            with Path(log).open(encoding='utf-8', errors='strict') as source:
                for line in source:
                    match = re.match(r'^(?:.*?\binfo:\s*)?SESSION_REPLAY (.*)\s*$', line)
                    if match:
                        target.write(json.dumps(decode(match[1]), separators=(',', ':'), ensure_ascii=False, allow_nan=False) + '\n')
        return inspect(output)
    except BaseException:
        if created:
            output.unlink(missing_ok=True)
        raise


@dataclass
class Comparison:
    passed: bool
    ticks: int
    checks: int
    reason: str = ''
    first_tick: object = None


def difference(want, got, path, abs_tol, rel_tol):
    if type(want) is dict and type(got) is dict:
        for key in sorted(set(want) | set(got)):
            child = path + '[' + json.dumps(key, ensure_ascii=False) + ']'
            if key not in got:
                return child + ': missing from candidate'
            if key not in want:
                return child + ': unexpected in candidate'
            found = difference(want[key], got[key], child, abs_tol, rel_tol)
            if found:
                return found
        return ''
    numeric = type(want) in (int, float) and type(got) in (int, float)
    if numeric:
        if not math.isfinite(want) or not math.isfinite(got):
            return path + ': nonfinite number'
        # Discrete fields remain exact when a floating-point tolerance is configured.
        if path.endswith(tuple('[' + json.dumps(key) + ']' for key in ('army', 'queue_length', 'type', 'target_id'))):
            equal = want == got
        else:
            equal = math.isclose(want, got, abs_tol=abs_tol, rel_tol=rel_tol)
    else:
        equal = type(want) is type(got) and want == got
    return '' if equal else f'{path}: original={want!r}, candidate={got!r}'


def leaves(value):
    return sum(leaves(v) for v in value.values()) if type(value) is dict else 1


def compare(oracle, actual, abs_tol=0, rel_tol=0):
    for value in (abs_tol, rel_tol):
        if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
            raise ValueError('tolerances must be finite nonnegative numbers')
    with Capture(oracle) as original, Capture(actual) as candidate:
        if original.header['role'] != 'original' or candidate.header['role'] != 'candidate':
            raise ValueError('comparison requires original and candidate capture roles')
        for key in ('case', 'collector', 'start_tick', 'end_tick', 'interval', 'session'):
            found = difference(original.header[key], candidate.header[key], key, 0, 0)
            if found:
                return Comparison(False, 0, 0, found)
        a, b = original.ticks(), candidate.ticks()
        count = checks = 0
        for record in a:
            other = next(b)
            found = difference(record['data'], other['data'], 'session', abs_tol, rel_tol)
            if found:
                return Comparison(False, count, checks, f'tick {record["tick"]}: {found}', record['tick'])
            count += 1
            checks += leaves(record['data'])
        try:
            next(b)
        except StopIteration:
            pass
        else:
            raise ValueError('candidate has unexpected ticks')
        if not checks:
            raise ValueError('capture contains no state checks')
        return Comparison(True, count, checks)
