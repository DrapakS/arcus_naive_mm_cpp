#include "portfolio.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "util/json.hpp"
#include "util/log.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

namespace {

Logger& plog() { return get_logger("naive_mm.portfolio"); }

// format(x, "+,.0f"): the sign always, thousands separated by commas ("-0" for a small loss, as Python).
std::string signed_usd(double x) { return (std::signbit(x) ? "-" : "+") + thousands(std::fabs(x)); }

// A JSON string as json.dumps writes it.
std::string json_string(const std::string& s) { return boost::json::serialize(boost::json::string(s)); }

[[noreturn]] void raise_errno(const std::string& what) { throw std::system_error(errno, std::generic_category(), what); }

// repr() of the OSError Python raises for `code`: "PermissionError(13, 'Permission denied')".
std::string os_error_repr(const std::error_code& code) {
    int e = code.value();
    const char* kind = e == EACCES || e == EPERM ? "PermissionError"
                       : e == ENOENT             ? "FileNotFoundError"
                       : e == EEXIST             ? "FileExistsError"
                       : e == EISDIR             ? "IsADirectoryError"
                       : e == ENOTDIR            ? "NotADirectoryError"
                       : e == EINTR              ? "InterruptedError"
                       : e == EAGAIN             ? "BlockingIOError"
                                                 : "OSError";
    return std::format("{}({}, {})", kind, e, pyrepr(std::string_view(std::generic_category().message(e))));
}

// Path.write_text: the whole of `text` in the file at `path`, created or emptied first.
void write_file(const std::string& path, const std::string& text) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) raise_errno(path);
    size_t done = 0;
    while (done < text.size()) {
        ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            int e = errno;
            ::close(fd);
            throw std::system_error(e, std::generic_category(), path);
        }
        done += static_cast<size_t>(n);
    }
    if (::close(fd) != 0) raise_errno(path);
}

// What a bot's file holds; null when it is not one (or is being replaced).
std::shared_ptr<const Holding> load_holding(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    std::ostringstream text;
    text << in.rdbuf();
    try {
        Json d = parse_json(text.str());
        auto field = [&d](std::string_view key) -> const Json& {
            const Json* v = jget(d, key);
            if (v == nullptr) throw std::invalid_argument(std::string(key));
            return *v;
        };
        auto h = std::make_shared<Holding>();
        h->market = to_str(field("market"));
        h->asset_class = to_str(field("class"));
        h->position = to_float(field("position"));
        h->mark = to_float(field("mark"));
        h->weight = to_float(field("weight"));
        h->at = to_float(field("at"));
        h->pid = to_int(field("pid"));
        return h;
    } catch (const std::exception&) {
        return nullptr;
    }
}

// The parts of a path as PurePosixPath keeps them: no empty or "." part.
std::string normalized(std::string_view path) {
    bool root = !path.empty() && path.front() == '/';
    std::vector<std::string> parts;
    for (const auto& part : split(path, '/'))
        if (!part.empty() && part != ".") parts.push_back(part);
    std::string out = (root ? "/" : "") + join(parts, "/");
    return out.empty() ? "." : out;
}

// Path(dir) / name.
std::string joined(const std::string& dir, const std::string& name) {
    if (!name.empty() && name.front() == '/') return normalized(name);
    return normalized(dir == "." ? name : dir + "/" + name);
}

}  // namespace

std::string expand_path(std::string_view path) {
    if (path.empty() || path.front() != '~') return normalized(path);
    size_t slash = path.find('/');
    std::string_view user = path.substr(1, slash == std::string_view::npos ? std::string_view::npos : slash - 1);
    std::optional<std::string> home;
    if (user.empty()) {
        if (const char* env = std::getenv("HOME"); env != nullptr) home = env;
        else if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr) home = pw->pw_dir;
    } else if (const passwd* pw = ::getpwnam(std::string(user).c_str()); pw != nullptr) {
        home = pw->pw_dir;
    }
    if (!home) return normalized(path);  // no such user: as it is
    return normalized(*home + std::string(slash == std::string_view::npos ? "" : path.substr(slash)));
}

Portfolio::Portfolio(std::string_view directory, std::string market_, std::string asset_class_, PortfolioOptions o)
    : dir(expand_path(directory)), market(std::move(market_)), asset_class(std::move(asset_class_)), weight(o.weight),
      limit_usd(o.limit_usd), factor(o.factor), shift_bp(o.shift_bp), stale_s(o.stale_s), interval_s(o.interval_s), position(std::move(o.position)),
      mark(std::move(o.mark)), clock(std::move(o.clock)), pid(o.pid ? *o.pid : static_cast<int64_t>(::getpid())) {
    if (std::find(ASSET_CLASSES.begin(), ASSET_CLASSES.end(), asset_class) == ASSET_CLASSES.end())
        throw std::invalid_argument(std::format("asset class {}: one of {}", pyrepr(std::string_view(asset_class)), join(ASSET_CLASSES, ", ")));
    name_ = std::format("{}.{}.json", market, pid);
    path = joined(dir, name_);
}

// ------------------------------------------------------------- the files

bool Portfolio::publish(bool force) {
    double pos = position(), now = clock();
    if (!force && written_ && written_->first == pos && now - written_->second < HEARTBEAT_S) return false;
    if (auto m = mark()) mark_ = m;
    if (!mark_) return false;  // no price yet: nothing to weigh the position by
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) throw std::system_error(ec, dir);
    // json.dumps of the same dict, key for key: the Python bots read this file and write theirs so
    std::string body = std::format(R"({{"market": {}, "class": {}, "position": {}, "mark": {}, "weight": {}, "at": {}, "pid": {}}})",
                                   json_string(market), json_string(asset_class), pyrepr(pos), pyrepr(*mark_), pyrepr(weight), pyrepr(now), pid);
    std::string tmp = std::string(remove_suffix(path, ".json")) + ".tmp";
    write_file(tmp, body);
    if (::rename(tmp.c_str(), path.c_str()) != 0) raise_errno(tmp);
    written_ = std::pair {pos, now};
    return true;
}

