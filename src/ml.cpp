#include "ml.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <regex>
#include <stdexcept>

#include "util/pyfmt.hpp"

namespace nmm::ml {

const std::vector<std::string> VENUES {"arcus", "binance", "binance_futures", "bybit"};
const std::vector<std::string> PRICES {"mid", "bid", "ask"};
const std::vector<std::string> TARGET_BASES {"now", "prev", "futures", "basis"};
const std::vector<std::string> KINDS {"linear", "lightgbm"};
const std::string FEATURE_FORMS =
    "{venue}_{price}_r{seconds}s, {venue}_{price}_d{seconds}s, basis_{venue}_{price}_ema{seconds}s, "
    "{venue}_imb1, {venue}_imb1_m{seconds}s or basis_pred_ema{seconds}s "
    "(venue one of arcus, binance, binance_futures, bybit, not arcus for d and basis; price one of mid, bid, ask; "
    "0 < seconds <= 600)";

namespace {

const std::regex FEATURE_RE(R"((arcus|binance_futures|binance|bybit)_(mid|bid|ask)_([rd])(\d+(?:\.\d+)?)s)");
const std::regex BASIS_RE(R"(basis_(binance_futures|binance|bybit)_(mid|bid|ask)_ema(\d+(?:\.\d+)?)s)");
const std::regex IMB_RE(R"((arcus|binance_futures|binance|bybit)_imb1(?:_m(\d+(?:\.\d+)?)s)?)");
const std::regex BPRED_RE(R"(basis_pred_ema(\d+(?:\.\d+)?)s)");

// bisect.bisect_right as CPython runs it: the same probes even on a list that is not sorted (a
// book fed frames out of order), so the same answers as the Python.
long bisect_right(const std::vector<double>& a, double x) {
    size_t lo = 0, hi = a.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (x < a[mid]) hi = mid;
        else lo = mid + 1;
    }
    return static_cast<long>(lo);
}

bool is_ema(const std::string& kind) { return kind == "basis" || kind == "bpred"; }

bool contains(const std::vector<std::string>& values, const std::string& v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

std::string join_sorted(std::set<std::string> values) {
    return join(std::vector<std::string>(values.begin(), values.end()), ", ");
}

// A Python list of strings as repr() writes it: ['a', 'b'].
std::string list_repr(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) out += (i ? ", " : "") + pyrepr(std::string_view(values[i]));
    return out + "]";
}

}  // namespace

std::optional<Feature> parse_feature(std::string_view name_view) {
    std::string name(name_view);
    std::smatch m;
    std::string venue, price, kind;
    double window = 0;
    if (std::regex_match(name, m, FEATURE_RE)) {
        venue = m[1], price = m[2], kind = m[3];
        window = std::strtod(m[4].str().c_str(), nullptr);
        if (kind == "d" && venue == "arcus") return std::nullopt;
    } else if (std::regex_match(name, m, BASIS_RE)) {
        venue = m[1], price = m[2], kind = "basis";
        window = std::strtod(m[3].str().c_str(), nullptr);
    } else if (std::regex_match(name, m, BPRED_RE)) {
        venue = "binance_futures", price = "mid", kind = "bpred";
        window = std::strtod(m[1].str().c_str(), nullptr);
    } else if (std::regex_match(name, m, IMB_RE)) {
        if (!m[2].matched) return Feature {name, m[1], IMB, 0.0, "imb"};
        venue = m[1], price = IMB, kind = "imb";
        window = std::strtod(m[2].str().c_str(), nullptr);
    } else {
        return std::nullopt;
    }
    if (!(0 < window && window <= MAX_WINDOW_S)) return std::nullopt;
    return Feature {name, venue, price, window, kind};
}

// ------------------------------------------------------------------ StepWindow

StepWindow::StepWindow(double horizon_s, double max_gap_s) : horizon_s(horizon_s), max_gap_s(max_gap_s) {
    if (horizon_s <= 0 || max_gap_s <= 0) throw std::invalid_argument("StepWindow needs horizon_s > 0 and max_gap_s > 0");
}

