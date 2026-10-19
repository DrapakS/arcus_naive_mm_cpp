// ML pricing: a model over the features of research/ml_pricing.ipynb.
//
// The notebook builds its dataset from the top-10 book collector, which builds every book over
// the same streams as this bot and stamps each change with the local time. Here the same
// quantities are worked out live, from the bot's own books, with the notebook's rules:
//
// * A price (a venue's mid, best bid or best ask) holds from each frame of its book to the next.
//   It is unknown while the book is not ready (not synced, crossed, every link down), and from
//   max_gap_s after a frame when no other frame follows that long (arcus_max_gap_s for Arcus,
//   whose quiet book may stand still longer).
// * {venue}_{price}_r{w}s is the price now over its time-weighted mean over the last w seconds,
//   minus one (venues arcus, binance, binance_futures, bybit; prices mid, bid, ask). A window not known all along gives no value, and then no price at all, as the
//   notebook drops such points.
// * {venue}_{price}_d{w}s (any venue but Arcus) is that venue's change over the window less
//   Arcus's: {venue}_{price}_r{w}s - arcus_{price}_r{w}s.
// * basis_{venue}_{price}_ema{tau}s (any venue but Arcus): p_venue * B / p_arcus - 1, B the
//   time-weighted exponential mean of p_arcus / p_venue with time constant tau. Its clock stops
//   while either price is unknown, and it has no value until BASIS_WARMUP_TAUS time constants of
//   known time are in.
// * {venue}_imb1 is the lopsidedness of its best level now, (V_bid - V_ask) / (V_bid + V_ask),
//   unknown while the book is not ready or both are empty; {venue}_imb1_m{w}s its time mean.
// * basis_pred_ema{tau}s is the basis pricing's own prediction of a target off futures: its fair
//   bid (futures mid x the mean of Arcus bid / futures mid) plus how far the target price stands
//   above the Arcus bid now, over the futures mid, minus one.
// * The model predicts P_next / P_base - 1: P_next the mean Arcus target price over the next
//   target_window_s, P_base by target_base: "now" the price now, "prev" its mean over the last
//   target_window_s, or the Binance futures price now ("futures": the model's whole, "basis": what
//   is left over the basis pricing's own estimate B(t) - 1, added back).
//
// The strategy prices off a fair Arcus *bid*: fair bid = base bid + P_base * prediction, the base
// bid taken the same way as P_base; priced off futures, Arcus bid + F (1 + prediction) - Arcus
// price now. With skip_missing a feature unknown now keeps its last known value (its scaler mean,
// 0 without one, if never known); the required features and the fair's own prices still have to
// be known. Means are exact for a step function: cumulative integrals at each change and two
// binary searches per window, the notebook's StepSeries.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lgbm.hpp"

namespace nmm::ml {

extern const std::vector<std::string> VENUES;
extern const std::vector<std::string> PRICES;        // mid, bid, ask
extern const std::vector<std::string> TARGET_BASES;  // now, prev, futures, basis
extern const std::vector<std::string> KINDS;         // [ml] kind: linear or lightgbm
constexpr double MAX_WINDOW_S = 600.0;
inline const std::string INTERCEPT = "intercept";  // the weights' key for the constant term
inline const std::string FUTURES = "binance_futures";  // the venue of target_base "futures" and "basis"
inline const std::string IMB = "imb";  // the series of a venue's best-level imbalance, next to its prices
extern const std::string FEATURE_FORMS;
constexpr double BASIS_WARMUP_TAUS = 3.0;  // no basis value until this many time constants of known time are in
constexpr size_t TRIM_EVERY = 256;  // stale points dropped at once (amortised), once that many are past the horizon
constexpr double NS = 1e-9;  // a window counts as known all along when at most this much of it is not

struct Feature {
    std::string name;
    std::string venue;  // arcus | binance | binance_futures
    std::string price;  // mid | bid | ask | imb
    double window_s = 0;  // r and d: the window; basis and bpred: the time constant of its mean; imb: 0 = now
    // r: change over the window; d: the venue's change less Arcus's; basis: off its basis to Arcus;
    // imb: the best level's imbalance now or its mean; bpred: the basis pricing's prediction
    std::string kind = "r";
    bool operator==(const Feature&) const = default;
};

// arcus_mid_r5s -> Feature(arcus, mid, 5.0); binance_mid_d5s -> Feature(binance, mid, 5.0, "d");
// basis_binance_futures_mid_ema30s -> Feature(binance_futures, mid, 30.0, "basis"); nullopt for any other name.
std::optional<Feature> parse_feature(std::string_view name);

// One price of one venue as the notebook sees it: each frame's value holds until the next
// frame; nullopt while unknown. Exact time-weighted means over windows ending now, for windows
// up to horizon_s long.
class StepWindow {
public:
    StepWindow(double horizon_s, double max_gap_s);
    // A frame of the book at `now` (non-decreasing): its price, nullopt when the book is not
    // ready. Whether the price changed (unknown counts as one).
    bool observe(double now, std::optional<double> price);
    // The price at `now`: the latest frame's, unless unknown or too old (max_gap_s).
    std::optional<double> value(double now) const;
    // Time-weighted mean over [a, b]; nullopt unless the price was known all along.
    std::optional<double> mean(double a, double b) const;

