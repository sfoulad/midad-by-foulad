#pragma once

#include <cstdint>

#include "I18nKeys.h"
/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Get localized string by ID
  const char* get(StrId id) const;

  const char* operator[](StrId id) const { return get(id); }

  Language getLanguage() const { return _language; }
  // True when the active UI language reads right-to-left (Arabic, Hebrew, Farsi). Themes
  // use this to mirror layout: titles/icons anchor to the right edge, values/options to
  // the left, tab bars flow right-to-left. Per-string glyph ordering is already handled
  // inside GfxRenderer (MiniBidi / ArabicShaper); this flag only drives layout anchoring.
  // Deliberately Arabic-only for now, NOT "any RTL-script language": simulator testing
  // found that flipping this true for Farsi (script/text/font otherwise unchanged --
  // isolated by testing Farsi content with this forced false, and separately with the
  // UI font forced back to Tajawal) reproducibly put ~1480 draws out of the panel
  // bounds during Home's boot render, all zero with this false. Some RTL-mirrored
  // layout path has a hardcoded assumption that held only because Arabic has been the
  // sole RTL language exercising it -- root cause not yet found. Farsi ships with
  // correct text/font and LTR layout until that's tracked down; flipping this back to
  // include FA without fixing the underlying bug WILL reproduce the corruption.
  bool isRtl() const { return _language == Language::AR; }
  void setLanguage(Language lang);
  const char* getLanguageName(Language lang) const;
  static Language languageFromCode(const char* code);

  // Get all unique characters used in a specific language
  // Returns a sorted string of unique characters
  static const char* getCharacterSet(Language lang);

 private:
  I18n() : _language(Language::EN) {}

  Language _language;
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