void Portfolio::read() {
    double now = clock();
    std::vector<Holding> found;
    std::map<std::string, std::pair<int64_t, std::shared_ptr<const Holding>>> seen;
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (ends_with(name, ".json") && name != name_) names.push_back(name);
    }
    if (ec) names.clear();
    for (const auto& name : names) {
        std::string file = joined(dir, name);
        struct stat st {};
        if (::stat(file.c_str(), &st) != 0) continue;  // gone meanwhile
        int64_t mtime = static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
        std::shared_ptr<const Holding> h;
        if (auto cached = files.find(name); cached != files.end() && cached->second.first == mtime) h = cached->second.second;
        else h = load_holding(file);  // null: being replaced, or not a bot's: read again once it changes
        seen[name] = {mtime, h};
        if (h == nullptr || h->asset_class != asset_class) continue;
        if (h->market == market) {
            tell(name, std::format("{}: another bot on {} (pid {}), not counted: one market, one position", name, h->market, h->pid));
            continue;
        }
        if (now - h->at > stale_s) {
            tell(name, std::format("{}: not rewritten for {:.0f} s (its bot stopped?), left out", name, now - h->at));
            continue;
        }
        told_.erase(name);
        auto old = std::find_if(found.begin(), found.end(), [&h](const Holding& f) { return f.market == h->market; });
        if (old == found.end()) {
            found.push_back(*h);
            continue;
        }
        tell("two:" + h->market, std::format("two bots on {} in {}: the newer file counts", h->market, dir));
        if (old->at < h->at) *old = *h;
    }
    files = std::move(seen);
    others = std::move(found);
    others_usd = 0.0;
    for (const auto& h : others) others_usd += h.usd();
}

void Portfolio::tell(const std::string& key, const std::string& text) {
    if (told_.insert(key).second) plog().warning("portfolio {}: {}", asset_class, text);
}

void Portfolio::close() { ::unlink(path.c_str()); }

namespace {
// Removes our file when run() ends, however it ends.
struct Closer {
    Portfolio* portfolio;
    ~Closer() { portfolio->close(); }
};
}  // namespace

awaitable<void> Portfolio::run() {
    Closer closer {this};
    for (;;) {
        try {
            publish();
            read();
        } catch (const std::system_error& err) {
            tell("io", std::format("{}: {}", dir, os_error_repr(err.code())));
        }
        co_await sleep(interval_s);
    }
}

// --------------------------------------------------------------- pricing

double Portfolio::exposure(double pos, double price) const { return others_usd + weight * pos * price; }

double Portfolio::lean(double pos, double price) const { return std::max(-1.0, std::min(1.0, exposure(pos, price) / limit_usd)); }

Decimal Portfolio::multiplier(const std::string& side, const Decimal& pos, const Decimal& price) const {
    if (!factor) return Decimal(1);
    double u = lean(pos.to_double(), price.to_double());
    return Decimal(std::format("{:.6f}", 1 + factor * u * (side == "BUY" ? 1 : -1)));  // Decimal(str(round(x, 6)))
}

Decimal Portfolio::shift(const Decimal& pos, const Decimal& price) const {
    if (!shift_bp) return Decimal(0);
    double s = shift_bp * lean(pos.to_double(), price.to_double()) / 1e4;
    return Decimal::from_double(std::stod(std::format("{:.10f}", s)));  // Decimal(str(round(x, 10)))
}

std::optional<std::string> Portfolio::note(const std::string& side, const Decimal& pos, const Decimal& price) const {
    if (others.empty()) return std::nullopt;
    double p = pos.to_double(), x = price.to_double();
    std::string text = std::format("class {} {} $ (u {:+.2f}): markup x{}", asset_class, signed_usd(exposure(p, x)), lean(p, x),
                                   multiplier(side, pos, price).fixed(2));
    if (shift_bp) text += std::format(", quotes {:+.2f} bp", -shift_bp * lean(p, x));
    return text;
}

std::string Portfolio::status(const Decimal& pos, const std::optional<Decimal>& price) const {
    double now = clock();
    std::vector<std::string> parts;
    for (const auto& h : others)
        parts.push_back(std::format("{} {} $ (x{:g}, {:.0f} s ago)", h.market, signed_usd(h.usd()), h.weight, now - h.at));
    std::string text = std::format("portfolio {}: {}", asset_class, parts.empty() ? "no other bot" : join(parts, ", "));
    if (!price) return text + "; ours: no price yet";
    double p = pos.to_double(), x = price->to_double();
    double ours = weight * p * x;
    text += std::format("; ours {} $ -> {} $, u {:+.2f}: buys x{}, sells x{}", signed_usd(ours), signed_usd(exposure(p, x)),
                        lean(p, x), multiplier("BUY", pos, *price).fixed(2), multiplier("SELL", pos, *price).fixed(2));
    if (shift_bp) text += std::format(", quotes {:+.2f} bp", -shift_bp * lean(p, x));
    return text;
}

const Holding* Portfolio::other(const std::string& m) const {
    for (const auto& h : others)
        if (h.market == m) return &h;
    return nullptr;
}

}  // namespace nmm
