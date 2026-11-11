#pragma once

#include "libmisterplex/browse_fetch.hpp"
#include "az_jump.hpp"
#include "grid_page.hpp"
#include "plexui_tv.hpp"
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

// Conf / GDM local servers. Name falls back to the host in baseUrl.
PlexServer makeLocalPlexServer(std::string name, std::string baseUrl, const std::string& token);
// Synthetic id assigned to a conf/GDM row before /identity is known.
bool isSyntheticLocalServerId(const std::string& id);
// plex.tv machineIdentifier/clientIdentifier, or empty for a local: seed so
// /identity of the same PMS (LAN, plex.direct, or 127.0.0.1) is accepted.
std::string identityExpectId(const PlexServer& s);
// Seeds first; account rows win on the same machine id or base URL (name + token).
std::vector<PlexServer> mergePlexServerLists(const std::vector<PlexServer>& seeds,
                                             const std::vector<PlexServer>& account);
// reviewer: seeds + plex.tv rows. A tokenless plex.tv row is kept only when it matches a
// seed (machine/client id, or any connection host:port or uri) and keeps the seed's conf
// token; unmatched tokenless rows are dropped.
// #328: bind each conf seed's token to its machine id (learned via tokenless /identity at the
// conf base, or the conf's own id). Unverified: token withheld.
void proveSeedIdentities(HttpGetFn get, std::vector<PlexServer>& seeds,
                         const std::string& accountToken, const BrowseCancelFn& cancel);
std::vector<PlexServer> mergeSeedsWithAccount(const std::vector<PlexServer>& seeds,
                                              const std::vector<PlexServer>& account);
// User-visible text for HTTP 401/403 from a PMS library call, else "".
std::string plexAuthErrorText(int status);
// Collapse rows that resolved to the same real machineIdentifier.
void collapseServersByIdentity(std::vector<PlexServer>& servers);
// PMS GDM advertise (Content-Type: plex/media-server). Players are ignored.
bool parseGdmAdvertise(const std::string& packet, const std::string& fromAddr, PlexServer& out);
// Best-effort LAN probe. Empty on error or timeout. Does not throw.
std::vector<PlexServer> discoverLocalPlexServers(int timeoutMs);
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
                                                    const ServersUpdateFn& onUpdate,
                                                    bool* tlsFailed = nullptr,
                                                    const std::vector<PlexServer>& seeds = {},
                                                    int* resourcesStatus = nullptr);

std::vector<PlexSection> parseLibrarySections(const std::string& xml);
std::vector<PlexHubRow> parseHubsHome(const std::string& xml);
std::vector<PlexHubRow> parseSectionHubs(const std::string& xml);
// /library/onDeck (or a hubs Metadata list). XML or PMS JSON.
std::vector<PlexItem> parseOnDeckItems(const std::string& body);
void keepContinueWatchingItems(std::vector<PlexItem>& items);
PlexHubRow buildContinueWatchingRow(std::vector<PlexItem> items);
std::vector<PlexItem> parseLibraryAll(const std::string& xml);
std::vector<GridCategoryChip> parseSectionCollections(const std::string& xml);
std::vector<GridCategoryChip> parseSectionGenres(const std::string& xml);
PlexItem parseMetadataDetail(const std::string& xml);

std::vector<PlexServer> fetchPlexServers(HttpGetFn get, const std::string& accountToken,
                                         const BrowseCancelFn& cancel,
                                         const std::vector<PlexServer>& seeds = {});
bool fetchLibrarySections(HttpGetFn get, const PlexServer& server, const BrowseCancelFn& cancel,
                          std::vector<PlexSection>& sections, std::string& err);
bool fetchServerHome(HttpGetFn get, const PlexServer& server, const BrowseCancelFn& cancel,
                     std::vector<PlexSection>& sections, std::vector<PlexHubRow>& hubs,
                     std::string& err);
void buildSidebarEntries(const std::vector<PlexServer>& servers,
                         const std::vector<std::vector<PlexSection>>& perServerSections,
                         std::vector<SidebarLibraryEntry>& out);
// Home sidebar row that opens the server list when more than one PLEX_SERVERS entry is loaded.
void ensureServerPickerRow(std::vector<SidebarLibraryEntry>& entries, size_t serverCount);
void appendSidebarSignIn(std::vector<SidebarLibraryEntry>& out);
// Profile row. Empty profileTitle is the idle "Switch user" label.
void appendSidebarHomeUser(std::vector<SidebarLibraryEntry>& out, const std::string& profileTitle);
int pickHomeServerIndex(const std::vector<PlexServer>& servers);
// GH#14: put the Home pick at index 0 so Native RFC1918 is the first sidebar server, not kube.
int rotatePickedServerToFront(std::vector<PlexServer>& servers, int index,
                              std::vector<std::vector<PlexSection>>* perSec = nullptr,
                              std::vector<std::string>* errs = nullptr);