    double horizon_s, max_gap_s;
    std::optional<double> last_frame;  // the latest frame of the book, its value changed or not
    size_t points() const { return t_.size(); }

private:
    std::optional<std::pair<double, double>> integral(double x) const;
    void append(double t, std::optional<double> p);
    void trim(double now);

    std::vector<double> t_;  // when each value began
    std::vector<std::optional<double>> p_;  // the value from then on
    std::vector<double> area_;  // integral of (p - ref) over the known time, from the first point to each point
    std::vector<double> known_;  // known time from the first point to each point
    std::optional<double> ref_;  // prices are integrated as deviations from it, for precision
};

// Until when the latest prices of these series stand known: max_gap_s after each one's latest frame.
double known_until(const StepWindow& a, const StepWindow& b);

// The time-weighted exponential mean of a ratio of two step prices, time constant tau_s. Each
// ratio counts for as long as it stood while known; the clock stops while it is not. Normalised
// by the weight of the time seen; no value until warmup_s of known time is in.
class BasisEma {
public:
    BasisEma(double tau_s, double warmup_s);
    // Count the standing ratio up to `now`: before the prices change at `now`.
    void fold(double now, double until);
    // The ratio from `now` on, nullopt while unknown: after the prices changed at `now`.
    void restart(double now, std::optional<double> ratio);
    double known_s(double now, double until) const;
    std::optional<double> value(double now, double until) const;

    double tau_s, warmup_s;

private:
    struct Folded {
        double mean, weight, seen;
    };
    Folded folded(double now, double until) const;

    double mean_ = 0.0;
    double weight_ = 0.0;  // 1 - exp(-known time / tau_s): the weight of what it has seen
    double seen_ = 0.0;  // known time folded in, s
    std::optional<double> ratio_;  // the ratio standing since since_; nullopt while unknown
    double since_ = 0.0;
};

struct Prediction {
    double value = 0;  // P_next / P_base - 1 predicted ("basis": the model's plus B(t) - 1)
    double prev = 0;  // P_base: the target price now, its mean over the last window, the futures price now
    double prev_bid = 0;  // the Arcus bid taken the same way
    double fair_bid = 0;  // prev_bid + prev * value
    std::vector<std::pair<std::string, double>> features;  // each feature's value now (a held one at its held value)
    std::vector<std::string> skipped;  // skip_missing: the features unknown now, held at their last value

    // The value of the feature `name` (std::out_of_range when the model has none).
    double feature(std::string_view name) const;
};

struct Params {
    double target_window_s = 3.0;
    std::string target_price = "mid";
    std::string target_base = "now";
    double max_gap_s = 10.0;
    double arcus_max_gap_s = 60.0;
    std::vector<std::pair<std::string, double>> mean, scale;  // the scaler the model was fit with: all features or none
    double baseline_tau_s = 30.0;
    bool skip_missing = false;
    std::vector<std::string> required;
};

// The fair Arcus bid from a model over the notebook's features. A subclass is the model itself
// (model(): the prediction from the features as the model reads them, scaled when it was fit on
// scaled features). Feed every frame of every book the model reads with observe(); ask
// predict() (or fair_bid()) for the price now.
class FeaturePricer {
public:
    virtual ~FeaturePricer();
    FeaturePricer(const FeaturePricer&) = delete;
    FeaturePricer& operator=(const FeaturePricer&) = delete;

