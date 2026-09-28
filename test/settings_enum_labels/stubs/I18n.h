#pragma once

// Minimal stand-in for lib/I18n/I18n.h. The real header pulls in
// I18nKeys.h/I18nStrings.h, which scripts/gen_i18n.py writes at build time and
// which a host-test build does not have. src/SettingsEnumLabels.h only needs
// StrId (the label tables' element type), so that is all this covers.
//
// Every name below is one src/SettingsEnumLabels.h actually uses. Adding a
// label there without adding its name here is a deliberate build break: the new
// string also has to be added to lib/I18n/translations/*.yaml.

enum class StrId {
  STR_ALIGN_LEFT,
  STR_ALIGN_RIGHT,
  STR_ALWAYS,
  STR_BOOK,
  STR_BOOKMARK_OPTION,
  STR_BOOK_S_STYLE,
  STR_BOTTOM,
  STR_CENTER,
  STR_CHAPTER,
  STR_CLOCK_FORMAT_12H,
  STR_CLOCK_FORMAT_24H,
  STR_CONFIRM,
  STR_CROP,
  STR_DICTIONARY,
  STR_DISABLED,
  STR_EXTRA_WIDE,
  STR_FILTER_CONTRAST,
  STR_FIT,
  STR_FMT_AUTHOR_TITLE,
  STR_FMT_TITLE,
  STR_FMT_TITLE_AUTHOR,
  STR_FOOTNOTES,
  STR_FORCE_REFRESH,
  STR_GOAL_15M,
  STR_GOAL_1H,
  STR_GOAL_2H,
  STR_GOAL_30M,
  STR_GOAL_45M,
  STR_GOAL_90M,
  STR_HIDE,
  STR_IGNORE,
  STR_IMAGES_DISPLAY,
  STR_IMAGES_PLACEHOLDER,
  STR_IMAGES_SUPPRESS,
  STR_INVERTED,
  STR_IN_READER,
  STR_JUSTIFY,
  STR_KOSYNC,
  STR_LANDSCAPE_CCW,
  STR_LANDSCAPE_CW,
  STR_LARGE,
  STR_LONG_PRESS_BEHAVIOR_OFF,
  STR_LONG_PRESS_BEHAVIOR_ORIENTATION,
  STR_LONG_PRESS_BEHAVIOR_SKIP,
  STR_MEDIUM,
  STR_MENU_STYLE_LIST,
  STR_MENU_STYLE_TOOLBAR,
  STR_NEVER,
  STR_NEXT_PREV,
  STR_NONE_OPT,
  STR_NORMAL,
  STR_ORIENTATION_INVERTED,
  STR_PAGES_1,
  STR_PAGES_10,
  STR_PAGES_15,
  STR_PAGES_30,
  STR_PAGES_5,
  STR_PAGE_TURN,
  STR_PORTRAIT,
  STR_PREV_NEXT,
  STR_PROGRESS_BAR_MEDIUM,
  STR_PROGRESS_BAR_THICK,
  STR_PROGRESS_BAR_THIN,
  STR_READER_MENU,
  STR_SLEEP,
  STR_SMALL,
  STR_STATE_INVERTED_TAP,
  STR_STATE_OFF,
  STR_STATE_ON,
  STR_STATE_SWIPE,
  STR_STATE_SWIPE_UP,
  STR_STATE_TAP,
  STR_TIGHT,
  STR_TOP,
  STR_WIDE,
  STR_X_LARGE,
};

class I18n {
 public:
  static I18n& getInstance() {
    static I18n instance;
    return instance;
  }
  const char* get(StrId) const { return ""; }
};

#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