bool StepWindow::observe(double now, std::optional<double> price) {
    auto last = last_frame;
    last_frame = now;
    if (last && now - *last > max_gap_s && !p_.empty() && p_.back())
        append(*last + max_gap_s, std::nullopt);  // unknown from max_gap_s after the last frame
    std::optional<double> current = p_.empty() ? std::nullopt : p_.back();
    if (current == price) return false;
    append(now, price);
    if (t_.size() > TRIM_EVERY) trim(now);
    return true;
}

std::optional<double> StepWindow::value(double now) const {
    if (p_.empty() || !p_.back() || now - *last_frame >= max_gap_s) return std::nullopt;
    return p_.back();
}

std::optional<double> StepWindow::mean(double a, double b) const {
    auto lo = integral(a), hi = integral(b);
    if (!lo || !hi) return std::nullopt;
    double known = hi->second - lo->second;
    if (known <= 0 || known < (b - a) - NS) return std::nullopt;
    return *ref_ + (hi->first - lo->first) / known;
}

std::optional<std::pair<double, double>> StepWindow::integral(double x) const {
    auto i = bisect_right(t_, x) - 1;
    if (i < 0) return std::nullopt;
    const auto& p = p_[static_cast<size_t>(i)];
    double t = t_[static_cast<size_t>(i)];
    if (!p) return std::make_pair(area_[static_cast<size_t>(i)], known_[static_cast<size_t>(i)]);
    double end = x;
    if (static_cast<size_t>(i) == t_.size() - 1)  // the latest value: known only up to max_gap_s after the last frame
        end = std::min(x, *last_frame + max_gap_s);
    double dt = std::max(end - t, 0.0);
    return std::make_pair(area_[static_cast<size_t>(i)] + (*p - *ref_) * dt, known_[static_cast<size_t>(i)] + dt);
}

void StepWindow::append(double t, std::optional<double> p) {
    if (!ref_ && p) ref_ = p;
    double area = 0.0, known = 0.0;
    if (!t_.empty()) {
        const auto& prev = p_.back();
        double dt = t - t_.back();
        area = area_.back() + (prev ? (*prev - *ref_) * dt : 0.0);
        known = known_.back() + (prev ? dt : 0.0);
    }
    t_.push_back(t);
    p_.push_back(p);
    area_.push_back(area);
    known_.push_back(known);
}

void StepWindow::trim(double now) {
    // Drop the points no window ending now can reach (keeping the one in force at now -
    // horizon_s), and rebase what is left on its first point and on the latest known price,
    // so the sums stay small however long the bot runs.
    long cut = bisect_right(t_, now - horizon_s) - 1;
    if (cut < static_cast<long>(TRIM_EVERY / 2)) return;
    t_.erase(t_.begin(), t_.begin() + cut);
    p_.erase(p_.begin(), p_.begin() + cut);
    area_.erase(area_.begin(), area_.begin() + cut);
    known_.erase(known_.begin(), known_.begin() + cut);
    double area0 = area_[0], known0 = known_[0];
    std::optional<double> ref = ref_;
    for (auto it = p_.rbegin(); it != p_.rend(); ++it)
        if (*it) {
            ref = *it;
            break;
        }
    double shift = ref && ref_ ? *ref - *ref_ : 0.0;
    for (auto& k : known_) k = k - known0;
    // integral of (p - ref') = integral of (p - ref) - (ref' - ref) * known time
    for (size_t i = 0; i < area_.size(); ++i) area_[i] = area_[i] - area0 - shift * known_[i];
    ref_ = ref;
}

double known_until(const StepWindow& a, const StepWindow& b) {
    auto until = [](const StepWindow& s) {
        return s.last_frame ? *s.last_frame + s.max_gap_s : -std::numeric_limits<double>::infinity();
    };
    return std::min(until(a), until(b));
}

// ------------------------------------------------------------------ BasisEma

BasisEma::BasisEma(double tau_s, double warmup_s) : tau_s(tau_s), warmup_s(warmup_s) {
    if (tau_s <= 0 || warmup_s < 0) throw std::invalid_argument("BasisEma needs tau_s > 0 and warmup_s >= 0");
}

