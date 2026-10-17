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
#include "utils/guc.h"
/* varatt.h (where VARDATA/VARSIZE/SET_VARSIZE live) exists only in PG >= 16;
 * earlier versions define those macros in postgres.h/c.h. */
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

#include <hdr/hdr_histogram.h>

#include "pghdr_alloc.h"

PG_MODULE_MAGIC;

/* Default histogram parameters for hdr_add(value) (also the GUC defaults):
 * lowest 1, highest 1e9, 3 sig figs (~0.1% relative precision). */
#define HDR_DEFAULT_LOWEST  1
#define HDR_DEFAULT_HIGHEST 1000000000
#define HDR_DEFAULT_SIGFIGS 3

/*
 * Merge policy for histograms with different parameters, controlled by the
 * hdr_histogram.merge_mode GUC (registered in _PG_init below).
 */
typedef enum HdrMergeMode
{
    HDR_MERGE_STRICT,
    HDR_MERGE_LOSSLESS,
    HDR_MERGE_LEFT,
    HDR_MERGE_UNION
} HdrMergeMode;

static int hdr_merge_mode = HDR_MERGE_LOSSLESS;

/* Histogram parameters for hdr_add(value), settable per session. */
static double hdr_cfg_lowest = HDR_DEFAULT_LOWEST;
static double hdr_cfg_highest = HDR_DEFAULT_HIGHEST;
static int hdr_cfg_sig_figs = HDR_DEFAULT_SIGFIGS;

static const struct config_enum_entry hdr_merge_mode_options[] = {
    {"strict", HDR_MERGE_STRICT, false},
    {"lossless", HDR_MERGE_LOSSLESS, false},
    {"left", HDR_MERGE_LEFT, false},
    {"union", HDR_MERGE_UNION, false},
    {NULL, 0, false}
};

void
_PG_init(void)
{
    DefineCustomRealVariable("hdr_histogram.lowest",
                             "Lowest discernible value used by hdr_add.",
                             NULL,
                             &hdr_cfg_lowest,
                             HDR_DEFAULT_LOWEST,
                             1.0, 1e18,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomRealVariable("hdr_histogram.highest",
                             "Highest trackable value used by hdr_add.",
                             NULL,
                             &hdr_cfg_highest,
                             HDR_DEFAULT_HIGHEST,
                             2.0, 1e18,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("hdr_histogram.sig_figs",
                            "Significant figures used by hdr_add (1..5).",
                            NULL,
                            &hdr_cfg_sig_figs,
                            HDR_DEFAULT_SIGFIGS,
                            1, 5,
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);

    DefineCustomEnumVariable("hdr_histogram.merge_mode",
                             "Policy for merging histograms with different parameters.",
                             NULL,
                             &hdr_merge_mode,
                             HDR_MERGE_LOSSLESS,
                             hdr_merge_mode_options,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);
}

/*
 * hdr_encode_compressed / hdr_decode_compressed are implemented in
 * third_party/HdrHistogram_c/src/hdr_histogram_log.c but are NOT declared in
 * the public header (they live in the test header). Declare them here.
 *
 * encode: allocates *compressed_histogram (caller frees).
 * decode: allocates *histogram (caller hdr_close()s).
 */
int hdr_encode_compressed(struct hdr_histogram *h, uint8_t **compressed_histogram, size_t *compressed_len);
int hdr_decode_compressed(uint8_t *buffer, size_t length, struct hdr_histogram **histogram);

/* A serialized histogram is a few KB; reject anything absurdly large at the
 * input boundary (also bounds the work a crafted blob can make the zlib decode
 * do). */
#define HDR_BLOB_MAX_SIZE   (1 * 1024 * 1024)

/*
 * The V2 compression cookie (HdrHistogram_c) with the word-size bits masked
 * off.  We produce/import only V2; V0/V1 are legacy and rejected.
 */
#define HDR_V2_COMPRESSION_COOKIE   0x1c849304u

/* The encoding header carries an uncompressed counts length (payload_len) that
 * the decoder allocates directly.  A legit 5-sig-fig histogram over the widest
 * allowed range is ~6 MB; 16 MB is a generous cap that blocks crafted blobs
 * from demanding gigabytes. */
#define HDR_BLOB_MAX_COUNTS         (16 * 1024 * 1024)

/* The type is a varlena: [4-byte length][compressed HDR blob]. */
typedef struct varlena hdr_histogram;

/* ---------------------------------------------------------------------------
 * Allocator bridge: HdrHistogram_c's hdr_malloc/hdr_calloc/hdr_realloc/
 * hdr_free are redirected (via pghdr_alloc.h) to these, which in turn use
 * palloc/palloc0/repalloc/pfree.  All histogram memory therefore lives in
 * PostgreSQL memory contexts and is reclaimed automatically - including on
 * mid-query errors, which previously leaked.
 * ------------------------------------------------------------------------ */

void *
pghdr_alloc(size_t size)
{
    return palloc(size);
}

void *
pghdr_calloc(size_t count, size_t size)
{
    if (count != 0 && size > (size_t) -1 / count)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("hdr_histogram: allocation size overflow")));
    return palloc0(count * size);
}

