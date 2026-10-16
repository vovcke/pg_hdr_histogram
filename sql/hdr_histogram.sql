CREATE EXTENSION hdr_histogram;

-- A known dataset spanning several orders of magnitude.
CREATE TABLE vals (v double precision);
INSERT INTO vals VALUES
    (1), (2), (3), (5), (10), (20), (50),
    (100), (200), (500), (1000), (2000), (5000),
    (10000), (20000), (50000), (100000), (1000000), (100000000);

-- Default histogram parameters used by hdr_add(value).
SELECT hdr_sig_figs(h) AS sig_figs,
       hdr_lowest(h)   AS lowest,
       hdr_highest(h)  AS highest
FROM (SELECT hdr_add(v) AS h FROM vals) s;

-- Summary statistics over the whole dataset.
SELECT hdr_total_count(h) AS count,
       hdr_min(h)         AS min,
       hdr_max(h)         AS max,
       hdr_mean(h)        AS mean,
       hdr_stddev(h)      AS stddev
FROM (SELECT hdr_add(v) AS h FROM vals) s;

-- Exact percentiles.
SELECT hdr_percentile(h, 0)   AS p0,
       hdr_percentile(h, 25)  AS p25,
       hdr_percentile(h, 50)  AS p50,
       hdr_percentile(h, 90)  AS p90,
       hdr_percentile(h, 99)  AS p99,
       hdr_percentile(h, 100) AS p100
FROM (SELECT hdr_add(v) AS h FROM vals) s;

-- NULLs are skipped by the aggregate.
SELECT hdr_total_count(hdr_add(v)) AS count
FROM (VALUES (1.0::float8), (NULL::float8), (2.0::float8), (3.0::float8)) AS t(v);

-- An aggregate over zero rows is NULL.
SELECT (SELECT hdr_add(v) FROM (SELECT 1.0::float8 AS v WHERE false) s) IS NULL
    AS empty_is_null;

-- hdr_merge: lossless merge of pre-built histograms.
CREATE TABLE hist (grp int, h hdr_histogram);
INSERT INTO hist SELECT 1, hdr_add(v) FROM vals WHERE v <= 1000;
INSERT INTO hist SELECT 1, hdr_add(v) FROM vals WHERE v > 1000;

SELECT hdr_total_count(hdr_merge(h))    AS count,
       hdr_min(hdr_merge(h))            AS min,
       hdr_max(hdr_merge(h))            AS max,
       hdr_percentile(hdr_merge(h), 50) AS p50
FROM hist;

-- Text round-trip via the type's I/O functions.
SELECT hdr_total_count((hdr_histogram_out(h)::text)::hdr_histogram) AS rt_count
FROM (SELECT hdr_add(v) AS h FROM vals) s;

-- Out-of-range and non-finite values are rejected.
SELECT hdr_total_count(hdr_add(-1.0::float8));
SELECT hdr_total_count(hdr_add(1e10::float8));
SELECT hdr_total_count(hdr_add('NaN'::float8));

-- A leading NULL is skipped without crashing.
SELECT hdr_total_count(hdr_add(v)) AS count
FROM (VALUES (NULL::float8), (1.0::float8), (2.0::float8)) AS t(v);

-- An all-NULL aggregate is NULL.
SELECT (SELECT hdr_add(v) FROM (VALUES (NULL::float8), (NULL::float8)) AS t(v))
    IS NULL AS all_null_is_null;

-- Percentile argument validation: NaN and out-of-range are rejected.
SELECT hdr_percentile(hdr_add(1.0::float8), 'NaN'::float8);
SELECT hdr_percentile(hdr_add(1.0::float8), -1.0::float8);
SELECT hdr_percentile(hdr_add(1.0::float8), 100.1::float8);

-- Merge policy (hdr_histogram.merge_mode).  Two histograms with different
-- ranges: A = 1..1e6 (value 100), B = 1..1e12 (value 5e11).
SET hdr_histogram.merge_mode = 'strict';
SELECT hdr_total_count(hdr_merge(h))
FROM (VALUES ('1c84931400000021789c9369992cccc0c0c0cc0001309a114cf23b39d87f80081c676402004b7803d8'::hdr_histogram),
             ('1c84931400000025789c9369992cccc0c0c0c20001cc509a11885f5c592ac060ff0122d07d979909007093065c'::hdr_histogram)) t(h);

SET hdr_histogram.merge_mode = 'lossless';
SELECT hdr_total_count(hdr_merge(h)) AS count,
       hdr_highest(hdr_merge(h))   AS highest
FROM (VALUES ('1c84931400000025789c9369992cccc0c0c0c20001cc509a11885f5c592ac060ff0122d07d979909007093065c'::hdr_histogram),
             ('1c84931400000021789c9369992cccc0c0c0cc0001309a114cf23b39d87f80081c676402004b7803d8'::hdr_histogram)) t(h);

SET hdr_histogram.merge_mode = 'left';
SELECT hdr_total_count(hdr_merge(h)) AS count,
       hdr_highest(hdr_merge(h))   AS highest
FROM (VALUES ('1c84931400000021789c9369992cccc0c0c0cc0001309a114cf23b39d87f80081c676402004b7803d8'::hdr_histogram),
             ('1c84931400000025789c9369992cccc0c0c0c20001cc509a11885f5c592ac060ff0122d07d979909007093065c'::hdr_histogram)) t(h);

SET hdr_histogram.merge_mode = 'union';
SELECT hdr_total_count(hdr_merge(h)) AS count,
       hdr_highest(hdr_merge(h))   AS highest
FROM (VALUES ('1c84931400000021789c9369992cccc0c0c0cc0001309a114cf23b39d87f80081c676402004b7803d8'::hdr_histogram),
             ('1c84931400000025789c9369992cccc0c0c0c20001cc509a11885f5c592ac060ff0122d07d979909007093065c'::hdr_histogram)) t(h);

SET hdr_histogram.merge_mode = 'lossless';
