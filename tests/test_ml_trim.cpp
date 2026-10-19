// StepWindow drops stale points once there are as many of them as live ones: O(1) a frame on a dense
// stream (it rebuilt everything each 128 points before), the means exactly as before.

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <utility>
#include <vector>

#include "ml.hpp"

using namespace nmm::ml;

namespace {

using Points = std::vector<std::pair<double, double>>;

// Time-weighted mean over [a, b] of a step price known throughout (points: (t, p), sorted).
double brute_mean(const Points& points, double a, double b) {
    double total = 0.0;
    for (size_t i = 0; i < points.size(); ++i) {
        double next = i + 1 < points.size() ? points[i + 1].first : b;
        double lo = std::max(points[i].first, a), hi = std::min(next, b);
        if (hi > lo) total += points[i].second * (hi - lo);
    }
    return total / (b - a);
}

}  // namespace

TEST(MlTrim, DenseStreamMeansAreExactAndTheKeptPointsBounded) {
    std::mt19937_64 gen(3);
    StepWindow w(30.0, 10.0);
    double t = 0.0, p = 100.0;
    Points points;
    for (int k = 0; k < 60'000; ++k) {  // 1000 frames a second for a minute
        t += 0.001;
        p += std::uniform_int_distribution<int>(0, 1)(gen) ? 0.01 : -0.01;
        w.observe(t, p);
        points.emplace_back(t, p);
        if (k % 7'919 == 0 && t > 31) {
            for (double win : {1.0, 5.0, 10.0, 29.0}) {
                auto got = w.mean(t - win, t);
                ASSERT_TRUE(got) << win;
                EXPECT_NEAR(*got, brute_mean(points, t - win, t), 1e-9) << k << " " << win;
            }
        }
    }
    size_t live = 0;
    for (const auto& [x, _] : points)
        if (x > t - 30.0) ++live;
    EXPECT_LE(w.points(), 2 * live + TRIM_EVERY);  // never more than twice what the horizon holds
    for (double win : {1.0, 10.0}) {
        auto got = w.mean(t - win, t);
        ASSERT_TRUE(got) << win;
        EXPECT_NEAR(*got, brute_mean(points, t - win, t), 1e-9) << win;
    }
}
