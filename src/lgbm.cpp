#include "lgbm.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "util/pyfmt.hpp"

namespace nmm::lgbm {

namespace {

const std::set<std::string, std::less<>> IDENTITY_OBJECTIVES = {
    "regression", "regression_l2", "l2", "mean_squared_error", "mse", "l2_root", "root_mean_squared_error", "rmse",
    "regression_l1", "l1", "mean_absolute_error", "mae", "huber", "fair", "quantile", "mape",
    "mean_absolute_percentage_error",
};

// str.splitlines(): on \n, \r\n and \r.
std::vector<std::string_view> splitlines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' || text[i] == '\r') {
            lines.push_back(text.substr(start, i - start));
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            start = i + 1;
        }
    }
    if (start < text.size()) lines.push_back(text.substr(start));
    return lines;
}

// Python's int() of a token; std::invalid_argument as its ValueError.
long long parse_int(std::string_view token) {
    std::string s(strip(token));
    size_t i = !s.empty() && (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (i == s.size() || s.find_first_not_of("0123456789", i) != std::string::npos)
        throw std::invalid_argument(std::format("invalid literal for int() with base 10: {}", pyrepr(std::string_view(token))));
    errno = 0;
    long long v = std::strtoll(s.c_str(), nullptr, 10);
    if (errno == ERANGE) throw std::invalid_argument("integer out of range: " + s);
    return v;
}

double parse_float(std::string_view token) {
    std::string s(token);
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size())
        throw std::invalid_argument(std::format("could not convert string to float: {}", pyrepr(token)));
    return v;
}

std::vector<double> floats(std::string_view text) {
    std::vector<double> values;
    for (const auto& token : split_ws(text)) {
        double v = parse_float(token);
        if (!std::isfinite(v)) throw std::invalid_argument("the LightGBM model has a value that is not a finite number");
        values.push_back(v);
    }
    return values;
}

std::vector<int> ints(std::string_view text) {
    std::vector<int> values;
    for (const auto& token : split_ws(text)) values.push_back(static_cast<int>(parse_int(token)));
    return values;
}

Tree read_tree(std::string_view block, int n_features, size_t index) {
    std::map<std::string, std::string, std::less<>> kv;
    auto lines = splitlines(block);
    for (size_t i = 1; i < lines.size(); ++i) {
        auto eq = lines[i].find('=');
        if (eq != std::string_view::npos) kv[std::string(lines[i].substr(0, eq))] = std::string(lines[i].substr(eq + 1));
    }
    std::string where = std::format("tree {} of the LightGBM model", index);
    auto get = [&](std::string_view key) -> const std::string& {
        auto it = kv.find(key);
        // a tree without one of its lines: a cut or edited file
        if (it == kv.end()) throw std::invalid_argument(std::format("a tree of the LightGBM model has no {} line", key));
        return it->second;
    };
    auto get_or = [&](std::string_view key, std::string_view fallback) {
        auto it = kv.find(key);
        return it == kv.end() ? std::string(fallback) : it->second;
    };
    int leaves = static_cast<int>(parse_int(get("num_leaves")));
    if (parse_int(get_or("num_cat", "0"))) throw std::invalid_argument(where + " has categorical splits: not supported");
    if (parse_int(get_or("is_linear", "0"))) throw std::invalid_argument(where + " is a linear tree: not supported");
    Tree tree;
    if (leaves == 1) {
        tree.leaf_value = floats(get("leaf_value"));
        return tree;
    }
    tree.split_feature = ints(get("split_feature"));
    tree.threshold = floats(get("threshold"));
    tree.decision_type = ints(get("decision_type"));
    tree.left_child = ints(get("left_child"));
    tree.right_child = ints(get("right_child"));
    tree.leaf_value = floats(get("leaf_value"));
    size_t internal = static_cast<size_t>(std::max(leaves - 1, 0));
    if (static_cast<int>(tree.leaf_value.size()) != leaves || tree.split_feature.size() != internal ||
        tree.threshold.size() != internal || tree.decision_type.size() != internal || tree.left_child.size() != internal ||
        tree.right_child.size() != internal)
        throw std::invalid_argument(std::format("{} is malformed: its arrays do not match num_leaves={}", where, leaves));
    for (size_t node = 0; node < internal; ++node) {
        if (tree.split_feature[node] < 0 || tree.split_feature[node] >= n_features)
            throw std::invalid_argument(std::format("{} splits on feature {}, the model has {}", where, tree.split_feature[node], n_features));
        if (tree.decision_type[node] & 1) throw std::invalid_argument(where + " has a categorical split: not supported");
        for (int child : {tree.left_child[node], tree.right_child[node]}) {
            if (!(-leaves <= child && child < static_cast<int>(internal)) || child == 0)
                throw std::invalid_argument(std::format("{} is malformed: child {} of node {}", where, child, node));
        }
    }
    return tree;
}