    // A frame of `venue`'s book at `now`: its best bid and ask (and their volumes, for the
    // imbalance), nullopt while the book is not ready. Whether any value the model reads changed.
    bool observe(const std::string& venue, double now, std::optional<double> bid, std::optional<double> ask,
                 std::optional<double> bid_size = {}, std::optional<double> ask_size = {});
    // One feature's value at `now` (a fraction); nullopt while unknown.
    std::optional<double> feature(const Feature& f, double now);
    // The model at `now`; nullopt while any feature it needs is unknown.
    std::optional<Prediction> predict(double now);
    std::optional<double> fair_bid(double now);
    // What keeps the model from pricing now, for the status line; nullopt when it can.
    std::optional<std::string> missing(double now);
    // What the prediction is measured from, for the log lines: "the mid now", "the 5 s mean mid".
    std::string base_text() const;
    virtual std::string describe() const = 0;

    std::vector<Feature> features;
    bool scaled = false;
    bool skip_missing = false;
    std::set<std::string> required;
    std::vector<std::string> venues;  // the books the model reads
    std::vector<std::string> needed_venues;  // of them, the books it cannot price without
    double warmup_s = 0;
    double target_window_s;
    std::string target_price, target_base;
    double baseline_tau_s;
    bool off_futures;
    // (venue, price) -> its series; venue -> the prices of it the model reads
    std::map<std::pair<std::string, std::string>, StepWindow> series;
    std::map<std::string, std::vector<std::string>> kinds;

protected:
    FeaturePricer(std::vector<Feature> features, const Params& params);
    // The prediction (P_next / P_base - 1) from the features as the model reads them.
    virtual double model(const std::vector<double>& z) const = 0;
    std::string target_text() const;
    std::vector<std::pair<double, double>> scaler_;  // per feature, in the model's order: (mean, scale)

private:
    struct EmaFeed {
        BasisEma* ema;
        StepWindow* arcus;
        StepWindow* other;
    };
    std::vector<const Feature*> must() const;
    std::optional<double> change(const std::string& venue, const std::string& price, double window_s, double now);
    StepWindow& at(const std::string& venue, const std::string& price);

    std::map<std::string, double> last_;  // skip_missing: each feature's last known value
    std::map<std::string, BasisEma> emas_;  // each basis feature's mean
    std::map<std::string, std::vector<EmaFeed>> ema_feeds_;  // venue -> the means its frames move
    std::unique_ptr<BasisEma> baseline_;  // "basis": the basis pricing's estimate of the target, B(t) - 1
};

// A linear model: weights, feature name -> weight, plus "intercept" for the constant term;
// prediction = intercept + sum of weight x feature (as the model reads it).
class LinearPricer : public FeaturePricer {
public:
    explicit LinearPricer(std::vector<std::pair<std::string, double>> weights, Params params = {});
    std::string describe() const override;

    double intercept = 0;
    std::vector<std::pair<Feature, double>> terms;

protected:
    double model(const std::vector<double>& z) const override;

private:
    struct Terms {
        double intercept;
        std::vector<std::pair<Feature, double>> terms;
    };
    static Terms parse_terms(const std::vector<std::pair<std::string, double>>& weights, const Params& params);
    LinearPricer(Terms parsed, const Params& params);
    std::vector<double> weights_;
};

// A LightGBM model (lgbm.hpp: its trees). `features`: the model's columns in order, the
// notebook's names; needed when the model was fit on an array and calls them Column_0,
// Column_1...; else the model's own feature names are used, and `features`, if given, must be
// the same.
class LightGBMPricer : public FeaturePricer {
public:
    LightGBMPricer(std::shared_ptr<const lgbm::Forest> forest, std::vector<std::string> features = {}, Params params = {});
    std::string describe() const override;

    std::shared_ptr<const lgbm::Forest> forest;

protected:
    double model(const std::vector<double>& z) const override;

private:
    static std::vector<Feature> parse_columns(const lgbm::Forest& forest, const std::vector<std::string>& features);
};

}  // namespace nmm::ml
