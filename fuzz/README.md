# Fuzzing

`fuzz_decode.c` fuzzes the histogram blob decode path — the same input
validation `hdr_histogram_in`/`hdr_histogram_recv` perform, followed by
`hdr_decode_compressed`. The `hdr_histogram` type is `trusted = true` and parses
arbitrary binary input, so this path must be memory-safe against crafted blobs.

## With libFuzzer (clang)

```bash
clang -fsanitize=fuzzer,address,undefined -O1 -g \
    -I third_party/HdrHistogram_c/include -I third_party/HdrHistogram_c/src \
    fuzz/fuzz_decode.c third_party/HdrHistogram_c/src/*.c -lz -lm -o fuzz_decode
./fuzz_decode
```

## Standalone poor-man's fuzzer (gcc)

```bash
gcc -fsanitize=address,undefined -O1 -g -DSTANDALONE \
    -I third_party/HdrHistogram_c/include -I third_party/HdrHistogram_c/src \
    fuzz/fuzz_decode.c third_party/HdrHistogram_c/src/*.c -lz -lm -o fuzz_decode
./fuzz_decode
```

The standalone build seeds itself with a few valid blobs, then mutates them
(bit flips, truncation, extension) and also throws pure-random bytes at the
decoder, 20 M iterations, under AddressSanitizer + UndefinedBehaviorSanitizer.

The blob header fields (`payload_len`, `lowest`/`highest`/`sig_figs`) are
pre-validated before the vendored decoder runs, because `hdr_decode_compressed`
allocates `payload_len` bytes and `hdr_calculate_bucket_config` does arithmetic
that overflows for crafted parameter values.
