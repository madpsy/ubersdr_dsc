#!/usr/bin/env bash
# Checks that src/pcm_v4.hpp agrees with the server's audio protocol version 4
# encoder, by decoding streams the SERVER produced and hashing the samples.
#
# This needs no server and no network: testdata/*.bin are recorded packet
# streams and the hashes below are of the samples that went into them.
#
# It matters more than it looks.  The version 4 predictor is backward adaptive
# — the encoder and this decoder derive their filter taps independently from
# the samples already seen and never exchange a coefficient — so an arithmetic
# difference between the two (a rounding rule, a sign convention, the point at
# which the tap clamp is skipped) produces plausible-sounding noise rather than
# an error.  Nothing short of comparing the actual samples catches it.
#
#   pcmv4_stream.bin     ordinary mono audio, silent packets carrying no body,
#                        an escape to verbatim samples, a sample-rate change and
#                        the interleaved I/Q the DSC channels actually use.
#   pcmv4_rice_edge.bin  a Rice codeword whose unary run is exactly 63 bits long
#                        and is counted out of a full 64-bit accumulator, so the
#                        decoder shifts by 64.  Go defines that as zero and C++
#                        does not, and the difference is silent.
#
# Usage:
#   ./run.sh                     # compiles the decoder standalone with g++
#   ./run.sh path/to/binary      # uses a binary CMake already built
set -uo pipefail

cd "$(dirname "$0")"

BIN="${1:-}"
if [ -z "$BIN" ]; then
    BIN="${TMPDIR:-/tmp}/ubersdr-dsc-pcmv4_conformance"
    g++ -std=c++11 -O2 -Wall -Wextra -o "$BIN" pcmv4_conformance.cpp \
        || { echo "conformance build failed"; exit 1; }
fi

PCMV4_SHA256=ba368c898ae406c5acc806653d9f2dbbfa40086eca3707fda5d77c13948f78d1
PCMV4_RICE_EDGE_SHA256=83e3d94b509efbf7a212a3e10193b3eb281fe1460cbfeef6aabe474c92a718c7

pass=0; fail=0

check() {
    local name="$1" fixture="$2" want="$3" got
    got=$("$BIN" "$fixture" 2>/dev/null | sha256sum | cut -d" " -f1)
    if [ "$got" = "$want" ]; then
        echo "PASS $name"; pass=$((pass+1))
    else
        echo "FAIL $name: decoded samples hash to $got, want $want"; fail=$((fail+1))
    fi
}

check pcmv4-conformance testdata/pcmv4_stream.bin    "$PCMV4_SHA256"
check pcmv4-rice-edge   testdata/pcmv4_rice_edge.bin "$PCMV4_RICE_EDGE_SHA256"

# The demodulator's sample-rate handling, if it has been built. Needs libdsc,
# so unlike the conformance cases above it is not standalone -- run
# `cmake --build build --target dsc_rate_test` first, or via ctest.
RATE_TEST="${RATE_TEST:-../build/test/dsc_rate_test}"
if [ -x "$RATE_TEST" ]; then
    if LD_LIBRARY_PATH="$(dirname "$RATE_TEST")/../src" "$RATE_TEST" >/dev/null 2>&1; then
        echo "PASS dsc-rate"; pass=$((pass+1))
    else
        echo "FAIL dsc-rate: see $RATE_TEST for detail"; fail=$((fail+1))
    fi
else
    echo "SKIP dsc-rate (not built: cmake --build build --target dsc_rate_test)"
fi

echo
echo "passed $pass, failed $fail"
[ "$fail" -eq 0 ]
