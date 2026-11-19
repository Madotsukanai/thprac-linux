#pragma once

#include "thprac_identify.h"

#include <string>

namespace THPrac {
namespace WineAttach {

// True when running under Wine/Proton (ntdll exports wine_get_version).
bool IsWine();

// Look for games running in *other* Wine prefixes on the same Linux host
// (other Proton instances, plain Wine, Lutris, Bottles, ...) and apply thprac
// to them. Windows PIDs and handles are private to one wineserver, so the
// launcher cannot touch those processes directly; instead it re-runs itself
// ("thprac.exe --attach") inside the target prefix, using the same Wine build
// and environment as the running game.
//
//   wanted : restrict to one game (ID_UNKNOWN = any)
//   manual : explicit user action (also considers renamed executables and
//            retries processes that were already attempted)
//   prompt : ask for confirmation / report success like FindAndAttach does
//   prompted : set to true if a message box was shown
//
// Returns true if at least one game was handled (applied or reported as failed).
//   steamAppId : Steam AppID of the game, when known (parsed from the launch
//                command). The attach helper can then be dispatched straight
//                into the game's own container, which is the only way to reach
//                it from inside another pressure-vessel sandbox.
bool AttachOtherPrefixes(THGameID wanted, bool manual, bool prompt, bool* prompted,
    const char* steamAppId = nullptr);

// Keep a background attach helper resident inside the Steam container of the
// given game so that it gets thprac applied even when it was started outside
// the launcher (e.g. "steam steam://rungameid/..." from a terminal or the
// Steam client). The launcher only watches games it started itself, and a
// plain scan cannot see other Steam containers from inside a pressure-vessel
// sandbox. Cheap to call every frame; each container dispatch runs on its own
// thread, is silent (no prompts or dialogs), and only one attempt per
// container runs at a time. Returns true while an attempt is in flight for
// this container.
bool WatchOngoingGame(THGameID wanted, const std::string& appId);

// Same as WatchOngoingGame for th06nc, whose Steam container is known even
// without a launcher-configured instance.
bool WatchOngoingNcGame();

// Launcher game paths are stored as Linux paths so that they stay valid in every
// Wine prefix (a C:\users\<name>\... path only exists in the prefix it came from).
// Both functions take and return UTF-8 and return an empty string when the path
// cannot be converted (not running under Wine, file does not exist, ...).
//   DosToUnixPath: only converts paths that exist, so a path that belongs to
//                  another prefix is never turned into a wrong Linux path.
//   UnixToDosPath: path as seen from this prefix (usually Z:\home\...).
std::string DosToUnixPath(const std::string& dosUtf8);
std::string UnixToDosPath(const std::string& unixUtf8);

}
}
