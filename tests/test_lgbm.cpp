#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "lgbm.hpp"

using namespace nmm::lgbm;

namespace {

const std::string DATA = std::string(NMM_SOURCE_DIR) + "/tests/data";
const std::string SMALL = DATA + "/lgbm_zero_missing.txt";  // 4 trees, zero-as-missing splits with both default sides (LightGBM 4.7)
// LightGBM's own predictions (Booster.predict) for these rows of the small model
const std::vector<std::vector<double>> SMALL_X = {{0.0, 0.0, 0.0, 0.0}, {1.0, 0.0, -1.0, 2.0}, {-0.5, 0.3, 0.0, 0.0},
                                                  {2.0, -1.0, 1.5, -2.0}, {1e-36, -1e-36, 0.2, 0.0}, {0.1, 1e-30, -0.2, 0.05}};
const std::vector<double> SMALL_Y = {0.7046799658585134, 1.9364597433928028, -1.0602114678626007, 1.107381692549305,
                                     0.7046799658585134, -0.28716535758507533};
// the owner's BTC model and LightGBM's predictions for these (scaled) rows
const std::string BIG = std::string(NMM_SOURCE_DIR) + "/models/btc_lgbm_v1.txt";
const std::vector<std::vector<double>> BIG_X = {
    std::vector<double>(16, 0.0),
    {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -1.90117, 0.406897, 0.235127, -0.280396, -3.77514, -0.808039, -0.072751, 0.169963,
     -2.295204, -0.71663},
    {-1.467779, -1.213256, 1.591348, -1.211302, -0.048783, 1.326585, -0.875401, -0.167553, 0.165696, 0.095673,
     -1.837584, 0.11421, 2.038235, -2.320717, 1.289074, 0.179031},
    {-0.962206, 3.000625, 1.14339, -1.798933, 0.111774, 0.865034, -0.283173, 1.024365, -0.099776, 1.000871, 2.157784,
     -1.013493, 0.304708, -0.694961, 0.190903, -1.780792},
};
const std::vector<double> BIG_Y = {-6.296538085445405e-06, -0.00010589119339145796, -0.00011214630734653571,
                                   0.0002140411343791176};

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void write_file(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out << data;
}

std::vector<double> predictions(const Forest& forest, const std::vector<std::vector<double>>& rows) {
    std::vector<double> out;
    for (const auto& x : rows) out.push_back(forest.predict(x));
    return out;
}

std::string replace_once(std::string text, const std::string& old, const std::string& with) {
    auto at = text.find(old);
    if (at != std::string::npos) text.replace(at, old.size(), with);
    return text;
}

// The message of the std::invalid_argument `fn` throws (fails the test when it throws none).
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

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() / ("nmm_lgbm_" + std::to_string(::getpid()) + "_" + std::to_string(rand()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    std::string operator/(const std::string& name) const { return (path / name).string(); }
};

}  // namespace

TEST(Lgbm, TheTreesGiveLightgbmsOwnPredictionsToTheLastBit) {
    auto small = load_model(SMALL);
    EXPECT_EQ(small->n_features(), 4u);
    EXPECT_EQ(small->trees.size(), 4u);
    EXPECT_EQ(small->objective, "regression");
    EXPECT_EQ(predictions(*small, SMALL_X), SMALL_Y);  // zero (|x| <= 1e-35) goes to each node's default side
    auto big = load_model(BIG);
    EXPECT_EQ(big->n_features(), 16u);
    EXPECT_EQ(big->trees.size(), 200u);
    EXPECT_TRUE(big->default_names());
    EXPECT_EQ(predictions(*big, BIG_X), BIG_Y);
    EXPECT_NE(big->describe().find("200 trees (up to 31 leaves), objective regression"), std::string::npos);
    EXPECT_GT(big->threshold_scale(), 0.1);  // fit on scaled features: splits at values of ~1
    EXPECT_LT(big->threshold_scale(), 10.0);
}

TEST(Lgbm, EveryTreeIsWalkedWithTheSameRule) {
    auto small = parse_model(read_file(SMALL));
    EXPECT_EQ(predictions(*small, SMALL_X), SMALL_Y);
    auto big = parse_model(read_file(BIG));
    EXPECT_EQ(predictions(*big, BIG_X), BIG_Y);
    // a tree walked node by node gives the same leaf as the sum of the forest
    for (const auto& x : BIG_X) {
        double s = 0.0;
        for (const auto& tree : big->trees) s += tree.walk(x);
        EXPECT_EQ(s, big->predict(x));
    }
    EXPECT_GT(big->trees[0].depth(), 1);
}

TEST(Lgbm, TheModelIsTakenOutOfAPickleWithoutUnpicklingIt) {
    TempDir tmp;
    std::string text = read_file(SMALL);
    // what a Booster or an LGBMRegressor pickles: the model text among other state (pickle.dumps, protocol 5)
    std::string data = read_file(DATA + "/lgbm_zero_missing.pk");
    EXPECT_EQ(text_from_pickle(data), text);
    EXPECT_EQ(text_from_pickle(read_file(DATA + "/lgbm_zero_missing_p0.pk")), text);  // protocol 0: escaped text
    EXPECT_EQ(text_from_pickle(read_file(DATA + "/lgbm_zero_missing_bytes.pk")), text);  // the model as bytes
    std::string path = tmp / "model.pk";
    write_file(path, data);
    EXPECT_EQ(predictions(*load_model(path), SMALL_X), SMALL_Y);
    // a pickle that would run code when loaded is only read: nothing in it runs
    std::string ran = tmp / "ran";
    std::string evil = std::string("\x80\x04" "cos\nsystem\n(S'touch ") + ran + "'\ntR.";
    EXPECT_NE(value_error([&] { text_from_pickle(evil); }).find("no LightGBM model"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(ran));
    write_file(path, "\x80\x04garbage");
    std::string why = value_error([&] { load_model(path); });
    EXPECT_TRUE(why.find("not a readable pickle") != std::string::npos || why.find("no LightGBM model") != std::string::npos) << why;
    write_file(path, "not a model");
    EXPECT_NE(value_error([&] { load_model(path); }).find("neither a LightGBM model"), std::string::npos);
    try {
        load_model(tmp / "none.txt");
        ADD_FAILURE() << "no error for a missing file";
    } catch (const std::runtime_error& exc) {
        EXPECT_NE(std::string(exc.what()).find("No such file"), std::string::npos) << exc.what();
    }
}

TEST(Lgbm, ModelsItCannotPriceWithAreRefused) {
    std::string text = read_file(SMALL);
    struct Case {
        std::string old_, new_, match;
    };
    for (const auto& c : std::vector<Case> {
             {"objective=regression", "objective=binary sigmoid:1", "transforms its score"},
             {"objective=regression", "objective=regression sqrt", "transforms its score"},
             {"objective=regression", "objective=poisson", "transforms its score"},
             {"num_class=1", "num_class=3", "several outputs"},
             {"num_cat=0", "num_cat=1", "categorical splits"},
             {"is_linear=0", "is_linear=1", "linear tree"},
             {"max_feature_idx=3", "max_feature_idx=7", "does not list its features"},
         }) {
        std::string why = value_error([&] { parse_model(replace_once(text, c.old_, c.new_)); });
        EXPECT_NE(why.find(c.match), std::string::npos) << c.new_ << ": " << why;
    }
    EXPECT_NE(value_error([] { parse_model("version=v4\n"); }).find("not a LightGBM model"), std::string::npos);
    auto first = text.find("decision_type=") + std::string("decision_type=").size();
    std::string bad = text.substr(0, first) + "5" + text.substr(first + 1);  // bit 0 of a decision type: categorical
    EXPECT_NE(value_error([&] { parse_model(bad); }).find("categorical split"), std::string::npos);
}
