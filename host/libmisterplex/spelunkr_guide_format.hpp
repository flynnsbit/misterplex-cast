#pragma once

#include "spelunkr_sections.hpp"
// Format a Spelunkr / Back When guide row: a source badge + right-aligned
// dial + channel name + now-playing title. No "SPELUNKR" / "BACK WHEN" text
// column (GH#650).

#include <cstdint>
#include <string>
#include <string_view>

namespace misterplex::owenb_osd {

enum class GuideSource : uint8_t { None = 0, Spelunkr, BackWhen };

inline constexpr const char* kGuideSectionSpelunkr = misterplex::spelunkr::kSpelunkrSection;
inline constexpr const char* kGuideSectionBackWhen = misterplex::spelunkr::kBackWhenSection;
inline constexpr int kGuideDialCols = 4;

inline GuideSource guideSourceFromSection(std::string_view section) {
    if (section == kGuideSectionBackWhen)
        return GuideSource::BackWhen;
    if (section == kGuideSectionSpelunkr)
        return GuideSource::Spelunkr;
    return GuideSource::None;
}

inline std::string formatSpelunkrGuideText(int dial, std::string_view name, std::string_view nowTitle) {
    std::string d = std::to_string(dial);
    if (d.size() < static_cast<size_t>(kGuideDialCols))
        d.insert(0, static_cast<size_t>(kGuideDialCols) - d.size(), ' ');
    std::string line = d;
    line += "  ";
    line += name;
    if (!nowTitle.empty()) {
        line += "  ";
        line += nowTitle;
    }
    return line;
}

struct FormattedGuideRow {
    GuideSource source = GuideSource::None;
    std::string text;
};

inline FormattedGuideRow formatSpelunkrGuideRow(std::string_view section, int dial, std::string_view name,
                                               std::string_view nowTitle) {
    FormattedGuideRow r;
    r.source = guideSourceFromSection(section);
    r.text = formatSpelunkrGuideText(dial, name, nowTitle);
    return r;
}

}  // namespace misterplex::owenb_osd
