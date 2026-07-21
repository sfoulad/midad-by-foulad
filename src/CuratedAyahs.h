#pragma once

#include <cstddef>

// Short individual ayahs (not whole surahs) shown as a rotating quote at the
// top of the Dashboard sleep screen, only when QuranBook::isPinned() (Settings
// -> System -> Quran enabled). Text extracted directly from the app's own
// embedded Quran source (tools/quran/source/kfgqpc_chapters/chapter-*.xhtml --
// the same KFGQPC Uthmanic Hafs Mushaf text QuranBook.cpp ships), not typed
// from memory: accuracy matters for religious text. Each entry is one or two
// complete, consecutive ayah(s) by their real ayah-number boundaries (never a
// fragment cut mid-ayah), chosen for reading well on their own.
//
// An on-device photo showed an earlier version of this list (whole short
// surahs, e.g. Al-Falaq) wrapping to 4 lines and pushing the stat cards below
// the fold -- single ayahs stay well within 1-2 lines, avoiding that.
namespace CuratedAyahs {

struct Entry {
  const char* reference;  // Arabic surah name + ayah number(s)
  const char* text;
};

constexpr Entry kEntries[] = {
    {"سورة الشرح - ٥", "فَإِنَّ مَعَ ٱلۡعُسۡرِ يُسۡرًا"},
    {"سورة الشرح - ١", "أَلَمۡ نَشۡرَحۡ لَكَ صَدۡرَكَ"},
    {"سورة الشرح - ٧-٨", "فَإِذَا فَرَغۡتَ فَٱنصَبۡ وَإِلَىٰ رَبِّكَ فَٱرۡغَب"},
    {"سورة الإخلاص - ١", "قُلۡ هُوَ ٱللَّهُ أَحَدٌ"},
    {"سورة الكوثر - ١", "إِنَّآ أَعۡطَيۡنَٰكَ ٱلۡكَوۡثَرَ"},
    {"سورة العصر - ١-٢", "وَٱلۡعَصۡرِ إِنَّ ٱلۡإِنسَٰنَ لَفِي خُسۡرٍ"},
};
constexpr size_t kCount = sizeof(kEntries) / sizeof(kEntries[0]);

}  // namespace CuratedAyahs
