// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test for Histogram::Percentile's rank-linear interpolation (issue
// #261). Plain asserts, no gtest: the emit-app also builds standalone in the
// SUT Dockerfiles, where no test framework is available, so this binary is
// only wired up for in-tree builds (MICROTEL_BUILD_BENCH=ON).

#undef NDEBUG
#include "histogram.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace
{

// The committed v1.0 snapshot's shape: 1935 samples in [128,256), 7833 in
// [256,512). The old midpoint code answered p50=384 for every distribution
// landing in that octave; rank-linear interpolation must track the rank.
void TestSnapshotShapeInterpolates()
{
    bench::Histogram h;
    for (int i = 0; i < 1935; ++i)
    {
        h.Record(128 + (static_cast<uint64_t>(i) % 128));
    }
    for (int i = 0; i < 7833; ++i)
    {
        h.Record(256 + (static_cast<uint64_t>(i) % 256));
    }

    // target = 9768 * 0.5 = 4884; fraction = (4884 - 1935) / 7833 = 0.37648;
    // value = 256 + 0.37648 * 256 = 352.38 -> 352.
    const uint64_t p50 = h.Percentile(0.50);
    assert(p50 >= 351 && p50 <= 354);
    assert(p50 % 192 != 0);  // the defect: midpoints made this 384 always

    // Percentiles are monotone in p and stay inside the observed extremes.
    const uint64_t p95 = h.Percentile(0.95);
    const uint64_t p99 = h.Percentile(0.99);
    assert(p50 <= p95 && p95 <= p99);
    assert(p50 >= h.Min() && p99 <= h.Max());
}

// A single observation: every percentile is that observation, exactly —
// the min/max clamp, not the bucket geometry, must answer.
void TestSingleObservationClamps()
{
    bench::Histogram h;
    h.Record(200);
    assert(h.Percentile(0.0) == 200);
    assert(h.Percentile(0.5) == 200);
    assert(h.Percentile(0.99) == 200);
}

// Uniform fill of one octave: interpolated quartiles track rank position
// inside the bucket instead of collapsing to the midpoint.
void TestUniformOctaveQuartiles()
{
    bench::Histogram h;
    for (uint64_t v = 256; v < 512; ++v)
    {
        h.Record(v);
    }
    const uint64_t p25 = h.Percentile(0.25);
    const uint64_t p75 = h.Percentile(0.75);
    assert(p25 >= 316 && p25 <= 324);
    assert(p75 >= 444 && p75 <= 452);
}

// Empty histogram answers 0, as documented.
void TestEmpty()
{
    const bench::Histogram h;
    assert(h.Percentile(0.5) == 0);
}

// The top bucket is open-ended; the observed max bounds it.
void TestOpenEndedTopBucketUsesMax()
{
    bench::Histogram h;
    h.Record(~uint64_t{0} - 5);
    h.Record(~uint64_t{0} - 3);
    assert(h.Percentile(0.99) <= h.Max());
    assert(h.Percentile(0.99) >= h.Min());
}

}  // namespace

int main()
{
    TestSnapshotShapeInterpolates();
    TestSingleObservationClamps();
    TestUniformOctaveQuartiles();
    TestEmpty();
    TestOpenEndedTopBucketUsesMax();
    std::puts("histogram_selftest: all assertions passed");
    return 0;
}
