<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Monitoring

A SynQt system is several processes on several machines, connected by links a browser
cannot see. When something goes wrong in one of them, the question is almost never "what
does this log file say" but "what did that click do". Monitoring answers that question.
One click becomes one trace, and the trace runs through every entity it touched.

It is off until you add a monitor. Nothing is recorded, nothing is stored, and the check
each instrumented call site pays is a single atomic read measured at 0.23 ns
([the baseline](https://github.com/Kidev/SynQt/blob/main/benchmarks/README.md)). You do
not rebuild to turn it on, and you do not rebuild to turn it up.

## Adding one

```cli
synqt add entity ops --type monitor
```

That writes four things, and it writes all four because any three of them leave something
that does not work or is not safe:

* the **monitor entity**, which keeps the history and serves the console on its own port;
* a **console client**, marked `console: true`, delivered only to an operator;
* the **sign-in gate**, a static page an anonymous visitor gets instead of the console;
* `monitoring.entity`, the one line that makes every service report.

You can also draw it. `synqt design` has a monitor on its palette; dropping one and
applying runs the same scaffolder, and the change set names all four files before anything
is written. The copy of the designer [on this site](visual-editor.md) shows the row and says
to use the command instead, because it has no scaffolder behind it and a monitor without its
console is not a monitor.

`monitoring.entity` is one line, which is what makes it worth checking: a `type: monitor`
entity nothing names still builds, starts and serves its console with an empty history, so
`synqt check` warns about one, and about a second monitor beside a wired one.

Then give yourself a way in:

```cli
synqt monitor operator add alice
```

It prints an entry for the monitor's `.env`. Credentials live in the entity's environment,
never in `synqt.yaml`, for the same reason every other credential in SynQt does.

With no operator configured the console refuses everybody, and it says so at startup, so
an empty console is not mistaken for a broken deployment.

## What is recorded

Every event is a record rather than a sentence. It carries when, which entity, how much it
matters, what it is about, the trace it belongs to, how long it took if it ended a span,
and a map of attributes. The message is the human half and carries no fact that is not also
an attribute, so filtering and searching are not a substring hunt.

Six categories, and the vocabulary is closed so that a filter list does not depend on what
happened to be logged today:

| Category | What lands in it |
| --- | --- |
| `lifecycle` | an entity or a link starting, stopping, reconnecting |
| `transport` | an upgrade, a mesh handshake, a socket closing |
| `authorization` | a refusal, a scope check, a session elevation |
| `call` | a slot crossing a link |
| `data` | a model publish, a property push, a provider query |
| `application` | whatever an entity's QML says through [`Log`](entities.md#log-in-every-entity-whatever-its-type) |

Six severities, `trace` through `fatal`, on the OpenTelemetry ladder.

## What is not recorded, and why

**No credential, ever.** A session is named in the record by a handle, half of its
SHA-256, never by the value a browser sends. An upgrade is recorded as a decision and a
reason, never as the request that carried it. A provider password, an OAuth token and an
`Authorization` header never reach the pipeline at all.

**No call arguments, unless a member asks.** A recorded call carries the shape of the call:
which member, whether a person or an entity called it, how many arguments there were, how
long it took, and which check refused it. It does not carry what the arguments were,
because those are what somebody typed. A member whose values are worth keeping says so with
[`capture`](programming-model.md#recording-a-calls-values-capture).

**No identity fields in a capture.** `synqt check` refuses `capture` on a member whose
arguments carry `sub`, `email` or `login`, directly or through a `record`, because that
turns the operations record into a second copy of the identity store, kept longer than a
session, read by people it is not about, and exported wherever an operator points their
collector. Say `monitoring: {capture_identity: acknowledged}` if that is what you want.

**Nothing a browser claimed.** A client never reports events. It cannot reach the mesh, and
a browser reporting as an entity would put a value a visitor controls where an
authenticated entity name belongs, which is the one conflation SynQt's
[two identity systems](security.md) exist to prevent. What a visitor does reaches the
record through the edge that served them, where it is a fact the edge observed.

## Turning it up

`monitoring.levels` sets the lowest severity each category records. It is read at startup
from the resolved topology, so turning `call` up during an incident is a configuration
change and a restart rather than a rebuild:

```yaml
monitoring:
  entity: ops
  levels:
    call: debug
    data: off
```

A category set to `off` records nothing at any severity. A category nobody names keeps its
default. `synqt check` refuses a category or a level this build does not know, because a
misspelling fails as an empty console, which reads as though there is nothing to find.

## The three tiers

**Hot, in RAM.** A bounded ring in every entity, drained by one writer thread that batches
on size or elapsed time. It never blocks the caller and it never grows. Past the bound the
oldest is dropped and counted, and the count is itself an event, so a gap is visible rather
than silent.

**Warm, on the monitor.** SQLite, with WAL, STRICT tables, one transaction per received
batch, indexes on time and on (entity, category, time), and FTS5 for searching messages and
attributes. This is what the console reads and what you have when nothing else is running.
It needs no second process to deploy or to back up.

Retention is not a cron job somebody has to remember. It is a call the monitor makes on a
timer, bounded by both age and total bytes:

```yaml
  - name: ops
    type: monitor
    retention:
      max_age_days: 14
      max_bytes: 536870912
```

**Cold, elsewhere, off by default.** See the next section.

## Exporting to what you already run

If your team already runs an OpenTelemetry collector, Grafana, Loki, Jaeger or a hosted
backend, SynQt sends the same events there and you keep the dashboards you have:

```yaml
  - name: ops
    type: monitor
    export:
      otlp:
        endpoint: http://127.0.0.1:4318
      jsonl:
        path: build/ops/state/events.jsonl
        max_bytes: 67108864
        keep: 5
```

`otlp` is OTLP over HTTP with JSON encoding, which every collector accepts and which needs
no protobuf dependency and no second licence to think about. `endpoint` is the collector's
base URL with no signal path on it. SynQt appends `/v1/logs` and `/v1/traces` itself. An
entity becomes an OpenTelemetry resource with a `service.name`, so it arrives as a service
without a mapping anybody had to write, and a call that closed a span arrives as a span
with its parent link intact.

`jsonl` writes one JSON object per line, which Promtail, Vector, Filebeat and Fluent Bit
all tail with no parser to write. It is capped and rotated, because a monitor that fills
the disk of the machine it is watching has become the outage.

If the collector needs an API key, it goes in the monitor's environment and there is no
configuration key to put it anywhere else:

```
SYNQT_MONITOR_OTLP_HEADERS=x-honeycomb-team: your-key-here
```

One `Key: value` per line.

Two properties hold whatever is at the other end. Exporting happens **after** the history is
written, so a collector that is down costs the monitor no record and no time. And nothing
queues without a bound: past `max_in_flight` requests a batch is dropped and counted,
because an exporter buffering in front of a collector that stopped answering is how a
monitoring tool takes the machine down with the thing it was watching.

## The console

The console is a separate client, built separately, delivered separately, and it reads a
contract whose every type is a string, a number or a bool. Nothing about it depends on the
topology it is watching. Adding an entity or renaming a connect point does not rebuild it,
because a new entity is another row on the ingest stream.

It shows what every entity is doing right now, how many events arrived and how many were
dropped, which entities are live and which have gone quiet, a filter by entity and
severity, a search across messages and attributes, and one trace end to end.

![The SynQt monitoring console: a counters strip, a tile per reporting entity, the filter
row, and the event table](assets/monitoring-console.png)

That is a real console rather than a drawing of one. Two entities are reporting: `ops`, the
monitor watching itself, and `web`, the edge reporting to it over the mesh. The tiles carry
each one's event count and liveness dot, the counters above them separate what arrived from
what was stored and what was dropped, and the table below is the record, with a timestamp, the
severity, which entity said it, the category, the message, how long the call took, and a
link that opens the whole trace it belonged to. It is taken by `tests/monitor-console`, on
a run that has just finished driving that console in a browser, so it is regenerated from a
passing suite rather than pasted in once and left to age.

Liveness is reported from the link itself. An entity that stops heartbeating is shown as down,
which is what makes a catastrophic failure of the main application show up as a red tile
rather than as silence.

One question returns at most two thousand rows, whatever it asked for. The console sends a
row count and the store decides what to honour, because every row is built in memory and
serialized back over the link, so an unbounded count is a question that materializes the
whole history at once. Two thousand is already far more than anyone reads down a screen.
When it is not enough, narrow the question rather than widening the answer.
Everything else on this path is bounded the same way: the ring, the
batch, the spool and the retention sweep all have a ceiling, and the one at the end had
none.

Severity and category cross the ingest link as numbers, and a number this build has no word
for is read as `info` and `lifecycle` rather than kept as itself. That matters for what an
operator can find. An event carrying an unknown category is written to the history and then
matches no category filter and no severity floor, so it is in the record and cannot be found
in it. An entity built against a later vocabulary reports as something readable instead.

## Reaching it

The monitor binds `127.0.0.1` by default and `synqt check` refuses any other host. Reaching
the console should mean reaching the machine first, through a VPN, an SSH tunnel, or being
on the host. The console shows every request the system has served and every refusal, behind one
password and no second factor, so it is not a thing to expose because somebody copied an
edge's `public:` block.

A deployment behind its own authenticating proxy is a real shape, and this framework does
not get to decide it is wrong. What it does get to do is make it explicit:

```yaml
monitoring:
  entity: ops
  public: acknowledged
```

Two gates apply either way. The bundle is delivered through
[`bundles:`](project-layout-and-config.md), so an anonymous caller is handed the sign-in
page and the console bundle is not addressable to them at all. It is a 404 rather than a 403.
Signing in raises that same session to the `operator` scope, which is what makes the
console fetchable and what gates its connect point.

That map is the whole delivery gate, so `synqt check` reads it rather than trusting it. A
`console: true` client mapped to any scope but `operator` is refused, on the monitor and on
an application edge alike, and so is a monitor whose default scope resolves to a client at
all. The second is what a monitor with no `bundles:` block falls back to, the project's
first client, which the generated main bakes in as what that port serves. `operator` is not in your project's
scope vocabulary, because an operator is not a user of your application and a scope that
meant both would make one login reach the other's surface.

## The identity it uses

The monitor has its own rather than the application's. Credentials are PBKDF2-SHA256
over at least 600,000 iterations, read from `SYNQT_MONITOR_OPERATORS` in the monitor's
environment. A credential derived with fewer iterations is refused at load rather than
accepted with a warning, one malformed entry does not lock everyone else out, and an empty
store refuses everybody rather than allowing all.

`synqt monitor operator add <name>` mints an entry. There is no `list` and no `remove`:
the list lives in the deployment's environment, and a CLI that edited that file would be a
CLI editing a running deployment's secrets.

The sign-in route is rationed per client address, ten attempts a minute, counted before the
password is read. A monitor reached through a proxy names it in `public.trusted_proxies`
like any other browser-facing entity, otherwise every operator arrives from the proxy and
shares one budget: ten wrong guesses from anywhere would answer `429` to all of them.

## When the monitor is down

Nothing stops. An entity whose monitor is unreachable keeps running with no degradation
other than a spool file: batches it could not hand over are written to a bounded file under
its own build directory and replayed when the monitor returns. Past the cap the oldest are
dropped, the newest kept, and the number dropped is published to the monitor when it comes
back, so the gap is visible rather than silent.

## Testing what an entity says

`Log.info("bid accepted", { amount: amount })` is a fact about how an entity behaves, so it
is testable like any other. The QML harness hands the events back; see
[asserting on what an entity said](testing.md#asserting-on-what-an-entity-said).

## Configuration reference

The `monitoring:` block, at the top level of `synqt.yaml`:

| Key | Meaning |
| --- | --- |
| `entity` | the name of the `type: monitor` entity every service reports to |
| `levels` | the lowest severity each category records, and a category may be `off` |
| `capture_identity` | `acknowledged` to allow `capture` on a member carrying an identity |
| `public` | `acknowledged` to allow the monitor to bind a non-loopback host |

On the monitor entity itself:

| Key | Meaning |
| --- | --- |
| `public` | `host` and `port` the console is served on, loopback by default |
| `retention` | `max_age_days` and `max_bytes`, both applied on a timer |
| `export` | `otlp` and `jsonl`, both off unless written |
| `bundles` | what each scope may download; written by the scaffold |

On the console client: `console: true`, and `edge:` naming the monitor.
