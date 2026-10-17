/*
 * Fuzz target for the histogram blob decode path.
 *
 * The hdr_histogram type is `trusted = true` and parses arbitrary binary input,
 * so the decode path must be memory-safe against crafted blobs.  This harness
 * mirrors the extension's input validation (check_histogram_blob in
 * hdr_histogram.c): cookie + length check, header inflate, payload_len cap,
 * and parameter sanity - then calls hdr_decode_compressed.
 *
 * Run (libFuzzer, clang):
 *   clang -fsanitize=fuzzer,address,undefined -O1 -g \
 *       -I third_party/HdrHistogram_c/include -I third_party/HdrHistogram_c/src \
 *       fuzz/fuzz_decode.c third_party/HdrHistogram_c/src/*.c -lz -lm -o fuzz_decode
 *   ./fuzz_decode
 *
 * Run (standalone poor-man's fuzzer, gcc):
 *   gcc -fsanitize=address,undefined -O1 -g -DSTANDALONE \
 *       -I third_party/HdrHistogram_c/include -I third_party/HdrHistogram_c/src \
 *       fuzz/fuzz_decode.c third_party/HdrHistogram_c/src/*.c -lz -lm -o fuzz_decode
 *   ./fuzz_decode
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>

#include <hdr/hdr_histogram.h>

int hdr_encode_compressed(struct hdr_histogram *h, uint8_t **compressed, size_t *len);
int hdr_decode_compressed(uint8_t *buffer, size_t length, struct hdr_histogram **histogram);

#define HDR_V2_COMPRESSION_COOKIE   0x1c849304u
#define HDR_BLOB_MAX_COUNTS         (16 * 1024 * 1024)

static uint32_t
read_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static uint64_t
read_be64(const uint8_t *p)
{
    return ((uint64_t) read_be32(p) << 32) | read_be32(p + 4);
}

/* Mirrors params_are_sane() in hdr_histogram.c (overflow-safe). */
static int
params_are_sane(int64_t lowest, int64_t highest, int sigfigs)
{
    int sub_bucket_half_count_magnitude;
    int unit_magnitude;

    if (lowest < 1 || sigfigs < 1 || sigfigs > 5)
        return 0;
    if (lowest > highest / 2)
        return 0;

    unit_magnitude = (int) (log((double) lowest) / log(2.0));
    sub_bucket_half_count_magnitude =
        (int) ceil(log(2.0 * pow(10.0, sigfigs)) / log(2.0)) - 1;

    return unit_magnitude + sub_bucket_half_count_magnitude <= 61;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t enc_hdr[40];
    uint32_t compressed_len;
    uint32_t payload_len;
    int sigfigs;
    int64_t lowest, highest;
    z_stream strm;
    int ret;
    struct hdr_histogram *h = NULL;

    if (size < 8)
        return 0;
    if ((read_be32(data) & ~0xf0u) != HDR_V2_COMPRESSION_COOKIE)
        return 0;

    compressed_len = read_be32(data + 4);
    if ((uint64_t) compressed_len > size - 8)
        return 0;

    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK)
        return 0;
    strm.next_in = (Bytef *) (data + 8);
    strm.avail_in = (uInt) compressed_len;
    strm.next_out = enc_hdr;
    strm.avail_out = sizeof(enc_hdr);
    ret = inflate(&strm, Z_SYNC_FLUSH);
    inflateEnd(&strm);
    if (ret != Z_OK)
        return 0;

    payload_len = read_be32(enc_hdr + 4);
    if (payload_len == 0 || payload_len > HDR_BLOB_MAX_COUNTS)
        return 0;

    sigfigs = (int) read_be32(enc_hdr + 12);
    lowest = (int64_t) read_be64(enc_hdr + 16);
    highest = (int64_t) read_be64(enc_hdr + 24);
    if (!params_are_sane(lowest, highest, sigfigs))
        return 0;

    hdr_decode_compressed((uint8_t *) data, size, &h);
    if (h != NULL)
        hdr_close(h);

    return 0;
}

#ifdef STANDALONE
static uint32_t rng = 0x9e3779b9;

static uint32_t
rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static size_t
make_seed(uint8_t *out, size_t outcap, int64_t lowest, int64_t highest,
          int sigfigs, int nvalues)
{
    struct hdr_histogram *h = NULL;
    uint8_t *enc = NULL;
    size_t enc_len = 0;
    int i;

    hdr_init(lowest, highest, sigfigs, &h);
    for (i = 0; i < nvalues; i++)
        hdr_record_value(h, lowest + (int64_t) (rnd() % (uint64_t) (highest - lowest + 1)));
    hdr_encode_compressed(h, &enc, &enc_len);
    hdr_close(h);

    if (enc_len > outcap)
        enc_len = outcap;
    memcpy(out, enc, enc_len);
    free(enc);
    return enc_len;
}

int
main(void)
{
    uint8_t buf[65536];
    uint8_t seeds[3][65536];
    size_t seed_len[3];
    long i;

    seed_len[0] = make_seed(seeds[0], sizeof(seeds[0]), 1, 1000000000LL, 3, 1000);
    seed_len[1] = make_seed(seeds[1], sizeof(seeds[1]), 1, 1000000LL, 3, 100);
    seed_len[2] = make_seed(seeds[2], sizeof(seeds[2]), 1, 1000000000000LL, 5, 1000);

    for (i = 0; i < 20000000; i++)
    {
        size_t len;
        int which = (int) (rnd() % 3);
        uint8_t *seed = seeds[which];
        size_t slen = seed_len[which];

        if ((i & 7) != 0)
        {
            len = slen;
            memcpy(buf, seed, len);
            for (int k = 0; k < 16; k++)
            {
                size_t pos = rnd() % len;
                buf[pos] ^= (uint8_t) (1u << (rnd() & 7));
            }
            if (rnd() & 1)
                len = rnd() % (slen + 1);
            if (rnd() & 1)
            {
                size_t extra = rnd() % 64;
                for (size_t j = 0; j < extra && len < sizeof(buf); j++, len++)
                    buf[len] = (uint8_t) rnd();
            }
        }
        else
        {
            len = rnd() % 4096;
            for (size_t j = 0; j < len; j++)
                buf[j] = (uint8_t) rnd();
        }

        LLVMFuzzerTestOneInput(buf, len);
    }

    printf("done: 20M inputs, no crash\n");
    return 0;
}
#endif
