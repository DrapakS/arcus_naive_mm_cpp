// LightGBM models without the LightGBM library: the trees of a saved model, walked in C++.
//
// A model is read from LightGBM's text format (Booster.save_model, Booster.model_to_string) or
// from a pickle of a Booster or of a scikit-learn LGBMRegressor: such a pickle holds the model
// as that same text, one string, and it is taken out of the pickle's opcodes without unpickling
// it (unpickling runs whatever code the file names; this never does).
//
// Supported: one regression output that is the raw score (objectives regression, regression_l1,
// huber, fair, quantile, mape), numerical splits with any missing-value rule, average_output
// (random forest mode). Refused: categorical splits, linear trees, several classes, and
// objectives whose output transforms the score (binary, poisson, reg_sqrt...).
//
// Each tree is walked with LightGBM's own decision rule: for finite inputs the sum gives
// LightGBM's prediction to the last bit.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nmm::lgbm {

constexpr double KZERO = 1e-35;  // LightGBM's kZeroThreshold: |x| at most this counts as zero for "zero as missing"
constexpr int MISSING_NONE = 0, MISSING_ZERO = 1, MISSING_NAN = 2;

struct Tree {
    std::vector<int> split_feature;
    std::vector<double> threshold;
    std::vector<int> decision_type;
    std::vector<int> left_child;  // >= 0: an internal node; < 0: leaf ~child
    std::vector<int> right_child;
    std::vector<double> leaf_value;

    int depth() const;
    // LightGBM's NumericalDecision for a finite x.
    bool goes_left(int node, double x) const;
    double walk(const std::vector<double>& x) const;
};

class Forest {
public:
    std::vector<std::string> feature_names;
    std::vector<Tree> trees;
    std::string objective;
    bool average_output = false;

    // x: one finite value per feature, in feature_names order. The leaves summed in
    // tree order from 0.0 (over the number of trees when averaged).
    double predict(const std::vector<double>& x) const;
    size_t n_features() const { return feature_names.size(); }
    // Column_0, Column_1...: fit on an array, the model does not know its features' names.
    bool default_names() const;
    // The median size of the split values (zero splits aside): the scale of the inputs the model was fit on.
    double threshold_scale() const;
    std::string describe() const;
};

// A LightGBM model in its text format; std::invalid_argument on a bad or unsupported model.
std::shared_ptr<const Forest> parse_model(std::string_view text);

// The LightGBM model text inside a pickle (of a Booster or an LGBMRegressor), read from its
// opcodes (as pickletools.genops): nothing in the pickle is run. std::invalid_argument when
// it is no readable pickle or holds no model.
std::string text_from_pickle(std::string_view data);

// A LightGBM model file: the text format, or a pickle holding one. Read once per file
// version (path, mtime, size): the config check and the agent both ask. std::runtime_error
// when the file cannot be read, std::invalid_argument when it holds no usable model.
std::shared_ptr<const Forest> load_model(const std::string& path);

}  // namespace nmm::lgbm
