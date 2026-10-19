#include <gtest/gtest.h>

#include "util/decimal.hpp"
#include "util/pyfmt.hpp"

using nmm::Decimal;

TEST(Decimal, ParsesAndPrintsPlain) {
    EXPECT_EQ(Decimal("83191.50").str(), "83191.5");
    EXPECT_EQ(Decimal("-0.0001202").str(), "-0.0001202");
    EXPECT_EQ(Decimal("1e-05").str(), "0.00001");
    EXPECT_EQ(Decimal("1.5E+3").str(), "1500");
    EXPECT_EQ(Decimal("100").str(), "100");
    EXPECT_EQ(Decimal("0").str(), "0");
    EXPECT_FALSE(Decimal::try_parse("abc"));
    EXPECT_FALSE(Decimal::try_parse(""));
}

TEST(Decimal, ArithmeticIsExact) {
    EXPECT_EQ((Decimal("0.1") + Decimal("0.2")).str(), "0.3");
    EXPECT_EQ((Decimal("0.0001202") * Decimal("83191.5")).str(), "9.9996183");
    EXPECT_EQ((Decimal("1") / Decimal("3")).str(), "0.333333333333333333");
    EXPECT_EQ((Decimal("2") / Decimal("3")).str(), "0.666666666666666667");
    EXPECT_EQ((Decimal("-7") / Decimal("2")).str(), "-3.5");
    EXPECT_EQ((Decimal("83191.53") * Decimal("0.9995")).str(), "83149.934235");
    EXPECT_TRUE(Decimal("0.001") > 0);
    EXPECT_EQ(Decimal(5) * Decimal("0.2"), Decimal(1));
}

TEST(Decimal, RoundsToUnits) {
    Decimal tick("0.1");
    EXPECT_EQ(Decimal("83191.57").floor_to(tick).str(), "83191.5");
    EXPECT_EQ(Decimal("83191.51").ceil_to(tick).str(), "83191.6");
    EXPECT_EQ(Decimal("-1.25").floor_to(tick).str(), "-1.3");
    EXPECT_EQ(Decimal("-1.25").ceil_to(tick).str(), "-1.2");
    EXPECT_EQ(Decimal("83191.5").units(tick), 831915);
    EXPECT_THROW(Decimal("83191.55").units(tick), std::invalid_argument);
    EXPECT_EQ(Decimal("2.5").round_int(), 2);
    EXPECT_EQ(Decimal("3.5").round_int(), 4);
}

TEST(Decimal, FormatsFixedHalfEven) {
    EXPECT_EQ(Decimal("1.005").fixed(2), "1.00");
    EXPECT_EQ(Decimal("1.015").fixed(2), "1.02");
    EXPECT_EQ(Decimal("0.0012").fixed(4, true), "+0.0012");
    EXPECT_EQ(Decimal("-0.001").fixed(2), "-0.00");
    EXPECT_EQ(Decimal(0).fixed(4, true), "+0.0000");
}

TEST(Decimal, FromDoubleTakesTheShortestRepr) {
    EXPECT_EQ(Decimal::from_double(83124.44).str(), "83124.44");
    EXPECT_EQ(Decimal::from_double(0.1).str(), "0.1");
    EXPECT_EQ(Decimal::from_double(1e-05).str(), "0.00001");
    EXPECT_DOUBLE_EQ(Decimal("83191.5").to_double(), 83191.5);
}

TEST(PyFmt, ReprLikePython) {
    EXPECT_EQ(nmm::pyrepr(83121.0), "83121.0");
    EXPECT_EQ(nmm::pyrepr(83121.3), "83121.3");
    EXPECT_EQ(nmm::pyrepr(1e-05), "1e-05");
    EXPECT_EQ(nmm::pyrepr(0.0001), "0.0001");
    EXPECT_EQ(nmm::pyrepr(1e16), "1e+16");
    EXPECT_EQ(nmm::pyrepr(1e15), "1000000000000000.0");
    EXPECT_EQ(nmm::pyrepr(0.1 + 0.2), "0.30000000000000004");
    EXPECT_EQ(nmm::pyrepr(-2.5), "-2.5");
    EXPECT_EQ(nmm::thousands(1234567.4), "1,234,567");
    EXPECT_EQ(nmm::thousands(720.0), "720");
}
