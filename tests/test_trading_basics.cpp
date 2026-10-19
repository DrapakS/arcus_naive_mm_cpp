// Ported from tests/test_trading_basics.py: credentials, request signing, market arithmetic.
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#include "trading/credentials.hpp"
#include "trading/market.hpp"
#include "trading/signing.hpp"
#include "util/pyfmt.hpp"

using namespace nmm;
namespace fs = std::filesystem;

namespace {

// RFC 8032 test 1 key pair and the EIP-55 example address: public test values.
const std::string SECRET = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
const std::string PUBLIC = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const std::string ADDRESS = "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed";

MarketSpec btc() {
    return MarketSpec::from_json(parse_json(R"({
        "marketId": 1, "marketDisplayName": "BTC-USD", "tickSize": "0.1", "stepSize": "0.00000001",
        "tickTiers": [{"upToPrice": "500000", "tick": "0.1"}, {"upToPrice": "1000000", "tick": "0.2"}, {"tick": "5"}],
        "minOrderSize": "0.0001", "minOrderNotional": "5", "maxOrderSize": "10000"})"));
}

struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/nmm-test-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() { fs::remove_all(path); }
};

fs::path write_credentials(const fs::path& dir, mode_t mode = 0600, std::map<std::string, std::string> overrides = {}) {
    std::map<std::string, std::string> values {{"address", "\"" + ADDRESS + "\""}, {"account_index", "1"},
                                               {"api_key", "\"" + PUBLIC + "\""}, {"signing_key", "\"" + SECRET + "\""}};
    for (auto& [k, v] : overrides) values[k] = v;
    fs::path path = dir / "credentials.toml";
    std::ofstream out(path);
    out << "[arcus]\n";
    for (auto& [k, v] : values) out << k << " = " << v << "\n";
    out.close();
    chmod(path.c_str(), mode);
    return path;
}