// ------------------------------------------------------------------ pickle opcodes

// Reads the opcodes of a pickle as pickletools.genops does, handing each string or bytes
// argument to `found`; nothing is built, nothing is run.
class PickleScanner {
public:
    explicit PickleScanner(std::string_view data) : data_(data) {}

    template <class F>
    void scan(F&& on_string) {
        for (;;) {
            size_t pos = pos_;
            if (pos_ >= data_.size()) throw std::invalid_argument("pickle exhausted before seeing STOP");
            unsigned char code = static_cast<unsigned char>(data_[pos_++]);
            switch (code) {
            // no argument
            case 0x97: case 0x98: case 'N': case 0x88: case 0x89: case ']': case 'a': case 'e': case 'l':
            case ')': case 't': case 0x85: case 0x86: case 0x87: case '}': case 'd': case 's': case 'u':
            case 0x8f: case 0x90: case 0x91: case '0': case '2': case '(': case '1': case 0x94: case 0x93:
            case 'R': case 'b': case 'o': case 0x81: case 0x92: case 'Q':
                break;
            case '.':
                return;  // STOP
            case 'I': case 'g': case 'p': decimal(false); break;
            case 'L': decimal(true); break;
            case 'J': take(4, "int4"); break;
            case 'K': take(1, "uint1"); break;
            case 'M': take(2, "uint2"); break;
            case 0x8a: take(uint(1, "long1"), "long1"); break;
            case 0x8b: {
                int64_t n = sint4("long4");
                if (n < 0) throw std::invalid_argument(std::format("long4 byte count < 0: {}", n));
                take(static_cast<size_t>(n), "long4");
                break;
            }
            case 'S': on_string(quoted_line()); break;
            case 'T': {
                int64_t n = sint4("string4");
                if (n < 0) throw std::invalid_argument(std::format("string4 byte count < 0: {}", n));
                on_string(std::string(take(static_cast<size_t>(n), "string4")));
                break;
            }
            case 'U': on_string(std::string(take(uint(1, "string1"), "string1"))); break;
            case 'B': on_string(std::string(take(uint(4, "bytes4"), "bytes4"))); break;
            case 'C': on_string(std::string(take(uint(1, "bytes1"), "bytes1"))); break;
            case 0x8e: on_string(std::string(take(uint(8, "bytes8"), "bytes8"))); break;
            case 0x96: take(uint(8, "bytearray8"), "bytearray8"); break;  // a bytearray: never the model
            case 'V': on_string(raw_unicode_escape(line())); break;
            case 0x8c: on_string(std::string(take(uint(1, "unicodestring1"), "unicodestring1"))); break;
            case 'X': on_string(std::string(take(uint(4, "unicodestring4"), "unicodestring4"))); break;
            case 0x8d: on_string(std::string(take(uint(8, "unicodestring8"), "unicodestring8"))); break;
            case 'F': parse_float(strip(line())); break;
            case 'G': take(8, "float8"); break;
            case 'h': case 'q': case 0x82: case 0x80: take(1, "uint1"); break;
            case 'j': case 'r': take(4, "uint4"); break;
            case 0x83: take(2, "uint2"); break;
            case 0x84: take(4, "int4"); break;
            case 'c': case 'i': line(); line(); break;  // module and name
            case 'P': line(); break;
            case 0x95: take(8, "uint8"); break;
            default:
                throw std::invalid_argument(std::format("at position {}, opcode {} unknown", pos, byte_repr(code)));
            }
        }
    }

private:
    static std::string byte_repr(unsigned char c) {
        if (c >= 0x20 && c < 0x7f && c != '\'' && c != '\\') return std::format("b'{}'", static_cast<char>(c));
        return std::format("b'\\x{:02x}'", c);
    }

