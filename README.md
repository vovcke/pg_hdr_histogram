# hdr_histogram

A PostgreSQL extension for **bounded-error latency percentiles** using HDR histograms.

Stores a full HDR histogram (`HdrHistogram_c`) as a first-class type and gives
you aggregates + scalar functions to build, merge and query **bounded-error**
percentiles (p50 / p99 / p99.9 / …) with **lossless merge** — unlike
Prometheus/Datadog, which approximate percentiles from fixed buckets.

Author: Vladimir Pavliv.

## Why

- **Bounded error, not fixed buckets.** HDR gives ~0.1% relative precision over
  6 orders of magnitude. `p99.9` is a measurement, not an interpolation between
  buckets.
- **Lossless merge.** Histograms merge with `hdr_merge` — you can aggregate across
  time windows, hosts, or pre-aggregated frames without losing the distribution.
- **No lock-in.** Your data lives in ordinary Postgres, queryable with plain SQL.

## Install

Requires `postgresql-server-dev` (for `pg_config`) and `zlib`.

```bash
make
sudo make install
```

Then in psql:

```sql
CREATE EXTENSION hdr_histogram;
```

## Usage

### Mode A — build a histogram from a column of raw values

```sql
SELECT hdr_percentile(hdr_add(latency_ns), 50)  AS p50,
       hdr_percentile(hdr_add(latency_ns), 99)  AS p99,
       hdr_percentile(hdr_add(latency_ns), 99.9) AS p999
FROM requests;
```

`hdr_add(double precision)` records each value (converted to `int64`, so pass
latency in a common unit like ns) into a histogram. Default range is `0 .. 1e9`
with 3 significant figures (~0.1% precision). Values outside that range, or
non-finite values (`NaN`, `±Inf`), raise an error.

### Mode B — merge already-stored histograms (the nanolens flow)

```sql
-- nanolens writes one compressed HDR histogram per frame (16 ms); store it in a
-- column of type hdr_histogram, with metadata in ordinary columns:
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

Any period is just a merge over a time range — per-frame histograms merge
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

> The GUC is registered when the extension's shared library is loaded — on the
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

## Design notes

- **Type** `hdr_histogram` is a varlena holding the compressed HDR blob
  (`hdr_encode_compressed`).
- **Aggregates** keep a live `struct hdr_histogram *` as their transition state
  (`internal`), so the blob is serialized only once, in the final function — not
  on every row.
- **Parallel-safe.** Both aggregates provide `COMBINEFUNC` + `SERIALFUNC` /
  `DESERIALFUNC`, so PostgreSQL can parallelize `GROUP BY` across workers and
  merge partial histograms losslessly.
- **`trusted = true`** in the control file: installable by non-superusers, which
  managed providers require.
- Memory: `HdrHistogram_c`'s allocator is redirected to `palloc`/`pfree`, so
  all histogram memory lives in PostgreSQL memory contexts and is reclaimed
  automatically - including on mid-query errors.

## Roadmap

- Configurable histogram parameters per `hdr_add` (lowest/highest/sig-figs).
- A `hdr_percentile_agg(value, p)` aggregate that returns the percentile directly
  (avoids materializing the histogram when you only need one number).
- OTel Exponential Histogram import/export.
