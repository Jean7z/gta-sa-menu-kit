#!/bin/sh
# Builds the host tests. Usage: ./build.sh  (run from this directory)
#   ./test_decode <in.mp3> <out.wav> [chunk_bytes]
#   ./test_ring
#   ./test_sink
# The player is local-only, so there is no network test and no crypto dependency.
set -e
cd "$(dirname "$0")"

R=..

cc -O2 -Wall -Wextra -I.. test_decode.c "$R/decode.c" -o test_decode
cc -O2 -Wall -Wextra -I.. test_ring.c "$R/ring.c" -lpthread -o test_ring
cc -O2 -Wall -Wextra -I.. test_sink.c "$R/sink.c" "$R/decode.c" "$R/ring.c" \
   -lpthread -o test_sink

echo "OK: test_decode / test_ring / test_sink"