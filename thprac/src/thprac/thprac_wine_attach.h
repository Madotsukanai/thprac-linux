#pragma once

#include "thprac_identify.h"

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
bool AttachOtherPrefixes(THGameID wanted, bool manual, bool prompt, bool* prompted);

}
}