BasisEma::Folded BasisEma::folded(double now, double until) const {
    if (!ratio_) return {mean_, weight_, seen_};
    double dt = std::min(now, until) - since_;
    if (dt <= 0) return {mean_, weight_, seen_};
    double keep = std::exp(-dt / tau_s);
    double weight = weight_ * keep + (1.0 - keep);
    return {mean_ + (1.0 - keep) / weight * (*ratio_ - mean_), weight, seen_ + dt};
}

void BasisEma::fold(double now, double until) {
    auto f = folded(now, until);
    mean_ = f.mean, weight_ = f.weight, seen_ = f.seen;
    since_ = now;
}

void BasisEma::restart(double now, std::optional<double> ratio) {
    ratio_ = ratio;
    since_ = now;
}

double BasisEma::known_s(double now, double until) const { return folded(now, until).seen; }

std::optional<double> BasisEma::value(double now, double until) const {
    auto f = folded(now, until);
    if (f.seen > 0 && f.seen >= warmup_s - NS) return f.mean;
    return std::nullopt;
}

// ------------------------------------------------------------------ Prediction

double Prediction::feature(std::string_view name) const {
    for (const auto& [n, v] : features)
        if (n == name) return v;
    throw std::out_of_range("no feature " + std::string(name) + " in the prediction");
}

// ------------------------------------------------------------------ FeaturePricer

FeaturePricer::~FeaturePricer() = default;

