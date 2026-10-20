# Test execution protocol, version 1

Each case runs in a dedicated test host and writes UTF-8 JSON objects, one per line, to
`events.jsonl`. The host flushes every event. The runner treats missing, malformed, truncated,
duplicated or inconsistent records as failures. Human logs remain diagnostic artifacts.

Every event has exactly these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | integer | Protocol version, currently 1 |
| `run` | string | Unique runner invocation identity |
| `case` | string | Registered case name |
| `sequence` | integer | Contiguous event number, starting at 1 |
| `event` | string | Event type below |
| `stage` | integer | Stage identity; 0 for host-wide diagnostics |
| `data` | object | Fields prescribed by the event type |

Duplicate JSON keys and nonfinite numbers are invalid. Identities must match the current
invocation; results from another case or an earlier invocation cannot satisfy completion.

## Events

| Event | Data fields |
| --- | --- |
| `host_start` | `kind`: `lua` or `command` |
| `stage_start` | `name`: string, `terminal`: boolean |
| `suite_start` | `suite`: integer, `name`: string |
| `assertion` | `suite`: integer, `label`: string, `ok`: boolean |
| `cleanup` | `suite`: integer, `label`: string, `ok`: boolean, `reason`: string |
| `suite_end` | `suite`: integer, `status`: `passed`, `failed` or `skipped`, `reason`: string |
| `suite_checkpoint` | `suite`: integer |
| `thread_start` | `thread`: integer |
| `thread_end` | `thread`: integer, `status`: `completed`, `cancelled` or `failed` |
| `stage_end` | `status`: `completed` or `checkpoint` |
| `case_complete` | `status`: `passed`, `failed` or `skipped` |
| `error` | `message`: string |
| `host_exit` | `returncode`: integer, `pending`: integer |

Suite and thread identities are unique within an invocation. Their events must belong to the
stage that created them. A stage cannot end with active suites or threads. Stage names and
terminal flags must match the case specification in order. A checkpoint repeats a terminal
stage after an application state handoff; completion advances to the next specified stage.

The first event is `host_start`; the last is `host_exit`. Lua cases require exactly one
`case_complete`, after their final stage ends. Successful suites must have executed assertions;
cleanup does not substitute for an assertion. A skip carries a prerequisite reason and still
runs cleanup. Command cases declare required output patterns, have no Lua stage lifecycle,
and require a clean host exit and successful command checks.

Any failed assertion, failed cleanup, cancelled or failed test thread, engine error or Lua
exception fails the case. An error after `case_complete` still fails it. The runner also verifies
the process exit code and requested captures before accepting a pass. JSON and JUnit summaries
are generated from these validated outcomes.
