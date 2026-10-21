#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace misterplex {
namespace plexui {

struct PlexConnection {
    std::string protocol;
    std::string address;
    std::string port;
    bool local = false;
    bool relay = false;
    std::string uri;
};

struct PlexServer {
    std::string name;
    std::string sourceTitle;
    std::string clientIdentifier;
    std::string machineIdentifier;
    std::string accessToken;
    bool owned = false;
    std::vector<PlexConnection> connections;
    // Chosen base URL (no trailing slash), e.g. http://127.0.0.1:32400
    std::string baseUrl;
    // OB port 0011: no address answered (all probes failed): listed greyed "Offline", never picked.
    bool offline = false;
};

struct PlexItem {
    std::string ratingKey;
    std::string key;
    std::string title;
    std::string thumb;
    std::string art;
    std::string updatedAt;
    std::string summary;
    std::string year;
    std::string contentRating;
    std::string tagline;
    std::string genres;
    std::string type;
    int runtimeMs = 0;
    float rating = 0.f;
    float audienceRating = 0.f;
    int64_t viewOffsetMs = 0;
};

struct PlexHubRow {
    std::string title;
    std::string hubKey;
    std::vector<PlexItem> items;
};

struct PlexSection {
    std::string key;
    std::string title;
    std::string type;
};

struct SidebarLibraryEntry {
    std::string title;
    std::string subtitle;
    std::string type;
    std::string sectionKey;
    int serverIndex = 0;
};

struct GridCategoryChip {
    enum class Kind : uint8_t { All, Collection, Genre };
    Kind kind = Kind::All;
    std::string title;
    std::string filterId;
};

enum class ViewId : uint8_t {
    ServerList,
    Home,
    LibraryGrid,
    Detail,
};

struct UiFocus {
    int row = 0;
    int col = 0;
    int sidebar = 0; // Home: 0=Home, 1+=section index+1
    int detailButton = 0; // 0=Play/Resume, 1=Resume only slot when visible
    bool gridOnChips = false;
    int gridChip = 0;
};

} // namespace plexui
} // namespace misterplex
