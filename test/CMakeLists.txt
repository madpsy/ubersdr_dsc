# -----------------------------------------------------------------------
# pcmv4_conformance: decode a packet stream the SERVER's encoder produced
# and hash the samples.
#
# The version 4 predictor is backward adaptive — the two ends derive their
# filter taps independently and never exchange a coefficient — so an
# arithmetic difference between src/pcm_v4.hpp and the server's Go encoder
# returns plausible noise rather than an error.  Comparing the decoded
# samples is the only thing that catches it.
#
# Needs nothing but a C++ compiler: pcm_v4.hpp is header-only.
#
#   cmake --build build --target pcmv4_conformance
#   ctest --test-dir build --output-on-failure
# -----------------------------------------------------------------------
add_executable(pcmv4_conformance pcmv4_conformance.cpp)
target_compile_features(pcmv4_conformance PRIVATE cxx_std_11)

add_test(NAME pcmv4_conformance
         COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/run.sh"
                 "$<TARGET_FILE:pcmv4_conformance>"
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# -----------------------------------------------------------------------
# dsc_rate_test: that the demodulator is tuned to the sample rate it is
# given, and that it demodulates a synthetic DSC burst at each rate.
#
# Links libdsc for dsc_rx and the DSC symbol decoder.
#
#   cmake --build build --target dsc_rate_test
#   ctest --test-dir build --output-on-failure
# -----------------------------------------------------------------------
add_executable(dsc_rate_test dsc_rate_test.cpp)
target_link_libraries(dsc_rate_test libdsc)

add_test(NAME dsc_rate COMMAND dsc_rate_test)