    std::string_view take(size_t n, const char* what) {
        if (data_.size() - pos_ < n)
            throw std::invalid_argument(std::format("expected {} bytes in a {}, but only {} remain", n, what, data_.size() - pos_));
        auto out = data_.substr(pos_, n);
        pos_ += n;
        return out;
    }

    size_t uint(size_t width, const char* what) {
        if (data_.size() - pos_ < width) throw std::invalid_argument(std::format("not enough data in stream to read {}", what));
        uint64_t v = 0;
        for (size_t i = 0; i < width; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(data_[pos_ + i])) << (8 * i);
        pos_ += width;
        if (v > data_.size()) return data_.size() + 1;  // more than there is: take() says so
        return static_cast<size_t>(v);
    }

    int64_t sint4(const char* what) {
        if (data_.size() - pos_ < 4) throw std::invalid_argument(std::format("not enough data in stream to read {}", what));
        uint32_t v = 0;
        for (size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(data_[pos_ + i])) << (8 * i);
        pos_ += 4;
        return static_cast<int32_t>(v);
    }

    std::string_view line() {
        auto nl = data_.find('\n', pos_);
        if (nl == std::string_view::npos) throw std::invalid_argument("no newline found when trying to read stringnl");
        auto out = data_.substr(pos_, nl - pos_);
        pos_ = nl + 1;
        return out;
    }

    void decimal(bool long_) {
        std::string s(line());
        if (long_ && !s.empty() && s.back() == 'L') s.pop_back();
        if (s == "00" || s == "01") return;  // INT's False and True
        parse_int(s);
    }

    // STRING: quoted, with Python's string escapes undone (codecs.escape_decode).
    std::string quoted_line() {
        std::string_view data = line();
        for (char q : {'"', '\''}) {
            if (!data.empty() && data.front() == q) {
                if (data.size() < 2 || data.back() != q)
                    throw std::invalid_argument(std::format("string quote '{}' not found at both ends", q));
                data = data.substr(1, data.size() - 2);
                return escape_decode(data);
            }
        }
        throw std::invalid_argument("no string quotes around the STRING argument");
    }

    static std::string escape_decode(std::string_view s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '\\' || i + 1 == s.size()) {
                out += s[i];
                continue;
            }
            char c = s[++i];
            switch (c) {
            case '\n': break;
            case '\\': out += '\\'; break;
            case '\'': out += '\''; break;
            case '"': out += '"'; break;
            case 'a': out += '\a'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'v': out += '\v'; break;
            case 'x': {
                auto hex = i + 2 < s.size() ? std::string(s.substr(i + 1, 2)) : std::string();
                if (hex.size() < 2 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    throw std::invalid_argument("invalid \\x escape");
                out += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
                i += 2;
                break;
            }
            default:
                if (c >= '0' && c <= '7') {
                    int v = c - '0';
                    for (int k = 0; k < 2 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7'; ++k) v = v * 8 + (s[++i] - '0');
                    out += static_cast<char>(v & 0xff);
                } else {
                    out += '\\';
                    out += c;
                }
            }
        }
        return out;
    }