FeaturePricer::FeaturePricer(std::vector<Feature> features_in, const Params& params)
    : target_window_s(params.target_window_s),
      target_price(params.target_price),
      target_base(params.target_base),
      baseline_tau_s(params.baseline_tau_s) {
    if (!contains(PRICES, params.target_price))
        throw std::invalid_argument("target_price must be one of " + join(PRICES, ", "));
    if (!contains(TARGET_BASES, params.target_base))
        throw std::invalid_argument("target_base must be one of " + join(TARGET_BASES, ", "));
    if (!(0 < params.target_window_s && params.target_window_s <= MAX_WINDOW_S))
        throw std::invalid_argument(std::format("target_window_s must be within 0..{:g}", MAX_WINDOW_S));
    if (!(0 < params.baseline_tau_s && params.baseline_tau_s <= MAX_WINDOW_S))
        throw std::invalid_argument(std::format("baseline_tau_s must be within 0..{:g}", MAX_WINDOW_S));
    features = std::move(features_in);
    if (features.empty()) throw std::invalid_argument("the ML model has no features");
    std::set<std::string> names;
    for (const auto& f : features) names.insert(f.name);
    if (names.size() != features.size()) throw std::invalid_argument("the ML model lists a feature twice");
    skip_missing = params.skip_missing;
    required = std::set<std::string>(params.required.begin(), params.required.end());
    std::set<std::string> not_there;
    for (const auto& r : required)
        if (!names.contains(r)) not_there.insert(r);
    if (!not_there.empty()) throw std::invalid_argument("required ML features not in the model: " + join_sorted(not_there));
    std::map<std::string, double> mean(params.mean.begin(), params.mean.end());
    std::map<std::string, double> scale(params.scale.begin(), params.scale.end());
    for (const auto& [label, table] : {std::pair<std::string, const std::map<std::string, double>*> {"mean", &mean},
                                       {"scale", &scale}}) {
        std::set<std::string> keys;
        for (const auto& kv : *table) keys.insert(kv.first);
        if (!table->empty() && keys != names) {
            std::set<std::string> missing_, extra;
            std::set_difference(names.begin(), names.end(), keys.begin(), keys.end(), std::inserter(missing_, missing_.end()));
            std::set_difference(keys.begin(), keys.end(), names.begin(), names.end(), std::inserter(extra, extra.end()));
            throw std::invalid_argument(std::format(
                "the scaler's {} must give every feature of the model and no other{}{}", label,
                missing_.empty() ? "" : ": missing " + join_sorted(missing_),
                extra.empty() ? "" : ": not in the model " + join_sorted(extra)));
        }
    }
    for (const auto& kv : scale)
        if (!(kv.second > 0)) throw std::invalid_argument("every scale of the scaler must be above 0");
    scaled = !mean.empty() || !scale.empty();
    for (const auto& f : features) {
        auto m = mean.find(f.name);
        auto s = scale.find(f.name);
        scaler_.emplace_back(m == mean.end() ? 0.0 : m->second, s == scale.end() ? 1.0 : s->second);
    }
    off_futures = target_base == "futures" || target_base == "basis";
    // "prev" reads the target price back over the target window; "now" only its value now
    double horizon = 0.0;
    for (const auto& f : features)
        if (!is_ema(f.kind)) horizon = std::max(horizon, f.window_s);
    if (target_base == "prev") horizon = std::max(horizon, target_window_s);
    // every window must be known all along, and every basis mean warmed up, before the first price
    warmup_s = horizon;
    for (const auto& f : features)
        if (is_ema(f.kind)) warmup_s = std::max(warmup_s, BASIS_WARMUP_TAUS * f.window_s);
    if (target_base == "basis") warmup_s = std::max(warmup_s, BASIS_WARMUP_TAUS * baseline_tau_s);
    std::set<std::pair<std::string, std::string>> wanted;
    for (const auto& f : features) wanted.emplace(f.venue, f.price);
    wanted.emplace("arcus", target_price);
    wanted.emplace("arcus", "bid");
    if (off_futures) wanted.emplace(FUTURES, target_price);
    for (const auto& f : features)
        if (f.kind == "d" || f.kind == "basis") wanted.emplace("arcus", f.price);  // they read Arcus's too
    for (const auto& key : wanted)
        series.emplace(key, StepWindow(horizon + 1.0, key.first == "arcus" ? params.arcus_max_gap_s : params.max_gap_s));
    for (const auto& venue : VENUES) {
        auto& prices = kinds[venue];
        for (const auto& [v, price] : wanted)
            if (v == venue) prices.push_back(price);
        if (!prices.empty()) venues.push_back(venue);
    }
    // the books it cannot price without: all of them, or with skip_missing those of the fair's
    // own prices and of the required features
    std::set<std::string> needed {"arcus"};
    if (off_futures) needed.insert(FUTURES);
    for (const Feature* f : must()) {
        needed.insert(f->venue);
        if (f->kind == "d" || f->kind == "basis" || f->kind == "bpred") needed.insert("arcus");
    }
    for (const auto& v : venues)
        if (!skip_missing || needed.contains(v)) needed_venues.push_back(v);
    // each basis feature's mean, and per venue the means its frames move
    for (const auto& f : features) {
        if (!is_ema(f.kind)) continue;
        auto& ema = emas_.emplace(f.name, BasisEma(f.window_s, BASIS_WARMUP_TAUS * f.window_s)).first->second;
        // bpred: the mean of Arcus bid / futures mid
        StepWindow* arcus = &at("arcus", f.kind == "bpred" ? "bid" : f.price);
        StepWindow* other = &at(f.venue, f.price);
        for (const auto& venue : {std::string("arcus"), f.venue}) ema_feeds_[venue].push_back({&ema, arcus, other});
    }
    if (target_base == "basis") {
        baseline_ = std::make_unique<BasisEma>(baseline_tau_s, BASIS_WARMUP_TAUS * baseline_tau_s);
        EmaFeed feed {baseline_.get(), &at("arcus", target_price), &at(FUTURES, target_price)};
        for (const auto& venue : {std::string("arcus"), FUTURES}) ema_feeds_[venue].push_back(feed);
    }
}

StepWindow& FeaturePricer::at(const std::string& venue, const std::string& price) {
    auto it = series.find({venue, price});
    if (it == series.end()) throw std::out_of_range("no series " + venue + " " + price);
    return it->second;
}

std::vector<const Feature*> FeaturePricer::must() const {
    std::vector<const Feature*> out;
    for (const auto& f : features)
        if (!skip_missing || required.contains(f.name)) out.push_back(&f);
    return out;
}

