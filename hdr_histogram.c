/*
 * hdr_histogram - a PostgreSQL extension for exact-latency percentiles.
 *
 * Stores a full HDR histogram (HdrHistogram_c) as a first-class type and
 * provides aggregates + scalar functions to build, merge and query exact
 * percentiles (p50 / p99 / p99.9 / ...) with lossless merge.
 *
 * Author: Vladimir Pavliv
 *
 * The heavy lifting (HDR histogram) is HdrHistogram_c, vendored under
 * third_party/. This file is the thin PostgreSQL adapter: Datum / varlena /
 * aggregate-state glue.
 */

#include "postgres.h"

#include "fmgr.h"
#include "libpq/pqformat.h"
#include "varatt.h"

#include <stdint.h>
#include <string.h>

#include <hdr/hdr_histogram.h>

PG_MODULE_MAGIC;

/*
 * hdr_encode_compressed / hdr_decode_compressed are implemented in
 * third_party/HdrHistogram_c/src/hdr_histogram_log.c but are NOT declared in
 * the public header (they live in the test header). Declare them here, same as
 * the nanolens core does in HdrCompressed.hpp.
 *
 * encode: allocates *compressed_histogram (caller free()s).
 * decode: allocates *histogram (caller hdr_close()s).
 */
int hdr_encode_compressed(struct hdr_histogram *h, uint8_t **compressed_histogram, size_t *compressed_len);
int hdr_decode_compressed(uint8_t *buffer, size_t length, struct hdr_histogram **histogram);

/* Default histogram parameters for hdr_add(value): 1..1e9 with 3 sig figs,
 * i.e. ~0.1% relative precision. Covers 1 ns .. 1 s latency. */
#define HDR_DEFAULT_LOWEST  1
#define HDR_DEFAULT_HIGHEST 1000000000LL
#define HDR_DEFAULT_SIGFIGS 3

/* The type is a varlena: [4-byte length][compressed HDR blob]. */
typedef struct varlena hdr_histogram;

/* ---------------------------------------------------------------------------
 * Hex codec (self-contained, so we don't depend on the exact header that
 * exposes Postgres's hex_encode/hex_decode across versions).
 * ------------------------------------------------------------------------ */

static int
hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decode a hex string ("deadbeef") into a palloc'd buffer; *outlen = bytes. */
static char *
hex_to_bytes(const char *hex, size_t hexlen, size_t *outlen)
{
    size_t      n = hexlen / 2;
    char       *out;

    if (hexlen % 2 != 0)
        elog(ERROR, "hdr_histogram: odd-length hex input");

    out = palloc(n);
    for (size_t i = 0; i < n; i++)
    {
        int         hi = hex_val(hex[i * 2]);
        int         lo = hex_val(hex[i * 2 + 1]);

        if (hi < 0 || lo < 0)
            elog(ERROR, "hdr_histogram: invalid hex input");
        out[i] = (char) ((hi << 4) | lo);
    }
    *outlen = n;
    return out;
}

/* Encode len bytes into a palloc'd hex string. */
static char *
bytes_to_hex(const char *src, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    char       *out = palloc(len * 2 + 1);

    for (size_t i = 0; i < len; i++)
    {
        out[i * 2] = digits[(unsigned char) src[i] >> 4];
        out[i * 2 + 1] = digits[(unsigned char) src[i] & 0x0F];
    }
    out[len * 2] = '\0';
    return out;
}

/* ---------------------------------------------------------------------------
 * Type I/O. The text/binary representation is the compressed HDR blob as hex,
 * round-trippable but rarely constructed by hand (normally the aggregate
 * builds it).
 * ------------------------------------------------------------------------ */

