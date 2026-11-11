#pragma once
// Decode XML/HTML entities in Plex display strings (titles, summaries, track names).

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace misterplex {

inline unsigned long xmlNamedEntityCp(const std::string& ent) {
    struct Row {
        const char* n;
        unsigned long cp;
    };
    // XML five + Latin-1 named entities Plex uses in titles / displayTitle.
    static const Row k[] = {
        {"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
        {"nbsp", 160},     {"iexcl", 161},    {"cent", 162},     {"pound", 163},    {"curren", 164},
        {"yen", 165},      {"brvbar", 166},   {"sect", 167},     {"uml", 168},      {"copy", 169},
        {"ordf", 170},     {"laquo", 171},    {"not", 172},      {"shy", 173},      {"reg", 174},
        {"macr", 175},     {"deg", 176},      {"plusmn", 177},   {"sup2", 178},     {"sup3", 179},
        {"acute", 180},    {"micro", 181},    {"para", 182},     {"middot", 183},   {"cedil", 184},
        {"sup1", 185},     {"ordm", 186},     {"raquo", 187},    {"frac14", 188},   {"frac12", 189},
        {"frac34", 190},   {"iquest", 191},   {"Agrave", 192},   {"Aacute", 193},   {"Acirc", 194},
        {"Atilde", 195},   {"Auml", 196},     {"Aring", 197},    {"AElig", 198},    {"Ccedil", 199},
        {"Egrave", 200},   {"Eacute", 201},   {"Ecirc", 202},    {"Euml", 203},     {"Igrave", 204},
        {"Iacute", 205},   {"Icirc", 206},    {"Iuml", 207},     {"ETH", 208},      {"Ntilde", 209},
        {"Ograve", 210},   {"Oacute", 211},   {"Ocirc", 212},    {"Otilde", 213},   {"Ouml", 214},
        {"times", 215},    {"Oslash", 216},   {"Ugrave", 217},   {"Uacute", 218},   {"Ucirc", 219},
        {"Uuml", 220},     {"Yacute", 221},   {"THORN", 222},    {"szlig", 223},    {"agrave", 224},
        {"aacute", 225},   {"acirc", 226},    {"atilde", 227},   {"auml", 228},     {"aring", 229},
        {"aelig", 230},    {"ccedil", 231},   {"egrave", 232},   {"eacute", 233},   {"ecirc", 234},
        {"euml", 235},     {"igrave", 236},   {"iacute", 237},   {"icirc", 238},    {"iuml", 239},
        {"eth", 240},      {"ntilde", 241},   {"ograve", 242},   {"oacute", 243},   {"ocirc", 244},
        {"otilde", 245},   {"ouml", 246},     {"divide", 247},   {"oslash", 248},   {"ugrave", 249},
        {"uacute", 250},   {"ucirc", 251},    {"uuml", 252},     {"yacute", 253},   {"thorn", 254},
        {"yuml", 255},     {"ndash", 8211},   {"mdash", 8212},   {"lsquo", 8216},   {"rsquo", 8217},
        {"sbquo", 8218},   {"ldquo", 8220},   {"rdquo", 8221},   {"bdquo", 8222},   {"dagger", 8224},
        {"Dagger", 8225},  {"bull", 8226},    {"hellip", 8230},  {"permil", 8240},  {"lsaquo", 8249},
        {"rsaquo", 8250},  {"euro", 8364},    {"trade", 8482},
    };
    for (const Row& r : k) {
        if (ent == r.n)
            return r.cp;
    }
    return 0;
}

inline void plexAppendUtf8(std::string& out, unsigned long u) {
    if (u < 0x20 || u > 0x10FFFF) {
        out += '?';
        return;
    }
    if (u < 0x80) {
        out += static_cast<char>(u);
        return;
    }
    if (u < 0x800) {
        out += static_cast<char>(0xC0 | (u >> 6));
        out += static_cast<char>(0x80 | (u & 0x3F));
        return;
    }
    if (u < 0x10000) {
        out += static_cast<char>(0xE0 | (u >> 12));
        out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (u & 0x3F));
        return;
    }
    out += static_cast<char>(0xF0 | (u >> 18));
    out += static_cast<char>(0x80 | ((u >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (u & 0x3F));
}

inline std::string xmlUnescape(const std::string& s) {
    if (s.find('&') == std::string::npos)
        return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 16) {
            out += s[i];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        unsigned long cp = 0;
        bool ok = true;
        if (ent.size() > 1 && ent[0] == '#') {
            char* end = nullptr;
            cp = (ent[1] == 'x' || ent[1] == 'X') ? std::strtoul(ent.c_str() + 2, &end, 16)
                                                  : std::strtoul(ent.c_str() + 1, &end, 10);
            ok = end && *end == '\0';
        } else {
            cp = xmlNamedEntityCp(ent);
            ok = cp != 0;
        }
        if (!ok) {
            out += s[i];
            continue;
        }
        plexAppendUtf8(out, cp);
        i = semi;
    }
    return out;
}

inline std::string plexDisplayText(const std::string& s) { return xmlUnescape(s); }

// Display fields on a Plex metadata object. One unescape at the JSON parse boundary.
// XML attributes are already unescaped by parseXmlStartTags / xmlAttr; do not run this
// again on those strings (`&amp;lt;` must stay `&lt;` after a single pass).
inline bool isPlexMetaTextKey(const std::string& key) {
    return key == "title" || key == "grandparentTitle" || key == "parentTitle" ||
           key == "summary" || key == "tagline" || key == "originalTitle" ||
           key == "titleSort" || key == "contentRating" || key == "displayTitle" ||
           key == "sourceTitle";
}

inline void decodePlexMetaFields(std::vector<std::pair<std::string, std::string>>& fields) {
    for (auto& f : fields) {
        if (isPlexMetaTextKey(f.first))
            f.second = plexDisplayText(f.second);
    }
}

} // namespace misterplex