void *
pghdr_realloc(void *ptr, size_t size)
{
    /* repalloc(NULL) is not allowed, unlike realloc(NULL). */
    if (ptr == NULL)
        return palloc(size);
    return repalloc(ptr, size);
}

void
pghdr_free(void *ptr)
{
    /* pfree(NULL) is not allowed, unlike free(NULL). */
    if (ptr != NULL)
        pfree(ptr);
}

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
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input syntax for type hdr_histogram: odd-length hex string")));

    out = palloc(n);
    for (size_t i = 0; i < n; i++)
    {
        int         hi = hex_val(hex[i * 2]);
        int         lo = hex_val(hex[i * 2 + 1]);

        if (hi < 0 || lo < 0)
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type hdr_histogram: invalid hex digit")));
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

/*
 * Read a big-endian uint32 from a byte buffer (the HDR blob is big-endian on
 * the wire).
 */
static uint32_t
read_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

/*
 * Validate a serialized histogram blob before handing it to
 * hdr_decode_compressed, and reject it with the given SQLSTATE otherwise.
 *
 * HdrHistogram_c's V2 decoder allocates hdr_calloc(payload_len), where
 * payload_len comes straight from the (untrusted) blob header, so a crafted
 * blob could otherwise demand gigabytes of memory.  We inflate just the
 * 40-byte encoding header and bound payload_len here, before the full decode
 * does any large allocation.
 */
static void
check_histogram_blob(const uint8_t *data, size_t len, int sqlerrcode)
{
    uint8_t     enc_hdr[40];
    uint32_t    compressed_len;
    uint32_t    payload_len;
    z_stream    strm;
    int         ret;
    struct hdr_histogram *h = NULL;

    /* Compression flyweight: [cookie u32][length i32][zlib data ...]. */
    if (len < 8)
        goto bad;

    /* only V2 (word-size bits masked off); V0/V1 are legacy and unsupported */
    if ((read_be32(data) & ~0xf0u) != HDR_V2_COMPRESSION_COOKIE)
        goto bad;

    compressed_len = read_be32(data + 4);
    if ((uint64_t) compressed_len > len - 8)
        goto bad;

    /* Inflate just the encoding header to read payload_len. */
    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK)
        goto bad;

    strm.next_in = (Bytef *) (data + 8);
    strm.avail_in = (uInt) compressed_len;
    strm.next_out = enc_hdr;
    strm.avail_out = sizeof(enc_hdr);
    ret = inflate(&strm, Z_SYNC_FLUSH);
    inflateEnd(&strm);
    if (ret != Z_OK)
        goto bad;

    /* Encoding flyweight: [cookie u32][payload_len i32][...]. */
    payload_len = read_be32(enc_hdr + 4);
    if (payload_len == 0 || payload_len > HDR_BLOB_MAX_COUNTS)
        goto bad;

    /* Header looks sane; let the library do the real decode. */
    if (hdr_decode_compressed((uint8_t *) data, len, &h) != 0)
        goto bad;

    hdr_close(h);
    return;

bad:
    ereport(ERROR,
            (errcode(sqlerrcode),
             errmsg("invalid hdr_histogram blob")));
}