bool verify(const std::string& public_hex, const std::string& message, const std::string& signature_hex) {
    auto bytes = [](const std::string& hex) {
        std::vector<unsigned char> out;
        for (size_t i = 0; i < hex.size(); i += 2) out.push_back(static_cast<unsigned char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        return out;
    };
    auto pub = bytes(public_hex), sig = bytes(signature_hex);
    EVP_PKEY* key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
              EVP_DigestVerify(ctx, sig.data(), sig.size(), reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

}  // namespace

TEST(TradingBasics, LoadsValidCredentials) {
    TempDir dir;
    auto creds = load_credentials(write_credentials(dir.path));
    EXPECT_EQ(creds.address, lower(ADDRESS));
    EXPECT_EQ(creds.account_index, 1);
    EXPECT_EQ(creds.api_key, PUBLIC);
    EXPECT_EQ(creds.str().find(SECRET), std::string::npos);
    EXPECT_TRUE(verify(PUBLIC, "hello", creds.sign("hello")));
}

TEST(TradingBasics, RejectsBadCredentials) {
    struct Case {
        mode_t mode;
        std::map<std::string, std::string> overrides;
        std::string message;
    };
    std::vector<Case> cases {
        {0644, {}, "readable by group or others"},
        {0600, {{"api_key", "\"" + std::string(64, '0') + "\""}}, "does not belong"},
        {0600, {{"address", "\"0x1234\""}}, "address"},
        {0600, {{"account_index", "255"}}, "account_index"},
        {0600, {{"signing_key", "\"" + std::string(64, 'z') + "\""}}, "signing_key"},
    };
    for (const auto& c : cases) {
        TempDir dir;
        try {
            load_credentials(write_credentials(dir.path, c.mode, c.overrides));
            ADD_FAILURE() << "accepted: " << c.message;
        } catch (const CredentialsError& err) {
            EXPECT_NE(std::string(err.what()).find(c.message), std::string::npos) << err.what();
        }
    }
}

TEST(TradingBasics, MissingFileAndPathResolution) {
    TempDir dir;
    try {
        load_credentials((dir.path / "nope.toml").string());
        ADD_FAILURE();
    } catch (const CredentialsError& err) {
        EXPECT_NE(std::string(err.what()).find("not found"), std::string::npos);
    }
    setenv("NAIVE_MM_CREDENTIALS", (dir.path / "env.toml").c_str(), 1);
    EXPECT_EQ(fs::path(resolve_credentials_path("cli.toml")).filename(), "cli.toml");
    EXPECT_EQ(fs::path(resolve_credentials_path()).filename(), "env.toml");
    unsetenv("NAIVE_MM_CREDENTIALS");
    EXPECT_TRUE(ends_with(resolve_credentials_path(), ".config/naive_mm/credentials.toml"));
}

TEST(TradingBasics, PlaceOrderMessageMatchesDocumentedLayout) {
    // The guide builds this exact string by hand (docs.arcus.xyz, "Trade over WebSocket").
    int64_t ts = 1712345678000000000, good_til = 4102444800000000;
    MarketSpec m = btc();
    std::string expected = std::format(
        R"({{"ad":"{}","ai":1,"c":"nmm-1","ct":{},"g":{},"m":1,"op":1,"p":{},"q":{},"r":0,"s":0,"t":3,"v":1}})", lower(ADDRESS), ts,
        good_til * 1000, m.price_ticks(Decimal("83569.9")), m.size_quantums(Decimal("0.001")));
    PlaceOrderFields f;
    f.address = ADDRESS;
    f.account_index = 1;
    f.market_id = 1;
    f.side = "BUY";
    f.price_ticks = 835699;
    f.size_quantums = 100000;
    f.time_in_force = "ALO";
    f.good_til_us = good_til;
    f.client_id = "nmm-1";
    f.timestamp_ns = ts;
    EXPECT_EQ(place_order_message(f), expected);
}

TEST(TradingBasics, CancelAndLegacyMessages) {
    EXPECT_EQ(cancel_order_message(ADDRESS, 0, 1, 5, std::string("nmm-1")),
              R"({"ad":")" + lower(ADDRESS) + R"(","ai":0,"c":"nmm-1","ct":5,"m":1,"op":2,"v":1})");
    EXPECT_TRUE(ends_with(cancel_order_message(ADDRESS, 0, 1, 5, std::nullopt, std::string("ab12")), R"("ct":5,"id":"ab12","m":1,"op":2,"v":1})"));
    EXPECT_THROW(cancel_order_message(ADDRESS, 0, 1, 5), std::invalid_argument);
    EXPECT_EQ(legacy_message(7, "scheduleCancel", JsonObject {{"time", 9}, {"address", "0xab"}, {"accountIndex", 0}}),
              R"(7scheduleCancel{"accountIndex":0,"address":"0xab","time":9})");
}

TEST(TradingBasics, MarketRoundingAndUnits) {
    MarketSpec m = btc();
    EXPECT_EQ(m.floor_price(83569.96), Decimal("83569.9"));
    EXPECT_EQ(m.ceil_price(83569.91), Decimal("83570.0"));
    EXPECT_EQ(m.ceil_price(Decimal("83570.0")), Decimal("83570.0"));
    EXPECT_EQ(m.tick_at(Decimal("600000")), Decimal("0.2"));
    EXPECT_EQ(m.tick_at(Decimal("2000000")), Decimal("5"));
    EXPECT_EQ(m.floor_price(600000.3), Decimal("600000.2"));
    EXPECT_EQ(m.price_ticks(Decimal("83569.9")), 835699);
    EXPECT_EQ(m.size_quantums(Decimal("0.001")), 100000);
    EXPECT_EQ(m.floor_size(Decimal("0.123456789")), Decimal("0.12345678"));
    EXPECT_THROW(m.price_ticks(Decimal("83569.95")), std::invalid_argument);
    EXPECT_EQ(fmt(Decimal("83570.0")), "83570");
    EXPECT_EQ(fmt(Decimal("0.00100")), "0.001");
}

TEST(TradingBasics, SignatureIsDeterministic) {
    auto creds = ArcusCredentials::make(ADDRESS, 1, SECRET);
    std::string message = R"({"ad":"0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed","ai":1,"c":"nmm1","ct":1,"m":1,"op":2,"v":1})";
    EXPECT_EQ(creds.sign(message), creds.sign(message));
    // RFC 8032 test 1: the empty message
    EXPECT_EQ(creds.sign(""), "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
}