bool FeaturePricer::observe(const std::string& venue, double now, std::optional<double> bid, std::optional<double> ask,
                            std::optional<double> bid_size, std::optional<double> ask_size) {
    static const std::vector<EmaFeed> none;
    auto fit = ema_feeds_.find(venue);
    const auto& feeds = fit == ema_feeds_.end() ? none : fit->second;
    for (const auto& feed : feeds) feed.ema->fold(now, known_until(*feed.arcus, *feed.other));  // the ratio that stood up to this frame
    bool changed = false;
    auto kit = kinds.find(venue);
    if (kit != kinds.end()) {
        for (const auto& kind : kit->second) {
            std::optional<double> price;
            if (bid && ask) {
                if (kind == IMB) {
                    double b = bid_size.value_or(0.0), a = ask_size.value_or(0.0);
                    double total = b + a;
                    if (total > 0) price = (b - a) / total;
                } else {
                    price = kind == "bid" ? *bid : kind == "ask" ? *ask : (*bid + *ask) / 2.0;
                }
            }
            changed = at(venue, kind).observe(now, price) || changed;
        }
    }
    for (const auto& feed : feeds) {  // and the one from now on
        auto a = feed.arcus->value(now), v = feed.other->value(now);
        feed.ema->restart(now, a && v ? std::optional<double>(*a / *v) : std::nullopt);
    }
    return changed;
}

std::optional<double> FeaturePricer::change(const std::string& venue, const std::string& price, double window_s, double now) {
    auto& s = at(venue, price);
    auto current = s.value(now);
    auto mean = s.mean(now - window_s, now);
    if (!current || !mean) return std::nullopt;
    return *current / *mean - 1.0;
}

std::optional<double> FeaturePricer::feature(const Feature& f, double now) {
    if (f.kind == "basis") {
        auto& arcus = at("arcus", f.price);
        auto& other = at(f.venue, f.price);
        auto a = arcus.value(now), v = other.value(now);
        auto basis = emas_.at(f.name).value(now, known_until(arcus, other));
        if (!a || !v || !basis) return std::nullopt;
        return *v * *basis / *a - 1.0;
    }
    if (f.kind == "bpred") {
        auto& bid = at("arcus", "bid");
        auto& futures = at(f.venue, f.price);
        auto b = bid.value(now), fu = futures.value(now), p = at("arcus", target_price).value(now);
        auto ratio = emas_.at(f.name).value(now, known_until(bid, futures));
        if (!b || !fu || !p || !ratio) return std::nullopt;
        return (*fu * *ratio + *p - *b) / *fu - 1.0;
    }
    if (f.kind == "imb") {
        auto& s = at(f.venue, IMB);
        return f.window_s == 0.0 ? s.value(now) : s.mean(now - f.window_s, now);
    }
    auto x = change(f.venue, f.price, f.window_s, now);
    if (f.kind == "d" && x) {
        auto base = change("arcus", f.price, f.window_s, now);
        if (!base) return std::nullopt;
        return *x - *base;
    }
    return x;
}

std::optional<Prediction> FeaturePricer::predict(double now) {
    Prediction out;
    std::vector<double> z;
    z.reserve(features.size());
    for (size_t i = 0; i < features.size(); ++i) {
        const auto& f = features[i];
        auto [mu, sigma] = scaler_[i];
        auto value = feature(f, now);
        double x;
        if (!value) {
            if (!skip_missing || required.contains(f.name)) return std::nullopt;
            auto held = last_.find(f.name);
            x = held == last_.end() ? mu : held->second;  // held at its last value (never known: its mean)
            out.skipped.push_back(f.name);
        } else {
            x = *value;
            if (skip_missing) last_[f.name] = x;
        }
        out.features.emplace_back(f.name, x);
        z.push_back((x - mu) / sigma);
    }
    double total = model(z);
    if (!std::isfinite(total)) return std::nullopt;
    auto& price = at("arcus", target_price);
    auto& bid = at("arcus", "bid");
    if (off_futures) {
        auto& futures = at(FUTURES, target_price);
        auto p = price.value(now), fu = futures.value(now), b = bid.value(now);
        if (!p || !fu || !b) return std::nullopt;
        if (baseline_) {
            auto baseline = baseline_->value(now, known_until(price, futures));
            if (!baseline) return std::nullopt;
            total += *baseline - 1.0;
        }
        out.value = total;
        out.prev = *fu;
        out.prev_bid = *b - *p + *fu;
        out.fair_bid = *b - *p + *fu * (1.0 + total);
        return out;
    }
    std::optional<double> prev, prev_bid;
    if (target_base == "now") {
        prev = price.value(now), prev_bid = bid.value(now);
    } else {
        double start = now - target_window_s;
        prev = price.mean(start, now), prev_bid = bid.mean(start, now);
    }
    if (!prev || !prev_bid) return std::nullopt;
    out.value = total;
    out.prev = *prev;
    out.prev_bid = *prev_bid;
    out.fair_bid = *prev_bid + *prev * total;
    return out;
}