std::vector<std::vector<PlexSection>> fetchAllServerSectionsParallel(
    HttpGetFn get, const std::vector<PlexServer>& servers, const BrowseCancelFn& cancel,
    std::vector<std::string>* errs = nullptr);
bool fetchMergedSharedContinueWatching(HttpGetFn get, const std::vector<PlexServer>& servers,
                                       const BrowseCancelFn& cancel, PlexHubRow& deck,
                                       std::unordered_map<std::string, int>& itemServerIndex);
bool fetchSectionGrid(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                      const BrowseCancelFn& cancel, std::vector<PlexItem>& items,
                      std::string& err, int* totalSize = nullptr);
// One budget for collections + genres + items. A connect failure stops the rest, so a
// down server pays one connect timeout instead of three, and a silent server cannot
// stack 12s + 12s + 20s.
constexpr int kLibraryOpenBudgetMs = 8000;

bool fetchSectionCollections(HttpGetFn get, const PlexServer& server,
                             const std::string& sectionKey, const BrowseCancelFn& cancel,
                             std::vector<GridCategoryChip>& chips, std::string& err);
bool fetchSectionGenres(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                        const BrowseCancelFn& cancel, std::vector<GridCategoryChip>& chips,
                        std::string& err);

// Join metadata refreshes. Controller shutdown calls this so a refresh cannot outlive it.
void plexUiApiCacheDrain();
#ifdef MPX_PLEXUI_UNIT_TEST
void plexUiApiCacheClear();
#endif
bool fetchSectionGridGenre(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                           const std::string& genreId, const BrowseCancelFn& cancel,
                           std::vector<PlexItem>& items, std::string& err, int* totalSize = nullptr);
// Later page of a library or genre grid. Flag off and start != 0: no request.
// start 0 uses the same first-page helpers (including the all-grid cache).
// genreId is raw; this helper encodes it. Empty genreId is the unfiltered grid.
// letter is a PMS firstCharacter (A–Z or '#'). AZ_JUMP off ignores it, so the
// query matches the unfiltered page. A letter skips the first-page cache.
bool fetchSectionGridPage(HttpGetFn get, const PlexServer& server, const std::string& sectionKey,
                          int start, const std::string& genreId, const BrowseCancelFn& cancel,
                          std::vector<PlexItem>& items, int* totalSize, std::string& err,
                          char letter = 0);
bool fetchCollectionChildren(HttpGetFn get, const PlexServer& server,
                             const std::string& collectionRatingKey, const BrowseCancelFn& cancel,
                             std::vector<PlexItem>& items, std::string& err);
bool fetchMetadata(HttpGetFn get, const PlexServer& server, const std::string& ratingKey,
                   const BrowseCancelFn& cancel, PlexItem& item, std::string& err);
// /library/metadata/<id>/children — seasons of a show, or episodes of a season.
std::vector<PlexItem> parseMetadataChildren(const std::string& xml);
bool fetchMetadataChildren(HttpGetFn get, const PlexServer& server, const std::string& ratingKey,
                           const BrowseCancelFn& cancel, std::vector<PlexItem>& items,
                           std::string& err);

// Query for POST /playQueues (type, continuous, uri). Not an HTTP body: PMS returns
// HTTP 400 when this string is posted as application/x-www-form-urlencoded.
std::string playQueueFormBody(const std::string& machineId, const std::string& ratingKey);
using HttpPostFn = std::function<BrowseHttpResult(
    const std::string& url, const std::string& contentType, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers, int connectMs, int timeoutMs,
    const BrowseCancelFn&)>;
using HttpHeaderGetFn = std::function<BrowseHttpResult(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    int connectMs, int timeoutMs, const BrowseCancelFn&)>;
// Show theme bytes. Token is a request header. URL must not contain X-Plex-Token.
bool fetchShowTheme(HttpHeaderGetFn get, const PlexServer& server, const PlexItem& item,
                    const BrowseCancelFn& cancel, std::string& body, std::string& err);
// POST /playQueues?type=video&continuous=1&uri=... with an empty body.
// The token is a header, never part of the URL.
bool createPlayQueue(HttpPostFn post, const PlexServer& server, const std::string& ratingKey,
                     const std::string& clientId, const BrowseCancelFn& cancel, PlexPlayQueue& out,
                     std::string& err);

void filterHomeHubs(std::vector<PlexHubRow>& hubs, const std::vector<PlexSection>& sections);

} // namespace plexui
} // namespace misterplex
