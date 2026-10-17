# pg_hdr_histogram

[![CI](https://github.com/vovcke/pg_hdr_histogram/actions/workflows/ci.yml/badge.svg)](https://github.com/vovcke/pg_hdr_histogram/actions/workflows/ci.yml)
[![PostgreSQL 14-18](https://img.shields.io/badge/PostgreSQL-14--18-336791.svg)](https://www.postgresql.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A PostgreSQL extension for **bounded-error latency percentiles** using
[HdrHistogram](https://github.com/HdrHistogram/HdrHistogram), the
high-dynamic-range histogram library by Gil Tene.

It stores a **serialized** HDR histogram - a compact compressed blob, not a live
in-memory object - as a first-class type. On disk it's a few bytes; in SQL it
behaves like a live histogram you can merge and query percentiles on.

Author: Vladimir Pavliv.

Vendors [HdrHistogram_c](https://github.com/HdrHistogram/HdrHistogram_c), the C
port of HdrHistogram by Michael Barker - see [Credits](#credits).

## Why HDR histogram

- **Bounded error, not fixed buckets.** HDR gives ~0.1% relative precision over
  6 orders of magnitude. `p99.9` is a measurement, not an interpolation between
  buckets.
- **Lossless merge.** Histograms merge with `hdr_merge` - aggregate across time
  windows, hosts, or pre-aggregated frames without losing the distribution.
- **No lock-in.** Histograms are ordinary rows in a Postgres table, queryable
  with plain SQL.

## Install

Supports PostgreSQL 14–18 (the CI matrix). Requires the PostgreSQL server dev
package, `zlib`, and a C compiler.

### Prerequisites (Ubuntu / Debian)

```bash
# find your PostgreSQL major version first:
pg_config --version                 # e.g. "PostgreSQL 18.6"

sudo apt install postgresql-server-dev-18 build-essential zlib1g-dev
```

Use your own major version in `postgresql-server-dev-<major>` (e.g. `-16` for
PG 16). On Red Hat-family systems the equivalents are `postgresql-server-devel`,
`gcc`, `make` and `zlib-devel`.

### Build & install

```bash
make
sudo make install
```

### Enable

```sql
CREATE EXTENSION hdr_histogram;
```

(The project is `pg_hdr_histogram`; the extension is installed as
`hdr_histogram`.)

Check that it works:

```sql
SELECT hdr_percentile(hdr_add(1.0), 50);
-- 1
```

### Tests

Regression tests (requires the extension installed and a running server):

```bash
make installcheck
```

Benchmark (size + throughput, see [Size & performance](#size--performance)):

```bash
createdb bench
psql bench -c 'CREATE EXTENSION hdr_histogram;'
psql bench -f bench/benchmark.sql
```

### Uninstall

```sql
DROP EXTENSION hdr_histogram;
```

To also remove the files:

```bash
sudo rm "$(pg_config --pkglibdir)/hdr_histogram.so" \
        "$(pg_config --sharedir)/extension/hdr_histogram.control" \
        "$(pg_config --sharedir)/extension/hdr_histogram--1.0.sql"
```

## Usage

### Build a histogram from raw values

```sql
SELECT hdr_percentile(hdr_add(latency_ns), 50)   AS p50,
       hdr_percentile(hdr_add(latency_ns), 99)   AS p99,
       hdr_percentile(hdr_add(latency_ns), 99.9) AS p999
FROM requests;
```

`hdr_add(double precision)` records each value (converted to `int64`, so pass
latency in a common unit like ns) into a histogram. Default range is `0 .. 1e9`
with 3 significant figures (~0.1% precision); tune them per session:

```sql
SET hdr_histogram.highest = 1e12;   -- track up to ~16 minutes of ns
SET hdr_histogram.sig_figs = 4;     -- ~0.01% precision
```

Values outside the range, or non-finite values (`NaN`, `±Inf`), raise an error.

### Store & merge serialized histograms

The `hdr_histogram` type holds a serialized HDR blob, so anything that produces
an HdrHistogram - your application code, a telemetry agent, another HdrHistogram
port - can write one straight into a column. No Postgres-specific format:

```sql
CREATE TABLE rps_histograms (
    metric_name text,
    ts          timestamptz,
    hist        hdr_histogram
);
CREATE INDEX ON rps_histograms (metric_name, ts);

-- "give me p42 of rps between 14:30 and 14:35":
SELECT hdr_percentile(hdr_merge(hist), 42)
FROM rps_histograms
WHERE metric_name = 'rps' AND ts BETWEEN '14:30' AND '14:35';
```

Any period is just a merge over a time range - per-frame histograms merge
losslessly into whatever window you want.

### Merge policy

When `hdr_merge` combines histograms with **different parameters** (range or
precision), the `hdr_histogram.merge_mode` GUC decides what happens:

| Mode | Behaviour |
|------|-----------|
| `lossless` (default) | merge only when it can be done without loss; otherwise error |
| `strict` | require the parameters to match exactly; otherwise error |
| `left` | keep the accumulator's parameters and silently drop values that don't fit |
| `union` | rebuild into the widest range / finest precision, so nothing is ever dropped |

```sql
SET hdr_histogram.merge_mode = 'union';

SELECT hdr_percentile(hdr_merge(hist), 99.9)
FROM rps_histograms
WHERE metric_name = 'rps' AND ts BETWEEN '14:30' AND '14:35';
```

The setting is per-session. Scope it to a transaction with `SET LOCAL`, or set a
default per database / role:

```sql
SET LOCAL hdr_histogram.merge_mode = 'union';
ALTER DATABASE mydb  SET hdr_histogram.merge_mode = 'union';
ALTER ROLE  myuser SET hdr_histogram.merge_mode = 'union';
```

On managed providers without `ALTER DATABASE` access, pass it in the connection
string:

```
options='-c hdr_histogram.merge_mode=union'
```

> The GUC is registered when the extension's shared library is loaded - on the
> first call to any `hdr_*` function, or after `LOAD 'hdr_histogram'`.
> `ALTER DATABASE`/`ALTER ROLE` settings are applied at session start, *before*
> the library loads, so they would be silently ignored. To make database/role
> defaults work, preload the library at session start:
>
> ```
> # postgresql.conf
> session_preload_libraries = 'hdr_histogram'
> ```
>
> A plain `SET` mid-session needs no preloading.

### Scalar functions

| Function | Returns |
|----------|---------|
| `hdr_percentile(h, p)` | `p`-th percentile (0..100) |
| `hdr_min(h)` / `hdr_max(h)` | min / max recorded value |
| `hdr_mean(h)` | arithmetic mean |
| `hdr_stddev(h)` | standard deviation |
| `hdr_total_count(h)` | number of recorded values |
| `hdr_sig_figs(h)` / `hdr_lowest(h)` / `hdr_highest(h)` | histogram parameters |

## Size & performance

Measured on a dev machine (PostgreSQL 18, single backend) - treat as ballpark,
your numbers will vary. Reproduce with:

```bash
createdb bench
psql bench -c 'CREATE EXTENSION hdr_histogram;'
psql bench -f bench/benchmark.sql
```

**A histogram is small, and its size depends on the *spread* of values, not on
how many there are** - the blob stores bucket counts, not the values:

| Data | Compressed blob size |
|------|----------------------|
| 1 value | ~42 B |
| 10 M values in a tight cluster | ~48 B |
| values spanning the full `1..1e9` range (uniform) | ~3.5 KB |
| skewed, long-tailed latency (log-uniform) | ~12 KB |

**Throughput:**

- `hdr_add` (build a histogram from raw values): ~9 M values/s.
- `hdr_merge` (merge stored histograms):
  - tiny (1-value) frames: ~90 µs single-threaded;
  - realistic (~1000-value) frames: ~110 µs;
  - parallel (2 workers + leader): roughly **3× faster**.
  The cost scales with blob size - each merge decompresses and recompresses.

## Design notes

- **Type** `hdr_histogram` is a varlena holding the compressed HDR blob
  (`hdr_encode_compressed`).
- **Aggregates** keep a live `struct hdr_histogram *` as their transition state
  (`internal`), so the blob is serialized only once, in the final function - not
  on every row.
- **Parallel-safe.** Both aggregates provide `COMBINEFUNC` + `SERIALFUNC` /
  `DESERIALFUNC`, so PostgreSQL can parallelize `GROUP BY` across workers and
  merge partial histograms losslessly.
- **`trusted = true`** in the control file: installable by non-superusers, which
  managed providers require.
- **Input is validated.** `hdr_histogram_in` / `hdr_histogram_recv` decode and
  discard the blob, so malformed input is rejected up front (with the proper
  `invalid_text_representation` / `invalid_binary_representation` SQLSTATEs)
  rather than stored as a corrupt value that fails later. The blob header
  (payload length and histogram parameters) is pre-checked so a crafted payload
  can't make the decoder allocate unbounded memory or overflow its bucket-config
  math; the decode path is also fuzzed (see `fuzz/`).
- Memory: `HdrHistogram_c`'s allocator is redirected to `palloc`/`pfree`, so
  all histogram memory lives in PostgreSQL memory contexts and is reclaimed
  automatically - including on mid-query errors.

## Related work

Percentiles from mergeable sketches are a well-trodden problem; other PostgreSQL
extensions solve it with different algorithms:

- [`tdigest`](https://github.com/tvondra/tdigest) - t-digest. Accurate on skewed
  distributions, but with no fixed relative-error guarantee, and merges are
  approximate.
- [`ddsketch`](https://github.com/tvondra/ddsketch) - DDSketch (Datadog's
  sketch), a log-bucketed sketch with a relative-error guarantee. Conceptually
  the closest to HDR, but a different format and API.
- [`query_histogram`](https://github.com/tvondra/query_histogram) - histograms
  of query *durations* for monitoring, not a general-purpose histogram type.

`hdr_histogram` differs by storing a full HDR histogram as a first-class type,
so histograms can be persisted, indexed and merged losslessly across time and
hosts, rather than only estimated in memory.

## Credits

The histogram implementation is entirely
[HdrHistogram_c](https://github.com/HdrHistogram/HdrHistogram_c) (vendored under
`third_party/`, version 0.11.10) - the C port, primarily by Michael Barker, of
Gil Tene's [HdrHistogram](https://github.com/HdrHistogram/HdrHistogram).  The
algorithm and its implementation are their work; this extension is a thin
adapter that packages it as a PostgreSQL type, aggregates and functions.  The
library is public domain (CC0); see `third_party/HdrHistogram_c/NOTICE` and the
per-file headers for the full author notices.
