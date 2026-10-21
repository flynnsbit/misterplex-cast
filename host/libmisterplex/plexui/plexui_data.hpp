#pragma once

#include "libmisterplex/browse_fetch.hpp"
#include "plexui_types.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace misterplex {
namespace plexui {

using HttpGetFn = std::function<BrowseHttpResult(const std::string& url, int connectMs,
                                                 int timeoutMs, const BrowseCancelFn&)>;

std::string urlEncode(const std::string& s);
std::string appendToken(std::string url, const std::string& token);
std::string photoTranscodeUrl(const std::string& baseUrl, const std::string& thumbPath,
                              const std::string& token, int w, int h);

std::vector<PlexServer> parsePlexResources(const std::string& xml);
void pickServerConnections(std::vector<PlexServer>& servers);
// The connection the local → public → relay probe actually kept in baseUrl:
// "local", "remote", "relay", "Offline", or "Connecting...".
std::string serverLinkLabel(const PlexServer& server);
void resolveServerBases(HttpGetFn get, std::vector<PlexServer>& servers,
                        const BrowseCancelFn& cancel);
// OB port 0011: every server and every address of a server probed in parallel; onUpdate(servers) after each
// server resolves (baseUrl set, or offline=true), from a probe thread. Returns after all probes ended
// (bounded by the probe timeouts); servers that never answered stay in the list with offline=true.
using ServersUpdateFn = std::function<void(const std::vector<PlexServer>&)>;
void resolveServerBasesProgressive(HttpGetFn get, std::vector<PlexServer>& servers,
                                   const BrowseCancelFn& cancel, const ServersUpdateFn& onUpdate);
// plex.tv resources -> servers, then resolveServerBasesProgressive. Offline servers are kept.
std::vector<PlexServer> fetchPlexServersProgressive(HttpGetFn get, const std::string& accountToken,
                                                    const BrowseCancelFn& cancel,
                                                    const ServersUpdateFn& onUpdate);

std::vector<PlexSection> parseLibrarySections(const std::string& xml);
std::vector<PlexHubRow> parseHubsHome(const std::string& xml);
std::vector<PlexHubRow> parseSectionHubs(const std::string& xml);
std::vector<PlexItem> parseLibraryAll(const std::string& xml);
std::vector<GridCategoryChip> parseSectionCollections(const std::string& xml);
std::vector<GridCategoryChip> parseSectionGenres(const std::string& xml);
PlexItem parseMetadataDetail(const std::string& xml);

std::vector<PlexServer> fetchPlexServers(HttpGetFn get, const std::string& accountToken,
                                         const BrowseCancelFn& cancel);
bool fetchLibrarySections(HttpGetFn get, const PlexServer& server, const BrowseCancelFn& cancel,
                          std::vector<PlexSection>& sections, std::string& err);
bool fetchServerHome(HttpGetFn get, const PlexServer& server, const BrowseCancelFn& cancel,
                     std::vector<PlexSection>& sections, std::vector<PlexHubRow>& hubs,
                     std::string& err);
void buildSidebarEntries(const std::vector<PlexServer>& servers,
                         const std::vector<std::vector<PlexSection>>& perServerSections,
                         std::vector<SidebarLibraryEntry>& out);
// preferred is a conf name, source title, or address. Empty keeps the first discovered server.
int pickHomeServerIndex(const std::vector<PlexServer>& servers, const std::string& preferred = {});
std::vector<std::vector<PlexSection>> fetchAllServerSectionsParallel(
    HttpGetFn get, const std::vector<PlexServer>& servers, const BrowseCancelFn& cancel);
bool fetchMergedSharedContinueWatching(HttpGetFn get, const std::vector<PlexServer>& servers,
                                       const BrowseCancelFn& cancel, PlexHubRow& deck,
                                       std::unordered_map<std::string, int>& itemServerIndex);
bool fetchSectionGrid(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                      const BrowseCancelFn& cancel, std::vector<PlexItem>& items,
                      std::string& err);
bool fetchSectionCollections(HttpGetFn get, const PlexServer& server,
                             const std::string& sectionKey, const BrowseCancelFn& cancel,
                             std::vector<GridCategoryChip>& chips, std::string& err);
bool fetchSectionGenres(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                        const BrowseCancelFn& cancel, std::vector<GridCategoryChip>& chips,
                        std::string& err);

#ifdef MPX_PLEXUI_UNIT_TEST
void plexUiApiCacheClear();
bool plexUiApiCacheDrain();
#endif
bool fetchSectionGridGenre(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                           const std::string& genreId, const BrowseCancelFn& cancel,
                           std::vector<PlexItem>& items, std::string& err);
bool fetchCollectionChildren(HttpGetFn get, const PlexServer& server,
                             const std::string& collectionRatingKey, const BrowseCancelFn& cancel,
                             std::vector<PlexItem>& items, std::string& err);
bool fetchMetadata(HttpGetFn get, const PlexServer& server, const std::string& ratingKey,
                   const BrowseCancelFn& cancel, PlexItem& item, std::string& err);

void filterHomeHubs(std::vector<PlexHubRow>& hubs, const std::vector<PlexSection>& sections);

} // namespace plexui
} // namespace misterplex
