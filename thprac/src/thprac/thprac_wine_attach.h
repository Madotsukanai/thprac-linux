#pragma once

#include "thprac_identify.h"

namespace THPrac {
namespace WineAttach {
bool IsWine();
bool AttachOtherPrefixes(THGameID wanted, bool manual, bool prompt, bool* prompted);
void DispatchSteamHook(const wchar_t* steamUrl, THGameID gameId);
}
}
