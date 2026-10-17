-- Benchmark for pg_hdr_histogram.
--
-- Run against a scratch database with the extension installed:
--     createdb bench
--     psql bench -c 'CREATE EXTENSION hdr_histogram;'
--     psql bench -X -v ON_ERROR_STOP=1 -f bench/benchmark.sql

\echo === Blob size (compressed bytes) ===

SELECT '1 value' AS case,
       octet_length(hdr_histogram_send(hdr_add(1000.0::float8))) AS bytes;

SELECT '10M values, tight cluster' AS case,
       octet_length(hdr_histogram_send(hdr_add(v))) AS bytes
FROM (SELECT (1000000 + g % 100)::float8 AS v
      FROM generate_series(1, 10000000) g) t;

SELECT 'full 1..1e9 range' AS case,
       octet_length(hdr_histogram_send(hdr_add(v))) AS bytes
FROM (SELECT ((g::bigint * 1103515245 + 12345) % 1000000000)::float8 AS v
      FROM generate_series(1, 1000000) g) t;

SELECT 'skewed (long-tailed) latency' AS case,
       octet_length(hdr_histogram_send(hdr_add(v))) AS bytes
FROM (SELECT round(exp((((g::bigint * 1103515245 + 12345) % 1000000000)::float8 / 1000000000.0) * ln(1000000000.0)))::float8 AS v
      FROM generate_series(1, 1000000) g) t;

\echo === Build throughput (hdr_add), 10M uniform values ===
\timing on
SELECT hdr_total_count(hdr_add(v))
FROM (SELECT ((g::bigint * 1103515245 + 12345) % 1000000000)::float8 AS v
      FROM generate_series(1, 10000000) g) t;
\timing off

\echo === Build throughput (hdr_add), 10M skewed values ===
\timing on
SELECT hdr_total_count(hdr_add(v))
FROM (SELECT round(exp((((g::bigint * 1103515245 + 12345) % 1000000000)::float8 / 1000000000.0) * ln(1000000000.0)))::float8 AS v
      FROM generate_series(1, 10000000) g) t;
\timing off

\echo === Merge throughput (hdr_merge), 100k frames (single-threaded) ===
DROP TABLE IF EXISTS bench_frames;
CREATE TABLE bench_frames AS
SELECT g AS id,
       hdr_add(((g::bigint * 1103515245 + 12345) % 1000000000)::float8) AS h
FROM generate_series(1, 100000) g
GROUP BY g;

\timing on
SELECT hdr_total_count(hdr_merge(h)) FROM bench_frames;
\timing off

DROP TABLE bench_frames;

\echo === Merge throughput (hdr_merge), 1M frames (parallel plan) ===
DROP TABLE IF EXISTS bench_frames_big;
CREATE TABLE bench_frames_big AS
SELECT g AS id,
       hdr_add(((g::bigint * 1103515245 + 12345) % 1000000000)::float8) AS h
FROM generate_series(1, 1000000) g
GROUP BY g;

\timing on
SELECT hdr_total_count(hdr_merge(h)) FROM bench_frames_big;
\timing off

DROP TABLE bench_frames_big;

\echo === Merge throughput (hdr_merge), 10k frames x 1000 values (realistic) ===
DROP TABLE IF EXISTS bench_frames_real;
CREATE TABLE bench_frames_real AS
SELECT g / 1000 AS id,
       hdr_add(round(exp((((g::bigint * 1103515245 + 12345) % 1000000000)::float8 / 1000000000.0) * ln(1000000000.0)))::float8) AS h
FROM generate_series(1, 10000000) g
GROUP BY g / 1000;

\timing on
SELECT hdr_total_count(hdr_merge(h)) FROM bench_frames_real;
\timing off

DROP TABLE bench_frames_real;
