#include "code/hotspot_detector.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "code/code_index.h"
#include "code/dependency.h"
#include "code/symbol.h"

namespace vectis::code {

namespace {

/// Derive a 1–3 severity label from how far a value overshoots its
/// threshold. A value exactly at the threshold is a 1; 2× is a 2;
/// 3× or more is a 3.
[[nodiscard]] int severity_from_ratio(int value, int threshold) noexcept
{
    if (threshold <= 0 || value < threshold) {
        return 1;
    }
    const int ratio = value / threshold;
    if (ratio >= 3) {
        return 3;
    }
    if (ratio >= 2) {
        return 2;
    }
    return 1;
}

} // namespace

std::vector<Hotspot> detect_hotspots(const CodeIndex& index, HotspotThresholds thresholds)
{
    std::vector<Hotspot> hotspots;

    const std::vector<FileEntry> files = index.snapshot_files();

    // --- Function-level hotspots ----------------------------------
    // Walk every file's symbols and flag any function / method whose
    // cyclomatic complexity exceeds the threshold.
    for (const FileEntry& file : files) {
        const std::vector<Symbol> symbols = index.symbols_in_file(file.id);
        for (const Symbol& sym : symbols) {
            const bool is_function =
                (sym.kind == SymbolKind::Function || sym.kind == SymbolKind::Method);
            if (!is_function || sym.complexity <= thresholds.function_complexity) {
                continue;
            }
            Hotspot h;
            h.file_id = file.id;
            h.symbol_id = sym.id;
            h.symbol_name = sym.name;
            h.line = sym.line_start;
            h.kind = sym.kind;
            h.complexity = sym.complexity;
            h.severity = severity_from_ratio(sym.complexity, thresholds.function_complexity);
            h.trigger = HotspotTrigger::Complexity;
            h.reason = "high cyclomatic complexity (" + std::to_string(sym.complexity) + ") in '" +
                       sym.name + "'";
            hotspots.push_back(std::move(h));
        }
    }

    // --- File-level hotspots --------------------------------------
    // For each file, accumulate reasons across large-size, high-fan-out,
    // and high-fan-in triggers into a single Hotspot entry. `trigger`
    // records the dimension with the highest value-to-threshold ratio
    // so diversification can keep one dominant dimension from filling
    // the top-N.
    for (const FileEntry& file : files) {
        std::vector<std::string> reasons;
        int worst_severity = 0;
        int best_ratio_x100 = 0;
        Hotspot h;
        h.file_id = file.id;

        auto consider = [&](int value, int threshold, HotspotTrigger trig) {
            if (value <= 0 || threshold <= 0) {
                return;
            }
            const int r = (value * 100) / threshold;
            if (r > best_ratio_x100) {
                best_ratio_x100 = r;
                h.trigger = trig;
            }
        };

        if (file.line_count > thresholds.file_line_count) {
            reasons.push_back("large file (" + std::to_string(file.line_count) + " lines)");
            worst_severity = std::max(
                worst_severity, severity_from_ratio(file.line_count, thresholds.file_line_count));
            h.line_count = file.line_count;
            consider(file.line_count, thresholds.file_line_count, HotspotTrigger::Size);
        }

        const int fan_out = static_cast<int>(index.dependencies_of(file.id).size());
        if (fan_out > thresholds.file_fan_out) {
            reasons.push_back("high fan-out (" + std::to_string(fan_out) + " dependencies)");
            worst_severity =
                std::max(worst_severity, severity_from_ratio(fan_out, thresholds.file_fan_out));
            h.fan_out = fan_out;
            consider(fan_out, thresholds.file_fan_out, HotspotTrigger::FanOut);
        }

        const int fan_in = static_cast<int>(index.dependents_of(file.id).size());
        if (fan_in > thresholds.file_fan_in) {
            reasons.push_back("high fan-in (" + std::to_string(fan_in) + " dependents)");
            worst_severity =
                std::max(worst_severity, severity_from_ratio(fan_in, thresholds.file_fan_in));
            h.fan_in = fan_in;
            consider(fan_in, thresholds.file_fan_in, HotspotTrigger::FanIn);
        }

        if (reasons.empty()) {
            continue;
        }
        h.severity = worst_severity;
        h.reason = reasons[0];
        for (std::size_t i = 1; i < reasons.size(); ++i) {
            h.reason += "; ";
            h.reason += reasons[i];
        }
        hotspots.push_back(std::move(h));
    }

    // Sort by severity descending, then by reason for stable output.
    std::sort(hotspots.begin(), hotspots.end(), [](const Hotspot& a, const Hotspot& b) {
        if (a.severity != b.severity) {
            return a.severity > b.severity;
        }
        return a.reason < b.reason;
    });
    return hotspots;
}

std::vector<Hotspot> diversify_top_n(std::vector<Hotspot> all, std::size_t cap)
{
    if (all.size() <= cap) {
        return all;
    }
    const std::size_t quota = std::max<std::size_t>(1, cap / 4);
    std::vector<bool> taken(all.size(), false);
    std::vector<Hotspot> picked;
    picked.reserve(cap);
    auto take_from = [&](HotspotTrigger t) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < all.size() && count < quota && picked.size() < cap; ++i) {
            if (taken[i] || all[i].trigger != t) {
                continue;
            }
            picked.push_back(all[i]);
            taken[i] = true;
            ++count;
        }
    };
    take_from(HotspotTrigger::Complexity);
    take_from(HotspotTrigger::FanIn);
    take_from(HotspotTrigger::FanOut);
    take_from(HotspotTrigger::Size);
    // Fill the remaining slots from the global severity-sorted leftover.
    for (std::size_t i = 0; i < all.size() && picked.size() < cap; ++i) {
        if (!taken[i]) {
            picked.push_back(all[i]);
        }
    }
    // The per-bucket walk interleaves severities, so re-sort to keep the
    // slim consumer's view severity-desc.
    std::sort(picked.begin(), picked.end(), [](const Hotspot& a, const Hotspot& b) {
        if (a.severity != b.severity) {
            return a.severity > b.severity;
        }
        return a.reason < b.reason;
    });
    return picked;
}

} // namespace vectis::code
