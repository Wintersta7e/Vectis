#include "code/hotspot_detector.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/language.h"
#include "code/symbol.h"

namespace {

using vectis::code::CodeIndex;
using vectis::code::Dependency;
using vectis::code::detect_hotspots;
using vectis::code::FileEntry;
using vectis::code::Hotspot;
using vectis::code::HotspotThresholds;
using vectis::code::Language;
using vectis::code::Symbol;
using vectis::code::SymbolKind;

std::int64_t add_file(CodeIndex& idx, const std::string& path, int lines = 0)
{
    FileEntry f;
    f.path_relative = path;
    f.language      = Language::Cpp;
    f.line_count    = lines;
    return idx.add_file(std::move(f));
}

void add_method(CodeIndex& idx, std::int64_t file_id,
                const std::string& name, int complexity)
{
    Symbol s;
    s.file_id    = file_id;
    s.name       = name;
    s.kind       = SymbolKind::Method;
    s.complexity = complexity;
    idx.add_symbols(std::array<Symbol, 1>{s});
}

TEST(HotspotDetectorTest, HighComplexityFunction_Flagged)
{
    CodeIndex idx;
    const auto file = add_file(idx, "heavy.cpp", 100);
    add_method(idx, file, "simple",    3);   // below threshold
    add_method(idx, file, "gnarly",    42);  // major hotspot

    const auto hotspots = detect_hotspots(idx);
    ASSERT_FALSE(hotspots.empty());

    // The first one should be the high-complexity method.
    bool saw_gnarly = false;
    for (const auto& h : hotspots) {
        if (h.reason.find("gnarly") != std::string::npos) {
            saw_gnarly = true;
            EXPECT_GE(h.severity, 2);
            EXPECT_NE(h.reason.find("42"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_gnarly);
}

TEST(HotspotDetectorTest, LargeFile_Flagged)
{
    CodeIndex idx;
    add_file(idx, "small.cpp", 100);
    const auto big = add_file(idx, "huge.cpp", 1500);

    const auto hotspots = detect_hotspots(idx);

    bool saw_huge = false;
    for (const auto& h : hotspots) {
        if (h.file_id == big) {
            saw_huge = true;
            EXPECT_EQ(h.symbol_id, 0);
            EXPECT_NE(h.reason.find("large file"), std::string::npos);
            EXPECT_NE(h.reason.find("1500"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_huge);
}

TEST(HotspotDetectorTest, HighFanOut_Flagged)
{
    CodeIndex idx;
    const auto hub = add_file(idx, "hub.cpp", 50);
    // Create a bunch of target files and edges so fan-out = 20.
    for (int i = 0; i < 20; ++i) {
        const auto target = add_file(idx, "target" + std::to_string(i) + ".cpp", 10);
        Dependency dep;
        dep.source_file_id = hub;
        dep.target_file_id = target;
        dep.kind           = "include";
        idx.add_dependency(std::move(dep));
    }

    const auto hotspots = detect_hotspots(idx);
    bool saw_hub = false;
    for (const auto& h : hotspots) {
        if (h.file_id == hub && h.reason.find("fan-out") != std::string::npos) {
            saw_hub = true;
            EXPECT_NE(h.reason.find("20"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_hub);
}

TEST(HotspotDetectorTest, BelowThresholds_NoHotspots)
{
    CodeIndex idx;
    const auto f = add_file(idx, "tiny.cpp", 50);
    add_method(idx, f, "ok", 5);

    const auto hotspots = detect_hotspots(idx);
    EXPECT_TRUE(hotspots.empty());
}

TEST(HotspotDetectorTest, SortedBySeverityDescending)
{
    CodeIndex idx;
    const auto minor_file = add_file(idx, "minor.cpp", 501);   // just-over
    const auto major_file = add_file(idx, "major.cpp", 3000);  // 3x over
    (void)minor_file;
    (void)major_file;

    const auto hotspots = detect_hotspots(idx);
    ASSERT_GE(hotspots.size(), 2U);
    // First hotspot should have severity >= last hotspot's severity.
    EXPECT_GE(hotspots.front().severity, hotspots.back().severity);
}

} // namespace
