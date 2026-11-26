#pragma once

#include "libmisterplex/analog_raster.hpp"
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/plexob_ini.hpp"

#include <functional>
#include <map>
#include <string>
#include <unistd.h>

namespace misterplex::spelunkr {

enum class CoreChange { Startup, RuntimeRaster };

inline bool coreChangeAllowed(bool manualOnly, CoreChange change,
                              const CoreGate &gate, const std::string &core,
                              const std::string &rbf, uint64_t stamp) {
  if (!manualOnly)
    return true;
  return change == CoreChange::RuntimeRaster && gate.owenbOpen() &&
         classifyLoadedCore(core, rbf) == CoreFamily::PlexOwenb &&
         static_cast<uint32_t>(stamp >> 32) == owenb_id::kMpxBuildIdTag &&
         static_cast<uint32_t>(stamp) == gate.buildId() && gate.buildId() != 0;
}

// prepareSpelunkrVideo vs reloadCoreForYcMode:
//   A  gate still closed after the wait, or ctrl-page map never opened AND the
//      gate is closed: OSD "VIDEO CORE UNAVAILABLE".
//   B  MANUAL_ONLY and the live names are MENU/other/mismatch: OSD
//      "VIDEO CORE REFUSED". Stamp matching stays a core-change rule
//      (coreChangeAllowed). A second /dev/mem map or a zero stamp must not
//      block first_picture on a gate-verified PlexOB (Ready · AirPlay lab path).
enum class VideoPrepError { Ok, Unavailable, Refused };

inline const char *videoPrepOsd(VideoPrepError e) {
  switch (e) {
  case VideoPrepError::Unavailable:
    return "VIDEO CORE UNAVAILABLE";
  case VideoPrepError::Refused:
    return "VIDEO CORE REFUSED";
  case VideoPrepError::Ok:
    break;
  }
  return "";
}

inline VideoPrepError videoPrepError(bool manualOnly, bool gateOpen, bool mapOpen,
                                     const CoreGate &gate, const std::string &core,
                                     const std::string &rbf, uint64_t stamp) {
  (void)mapOpen;
  (void)stamp;
  if (!gateOpen)
    return VideoPrepError::Unavailable;
  if (!manualOnly)
    return VideoPrepError::Ok;
#ifdef MUT_VIDEO_PREP_ALLOW_MENU
  (void)gate;
  (void)core;
  (void)rbf;
  return VideoPrepError::Ok;
#else
  (void)gate;
  const CoreFamily fam = classifyLoadedCore(core, rbf);
  // Empty/transient names: trust the open gate (it already classified PlexOB).
  if (fam == CoreFamily::PlexOwenb || fam == CoreFamily::Unknown)
    return VideoPrepError::Ok;
  return VideoPrepError::Refused;
#endif
}

inline bool recoverRasterCfg(const std::string &path, std::string &err) {
  const std::string saved = path + ".spelunkr-save";
  if (::access(saved.c_str(), F_OK) != 0) {
    if (errno == ENOENT)
      return true;
    err = "Spelunkr video CFG recovery copy inaccessible";
    return false;
  }
  std::string original;
  bool big = false, bit = false;
  if (!plexob::readSmallFile(saved, original, big) ||
      !plexob::cfgBit24(original, bit) ||
      !plexob::writeFileAtomic(path, original) || std::remove(saved.c_str()) != 0) {
    err = "Spelunkr original video CFG recovery failed";
    return false;
  }
  return true;
}

// GH#412: Spelunkr no longer pokes raster CFG. A leftover .spelunkr-save from the
// old temporary-bit-24 path must not be copied over the live file on every tune
// (that can replace the user's Interlaced with a stale Progressive snapshot).
inline bool discardStaleRasterCfgSave(const std::string &path, std::string &err) {
  const std::string saved = path + ".spelunkr-save";
  if (::access(saved.c_str(), F_OK) != 0) {
    if (errno == ENOENT)
      return true;
    err = "Spelunkr stale video CFG save inaccessible";
    return false;
  }
  if (std::remove(saved.c_str()) != 0) {
    err = "Spelunkr stale video CFG save remove failed";
    return false;
  }
  return true;
}

inline bool withTemporaryRasterCfg(const std::string &path,
                                   const std::function<bool()> &reload,
                                   std::string &err) {
  const std::string saved = path + ".spelunkr-save";
  std::string original;
  bool big = false, bit = false;
  const bool recovering = ::access(saved.c_str(), F_OK) == 0;
  if (!plexob::readSmallFile(recovering ? saved : path, original, big) ||
      !plexob::cfgBit24(original, bit)) {
    err = "Spelunkr original video CFG unreadable";
    return false;
  }
  if (!recovering && !plexob::writeFileAtomic(saved, original)) {
    err = "Spelunkr video CFG backup failed";
    return false;
  }
  const auto set = plexob::setCfgBit24(path, false);
  const bool ok =
      (set == plexob::CfgSet::Ok || set == plexob::CfgSet::Same) && reload();
  // Keep the recovery copy on any restore failure, including a killed daemon.
  if (!plexob::writeFileAtomic(path, original) ||
      std::remove(saved.c_str()) != 0) {
    err = "Spelunkr original video CFG restore failed";
    return false;
  }
  if (!ok)
    err = "Spelunkr temporary video raster reload failed";
  return ok;
}

inline bool analogTvFromIni(std::istream &in) {
    std::map<std::string, std::string> global, core;
    std::string section, line;
    while (std::getline(in, line)) {
        const auto t = plexob::trimIniLine(line);
        if (t.empty() || t.front() == '#' || t.front() == ';')
            continue;
        if (t.front() == '[' && t.back() == ']') {
            section = t;
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos || (section != "[MiSTer]" && section != "[PlexOB]"))
            continue;
        (section == "[PlexOB]" ? core : global)[plexob::trimIniLine(t.substr(0, eq))] =
            plexob::iniValueOnly(t.substr(eq + 1));
    }
    for (const auto &kv : core)
        global[kv.first] = kv.second;
    const auto &mode = global["vga_mode"];
    if (mode == "svideo" || mode == "cvbs" || mode == "subcarrier")
        return true;
    return global["vga_scaler"] != "1" && (global["composite_sync"] == "1" || global["vga_sog"] == "1" ||
                                           mode == "ypbpr" || global["direct_video"] == "1");
}

} // namespace misterplex::spelunkr