std::optional<double> FeaturePricer::fair_bid(double now) {
    auto prediction = predict(now);
    if (!prediction) return std::nullopt;
    return prediction->fair_bid;
}

std::optional<std::string> FeaturePricer::missing(double now) {
    auto must_ = must();
    std::set<std::pair<std::string, std::string>> keys {{"arcus", target_price}, {"arcus", "bid"}};
    if (off_futures) keys.emplace(FUTURES, target_price);
    for (const Feature* f : must_) {
        keys.emplace(f->venue, f->price);
        if (f->kind == "d" || f->kind == "basis") keys.emplace("arcus", f->price);
    }
    for (const auto& [key, s] : series)
        if (keys.contains(key) && !s.value(now)) return std::format("{} {} unknown now", key.first, key.second);
    std::vector<std::tuple<std::string, std::string, double>> windows;
    for (const Feature* f : must_)
        if (!is_ema(f->kind) && f->window_s) windows.emplace_back(f->venue, f->price, f->window_s);
    for (const Feature* f : must_)
        if (f->kind == "d") windows.emplace_back("arcus", f->price, f->window_s);
    if (target_base == "prev") {
        windows.emplace_back("arcus", target_price, target_window_s);
        windows.emplace_back("arcus", "bid", target_window_s);
    }
    for (const auto& [venue, kind, window] : windows)
        if (!at(venue, kind).mean(now - window, now))
            return std::format("{} {} not known for all of the last {:g} s", venue, kind, window);
    for (const Feature* f : must_) {
        if (!is_ema(f->kind)) continue;
        const auto& ema = emas_.at(f->name);
        double until = known_until(at("arcus", f->kind == "bpred" ? "bid" : f->price), at(f->venue, f->price));
        if (!ema.value(now, until))
            return std::format("{}: its basis mean has {:.1f} of {:g} s of known time", f->name, ema.known_s(now, until),
                               ema.warmup_s);
    }
    if (baseline_) {
        double until = known_until(at("arcus", target_price), at(FUTURES, target_price));
        if (!baseline_->value(now, until))
            return std::format("the target's basis mean B(t) has {:.1f} of {:g} s of known time",
                               baseline_->known_s(now, until), baseline_->warmup_s);
    }
    return std::nullopt;
}

std::string FeaturePricer::target_text() const {
    if (off_futures) {
        std::string text = std::format("the mean Arcus {} over the next {:g} s vs the futures {} now", target_price,
                                       target_window_s, target_price);
        if (target_base == "futures") return text;
        return std::format("{}, over the basis pricing's B(t) - 1 ({:g} s mean of Arcus / futures)", text, baseline_tau_s);
    }
    if (target_base == "now")
        return std::format("the mean Arcus {} over the next {:g} s vs the {} now", target_price, target_window_s, target_price);
    return std::format("Arcus {} over the next {:g} s vs the last {:g} s", target_price, target_window_s, target_window_s);
}

std::string FeaturePricer::base_text() const {
    if (off_futures) return std::format("the futures {} now", target_price);
    if (target_base == "now") return std::format("the {} now", target_price);
    return std::format("the {:g} s mean {}", target_window_s, target_price);
}

// ------------------------------------------------------------------ LinearPricer

