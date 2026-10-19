#include "trading/credentials.hpp"

#include <openssl/evp.h>
#include <sys/stat.h>

#include <cstdlib>
#include <format>
#include <regex>
#include <vector>

#include "tomlplusplus/toml.hpp"
#include "util/pyfmt.hpp"

namespace nmm {

struct ArcusCredentials::Key {
    EVP_PKEY* pkey = nullptr;
    ~Key() {
        if (pkey != nullptr) EVP_PKEY_free(pkey);
    }
};

namespace {

std::vector<unsigned char> from_hex(const std::string& hex) {
    std::vector<unsigned char> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<unsigned char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

bool is_hex(const std::string& s, size_t n) {
    return s.size() == n && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

std::shared_ptr<ArcusCredentials::Key> key_from_hex(const std::string& hex) {
    auto raw = from_hex(hex);
    auto key = std::make_shared<ArcusCredentials::Key>();
    key->pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, raw.data(), raw.size());
    if (key->pkey == nullptr) throw CredentialsError("not an Ed25519 private key");
    return key;
}

std::string public_hex(const ArcusCredentials::Key& key) {
    unsigned char pub[32];
    size_t len = sizeof pub;
    if (EVP_PKEY_get_raw_public_key(key.pkey, pub, &len) != 1) throw CredentialsError("cannot derive the public key");
    return to_hex(pub, len);
}

std::string expand_user(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : "") + path.substr(1);
}

}  // namespace

std::string ArcusCredentials::sign(std::string_view message) const {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    unsigned char sig[64];
    size_t len = sizeof sig;
    bool ok = ctx != nullptr && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key_->pkey) == 1 &&
              EVP_DigestSign(ctx, sig, &len, reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("Ed25519 signing failed");
    return to_hex(sig, len);
}

std::string ArcusCredentials::str() const {
    return std::format("{} subaccount {}, key {}…", address, account_index, api_key.substr(0, 8));
}

ArcusCredentials ArcusCredentials::make(std::string address, int account_index, const std::string& signing_key_hex) {
    ArcusCredentials c;
    c.key_ = key_from_hex(signing_key_hex);
    c.address = lower(address);
    c.account_index = account_index;
    c.api_key = public_hex(*c.key_);
    return c;
}

std::string ArcusCredentials::public_key_of(const std::string& signing_key_hex) { return public_hex(*key_from_hex(signing_key_hex)); }

std::string resolve_credentials_path(std::optional<std::string> cli_path) {
    if (cli_path && !cli_path->empty()) return expand_user(*cli_path);
    if (const char* env = std::getenv(CREDENTIALS_ENV_VAR); env != nullptr && *env) return expand_user(env);
    return expand_user(CREDENTIALS_DEFAULT_PATH);
}

ArcusCredentials load_credentials(const std::string& raw_path) {
    std::string path = expand_user(raw_path);
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        throw CredentialsError(std::format("credentials file {} not found; see credentials.example.toml", path));
    if (st.st_mode & 077) throw CredentialsError(std::format("{} is readable by group or others; run: chmod 600 {}", path, path));
    toml::table data;
    try {
        data = toml::parse_file(path);
    } catch (const toml::parse_error& err) {
        throw CredentialsError(std::format("{}: not valid TOML: {}", path, err.description()));
    }
    const toml::table* section = data["arcus"].as_table();
    if (section == nullptr) throw CredentialsError(std::format("{}: missing [arcus] table", path));

    static const std::regex address_re("^0x[0-9a-fA-F]{40}$");
    auto address = (*section)["address"].value<std::string>();
    if (!address || !(*section)["address"].is_string() || !std::regex_match(*address, address_re))
        throw CredentialsError(std::format("{}: arcus.address must be a 0x-prefixed 40-hex address", path));
    int64_t account_index = 0;
    if (auto node = (*section)["account_index"]; node) {
        if (!node.is_integer()) throw CredentialsError(std::format("{}: arcus.account_index must be the subaccount to trade, 0-9", path));
        account_index = *node.value<int64_t>();
    }
    if (account_index < 0 || account_index > 9)
        throw CredentialsError(std::format("{}: arcus.account_index must be the subaccount to trade, 0-9", path));
    std::string api_key, secret;
    for (auto [name, out] : {std::pair<const char*, std::string*> {"api_key", &api_key}, {"signing_key", &secret}}) {
        auto node = (*section)[name];
        if (!node.is_string() || !is_hex(*node.value<std::string>(), 64))
            throw CredentialsError(std::format("{}: arcus.{} must be 64 hex characters", path, name));
        *out = *node.value<std::string>();
    }
    ArcusCredentials creds;
    try {
        creds = ArcusCredentials::make(*address, static_cast<int>(account_index), secret);
    } catch (const CredentialsError&) {
        throw CredentialsError(std::format("{}: arcus.signing_key must be 64 hex characters", path));
    }
    if (creds.api_key != lower(api_key)) throw CredentialsError(std::format("{}: arcus.signing_key does not belong to arcus.api_key", path));
    return creds;
}

}  // namespace nmm
