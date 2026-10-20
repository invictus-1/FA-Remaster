"""Versioned execution records and fail-closed validation."""
import json
import re

VERSION = 1
FIELDS = {'version', 'run', 'case', 'sequence', 'event', 'stage', 'data'}
SCHEMAS = {
    'host_start': {'kind': str},
    'stage_start': {'name': str, 'terminal': bool},
    'stage_end': {'status': str},
    'suite_start': {'suite': int, 'name': str},
    'assertion': {'suite': int, 'label': str, 'ok': bool},
    'cleanup': {'suite': int, 'label': str, 'ok': bool, 'reason': str},
    'suite_end': {'suite': int, 'status': str, 'reason': str},
    'suite_checkpoint': {'suite': int},
    'thread_start': {'thread': int},
    'thread_end': {'thread': int, 'status': str},
    'error': {'message': str},
    'case_complete': {'status': str},
    'host_exit': {'returncode': int, 'pending': int},
}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON field: {key}')
        result[key] = value
    return result


def read_events(path):
    def invalid_constant(value):
        raise ValueError(f'invalid JSON number: {value}')
    return [json.loads(line, object_pairs_hook=unique_object, parse_constant=invalid_constant)
            for line in path.read_text(encoding='utf-8').splitlines()]


def evaluate(text, returncode, case, events=None, run=None):
    checks = 0
    try:
        if not events:
            raise ValueError('missing structured execution record')
        if events[0].get('event') != 'host_start' or events[-1].get('event') != 'host_exit':
            raise ValueError('execution record must contain host start and exit')
        stages, suites, threads, used_threads, failures, skips = {}, {}, {}, set(), [], []
        completed = None
        plan = case.plan()
        position = 0
        run = run or events[0].get('run')
        if type(run) is not str or not run:
            raise ValueError('missing run identity')
        for sequence, record in enumerate(events, 1):
            if type(record) is not dict or set(record) != FIELDS:
                raise ValueError('invalid execution record fields')
            if type(record['version']) is not int or record['version'] != VERSION:
                raise ValueError('unsupported execution record version')
            if type(record['sequence']) is not int or record['sequence'] != sequence:
                raise ValueError('missing or duplicate event sequence')
            if record['run'] != run or record['case'] != case.name:
                raise ValueError('execution record belongs to another run or case')
            event, stage, data = record['event'], record['stage'], record['data']
            if type(event) is not str or event not in SCHEMAS or type(stage) is not int or stage < 0:
                raise ValueError('invalid event or stage identifier')
            schema = SCHEMAS[event]
            if type(data) is not dict or set(data) != set(schema) or any(type(data[k]) is not t for k, t in schema.items()):
                raise ValueError(f'invalid {event} data')
            if event == 'host_start':
                if sequence != 1 or stage or data['kind'] != case.kind:
                    raise ValueError('invalid or duplicate host start')
            elif event == 'host_exit':
                if sequence != len(events) or stage or data['returncode'] != returncode or data['pending'] or threads:
                    raise ValueError('invalid host exit or unfinished test threads')
            elif event == 'error':
                failures.append(data['message'])
            elif event == 'stage_start':
                if stage != len(stages) + 1 or completed is not None or position >= len(plan):
                    raise ValueError('unexpected or duplicate stage')
                if any(not entry['ended'] for entry in stages.values()):
                    raise ValueError('stage started before previous stage ended')
                expected = plan[position]
                if data != {'name': expected.name, 'terminal': expected.terminal}:
                    raise ValueError('executed stages do not match the test specification')
                stages[stage] = {'ended': False, 'terminal': data['terminal']}
            elif event == 'case_complete':
                if completed is not None or stage not in stages or not stages[stage]['ended'] or threads:
                    raise ValueError('invalid or duplicate case completion')
                if any(not entry['ended'] for entry in stages.values()) or (position != len(plan) and not skips):
                    raise ValueError('case completed with missing stages')
                if data['status'] not in ('passed', 'failed', 'skipped'):
                    raise ValueError('invalid case outcome')
                completed = data['status']
                if completed == 'failed':
                    failures.append('case failed')
                elif completed == 'skipped' and not skips:
                    raise ValueError('case skipped without a prerequisite reason')
            else:
                if stage not in stages or stages[stage]['ended']:
                    raise ValueError('event outside an active stage')
                if event == 'suite_start':
                    sid = data['suite']
                    if sid < 1 or sid in suites:
                        raise ValueError('duplicate suite identity')
                    suites[sid] = {'stage': stage, 'ended': False, 'checks': 0}
                elif event in ('assertion', 'cleanup', 'suite_end', 'suite_checkpoint'):
                    suite = suites.get(data['suite'])
                    if not suite or suite['stage'] != stage or suite['ended']:
                        raise ValueError('event outside an active suite')
                    if event in ('assertion', 'cleanup'):
                        if event == 'assertion':
                            suite['checks'] += 1
                            checks += data['ok']
                        if not data['ok']:
                            failures.append(data['label'] + (': ' + data['reason'] if event == 'cleanup' else ''))
                    else:
                        suite['ended'] = True
                        if event == 'suite_end':
                            if data['status'] not in ('passed', 'failed', 'skipped'):
                                raise ValueError('invalid suite outcome')
                            if data['status'] == 'passed' and not suite['checks']:
                                raise ValueError('suite completed without executing checks')
                            if data['status'] == 'failed':
                                failures.append('suite failed')
                            elif data['status'] == 'skipped':
                                skips.append(data['reason'])
                elif event == 'thread_start':
                    tid = data['thread']
                    if tid < 1 or tid in used_threads:
                        raise ValueError('duplicate thread identity')
                    used_threads.add(tid)
                    threads[tid] = stage
                elif event == 'thread_end':
                    if threads.pop(data['thread'], None) != stage:
                        raise ValueError('unknown test thread ended')
                    if data['status'] not in ('completed', 'cancelled', 'failed'):
                        raise ValueError('invalid thread outcome')
                    if data['status'] != 'completed':
                        failures.append('test thread did not complete')
                elif event == 'stage_end':
                    if any(s['stage'] == stage and not s['ended'] for s in suites.values()) or stage in threads.values():
                        raise ValueError('stage ended with unfinished work')
                    if data['status'] not in ('completed', 'checkpoint'):
                        raise ValueError('invalid stage outcome')
                    stages[stage]['ended'] = True
                    if data['status'] == 'completed':
                        position += 1
        if returncode != 0:
            return 'failed', f'host exited with status {returncode}' + ('; ' + '; '.join(failures[:5]) if failures else ''), checks
        if failures:
            return 'failed', '; '.join(failures[:5]), checks
        if case.kind == 'lua':
            if completed is None:
                raise ValueError('test exited without case completion')
            if completed == 'passed' and (not checks or skips):
                raise ValueError('case passed without checks or despite a skip')
        for pattern in case.required:
            if not re.search(pattern, text, re.M):
                return 'failed', 'missing required output: ' + pattern, checks
        if case.kind == 'command':
            checks = len(case.required)
        return ('skipped', '; '.join(skips), checks) if skips else ('passed', '', checks)
    except (ValueError, KeyError, TypeError, AttributeError) as error:
        return 'failed', 'invalid execution record: ' + str(error), checks