    // UNICODE: \uXXXX and \UXXXXXXXX escapes, to UTF-8.
    static std::string raw_unicode_escape(std::string_view s) {
        std::string out;
        auto utf8 = [&out](uint32_t cp) {
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        };
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'u' || s[i + 1] == 'U')) {
                size_t n = s[i + 1] == 'u' ? 4 : 8;
                auto hex = std::string(s.substr(i + 2, n));
                if (hex.size() < n || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                    throw std::invalid_argument("truncated \\uXXXX escape");
                utf8(static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16)));
                i += 1 + n;
            } else {
                utf8(static_cast<unsigned char>(s[i]));  // latin-1
            }
        }
        return out;
    }

    std::string_view data_;
    size_t pos_ = 0;
};

}  // namespace

int Tree::depth() const {
    if (split_feature.empty()) return 0;
    int deepest = 0;
    std::vector<std::pair<int, int>> stack {{0, 1}};
    while (!stack.empty()) {
        auto [node, d] = stack.back();
        stack.pop_back();
        deepest = std::max(deepest, d);
        for (int child : {left_child[node], right_child[node]})
            if (child >= 0) stack.emplace_back(child, d + 1);
    }
    return deepest;
}

bool Tree::goes_left(int node, double x) const {
    int kind = decision_type[node];
    if (((kind >> 2) & 3) == MISSING_ZERO && -KZERO <= x && x <= KZERO) return (kind & 2) != 0;  // the default side
    return x <= threshold[node];
}

double Tree::walk(const std::vector<double>& x) const {
    if (split_feature.empty()) return leaf_value[0];
    int node = 0;
    for (;;) {
        int child = goes_left(node, x[split_feature[node]]) ? left_child[node] : right_child[node];
        if (child < 0) return leaf_value[~child];
        node = child;
    }
}

double Forest::predict(const std::vector<double>& x) const {
    double s = 0.0;
    for (const auto& tree : trees) s += tree.walk(x);
    return average_output && !trees.empty() ? s / static_cast<double>(trees.size()) : s;
}

bool Forest::default_names() const {
    for (size_t i = 0; i < feature_names.size(); ++i)
        if (feature_names[i] != std::format("Column_{}", i)) return false;
    return true;
}

double Forest::threshold_scale() const {
    std::vector<double> values;
    for (const auto& tree : trees)
        for (double t : tree.threshold)
            if (std::fabs(t) > KZERO) values.push_back(std::fabs(t));
    std::sort(values.begin(), values.end());
    return values.empty() ? 0.0 : values[values.size() / 2];
}

std::string Forest::describe() const {
    size_t leaves = 0;
    for (const auto& t : trees) leaves = std::max(leaves, t.leaf_value.size());
    return std::format("{} trees (up to {} leaves), objective {}{}", trees.size(), leaves, objective,
                       average_output ? ", averaged" : "");
}

