-- hdr_histogram extension: exact-latency percentiles via HDR histograms.

-- The type is a varlena holding a compressed HDR histogram blob. Normally it is
-- produced by the hdr_add / hdr_merge aggregates, not built by hand.

-- Shell type first, so the I/O functions below can reference the type in their
-- signatures; the full definition (with those functions) comes after.

CREATE TYPE hdr_histogram;

CREATE FUNCTION hdr_histogram_in(cstring)
RETURNS hdr_histogram
AS 'MODULE_PATHNAME', 'hdr_histogram_in'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_histogram_out(hdr_histogram)
RETURNS cstring
AS 'MODULE_PATHNAME', 'hdr_histogram_out'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_histogram_recv(internal)
RETURNS hdr_histogram
AS 'MODULE_PATHNAME', 'hdr_histogram_recv'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_histogram_send(hdr_histogram)
RETURNS bytea
AS 'MODULE_PATHNAME', 'hdr_histogram_send'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE TYPE hdr_histogram (
    INPUT          = hdr_histogram_in,
    OUTPUT         = hdr_histogram_out,
    RECEIVE        = hdr_histogram_recv,
    SEND           = hdr_histogram_send,
    INTERNALLENGTH = VARIABLE,
    ALIGNMENT      = int4,
    STORAGE        = extended
);

-- Scalar functions: histogram -> one number.

CREATE FUNCTION hdr_percentile(hdr_histogram, double precision)
RETURNS double precision
AS 'MODULE_PATHNAME', 'hdr_percentile'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_min(hdr_histogram)
RETURNS double precision
AS 'MODULE_PATHNAME', 'hdr_min_pg'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_max(hdr_histogram)
RETURNS double precision
AS 'MODULE_PATHNAME', 'hdr_max_pg'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_mean(hdr_histogram)
RETURNS double precision
AS 'MODULE_PATHNAME', 'hdr_mean_pg'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_stddev(hdr_histogram)
RETURNS double precision
AS 'MODULE_PATHNAME', 'hdr_stddev_pg'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_total_count(hdr_histogram)
RETURNS bigint
AS 'MODULE_PATHNAME', 'hdr_total_count'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_sig_figs(hdr_histogram)
RETURNS integer
AS 'MODULE_PATHNAME', 'hdr_sig_figs'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_lowest(hdr_histogram)
RETURNS double precision
AS 'MODULE_PATHNAME', 'hdr_lowest'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION hdr_highest(hdr_histogram)
RETURNS double precision
AS 'MODULE_PATHNAME', 'hdr_highest'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Aggregate: build a histogram from a column of scalar values.
--
--     SELECT hdr_percentile(hdr_add(latency_ns), 99.9) FROM requests;

CREATE FUNCTION hdr_add_transition(internal, double precision)
RETURNS internal
AS 'MODULE_PATHNAME', 'hdr_add_transition'
LANGUAGE C;

CREATE FUNCTION hdr_agg_final(internal)
RETURNS hdr_histogram
AS 'MODULE_PATHNAME', 'hdr_agg_final'
LANGUAGE C;

CREATE FUNCTION hdr_merge_combine(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'hdr_merge_combine'
LANGUAGE C;

CREATE FUNCTION hdr_agg_serialize(internal)
RETURNS bytea
AS 'MODULE_PATHNAME', 'hdr_agg_serialize'
LANGUAGE C STRICT;

-- NOTE: not STRICT - the deserialfunc's second "internal" argument is a NULL
-- dummy, and STRICT would short-circuit and skip deserialization.
CREATE FUNCTION hdr_agg_deserialize(bytea, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'hdr_agg_deserialize'
LANGUAGE C;

CREATE AGGREGATE hdr_add(double precision) (
    SFUNC        = hdr_add_transition,
    STYPE        = internal,
    COMBINEFUNC  = hdr_merge_combine,
    FINALFUNC    = hdr_agg_final,
    SERIALFUNC   = hdr_agg_serialize,
    DESERIALFUNC = hdr_agg_deserialize,
    PARALLEL     = SAFE
);

-- Aggregate: losslessly merge stored histograms (e.g. per-frame blobs).
--
--     SELECT hdr_percentile(hdr_merge(hist), 99.9)
--     FROM rps_histograms WHERE ts BETWEEN '14:30' AND '14:35';

CREATE FUNCTION hdr_merge_transition(internal, hdr_histogram)
RETURNS internal
AS 'MODULE_PATHNAME', 'hdr_merge_transition'
LANGUAGE C;

CREATE AGGREGATE hdr_merge(hdr_histogram) (
    SFUNC        = hdr_merge_transition,
    STYPE        = internal,
    COMBINEFUNC  = hdr_merge_combine,
    FINALFUNC    = hdr_agg_final,
    SERIALFUNC   = hdr_agg_serialize,
    DESERIALFUNC = hdr_agg_deserialize,
    PARALLEL     = SAFE
);
