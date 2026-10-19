#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

#include "ml.hpp"
#include "util/pyfmt.hpp"

using namespace nmm::ml;
using nmm::lgbm::load_model;

namespace {

constexpr double MS = 0.001;
const std::string LGBM_SMALL = std::string(NMM_SOURCE_DIR) + "/tests/data/lgbm_zero_missing.txt";  // 4 features, Column_0..3

using Opt = std::optional<double>;
using Frames = std::vector<std::pair<double, Opt>>;
using Weights = std::vector<std::pair<std::string, double>>;

// pytest.approx: within rel of the expected value or abs (abs alone: only abs).
bool approx(double got, double want, Opt rel = std::nullopt, Opt abs = std::nullopt) {
    double tol;
    if (!rel && abs) tol = *abs;
    else tol = std::max(rel.value_or(1e-6) * std::fabs(want), abs.value_or(1e-12));
    return std::fabs(got - want) <= tol;
}

// The price at x by the definition: the latest frame's, unless it is unknown or no frame came for max_gap since.
Opt brute_value(const Frames& frames, double x, double max_gap) {
    const std::pair<double, Opt>* last = nullptr;
    for (const auto& f : frames) {
        if (f.first > x) break;
        last = &f;
    }
    if (last == nullptr || !last->second || x - last->first >= max_gap) return std::nullopt;
    return last->second;
}

// Mean over [a, b) on the lattice the frames sit on: exact for a step function.
Opt brute_mean(const Frames& frames, double a, double b, double max_gap, double step = MS) {
    std::vector<double> values;
    double k = std::nearbyint(a / step);
    while (k * step < b - step / 2) {
        auto v = brute_value(frames, k * step, max_gap);
        if (!v) return std::nullopt;
        values.push_back(*v);
        k += 1;
    }
    // compensated, as Python's sum() of floats (3.12+)
    double sum = 0.0, c = 0.0;
    for (double v : values) {
        double t = sum + v;
        c += std::fabs(sum) >= std::fabs(v) ? (sum - t) + v : (v - t) + sum;
        sum = t;
    }
    return (sum + c) / static_cast<double>(values.size());
}

struct Rng {
    std::mt19937_64 gen;
    explicit Rng(uint64_t seed) : gen(seed) {}
    double random() { return std::uniform_real_distribution<double>(0.0, 1.0)(gen); }
    int randint(int a, int b) { return std::uniform_int_distribution<int>(a, b)(gen); }
    template <class T>
    T choice(const std::vector<T>& v) { return v[static_cast<size_t>(randint(0, static_cast<int>(v.size()) - 1))]; }
};

// Frames on a 1 ms lattice: random walks, repeats of the same price, the book not ready now and
// then, and silences longer than max_gap.
Frames random_frames(Rng& rng, int n, double max_gap) {
    Frames frames;
    long t = 0;
    double p = 100.0;
    for (int i = 0; i < n; ++i) {
        t += rng.random() > 0.02 ? rng.choice<long>({1, 2, 5, 20, 60}) : static_cast<long>(max_gap / MS) + rng.randint(1, 40);
        double r = rng.random();
        Opt price;
        if (r < 0.02) {
            price = std::nullopt;
        } else if (r < 0.35) {
            price = frames.empty() ? Opt(p) : frames.back().second;  // the price stands, a size changed
        } else {
            p += rng.choice<double>({-0.5, -0.1, 0.1, 0.5});
            price = p;
        }
        frames.emplace_back(static_cast<double>(t) * MS, price);
    }
    return frames;
}

using Ticks = std::vector<std::tuple<double, Opt, Opt>>;

void feed(FeaturePricer& model, const std::string& venue, const Ticks& frames, size_t from = 0, size_t to = SIZE_MAX) {
    for (size_t i = from; i < std::min(to, frames.size()); ++i) {
        const auto& [t, bid, ask] = frames[i];
        model.observe(venue, t, bid, ask);
    }
}

Frames mids_of(const Ticks& frames) {
    Frames out;
    for (const auto& [t, b, a] : frames) out.emplace_back(t, (*b + *a) / 2);
    return out;
}

Frames bids_of(const Ticks& frames) {
    Frames out;
    for (const auto& [t, b, a] : frames) out.emplace_back(t, *b);
    return out;
}

template <class F>
std::string value_error(F&& fn) {
    try {
        fn();
    } catch (const std::invalid_argument& exc) {
        return exc.what();
    }
    ADD_FAILURE() << "no std::invalid_argument";
    return "";
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::vector<std::string> V(std::initializer_list<std::string> v) { return v; }

}  // namespace

TEST(MlStepWindow, WindowMeansAreExactWithUnknownStretchesAndLongSilences) {
    Rng rng(7);
    double max_gap = 0.1;
    auto frames = random_frames(rng, 600, max_gap);
    StepWindow s(100.0, max_gap);  // long horizon: nothing trimmed here
    for (const auto& [t, p] : frames) s.observe(t, p);
    double end = frames.back().first;
    int checked = 0, known = 0;
    for (int i = 0; i < 300; ++i) {
        double a = rng.randint(0, static_cast<int>(end / MS) - 10) * MS;
        double b = a + rng.randint(1, 300) * MS;
        auto got = s.mean(a, b);
        auto want = brute_mean(frames, a, b, max_gap);
        ++checked;
        if (!want) {
            EXPECT_FALSE(got) << a << " " << b;
        } else {
            ++known;
            ASSERT_TRUE(got) << a << " " << b;
            EXPECT_TRUE(approx(*got, *want, std::nullopt, 1e-9)) << a << " " << b;
        }
    }
    EXPECT_GT(known, 50);  // both kinds of windows were tried
    EXPECT_GT(checked - known, 20);
    // the price now: the latest frame's, and unknown once no frame came for max_gap
    EXPECT_EQ(s.value(end), brute_value(frames, end, max_gap));
    EXPECT_FALSE(s.value(end + max_gap * 1.01));
}

TEST(MlStepWindow, OldPointsAreDroppedAndTheMeansStayExactOverALongRun) {
    Rng rng(3);
    StepWindow s(11.0, 10.0);
    Frames frames;
    long t = 0;
    double p = 82'000.0;
    for (int i = 0; i < 200'000; ++i) {  // ~2 h of a busy book, the price wandering 1000s of ticks
        t += rng.choice<long>({1, 3, 10, 40});
        p += rng.choice<double>({-0.1, 0.1, 0.0});
        frames.emplace_back(static_cast<double>(t) * MS, p);
        s.observe(static_cast<double>(t) * MS, p);
    }
    EXPECT_LT(s.points(), 3'000u);  // only the last 11 s or so are kept
    double end = frames.back().first;
    Frames tail;
    for (const auto& f : frames)
        if (f.first >= end - 12.0) tail.push_back(f);
    for (double w : {1.0, 5.0, 10.0}) {
        auto got = s.mean(end - w, end);
        ASSERT_TRUE(got);
        EXPECT_TRUE(approx(*got, *brute_mean(tail, end - w, end, 10.0), std::nullopt, 1e-9)) << w;
    }
}

TEST(MlFeatures, FeatureNamesFollowTheNotebook) {
    auto f = parse_feature("binance_futures_mid_r5s");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->venue, "binance_futures");
    EXPECT_EQ(f->price, "mid");
    EXPECT_EQ(f->window_s, 5.0);
    EXPECT_EQ(f->kind, "r");
    EXPECT_EQ(parse_feature("binance_ask_r0.5s")->window_s, 0.5);
    EXPECT_EQ(parse_feature("arcus_bid_r10s")->venue, "arcus");
    f = parse_feature("binance_mid_d5s");
    EXPECT_EQ((Feature {"binance_mid_d5s", "binance", "mid", 5.0, "d"}), *f);
    f = parse_feature("bybit_mid_d5s");  // Bybit among the venues again (19 Oct model)
    EXPECT_EQ((Feature {"bybit_mid_d5s", "bybit", "mid", 5.0, "d"}), *f);
    f = parse_feature("basis_binance_futures_bid_ema30s");
    EXPECT_EQ((Feature {"basis_binance_futures_bid_ema30s", "binance_futures", "bid", 30.0, "basis"}), *f);
    for (const char* bad : {"okx_mid_r5s", "arcus_last_r5s", "arcus_mid_5s", "arcus_mid_r0s", "arcus_mid_r601s", "intercept",
                            "arcus_mid_d5s", "basis_arcus_mid_ema30s", "basis_bybit_mid_ema0s", "basis_bybit_mid_ema601s",
                            "basis_bybit_mid_r5s", "bybit_mid_ema30s"})
        EXPECT_FALSE(parse_feature(bad)) << bad;
}

TEST(MlFeatures, TheBestLevelImbalanceNowAndItsTimeMean) {
    EXPECT_EQ(*parse_feature("binance_futures_imb1"), (Feature {"binance_futures_imb1", "binance_futures", "imb", 0.0, "imb"}));
    EXPECT_EQ(*parse_feature("binance_imb1_m1s"), (Feature {"binance_imb1_m1s", "binance", "imb", 1.0, "imb"}));
    EXPECT_EQ(*parse_feature("bybit_imb1"), (Feature {"bybit_imb1", "bybit", "imb", 0.0, "imb"}));
    EXPECT_FALSE(parse_feature("binance_imb2"));
    LinearPricer pr(Weights {{"intercept", 0.0}, {"binance_imb1", 1.0}, {"binance_imb1_m1s", 1.0}});
    Feature imb = pr.features[0], mean = pr.features[1];
    EXPECT_EQ(pr.venues, V({"arcus", "binance"}));  // Arcus for the target windows
    EXPECT_TRUE(pr.observe("binance", 10.0, 100.0, 100.1, 3.0, 1.0));  // (3 - 1) / 4
    EXPECT_EQ(pr.feature(imb, 10.0), 0.5);
    EXPECT_FALSE(pr.feature(mean, 10.5));  // the mean needs 1 s known
    EXPECT_FALSE(pr.observe("binance", 10.6, 100.0, 100.1, 3.0, 1.0));  // nothing it reads changed
    EXPECT_TRUE(pr.observe("binance", 11.0, 100.0, 100.1, 1.0, 3.0));  // only the volumes moved: -0.5 now
    EXPECT_EQ(pr.feature(imb, 11.2), -0.5);
    EXPECT_NEAR(*pr.feature(mean, 11.5), (0.5 * 0.5 - 0.5 * 0.5) / 1.0, 1e-12);
    pr.observe("binance", 12.0, 100.0, 100.1, 0.0, 0.0);  // an empty best level: unknown
    EXPECT_FALSE(pr.feature(imb, 12.0));
    pr.observe("binance", 13.0, std::nullopt, std::nullopt);  // the book not ready: unknown
    EXPECT_FALSE(pr.feature(imb, 13.0));
}

TEST(MlPricer, ThePredictionIsTheLinearModelAndTheFairBidItsPrice) {
    Weights weights {{"intercept", 0.00002}, {"arcus_mid_r1s", -0.5}, {"binance_futures_mid_r5s", 0.8}, {"binance_futures_bid_r1s", 0.1}};
    Params params;
    params.target_window_s = 5.0;
    params.target_base = "prev";  // the target up to 17 Oct
    LinearPricer model(weights, params);
    EXPECT_EQ(model.venues, V({"arcus", "binance_futures"}));
    EXPECT_EQ(model.kinds["arcus"], V({"bid", "mid"}));
    EXPECT_FALSE(model.predict(0.0));
    Ticks arcus, futures;
    for (int k = 0; k < 121; ++k) {  // 12 s, a frame every 100 ms; Arcus spread 0.2, futures 0.1
        double t = k * 0.1;
        double a_mid = 100.0 + 0.01 * k + (k % 7 == 0 ? 0.05 : 0.0);
        double f_mid = 100.5 + 0.012 * k;
        arcus.emplace_back(t, a_mid - 0.1, a_mid + 0.1);
        futures.emplace_back(t, f_mid - 0.05, f_mid + 0.05);
    }
    feed(model, "arcus", arcus, 0, 50);
    feed(model, "binance_futures", futures, 0, 50);
    EXPECT_FALSE(model.predict(4.9));  // 5 s of history are not there yet
    EXPECT_TRUE(has(*model.missing(4.9), "not known for all of the last 5 s"));
    feed(model, "arcus", arcus, 50);
    feed(model, "binance_futures", futures, 50);
    double now = 12.0;
    auto pred = model.predict(now);
    ASSERT_TRUE(pred);
    auto feat = [&](const Frames& frames, double w) {
        return *frames.back().second / *brute_mean(frames, now - w, now, 10.0, 0.1) - 1;
    };
    std::map<std::string, double> want {
        {"arcus_mid_r1s", feat(mids_of(arcus), 1.0)},
        {"binance_futures_mid_r5s", feat(mids_of(futures), 5.0)},
        {"binance_futures_bid_r1s", feat(bids_of(futures), 1.0)},
    };
    double sum = 0.0;
    for (const auto& [name, w] : weights) {
        if (name == "intercept") continue;
        EXPECT_TRUE(approx(pred->feature(name), want[name], std::nullopt, 1e-12)) << name;
        sum += w * want[name];
    }
    double y = 0.00002 + sum;
    EXPECT_TRUE(approx(pred->value, y, std::nullopt, 1e-15));
    double prev = *brute_mean(mids_of(arcus), now - 5, now, 60.0, 0.1);
    double prev_bid = *brute_mean(bids_of(arcus), now - 5, now, 60.0, 0.1);
    EXPECT_TRUE(approx(pred->prev, prev));
    EXPECT_TRUE(approx(pred->prev_bid, prev_bid));
    EXPECT_TRUE(approx(pred->fair_bid, prev_bid + prev * y));  // P_prev (1 + y), less the mean half spread
    EXPECT_TRUE(approx(pred->fair_bid, prev * (1 + y) - 0.1));
    // a book that is not ready makes its prices unknown: no prediction until its windows are clean again
    model.observe("binance_futures", 12.05, std::nullopt, std::nullopt);
    EXPECT_FALSE(model.predict(12.06));
    EXPECT_EQ(*model.missing(12.06), "binance_futures bid unknown now");
    Ticks later;
    for (int k = 0; k < 60; ++k) later.emplace_back(12.1 + k * 0.1, 101.9, 102.0);
    feed(model, "binance_futures", later);
    EXPECT_FALSE(model.predict(16.0));  // the 5 s window still holds the gap
    EXPECT_TRUE(model.predict(17.2));
}

TEST(MlPricer, TheTargetFromThePriceNowPricesOffTheMidAndBidNow) {
    // target_base "now" (the notebook from 18 Oct): P_next / p(t) - 1, so the fair is p(t) (1 + y) less the
    // half spread now; no target window to wait for, only the features' own.
    Weights weights {{"intercept", 0.00002}, {"arcus_mid_r1s", -0.5}, {"binance_futures_mid_r1s", 0.8}};
    LinearPricer model(weights);
    EXPECT_EQ(model.target_base, "now");
    EXPECT_EQ(model.target_window_s, 3.0);
    EXPECT_EQ(model.warmup_s, 1.0);
    EXPECT_TRUE(has(model.describe(), "over the next 3 s vs the mid now"));
    Ticks arcus, futures;
    for (int k = 0; k < 31; ++k) {
        arcus.emplace_back(k * 0.1, 100.0 + 0.01 * k - 0.1, 100.0 + 0.01 * k + 0.1);
        futures.emplace_back(k * 0.1, 100.5 + 0.012 * k - 0.05, 100.5 + 0.012 * k + 0.05);
    }
    feed(model, "arcus", arcus, 0, 9);
    feed(model, "binance_futures", futures, 0, 9);
    EXPECT_FALSE(model.predict(0.85));
    EXPECT_TRUE(has(*model.missing(0.85), "not known for all of the last 1 s"));
    feed(model, "arcus", arcus, 9);
    feed(model, "binance_futures", futures, 9);
    auto pred = model.predict(3.0);
    ASSERT_TRUE(pred);
    double y = 0.00002 + (-0.5 * pred->feature("arcus_mid_r1s") + 0.8 * pred->feature("binance_futures_mid_r1s"));
    auto [t, b, a] = arcus.back();
    double mid = (*b + *a) / 2, bid = *b;
    EXPECT_TRUE(approx(pred->value, y, std::nullopt, 1e-15));
    EXPECT_TRUE(approx(pred->prev, mid));
    EXPECT_TRUE(approx(pred->prev_bid, bid));
    EXPECT_TRUE(approx(pred->fair_bid, bid + mid * y));
    EXPECT_TRUE(approx(bid + mid * y, mid * (1 + y) - 0.1));
    Params next;
    next.target_base = "next";
    EXPECT_TRUE(has(value_error([&] { LinearPricer(weights, next); }), "target_base"));
}

TEST(MlPricer, ASilentBookGoesUnknownAfterMaxGapAndTheTargetCanBeTheBid) {
    Params params;
    params.target_price = "bid";
    params.max_gap_s = 2.0;
    params.arcus_max_gap_s = 60.0;
    LinearPricer model(Weights {{"binance_mid_r1s", 1.0}}, params);
    for (int k = 0; k < 80; ++k) {
        model.observe("arcus", k * 0.1, 50.0, 50.2);
        model.observe("binance", k * 0.1, 50.1 + 0.001 * k, 50.15 + 0.001 * k);
    }
    auto pred = model.predict(7.9);
    ASSERT_TRUE(pred);
    EXPECT_TRUE(approx(pred->fair_bid, 50.0 * (1 + pred->value)));  // target = bid: P_prev (1 + y) as it is
    EXPECT_FALSE(model.predict(9.95));  // binance silent since 7.9 s: unknown from 9.9 s on
    EXPECT_EQ(*model.missing(9.95), "binance mid unknown now");
}

TEST(MlPricer, TheChangeAgainstArcusIsTheVenueChangeLessArcus) {
    LinearPricer model(Weights {{"binance_futures_mid_d5s", 1.0}, {"binance_bid_d1s", -0.5}});
    EXPECT_EQ(model.venues, V({"arcus", "binance", "binance_futures"}));
    for (auto key : std::vector<std::pair<std::string, std::string>> {
             {"arcus", "mid"}, {"arcus", "bid"}, {"binance_futures", "mid"}, {"binance", "bid"}})
        EXPECT_TRUE(model.series.contains(key)) << key.first << " " << key.second;
    Ticks arcus, futures, binance;
    for (int k = 0; k < 80; ++k) {  // 8 s, a frame every 100 ms
        double a_mid = 100.0 + 0.01 * k + (k % 5 == 0 ? 0.03 : 0.0);
        double y_mid = 100.4 + 0.015 * k;
        double b_bid = 100.2 + 0.02 * (k % 6);
        arcus.emplace_back(k * 0.1, a_mid - 0.1, a_mid + 0.1);
        futures.emplace_back(k * 0.1, y_mid - 0.05, y_mid + 0.05);
        binance.emplace_back(k * 0.1, b_bid, b_bid + 0.01);
    }
    feed(model, "arcus", arcus);
    feed(model, "binance_futures", futures);
    feed(model, "binance", binance);
    double now = 7.9;
    auto pred = model.predict(now);
    ASSERT_TRUE(pred);
    auto change = [&](const Frames& frames, double w) {  // an r feature by the definition
        return *frames.back().second / *brute_mean(frames, now - w, now, 60.0, 0.1) - 1;
    };
    double want_y = change(mids_of(futures), 5.0) - change(mids_of(arcus), 5.0);
    double want_b = change(bids_of(binance), 1.0) - change(bids_of(arcus), 1.0);
    EXPECT_TRUE(approx(pred->feature("binance_futures_mid_d5s"), want_y, std::nullopt, 1e-13));
    EXPECT_TRUE(approx(pred->feature("binance_bid_d1s"), want_b, std::nullopt, 1e-13));
    EXPECT_TRUE(approx(pred->value, want_y - 0.5 * want_b, std::nullopt, 1e-15));
}

namespace {

// A price at x by the definition, as brute_value, by binary search.
Opt value_at(const std::vector<double>& times, const std::vector<Opt>& prices, double x, double max_gap) {
    long i = static_cast<long>(std::upper_bound(times.begin(), times.end(), x) - times.begin()) - 1;
    if (i < 0 || !prices[static_cast<size_t>(i)] || x - times[static_cast<size_t>(i)] >= max_gap) return std::nullopt;
    return prices[static_cast<size_t>(i)];
}

}  // namespace

TEST(MlPricer, TheBasisFeatureIsTheTimeMeanOfTheRatioItsClockStoppedWhileUnknown) {
    // basis_{venue}_{price}_ema{tau}s against the definition on the lattice the frames sit on (2^-10 s,
    // so every time is exact): the ratio of the two prices folds in over each step both are known,
    // exp(-step / tau) at a time, normalised by the weight of the time seen; no value before 3 tau of
    // known time. The frames have repeats, books not ready, and silences longer than max_gap on both venues.
    Rng rng(5);
    double step = std::ldexp(1.0, -10);
    double tau = 200 * step, gap_a = 300 * step, gap_v = 100 * step;
    auto frames = [&](int n, double p0, int gap_steps) {
        Frames out;
        long k = 0;
        double p = p0;
        for (int i = 0; i < n; ++i) {
            k += rng.random() > 0.02 ? rng.choice<long>({1, 2, 5, 20, 60}) : gap_steps + rng.randint(1, 40);
            double r = rng.random();
            if (r < 0.02) {
                out.emplace_back(static_cast<double>(k) * step, std::nullopt);
                continue;
            }
            if (r > 0.35) p += rng.choice<double>({-0.5, -0.1, 0.1, 0.5});
            out.emplace_back(static_cast<double>(k) * step, p);
        }
        return out;
    };
    Frames arcus = frames(500, 100.0, 300), other = frames(700, 100.17, 100);
    Params params;
    params.max_gap_s = gap_v;
    params.arcus_max_gap_s = gap_a;
    params.target_window_s = 5.0;
    params.target_base = "prev";
    LinearPricer model(Weights {{"basis_binance_mid_ema0.1953125s", 1.0}}, params);
    Feature feature = model.terms[0].first;
    EXPECT_EQ(feature.window_s, tau);
    EXPECT_EQ(model.warmup_s, 5.0);  // the 5 s target window is the longer wait here
    std::vector<double> ta, tv;
    std::vector<Opt> pa, pv;
    for (const auto& [t, p] : arcus) ta.push_back(t), pa.push_back(p);
    for (const auto& [t, p] : other) tv.push_back(t), pv.push_back(p);
    struct Event {
        double t;
        std::string venue;
        Opt p;
    };
    std::vector<Event> events;
    for (const auto& [t, p] : arcus) events.push_back({t, "arcus", p});
    for (const auto& [t, p] : other) events.push_back({t, "binance", p});
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
    double keep = std::exp(-step / tau);
    double num = 0, den = 0, seen = 0;
    long lattice = 0;
    size_t i = 0;
    int checked = 0, known = 0;
    long last = static_cast<long>(std::min(ta.back(), tv.back()) / step);
    for (long c = 1; c < last; c += 37) {
        double now = static_cast<double>(c) * step;
        while (i < events.size() && events[i].t <= now) {
            const auto& e = events[i];
            model.observe(e.venue, e.t, e.p ? Opt(*e.p - 0.01) : std::nullopt, e.p ? Opt(*e.p + 0.01) : std::nullopt);
            ++i;
        }
        while (lattice < c) {  // the step [lattice, lattice + 1)
            auto a = value_at(ta, pa, static_cast<double>(lattice) * step, gap_a);
            auto v = value_at(tv, pv, static_cast<double>(lattice) * step, gap_v);
            if (a && v) {
                num = num * keep + *a / *v * (1 - keep);
                den = den * keep + (1 - keep);
                seen = seen + step;
            }
            ++lattice;
        }
        auto a = value_at(ta, pa, now, gap_a), v = value_at(tv, pv, now, gap_v);
        Opt want;
        if (a && v && seen >= 3 * tau - 1e-9) want = *v * (num / den) / *a - 1;
        auto got = model.feature(feature, now);
        ++checked;
        if (!want) {
            EXPECT_FALSE(got) << now;
        } else {
            ++known;
            ASSERT_TRUE(got) << now;
            EXPECT_TRUE(approx(*got, *want, std::nullopt, 1e-12)) << now;
        }
    }
    EXPECT_GT(known, 20);  // both kinds of moments were tried
    EXPECT_GT(checked - known, 5);
}

TEST(MlPricer, ABasisFeatureWaitsForItsMeanToWarmUp) {
    Params params;
    params.target_window_s = 0.5;
    LinearPricer model(Weights {{"basis_binance_futures_mid_ema1s", 1.0}}, params);
    EXPECT_EQ(model.warmup_s, 3.0);
    EXPECT_EQ(model.venues, V({"arcus", "binance_futures"}));
    for (int k = 0; k < 29; ++k) {  // 2.8 s of the same prices
        model.observe("arcus", k * 0.1, 100.0, 100.2);
        model.observe("binance_futures", k * 0.1, 100.4, 100.5);
    }
    EXPECT_FALSE(model.predict(2.8));
    EXPECT_EQ(*model.missing(2.8), "basis_binance_futures_mid_ema1s: its basis mean has 2.8 of 3 s of known time");
    model.observe("arcus", 3.0, 100.0, 100.2);
    EXPECT_FALSE(model.missing(3.0));
    EXPECT_NEAR(model.predict(3.0)->feature("basis_binance_futures_mid_ema1s"), 0.0, 1e-15);
}

TEST(MlPricer, BadModelsAreRefused) {
    EXPECT_TRUE(has(value_error([] { LinearPricer(Weights {{"okx_mid_r5s", 1.0}}); }), "unknown ML feature"));
    EXPECT_TRUE(has(value_error([] { LinearPricer(Weights {{"arcus_mid_d5s", 1.0}}); }), "unknown ML feature"));
    EXPECT_TRUE(has(value_error([] { LinearPricer(Weights {{"intercept", 0.1}}); }), "no features"));
    Params last;
    last.target_price = "last";
    EXPECT_TRUE(has(value_error([&] { LinearPricer(Weights {{"arcus_mid_r1s", 1.0}}, last); }), "target_price"));
}

TEST(MlPricer, TheModelReadsScaledFeaturesWithTheScalerItWasFitWith) {
    Weights weights {{"intercept", 1e-7}, {"arcus_mid_r1s", 2e-5}, {"binance_mid_r5s", -1e-5}};
    Weights mean {{"arcus_mid_r1s", 1e-7}, {"binance_mid_r5s", -2e-7}};
    Weights scale {{"arcus_mid_r1s", 2e-5}, {"binance_mid_r5s", 5e-5}};
    Params with_scaler;
    with_scaler.mean = mean;
    with_scaler.scale = scale;
    LinearPricer raw(weights), scaled(weights, with_scaler);
    EXPECT_FALSE(raw.scaled);
    EXPECT_TRUE(scaled.scaled);
    EXPECT_TRUE(has(scaled.describe(), "(scaled)"));
    for (int k = 0; k < 80; ++k) {
        for (LinearPricer* model : {&raw, &scaled}) {
            model->observe("arcus", k * 0.1, 100.0 + 0.001 * k, 100.2 + 0.001 * k);
            model->observe("binance", k * 0.1, 100.5 + 0.002 * (k % 9), 100.6 + 0.002 * (k % 9));
        }
    }
    auto p_raw = raw.predict(7.9), p_scaled = scaled.predict(7.9);
    ASSERT_TRUE(p_raw && p_scaled);
    EXPECT_EQ(p_scaled->features, p_raw->features);  // the features themselves are the same, in fractions
    double xa = p_scaled->feature("arcus_mid_r1s"), xb = p_scaled->feature("binance_mid_r5s");
    double want = 1e-7 + 2e-5 * (xa - 1e-7) / 2e-5 - 1e-5 * (xb + 2e-7) / 5e-5;
    EXPECT_TRUE(approx(p_scaled->value, want, 1e-12));
    EXPECT_TRUE(approx(p_raw->value, 1e-7 + 2e-5 * xa - 1e-5 * xb, 1e-12));
    Params partial = with_scaler;
    partial.scale = {{"arcus_mid_r1s", 2e-5}};
    EXPECT_TRUE(has(value_error([&] { LinearPricer(weights, partial); }), "every feature"));
    Params zero = with_scaler;
    zero.scale = {{"arcus_mid_r1s", 2e-5}, {"binance_mid_r5s", 0.0}};
    EXPECT_TRUE(has(value_error([&] { LinearPricer(weights, zero); }), "above 0"));
}

namespace {

const std::vector<std::string> LGBM_NAMES {"arcus_mid_r1s", "binance_mid_d5s", "basis_binance_futures_mid_ema1s",
                                           "binance_futures_mid_r5s"};

void feed_all(FeaturePricer& model, double seconds) {
    for (int k = 0; k < static_cast<int>(seconds * 10); ++k) {  // a frame every 100 ms on each book
        double t = k * 0.1;
        model.observe("arcus", t, 100.0 + 0.01 * (k % 7), 100.2 + 0.01 * (k % 7));
        model.observe("binance", t, 100.4 + 0.02 * (k % 5), 100.41 + 0.02 * (k % 5));
        model.observe("binance_futures", t, 100.5 + 0.015 * k, 100.6 + 0.015 * k);
    }
}

}  // namespace

TEST(MlLightGBM, TheLightgbmModelReadsTheFeaturesInItsColumnOrder) {
    auto forest = load_model(LGBM_SMALL);
    Params params;
    params.target_window_s = 1.0;
    LightGBMPricer model(forest, LGBM_NAMES, params);
    EXPECT_EQ(model.venues, V({"arcus", "binance", "binance_futures"}));
    EXPECT_EQ(model.warmup_s, 5.0);
    std::vector<std::string> names;
    for (const auto& f : model.features) names.push_back(f.name);
    EXPECT_EQ(names, LGBM_NAMES);
    EXPECT_TRUE(has(model.describe(), "LightGBM model"));
    feed_all(model, 4.9);
    EXPECT_FALSE(model.predict(4.8));
    EXPECT_FALSE(has(model.missing(4.8).value_or(""), "of 3 s of known time"));
    feed_all(model, 6.0);  // from 0 s again, as the Python test does
    auto pred = model.predict(5.9);
    ASSERT_TRUE(pred);
    std::vector<double> x;
    for (const auto& n : LGBM_NAMES) x.push_back(pred->feature(n));
    EXPECT_EQ(pred->value, forest->predict(x));
    std::vector<double> mus {1e-6, -2e-6, 0.0, 3e-6};
    Params with_scaler = params;
    for (size_t i = 0; i < LGBM_NAMES.size(); ++i) {
        with_scaler.mean.emplace_back(LGBM_NAMES[i], mus[i]);
        with_scaler.scale.emplace_back(LGBM_NAMES[i], 5e-5);
    }
    LightGBMPricer scaled(forest, LGBM_NAMES, with_scaler);
    feed_all(scaled, 6.0);
    std::vector<double> z;
    for (size_t i = 0; i < LGBM_NAMES.size(); ++i) z.push_back((pred->feature(LGBM_NAMES[i]) - mus[i]) / 5e-5);
    EXPECT_EQ(scaled.predict(5.9)->value, forest->predict(z));
    EXPECT_TRUE(has(scaled.describe(), "(scaled)"));
    EXPECT_TRUE(has(value_error([&] { LightGBMPricer pricer(forest); }), "Column_0"));  // fit on an array: which feature is which?
    EXPECT_TRUE(has(value_error([&] { LightGBMPricer(forest, std::vector<std::string>(LGBM_NAMES.begin(), LGBM_NAMES.begin() + 3)); }),
                    "reads 4 features"));
    EXPECT_TRUE(has(value_error([&] { LightGBMPricer(forest, {LGBM_NAMES[0], LGBM_NAMES[1], LGBM_NAMES[2], LGBM_NAMES[0]}); }),
                    "twice"));
    Params partial = params;
    partial.mean = with_scaler.mean;
    partial.scale = {{"arcus_mid_r1s", 1.0}};
    EXPECT_TRUE(has(value_error([&] { LightGBMPricer(forest, LGBM_NAMES, partial); }), "every feature"));
}

TEST(MlLightGBM, ALightgbmModelThatNamesItsFeaturesNeedsNoList) {
    std::ifstream in(LGBM_SMALL);
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    std::string old = "feature_names=Column_0 Column_1 Column_2 Column_3";
    text.replace(text.find(old), old.size(), "feature_names=" + nmm::join(LGBM_NAMES, " "));
    auto path = std::filesystem::temp_directory_path() / ("nmm_named_" + std::to_string(::getpid()) + ".txt");
    std::ofstream(path) << text;
    auto forest = load_model(path.string());
    std::filesystem::remove(path);
    EXPECT_FALSE(forest->default_names());
    std::vector<std::string> names;
    for (const auto& f : LightGBMPricer(forest).features) names.push_back(f.name);
    EXPECT_EQ(names, LGBM_NAMES);
    std::vector<std::string> reversed(LGBM_NAMES.rbegin(), LGBM_NAMES.rend());
    EXPECT_TRUE(has(value_error([&] { LightGBMPricer(forest, reversed); }), "not the LightGBM model's features"));
}

TEST(MlPricer, TheTargetOffFuturesPricesOffTheFuturesMidLessTheArcusHalfSpread) {
    // target_base "futures" (the notebook from 18 Oct evening): P_next / f(t) - 1 over the next 30 s, so the
    // fair Arcus mid is f(t) (1 + y) and the fair bid that less the Arcus half spread now.
    Params params;
    params.target_window_s = 30.0;
    params.target_base = "futures";
    LinearPricer model(Weights {{"intercept", -0.0015}, {"binance_futures_mid_r1s", 0.8}}, params);
    EXPECT_TRUE(has(model.describe(), "over the next 30 s vs the futures mid now"));
    EXPECT_EQ(model.base_text(), "the futures mid now");
    Ticks arcus, futures;
    for (int k = 0; k < 31; ++k) {
        arcus.emplace_back(k * 0.1, 99.8 + 0.01 * k - 0.1, 99.8 + 0.01 * k + 0.1);
        futures.emplace_back(k * 0.1, 100.0 + 0.012 * k - 0.05, 100.0 + 0.012 * k + 0.05);
    }
    feed(model, "arcus", arcus);
    feed(model, "binance_futures", futures);
    auto pred = model.predict(3.0);
    ASSERT_TRUE(pred);
    double y = -0.0015 + 0.8 * pred->feature("binance_futures_mid_r1s");
    auto [ft, fb, fa] = futures.back();
    auto [at, ab, aa] = arcus.back();
    double f = (*fb + *fa) / 2, mid = (*ab + *aa) / 2, bid = *ab;
    EXPECT_TRUE(approx(pred->value, y, std::nullopt, 1e-15));
    EXPECT_TRUE(approx(pred->prev, f));
    EXPECT_TRUE(approx(pred->fair_bid, f * (1 + y) - (mid - bid)));
    EXPECT_TRUE(approx(pred->prev_bid, f - (mid - bid)));
}

TEST(MlPricer, TheTargetOverTheBasisAddsTheBasisPricingEstimateBack) {
    // target_base "basis": the model predicts P_next / f(t) - 1 less B(t) - 1, B the 30 s mean of Arcus /
    // futures (here they stand still, so B is their ratio); the prediction is the model's plus B - 1.
    Weights weights {{"intercept", 0.00001}, {"arcus_mid_r1s", 0.5}};
    Params params;
    params.target_window_s = 30.0;
    params.target_base = "basis";
    params.baseline_tau_s = 1.0;
    LinearPricer model(weights, params);
    EXPECT_EQ(model.warmup_s, 3.0);
    EXPECT_TRUE(has(model.describe(), "over the basis pricing's B(t) - 1 (1 s mean"));
    Ticks arcus, futures;
    for (int k = 0; k < 41; ++k) {
        arcus.emplace_back(k * 0.1, 99.85, 99.95);
        futures.emplace_back(k * 0.1, 100.0, 100.02);
    }
    feed(model, "arcus", arcus, 0, 20);
    feed(model, "binance_futures", futures, 0, 20);
    EXPECT_FALSE(model.predict(2.0));
    EXPECT_TRUE(has(*model.missing(2.0), "the target's basis mean B(t)"));
    feed(model, "arcus", arcus, 20);
    feed(model, "binance_futures", futures, 20);
    auto pred = model.predict(4.0);
    ASSERT_TRUE(pred);
    double ratio = 99.9 / 100.01;
    EXPECT_TRUE(approx(pred->value, 0.00001 + ratio - 1.0, std::nullopt, 1e-12));
    EXPECT_TRUE(approx(pred->fair_bid, 100.01 * (1 + pred->value) - 0.05));  // = 99.9 (1 + 1e-5) less the half spread
    Params bad = params;
    bad.baseline_tau_s = 0.0;
    EXPECT_TRUE(has(value_error([&] { LinearPricer(weights, bad); }), "baseline_tau_s"));
}

TEST(MlPricer, TheBasisPricingPredictionAsAFeature) {
    // basis_pred_ema{tau}s: the basis bot's fair bid (futures mid x the mean of Arcus bid / futures mid) plus how
    // far the target price stands above the Arcus bid now, over the futures mid - 1 (the notebook's feature).
    EXPECT_EQ(*parse_feature("basis_pred_ema30s"), (Feature {"basis_pred_ema30s", "binance_futures", "mid", 30.0, "bpred"}));
    Params params;
    params.target_window_s = 30.0;
    params.target_base = "futures";
    LinearPricer model(Weights {{"basis_pred_ema1s", 1.0}}, params);
    EXPECT_EQ(model.warmup_s, 3.0);
    Ticks a1, f1, a2, f2;
    for (int k = 0; k < 20; ++k) a1.emplace_back(k * 0.1, 99.85, 99.95), f1.emplace_back(k * 0.1, 100.0, 100.02);
    for (int k = 0; k < 21; ++k) a2.emplace_back(2.0 + k * 0.1, 99.85, 99.95), f2.emplace_back(2.0 + k * 0.1, 100.0, 100.02);
    feed(model, "arcus", a1);
    feed(model, "binance_futures", f1);
    EXPECT_FALSE(model.predict(1.9));
    EXPECT_TRUE(has(*model.missing(1.9), "basis_pred_ema1s: its basis mean"));
    feed(model, "arcus", a2);
    feed(model, "binance_futures", f2);
    auto pred = model.predict(4.0);
    ASSERT_TRUE(pred);
    // prices standing still: B_bid = 99.85 / 100.01, so the feature is the mid's basis now, 99.9 / 100.01 - 1
    EXPECT_TRUE(approx(pred->feature("basis_pred_ema1s"), 99.9 / 100.01 - 1.0, std::nullopt, 1e-12));
    EXPECT_TRUE(approx(pred->fair_bid, 99.85));  // weight 1 on it: exactly the basis bot's fair bid
}

TEST(MlPricer, WithSkipMissingAFeatureUnknownNowKeepsItsLastValue) {
    // [ml] skip_missing: a silent book (here the Binance spot imbalance) holds its feature at the last value (the
    // mean if never known), not the price; a required feature, or the prices of the fair itself, still stop it.
    Weights weights {{"intercept", -0.001}, {"basis_pred_ema1s", 0.0002}, {"binance_imb1", 0.00001}};
    Params kw;
    kw.target_window_s = 30.0;
    kw.target_base = "futures";
    kw.mean = {{"basis_pred_ema1s", -0.001}, {"binance_imb1", 0.1}};
    kw.scale = {{"basis_pred_ema1s", 0.0002}, {"binance_imb1", 0.5}};
    Params loose_kw = kw;
    loose_kw.skip_missing = true;
    loose_kw.required = {"basis_pred_ema1s"};
    LinearPricer strict(weights, kw), loose(weights, loose_kw);
    EXPECT_EQ(strict.needed_venues, V({"arcus", "binance", "binance_futures"}));
    EXPECT_EQ(loose.needed_venues, V({"arcus", "binance_futures"}));
    Ticks arcus, futures;
    for (int k = 0; k < 41; ++k) arcus.emplace_back(k * 0.1, 99.85, 99.95), futures.emplace_back(k * 0.1, 100.0, 100.02);
    for (LinearPricer* model : {&strict, &loose}) {
        feed(*model, "arcus", arcus);
        feed(*model, "binance_futures", futures);
    }
    EXPECT_FALSE(strict.predict(4.0));
    EXPECT_TRUE(has(*strict.missing(4.0), "binance imb unknown now"));
    auto pred = loose.predict(4.0);
    ASSERT_TRUE(pred);
    EXPECT_EQ(pred->skipped, V({"binance_imb1"}));
    EXPECT_EQ(pred->feature("binance_imb1"), 0.1);
    double z = (pred->feature("basis_pred_ema1s") + 0.001) / 0.0002;
    EXPECT_TRUE(approx(pred->value, -0.001 + 0.0002 * z));  // never known: at its mean, it counts nothing
    loose.observe("binance", 4.05, 100.0, 100.01, 3.0, 1.0);  // the spot book shows up: imbalance +0.5
    EXPECT_TRUE(loose.predict(4.1)->skipped.empty());
    EXPECT_EQ(loose.predict(4.1)->feature("binance_imb1"), 0.5);
    loose.observe("binance", 4.2, std::nullopt, std::nullopt);  // and goes away again: held at +0.5
    auto held = loose.predict(4.3);
    EXPECT_EQ(held->skipped, V({"binance_imb1"}));
    EXPECT_EQ(held->feature("binance_imb1"), 0.5);
    LinearPricer early(weights, loose_kw);
    Ticks a10, f10;
    for (int k = 0; k < 10; ++k) a10.emplace_back(k * 0.1, 99.85, 99.95), f10.emplace_back(k * 0.1, 100.0, 100.02);
    feed(early, "arcus", a10);
    feed(early, "binance_futures", f10);
    EXPECT_FALSE(early.predict(0.9));  // required: waits
    EXPECT_TRUE(has(*early.missing(0.9), "basis_pred_ema1s: its basis mean"));
    Params required;
    required.required = {"arcus_mid_r1s"};
    EXPECT_TRUE(has(value_error([&] { LinearPricer(weights, required); }), "required ML features not in the model"));
}