PG_FUNCTION_INFO_V1(hdr_histogram_in);
Datum
hdr_histogram_in(PG_FUNCTION_ARGS)
{
    char       *str = PG_GETARG_CSTRING(0);
    size_t      decoded_len = 0;
    char       *decoded = hex_to_bytes(str, strlen(str), &decoded_len);
    hdr_histogram *result;

    if (decoded_len > HDR_BLOB_MAX_SIZE)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input syntax for type hdr_histogram: blob too large")));

    check_histogram_blob((const uint8_t *) decoded, decoded_len,
                         ERRCODE_INVALID_TEXT_REPRESENTATION);

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
    const char *data;
    hdr_histogram *result;

    if (nbytes > HDR_BLOB_MAX_SIZE)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                 errmsg("invalid hdr_histogram blob: too large")));

    data = pq_getmsgbytes(buf, nbytes);
    check_histogram_blob((const uint8_t *) data, nbytes,
                         ERRCODE_INVALID_BINARY_REPRESENTATION);

    result = (hdr_histogram *) palloc(VARHDRSZ + nbytes);
    SET_VARSIZE(result, VARHDRSZ + nbytes);
    memcpy(VARDATA(result), data, nbytes);

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
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("hdr_histogram: invalid compressed blob")));

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
    pfree(encoded);

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

    if (isnan(p) || p < 0.0 || p > 100.0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("percentile must be within [0, 100]")));

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
 * The transition state is a live `struct hdr_histogram *`, passed around as
 * PostgreSQL `internal`. We keep it live so we do NOT re-serialize the blob
 * on every row - we only encode it once, in the final function.  The
 * histogram lives in the aggregate memory context (via the palloc bridge), so
 * it is reclaimed automatically.
 * ------------------------------------------------------------------------ */

/* Get the aggregate memory context: the transition/combine/deserialize
 * functions are invoked in the per-tuple context, but the transition state
 * must live in the aggregate context. */
static MemoryContext
agg_context(FunctionCallInfo fcinfo)
{
    MemoryContext aggcontext;

    /* accept both regular (AGG_CONTEXT_AGGREGATE) and window (AGG_CONTEXT_WINDOW) */
    if (AggCheckCallContext(fcinfo, &aggcontext) == 0)
        elog(ERROR, "hdr_histogram: aggregate function called in non-aggregate context");
    return aggcontext;
}

static struct hdr_histogram *
agg_init_state_with(MemoryContext aggcontext, int64_t lowest, int64_t highest,
                    int sigfigs)
{
    struct hdr_histogram *h = NULL;
    MemoryContext oldcontext;
    int         rc;

    oldcontext = MemoryContextSwitchTo(aggcontext);
    rc = hdr_init(lowest, highest, sigfigs, &h);
    MemoryContextSwitchTo(oldcontext);

    if (rc != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("hdr_histogram: invalid histogram parameters (lowest=%lld, highest=%lld, sig_figs=%d)",
                        (long long) lowest, (long long) highest, sigfigs)));

    return h;
}

static struct hdr_histogram *
agg_init_state(MemoryContext aggcontext)
{
    return agg_init_state_with(aggcontext, (int64_t) hdr_cfg_lowest,
                               (int64_t) hdr_cfg_highest, hdr_cfg_sig_figs);
}

static bool
params_equal(const struct hdr_histogram *a, const struct hdr_histogram *b)
{
    return a->lowest_discernible_value == b->lowest_discernible_value &&
           a->highest_trackable_value  == b->highest_trackable_value &&
           a->significant_figures      == b->significant_figures;
}

/* Does a's configuration fully cover b's, so b merges into a losslessly? */
static bool
params_cover(const struct hdr_histogram *a, const struct hdr_histogram *b)
{
    return a->lowest_discernible_value <= b->lowest_discernible_value &&
           a->highest_trackable_value  >= b->highest_trackable_value &&
           a->significant_figures      >= b->significant_figures;
}

/*
 * Merge a and b into a fresh histogram whose parameters are the union of both
 * (widest range, finest precision).  The result is allocated in aggcontext;
 * a is freed (b is freed by the caller).  Never drops values.
 */
static struct hdr_histogram *
merge_union(MemoryContext aggcontext, struct hdr_histogram *a,
            struct hdr_histogram *b)
{
    int64_t     lowest = Min(a->lowest_discernible_value, b->lowest_discernible_value);
    int64_t     highest = Max(a->highest_trackable_value, b->highest_trackable_value);
    int         sigfigs = Max(a->significant_figures, b->significant_figures);
    struct hdr_histogram *c;
    MemoryContext oldcontext;
    int         rc;

    /* If a already covers b, no rebuild is needed. */
    if (params_cover(a, b))
    {
        hdr_add(a, b);
        return a;
    }

    oldcontext = MemoryContextSwitchTo(aggcontext);
    rc = hdr_init(lowest, highest, sigfigs, &c);
    MemoryContextSwitchTo(oldcontext);
    if (rc != 0)
        elog(ERROR, "hdr_histogram: hdr_init failed");

    /* The union range/precision covers both inputs, so nothing can drop. */
    if (hdr_add(c, a) != 0 || hdr_add(c, b) != 0)
        elog(ERROR, "hdr_histogram: union merge dropped values");

    hdr_close(a);
    return c;
}

