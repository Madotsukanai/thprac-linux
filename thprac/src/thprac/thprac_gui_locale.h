#pragma once
#if defined(_MSC_VER)
#define TH_FORCEINLINE __forceinline
#else
#define TH_FORCEINLINE inline __attribute__((always_inline))
#endif
#include "thprac_locale_def.h"
#include "thprac_cfg.h"

namespace THPrac {
namespace Gui {
    extern Locale __glocale_current;

    void LocaleSet(Locale locale);
    void LocaleSetFromSysLang();
    TH_FORCEINLINE Locale LocaleGet() {
        return __glocale_current;
    }
    TH_FORCEINLINE const char** LocaleGetCurrentGlossary() {
        return th_glossary_str[LocaleGet()];
    };
    TH_FORCEINLINE const char* LocaleGetStr(th_glossary_t name) {
        return LocaleGetCurrentGlossary()[name];
    };

    bool LocaleCreateFont(float font_size);
    bool LocalAddMergeFont(float font_size, int locale, bool merge);
    bool LocaleCreateMergeFont(float font_size);

// TODO: These can't be refactored into functions as-is, because they depend on
// the current game's namespace being active where they're used. Fixing this
// will require an architectural change.
#define XCBA(stage, type) th_sections_cba[stage][type]
#define XSSS(rank) (th_sections_str[::THPrac::Gui::LocaleGet()][rank])

}

TH_FORCEINLINE const char* S(th_glossary_t name)
{
    return Gui::LocaleGetStr(name);
}
}
