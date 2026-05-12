#include <gtest/gtest.h>

#include "code/symbol.h"

namespace {

using vectis::code::symbol_kind_from_name;
using vectis::code::symbol_kind_name;
using vectis::code::SymbolKind;

TEST(SymbolKindTest, NameRoundTripCoversAllValues)
{
    constexpr SymbolKind all[] = {
        SymbolKind::Function, SymbolKind::Method,    SymbolKind::Class,
        SymbolKind::Struct,   SymbolKind::Interface, SymbolKind::Enum,
        SymbolKind::Type,     SymbolKind::Namespace, SymbolKind::Manifest,
    };
    for (SymbolKind k : all) {
        EXPECT_EQ(symbol_kind_from_name(symbol_kind_name(k)), k)
            << "round-trip failed for " << symbol_kind_name(k);
    }
}

TEST(SymbolKindTest, ManifestNameAndRoundTrip)
{
    EXPECT_EQ(symbol_kind_name(SymbolKind::Manifest), "manifest");
    EXPECT_EQ(symbol_kind_from_name("manifest"), SymbolKind::Manifest);
}

TEST(SymbolKindTest, UnknownNameAndReverse)
{
    EXPECT_EQ(symbol_kind_name(SymbolKind::Unknown), "unknown");
    EXPECT_EQ(symbol_kind_from_name("not-a-kind"), SymbolKind::Unknown);
}

} // namespace