std::shared_ptr<const Forest> parse_model(std::string_view text) {
    if (!starts_with(text, "tree\n"))
        throw std::invalid_argument("not a LightGBM model: the text format starts with a 'tree' line");
    auto split = text.find("\nTree=");
    std::string_view head = split == std::string_view::npos ? text : text.substr(0, split);
    std::string_view rest = split == std::string_view::npos ? std::string_view() : text.substr(split + 6);
    std::map<std::string, std::string, std::less<>> header;
    std::set<std::string, std::less<>> flags;
    auto lines = splitlines(head);
    for (size_t i = 1; i < lines.size(); ++i) {
        auto eq = lines[i].find('=');
        if (eq != std::string_view::npos) header[std::string(lines[i].substr(0, eq))] = std::string(lines[i].substr(eq + 1));
        else if (!strip(lines[i]).empty()) flags.emplace(strip(lines[i]));
    }
    auto get = [&](std::string_view key, std::string_view fallback) {
        auto it = header.find(key);
        return it == header.end() ? std::string(fallback) : it->second;
    };
    if (parse_int(get("num_class", "1")) != 1 || parse_int(get("num_tree_per_iteration", "1")) != 1)
        throw std::invalid_argument(
            "the LightGBM model has several outputs (classes): only one regression output is supported");
    auto objective = split_ws(get("objective", ""));
    bool sqrt = std::find(objective.begin() + (objective.empty() ? 0 : 1), objective.end(), "sqrt") != objective.end();
    if (objective.empty() || !IDENTITY_OBJECTIVES.contains(objective[0]) || sqrt) {
        std::string named = join(objective, " ");
        throw std::invalid_argument(std::format(
            "the LightGBM model's objective {} transforms its score: only regression-type objectives (regression, "
            "huber, quantile...) are supported",
            named.empty() ? "(none)" : named));
    }
    auto names = split_ws(get("feature_names", ""));
    long long n_features = parse_int(get("max_feature_idx", "-1")) + 1;
    if (names.empty() || static_cast<long long>(names.size()) != n_features)
        throw std::invalid_argument("the LightGBM model does not list its features (feature_names, max_feature_idx)");
    auto forest = std::make_shared<Forest>();
    if (!rest.empty()) {
        std::string body = "Tree=" + std::string(rest);
        if (auto end = body.find("end of trees"); end != std::string::npos) body.resize(end);
        std::vector<std::string_view> blocks;
        std::string_view b = body;
        size_t at = 0;
        bool first = true;
        while (at <= b.size()) {
            auto next = b.find("Tree=", at);
            if (!first) blocks.push_back(b.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
            first = false;
            if (next == std::string_view::npos) break;
            at = next + 5;
        }
        for (size_t i = 0; i < blocks.size(); ++i) forest->trees.push_back(read_tree(blocks[i], static_cast<int>(n_features), i));
    }
    if (forest->trees.empty()) throw std::invalid_argument("the LightGBM model has no trees");
    forest->feature_names = std::move(names);
    forest->objective = objective[0];
    forest->average_output = flags.contains("average_output");
    return forest;
}

std::string text_from_pickle(std::string_view data) {
    std::optional<std::string> found;
    try {
        PickleScanner(data).scan([&found](std::string arg) {
            if (starts_with(arg, "tree\n") && (!found || arg.size() > found->size())) found = std::move(arg);
        });
    } catch (const std::exception& exc) {  // malformed or truncated: say so, whatever tripped
        throw std::invalid_argument(std::string("not a readable pickle: ") + exc.what());
    }
    if (!found) throw std::invalid_argument("no LightGBM model inside this pickle");
    return *found;
}

std::shared_ptr<const Forest> load_model(const std::string& path) {
    static std::map<std::tuple<std::string, int64_t, int64_t>, std::shared_ptr<const Forest>> loaded;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        int err = errno;
        throw std::runtime_error(std::format("[Errno {}] {}: {}", err, std::strerror(err), pyrepr(std::string_view(path))));
    }
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(path, ec);
    int64_t mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
    auto key = std::make_tuple(ec ? path : resolved.string(), mtime_ns, static_cast<int64_t>(st.st_size));
    if (auto it = loaded.find(key); it != loaded.end()) return it->second;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        int err = errno;
        throw std::runtime_error(std::format("[Errno {}] {}: {}", err, std::strerror(err), pyrepr(std::string_view(path))));
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string data = buffer.str();
    std::shared_ptr<const Forest> forest;
    if (starts_with(data, "tree\n")) {
        forest = parse_model(data);
    } else if (!data.empty() && static_cast<unsigned char>(data[0]) == 0x80) {  // a pickle, protocol 2 or later
        forest = parse_model(text_from_pickle(data));
    } else {
        throw std::invalid_argument(path + ": neither a LightGBM model in its text format nor a pickle holding one");
    }
    if (loaded.size() >= 8) loaded.clear();
    loaded[key] = forest;
    return forest;
}

}  // namespace nmm::lgbm