LinearPricer::Terms LinearPricer::parse_terms(const std::vector<std::pair<std::string, double>>& weights, const Params& params) {
    if (!contains(PRICES, params.target_price))
        throw std::invalid_argument("target_price must be one of " + join(PRICES, ", "));
    Terms out {0.0, {}};
    for (const auto& [name, weight] : weights) {
        if (name == INTERCEPT) {
            out.intercept = weight;
            continue;
        }
        auto feature = parse_feature(name);
        if (!feature)
            throw std::invalid_argument(std::format("unknown ML feature {}: expected {}", pyrepr(std::string_view(name)), FEATURE_FORMS));
        out.terms.emplace_back(*feature, weight);
    }
    return out;
}

static std::vector<Feature> features_of(const std::vector<std::pair<Feature, double>>& terms) {
    std::vector<Feature> out;
    for (const auto& [f, w] : terms) out.push_back(f);
    return out;
}

LinearPricer::LinearPricer(std::vector<std::pair<std::string, double>> weights, Params params)
    : LinearPricer(parse_terms(weights, params), params) {}

LinearPricer::LinearPricer(Terms parsed, const Params& params)
    : FeaturePricer(features_of(parsed.terms), params), intercept(parsed.intercept), terms(std::move(parsed.terms)) {
    for (const auto& [f, w] : terms) weights_.push_back(w);
}

double LinearPricer::model(const std::vector<double>& z) const {
    double total = intercept;
    for (size_t i = 0; i < weights_.size(); ++i) total += weights_[i] * z[i];
    return total;
}

std::string LinearPricer::describe() const {
    std::vector<std::string> parts;
    for (size_t i = 0; i < terms.size(); ++i) {
        const auto& [f, w] = terms[i];
        if (scaled) {
            auto [mu, sigma] = scaler_[i];
            parts.push_back(std::format("{} {:+.6g} on (x {:+.6g}) / {:.6g}", f.name, w, -mu, sigma));
        } else {
            parts.push_back(std::format("{} {:+.6g}", f.name, w));
        }
    }
    return std::format("linear model of {}: intercept {:+.6g}, {} features{}: {}", target_text(), intercept, terms.size(),
                       scaled ? " (scaled)" : "", join(parts, ", "));
}

// ------------------------------------------------------------------ LightGBMPricer

std::vector<Feature> LightGBMPricer::parse_columns(const lgbm::Forest& forest, const std::vector<std::string>& features) {
    std::vector<std::string> names = features.empty() ? forest.feature_names : features;
    if (!features.empty() && !forest.default_names() && names != forest.feature_names)
        throw std::invalid_argument(std::format("ml.features {} are not the LightGBM model's features {}", list_repr(names),
                                                list_repr(forest.feature_names)));
    if (names.size() != forest.n_features())
        throw std::invalid_argument(std::format(
            "the LightGBM model reads {} features, ml.features names {}{}", forest.n_features(), names.size(),
            forest.default_names() ? " (the model calls them Column_0...: list them in ml.features, in order)" : ""));
    std::vector<Feature> parsed;
    for (const auto& name : names) {
        auto feature = parse_feature(name);
        if (!feature)
            throw std::invalid_argument(std::format(
                "unknown ML feature {}: expected {}{}", pyrepr(std::string_view(name)), FEATURE_FORMS,
                forest.default_names() ? " (the model calls its features Column_0...: list them in ml.features, in order)"
                                       : ""));
        parsed.push_back(*feature);
    }
    return parsed;
}

LightGBMPricer::LightGBMPricer(std::shared_ptr<const lgbm::Forest> forest_in, std::vector<std::string> features, Params params)
    : FeaturePricer(parse_columns(*forest_in, features), params), forest(std::move(forest_in)) {}

double LightGBMPricer::model(const std::vector<double>& z) const { return forest->predict(z); }

std::string LightGBMPricer::describe() const {
    std::vector<std::string> parts;
    for (size_t i = 0; i < features.size(); ++i) {
        if (scaled) {
            auto [mu, sigma] = scaler_[i];
            parts.push_back(std::format("{} (x {:+.6g}) / {:.6g}", features[i].name, -mu, sigma));
        } else {
            parts.push_back(features[i].name);
        }
    }
    return std::format("LightGBM model of {}: {}, {} features{}: {}", target_text(), forest->describe(), features.size(),
                       scaled ? " (scaled)" : "", join(parts, ", "));
}

}  // namespace nmm::ml
