// Arcus API credentials, read from a TOML file kept outside the repository.
//
// The file holds the wallet address, the subaccount and the Ed25519 API key pair
// (see credentials.example.toml). Only the Ed25519 signing key is secret: it
// can place and cancel orders but not withdraw, which needs the wallet itself.
#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nmm {

inline constexpr const char* CREDENTIALS_ENV_VAR = "NAIVE_MM_CREDENTIALS";
inline constexpr const char* CREDENTIALS_DEFAULT_PATH = "~/.config/naive_mm/credentials.toml";

class CredentialsError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

class ArcusCredentials {
public:
    std::string address;  // lowercase 0x-hex, as Arcus signs it
    int account_index = 0;  // subaccount to trade, 0-9
    std::string api_key;  // Ed25519 public key, 64 hex

    // The Ed25519 signature of `message`, hex.
    std::string sign(std::string_view message) const;
    // "0x... subaccount 1, key d75a9801…": never the secret.
    std::string str() const;

    // From a 32-byte private key (hex); throws CredentialsError on a bad key.
    static ArcusCredentials make(std::string address, int account_index, const std::string& signing_key_hex);
    // The public key (hex) of a private key (hex).
    static std::string public_key_of(const std::string& signing_key_hex);

    struct Key;

private:
    std::shared_ptr<Key> key_;
};

// --credentials, else $NAIVE_MM_CREDENTIALS, else the default path; "~" expanded.
std::string resolve_credentials_path(std::optional<std::string> cli_path = {});

ArcusCredentials load_credentials(const std::string& path);

}  // namespace nmm