/*
 * Merge b into a according to hdr_histogram.merge_mode, returning the
 * resulting accumulator (a fresh histogram for HDR_MERGE_UNION).  The caller
 * owns and frees b.
 */
static struct hdr_histogram *
merge_histograms(MemoryContext aggcontext, struct hdr_histogram *a,
                 struct hdr_histogram *b)
{
    switch (hdr_merge_mode)
    {
        case HDR_MERGE_STRICT:
            if (!params_equal(a, b))
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("hdr_merge: histograms have different parameters")));
            hdr_add(a, b);
            return a;

        case HDR_MERGE_LOSSLESS:
            if (!params_cover(a, b))
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("hdr_merge: cannot merge losslessly: the accumulator does not cover the incoming histogram's parameters")));
            hdr_add(a, b);
            return a;

        case HDR_MERGE_LEFT:
            /* keep accumulator parameters; drop incoming values that don't fit */
            hdr_add(a, b);
            return a;

        case HDR_MERGE_UNION:
            return merge_union(aggcontext, a, b);
    }

    pg_unreachable();
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
    {
        if (PG_ARGISNULL(0))
            PG_RETURN_NULL();
        PG_RETURN_POINTER(PG_GETARG_POINTER(0));
    }

    if (PG_ARGISNULL(0))
        state = agg_init_state(agg_context(fcinfo));
    else
        state = (struct hdr_histogram *) PG_GETARG_POINTER(0);

    value = PG_GETARG_FLOAT8(1);

    /* Reject inputs that HdrHistogram_c would otherwise silently drop. */
    if (isnan(value) || isinf(value))
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("hdr_add: value must be finite")));

    if (value < 0.0)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("hdr_add: value must be non-negative")));

    if (value > (double) state->highest_trackable_value)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("hdr_add: value exceeds the maximum supported value (%lld)",
                        (long long) state->highest_trackable_value)));

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
    MemoryContext aggcontext;

    if (PG_ARGISNULL(1))
    {
        if (PG_ARGISNULL(0))
            PG_RETURN_NULL();
        PG_RETURN_POINTER(PG_GETARG_POINTER(0));
    }

    other_v = (hdr_histogram *) PG_GETARG_VARLENA_P(1);
    other = decode_histogram(other_v);

    /*
     * Initialize the accumulator from the first histogram's parameters; the
     * merge policy (hdr_histogram.merge_mode) decides what happens when a
     * later histogram has different parameters.
     */
    aggcontext = agg_context(fcinfo);
    if (PG_ARGISNULL(0))
        state = agg_init_state_with(aggcontext,
                                    other->lowest_discernible_value,
                                    other->highest_trackable_value,
                                    other->significant_figures);
    else
        state = (struct hdr_histogram *) PG_GETARG_POINTER(0);

    state = merge_histograms(aggcontext, state, other);
    hdr_close(other);

    PG_RETURN_POINTER(state);
}

/* combinefunc for parallel aggregation: merge two partial states. */
PG_FUNCTION_INFO_V1(hdr_merge_combine);
Datum
hdr_merge_combine(PG_FUNCTION_ARGS)
{
    struct hdr_histogram *a;
    struct hdr_histogram *b;

    /*
     * Handle NULL partial states: the deserialize function returns NULL for
     * a NULL serialized state (an all-NULL group in a worker).
     */
    if (PG_ARGISNULL(1))
    {
        if (PG_ARGISNULL(0))
            PG_RETURN_NULL();
        PG_RETURN_POINTER(PG_GETARG_POINTER(0));
    }

    /* The first partial state for a group simply becomes the accumulator. */
    if (PG_ARGISNULL(0))
        PG_RETURN_POINTER(PG_GETARG_POINTER(1));

    a = (struct hdr_histogram *) PG_GETARG_POINTER(0);
    b = (struct hdr_histogram *) PG_GETARG_POINTER(1);

    a = merge_histograms(agg_context(fcinfo), a, b);
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
    bytea      *v;
    struct hdr_histogram *state;
    MemoryContext aggcontext;
    MemoryContext oldcontext;

    /* A NULL serialized state (all-NULL group in a worker) stays NULL. */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    v = PG_GETARG_BYTEA_P(0);

    /*
     * The deserialized state must be allocated in the aggregate context, not
     * the per-tuple context the deserialization function is invoked in;
     * otherwise it is reset before the combine function runs.
     */
    aggcontext = agg_context(fcinfo);

    oldcontext = MemoryContextSwitchTo(aggcontext);
    state = decode_histogram((hdr_histogram *) v);
    MemoryContextSwitchTo(oldcontext);

    PG_RETURN_POINTER(state);
}
