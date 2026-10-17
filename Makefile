# hdr_histogram - PostgreSQL extension build (PGXS)
#
# Requires: postgresql-server-dev (pg_config), and zlib (for HDR compression).

EXTENSION = hdr_histogram
DATA = hdr_histogram--1.0.0.sql

MODULE_big = hdr_histogram

REGRESS = hdr_histogram

# Our adapter + the vendored HdrHistogram_c core (histogram + log/compression).
# hdr_time.c is needed too: hdr_histogram_log.c references hdr_timespec_* from it.
OBJS = hdr_histogram.o \
       third_party/HdrHistogram_c/src/hdr_histogram.o \
       third_party/HdrHistogram_c/src/hdr_histogram_log.o \
       third_party/HdrHistogram_c/src/hdr_encoding.o \
       third_party/HdrHistogram_c/src/hdr_time.o

# <hdr/hdr_histogram.h> lives under include/; the .c files pull their sibling
# headers ("hdr_*.h") relative to src/.
PG_CPPFLAGS = -Ithird_party/HdrHistogram_c/include -Ithird_party/HdrHistogram_c/src \
              -DHDR_MALLOC_INCLUDE='"pghdr_alloc.h"'

# HdrHistogram_c uses zlib for hdr_encode_compressed.
SHLIB_LINK = -lz

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