PG_FUNCTION_INFO_V1(hdr_histogram_in);
Datum
hdr_histogram_in(PG_FUNCTION_ARGS)
{
    char       *str = PG_GETARG_CSTRING(0);
    size_t      decoded_len = 0;
    char       *decoded = hex_to_bytes(str, strlen(str), &decoded_len);
    hdr_histogram *result;

    result = (hdr_histogram *) palloc(VARHDRSZ + decoded_len);
    SET_VARSIZE(result, VARHDRSZ + decoded_len);
    memcpy(VARDATA(result), decoded, decoded_len);
    pfree(decoded);

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(hdr_histogram_out);
Datum
hdr_histogram_out(PG_FUNCTION_ARGS)
{
    hdr_histogram *v = (hdr_histogram *) PG_GETARG_VARLENA_P(0);
    char       *hex = bytes_to_hex(VARDATA(v), VARSIZE(v) - VARHDRSZ);

    PG_RETURN_CSTRING(hex);
}

PG_FUNCTION_INFO_V1(hdr_histogram_recv);
Datum
hdr_histogram_recv(PG_FUNCTION_ARGS)
{
    StringInfo  buf = (StringInfo) PG_GETARG_POINTER(0);
    int         nbytes = buf->len - buf->cursor;
    hdr_histogram *result = (hdr_histogram *) palloc(VARHDRSZ + nbytes);

    SET_VARSIZE(result, VARHDRSZ + nbytes);
    memcpy(VARDATA(result), pq_getmsgbytes(buf, nbytes), nbytes);

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(hdr_histogram_send);
Datum
hdr_histogram_send(PG_FUNCTION_ARGS)
{
    hdr_histogram *v = (hdr_histogram *) PG_GETARG_VARLENA_P(0);
    StringInfoData buf;

    pq_begintypsend(&buf);
    pq_sendbytes(&buf, VARDATA(v), VARSIZE(v) - VARHDRSZ);

    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* ---------------------------------------------------------------------------
 * Helpers: decode a varlena into a live histogram (caller hdr_close()s).
 * ------------------------------------------------------------------------ */

static struct hdr_histogram *
decode_histogram(hdr_histogram *v)
{
    struct hdr_histogram *h = NULL;
    int         rc;

    rc = hdr_decode_compressed((uint8_t *) VARDATA(v), VARSIZE(v) - VARHDRSZ, &h);
    if (rc != 0)
        elog(ERROR, "hdr_histogram: invalid compressed blob");

    return h;
}

/* Serialize a live histogram into a new varlena. */
static hdr_histogram *
encode_histogram(struct hdr_histogram *h)
{
    uint8_t    *encoded = NULL;
    size_t      encoded_len = 0;
    hdr_histogram *result;
    int         rc;

    rc = hdr_encode_compressed(h, &encoded, &encoded_len);
    if (rc != 0)
        elog(ERROR, "hdr_histogram: hdr_encode_compressed failed");

    result = (hdr_histogram *) palloc(VARHDRSZ + encoded_len);
    SET_VARSIZE(result, VARHDRSZ + encoded_len);
    memcpy(VARDATA(result), encoded, encoded_len);
    free(encoded);

    return result;
}

/* ---------------------------------------------------------------------------
 * Scalar functions: histogram -> one number.
 * ------------------------------------------------------------------------ */

PG_FUNCTION_INFO_V1(hdr_percentile);
Datum
hdr_percentile(PG_FUNCTION_ARGS)
{
    hdr_histogram *v = (hdr_histogram *) PG_GETARG_VARLENA_P(0);
    double      p = PG_GETARG_FLOAT8(1);
    struct hdr_histogram *h;
    int64_t     value;

    if (p < 0.0 || p > 100.0)
        elog(ERROR, "percentile must be within [0, 100], got %f", p);

    h = decode_histogram(v);
    value = hdr_value_at_percentile(h, p);
    hdr_close(h);

    PG_RETURN_FLOAT8((double) value);
}

#define SCALAR_INT64_FN(fn_name, hdr_call)                 \
    PG_FUNCTION_INFO_V1(fn_name);                          \
    Datum                                                   \
    fn_name(PG_FUNCTION_ARGS)                               \
    {                                                       \
        hdr_histogram *v = (hdr_histogram *) PG_GETARG_VARLENA_P(0); \
        struct hdr_histogram *h = decode_histogram(v);      \
        int64_t     val = hdr_call(h);                      \
        hdr_close(h);                                       \
        PG_RETURN_FLOAT8((double) val);                     \
    }

#define SCALAR_DOUBLE_FN(fn_name, hdr_call)                \
    PG_FUNCTION_INFO_V1(fn_name);                          \
    Datum                                                   \
    fn_name(PG_FUNCTION_ARGS)                               \
    {                                                       \
        hdr_histogram *v = (hdr_histogram *) PG_GETARG_VARLENA_P(0); \
        struct hdr_histogram *h = decode_histogram(v);      \
        double      val = hdr_call(h);                      \
        hdr_close(h);                                       \
        PG_RETURN_FLOAT8(val);                              \
    }

/* The *_pg C symbols differ from the SQL-visible names (hdr_min etc.) because
 * the HdrHistogram_c library already exports hdr_min / hdr_max / hdr_mean /
 * hdr_stddev - C has no overloading, so the PG entry points get a suffix. */
SCALAR_INT64_FN(hdr_min_pg, hdr_min)
SCALAR_INT64_FN(hdr_max_pg, hdr_max)
SCALAR_DOUBLE_FN(hdr_mean_pg, hdr_mean)
SCALAR_DOUBLE_FN(hdr_stddev_pg, hdr_stddev)

PG_FUNCTION_INFO_V1(hdr_total_count);
Datum
hdr_total_count(PG_FUNCTION_ARGS)
{
    hdr_histogram *v = (hdr_histogram *) PG_GETARG_VARLENA_P(0);
    struct hdr_histogram *h = decode_histogram(v);
    int64_t     count = h->total_count;

    hdr_close(h);
    PG_RETURN_INT64(count);
}

PG_FUNCTION_INFO_V1(hdr_sig_figs);
Datum
hdr_sig_figs(PG_FUNCTION_ARGS)
{
    hdr_histogram *v = (hdr_histogram *) PG_GETARG_VARLENA_P(0);
    struct hdr_histogram *h = decode_histogram(v);
    int         sig = h->significant_figures;

    hdr_close(h);
    PG_RETURN_INT32(sig);
}

PG_FUNCTION_INFO_V1(hdr_lowest);
Datum
hdr_lowest(PG_FUNCTION_ARGS)
{
    hdr_histogram *v = (hdr_histogram *) PG_GETARG_VARLENA_P(0);
    struct hdr_histogram *h = decode_histogram(v);
    int64_t     low = h->lowest_discernible_value;

    hdr_close(h);
    PG_RETURN_FLOAT8((double) low);
}

PG_FUNCTION_INFO_V1(hdr_highest);
Datum
hdr_highest(PG_FUNCTION_ARGS)
{
    hdr_histogram *v = (hdr_histogram *) PG_GETARG_VARLENA_P(0);
    struct hdr_histogram *h = decode_histogram(v);
    int64_t     high = h->highest_trackable_value;

    hdr_close(h);
    PG_RETURN_FLOAT8((double) high);
}

/* ---------------------------------------------------------------------------
 * Aggregate state machinery.
 *
 * The transition state is a live `struct hdr_histogram *` (malloc'd by
 * HdrHistogram_c), passed around as PostgreSQL `internal`. We keep it live so
 * we do NOT re-serialize the blob on every row - we only encode it once, in the
 * final function.
 *
 * Trade-off: HdrHistogram_c uses malloc(), not palloc(), so a mid-query error
 * leaks the in-flight states (bounded by the query). Acceptable for v1.
 * ------------------------------------------------------------------------ */

static struct hdr_histogram *
agg_init_state(void)
{
    struct hdr_histogram *h = NULL;
    int         rc = hdr_init(HDR_DEFAULT_LOWEST, HDR_DEFAULT_HIGHEST,
                              HDR_DEFAULT_SIGFIGS, &h);

    if (rc != 0)
        elog(ERROR, "hdr_histogram: hdr_init failed");

    return h;
}

/* finalfunc shared by both aggregates: live histogram -> varlena. */
PG_FUNCTION_INFO_V1(hdr_agg_final);
Datum
hdr_agg_final(PG_FUNCTION_ARGS)
{
    struct hdr_histogram *state;
    hdr_histogram *result;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    state = (struct hdr_histogram *) PG_GETARG_POINTER(0);
    result = encode_histogram(state);
    hdr_close(state);

    PG_RETURN_POINTER(result);
}

/* --- hdr_add(double precision): build a histogram from scalar values. --- */

PG_FUNCTION_INFO_V1(hdr_add_transition);
Datum
hdr_add_transition(PG_FUNCTION_ARGS)
{
    struct hdr_histogram *state;
    double      value;

    if (PG_ARGISNULL(1))
        PG_RETURN_DATUM(PG_GETARG_DATUM(0));    /* skip NULLs */

    if (PG_ARGISNULL(0))
        state = agg_init_state();
    else
        state = (struct hdr_histogram *) PG_GETARG_POINTER(0);

    value = PG_GETARG_FLOAT8(1);
    hdr_record_value(state, (int64_t) value);

    PG_RETURN_POINTER(state);
}

/* --- hdr_merge(hdr_histogram): losslessly merge stored histograms. --- */

PG_FUNCTION_INFO_V1(hdr_merge_transition);
Datum
hdr_merge_transition(PG_FUNCTION_ARGS)
{
    struct hdr_histogram *state;
    hdr_histogram *other_v;
    struct hdr_histogram *other;

    if (PG_ARGISNULL(1))
        PG_RETURN_DATUM(PG_GETARG_DATUM(0));    /* skip NULLs */

    if (PG_ARGISNULL(0))
        state = agg_init_state();
    else
        state = (struct hdr_histogram *) PG_GETARG_POINTER(0);

    other_v = (hdr_histogram *) PG_GETARG_VARLENA_P(1);
    other = decode_histogram(other_v);
    hdr_add(state, other);
    hdr_close(other);

    PG_RETURN_POINTER(state);
}

/* combinefunc for parallel aggregation: merge two partial states. */
PG_FUNCTION_INFO_V1(hdr_merge_combine);
Datum
hdr_merge_combine(PG_FUNCTION_ARGS)
{
    struct hdr_histogram *a = (struct hdr_histogram *) PG_GETARG_POINTER(0);
    struct hdr_histogram *b = (struct hdr_histogram *) PG_GETARG_POINTER(1);

    hdr_add(a, b);
    hdr_close(b);

    PG_RETURN_POINTER(a);
}

/* serial/deserial for parallel workers: state <-> bytea (compressed blob). */
PG_FUNCTION_INFO_V1(hdr_agg_serialize);
Datum
hdr_agg_serialize(PG_FUNCTION_ARGS)
{
    struct hdr_histogram *state = (struct hdr_histogram *) PG_GETARG_POINTER(0);
    hdr_histogram *v = encode_histogram(state);

    PG_RETURN_BYTEA_P((bytea *) v);
}

PG_FUNCTION_INFO_V1(hdr_agg_deserialize);
Datum
hdr_agg_deserialize(PG_FUNCTION_ARGS)
{
    bytea      *v = PG_GETARG_BYTEA_P(0);
    struct hdr_histogram *state;

    state = decode_histogram((hdr_histogram *) v);

    PG_RETURN_POINTER(state);
}
