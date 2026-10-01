#!/bin/sh
# Builds the host tests. Usage: ./build.sh  (run from this directory)
#   ./test_decode <in.mp3> <out.wav> [chunk_bytes]
#   ./test_ring
#   ./test_net <url> <out.wav> [max_seconds]
# mbedTLS objects are cached in .build/, so only the first run pays for them.
set -e
cd "$(dirname "$0")"

R=..
V=$R/vendor/mbedtls-2.28.10

mkdir -p .build
if [ ! -f .build/libmbedtls.a ]; then
    for f in "$V"/library/*.c; do
        cc -c -O2 -fPIC -I"$V/include" "$f" -o ".build/$(basename "$f" .c).o"
    done
    ar rcs .build/libmbedtls.a .build/*.o
fi
cc -c -O2 -I"$V/include" "$R/ca_bundle.c" -o .build/ca_bundle.o

cc -O2 -Wall -Wextra -I.. test_decode.c "$R/decode.c" -o test_decode
cc -O2 -Wall -Wextra -I.. test_ring.c "$R/ring.c" -lpthread -o test_ring
cc -O2 -Wall -Wextra -I.. test_sink.c "$R/sink.c" "$R/decode.c" "$R/ring.c" \
   -lpthread -o test_sink
cc -O2 -Wall -Wextra -I.. -I"$V/include" \
   test_net.c "$R/net.c" "$R/decode.c" "$R/ring.c" \
   .build/libmbedtls.a .build/ca_bundle.o -lpthread -o test_net

echo "OK: test_decode / test_ring / test_sink / test_net"
