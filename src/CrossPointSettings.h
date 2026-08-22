#pragma once
#include <ArduinoJson.h>
#include <Epub/ReaderRenderSpec.h>
#include <HalStorage.h>
#include <PersistableStore.h>

#include <cstdint>

class CrossPointSettings : public PersistableStore<CrossPointSettings> {
 private:
  // Private constructor for singleton (see PersistableStore.h)
  CrossPointSettings() = default;
  ~CrossPointSettings() = default;

  friend class PersistableStore<CrossPointSettings>;

 public:
  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    // Appended rather than inserted -- the settings list below stores the
    // picked label's index straight into this field, so any reordering of
    // existing values needs the display list re-synced too (see the comment
    // there). Appending keeps every existing saved value's meaning intact.
    DASHBOARD = 7,
    TRANSPARENT_CUSTOM = 8,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Status bar enum - legacy
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum STATUS_BAR_CLOCK_MODE {
    STATUS_BAR_CLOCK_HIDE = 0,
    STATUS_BAR_CLOCK_RIGHT = 1,
    STATUS_BAR_CLOCK_LEFT = 2,
    STATUS_BAR_CLOCK_MODE_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTONS_DISABLED = 2, SIDE_BUTTON_LAYOUT_COUNT };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName).
  // Noto Sans (formerly = 1) was trimmed from flash to keep OTA images small.
  // Noto Serif itself (formerly = 0) was replaced outright by Bitter -- an
  // e-ink-tuned slab serif with a flatter, more uniform stroke weight that
  // anti-aliases with noticeably less ghosting on this panel's 2-bit
  // grayscale, the same reasoning CrossInk (uxjulia/crossink) documents for
  // its own font choice. Measured cost of the swap: ~1071KB (Bitter, 4
  // sizes x 4 styles) vs ~1055KB (NotoSerif) -- effectively flat. Existing
  // saved fontFamily=0 therefore silently becomes Bitter on next boot, same
  // as any other "the default serif changed" migration this fork has done
  // before. Lexend Deca is a genuinely NEW second option (a sans, chosen for
  // the same anti-aliasing reasoning) -- measured cost ~297KB (4 sizes x 2
  // styles; Lexend Deca has no italic master at all, so italic/bold-italic
  // fall back to upright via EpdFontFamily's style-fallback, same as any
  // font missing a style). Saved fontFamily values >= FONT_FAMILY_COUNT
  // clamp back to Bitter on load.
  enum FONT_FAMILY { BITTER = 0, LEXENDDECA = 1, FONT_FAMILY_COUNT };
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;
  // The LATIN reading font size is a point size (fontPointSize below), not this
  // enum -- see ReaderFontSizes.h. This enum survives ONLY because arabicFontSize
  // / bookArabicFontSize still use it (the Arabic size picker stays a fixed
  // 4-slot SMALL/MEDIUM/LARGE/EXTRA_LARGE choice). Legacy 1.4-and-earlier files
  // stored the Latin size in this same 0..3 shape too; fromJson() folds that
  // range up into a point size (see LEGACY_FONT_SIZE_MAX).
  enum FONT_SIZE { SMALL = 0, MEDIUM = 1, LARGE = 2, EXTRA_LARGE = 3, FONT_SIZE_COUNT };
  static constexpr uint8_t LEGACY_FONT_SIZE_MAX = 3;
  static constexpr uint8_t DEFAULT_FONT_POINT_SIZE = 14;
  // Arabic reading-font family options (built-in only; SD card fonts use
  // sdArabicFontFamilyName). Independent of FONT_FAMILY/fontFamily above -- see
  // ArabicFontSystem. Reuses FONT_SIZE (Small/Medium/Large/X-Large) for arabicFontSize.
  // Noto Sans Arabic / Amiri / Scheherazade New / Cairo (formerly indices 0/2/3/4)
  // were trimmed from flash; out-of-range saved values clamp to Noto Naskh.
  // AMIRI's own glyph data was later dropped too (~577KB of source, to make room for
  // other features) -- index 1 stays defined and its numeric VALUE unchanged so any
  // already-persisted setting or Quran per-book sidecar byte (bookArabicFontFamily,
  // sdArabicFontFamilyName's built-in-family index, etc.) written under the old
  // 3-family numbering keeps meaning what it always meant; ArabicFontSystem's
  // kBuiltinArabicReadingFontIds table now points that row at the already-loaded
  // NOTONASKHARABIC font ids instead of removed AMIRI_*_FONT_ID ones.
  enum ARABIC_FONT_FAMILY { NOTONASKHARABIC = 0, AMIRI = 1, UTHMANICHAFS = 2, TAJAWAL = 3, ARABIC_FONT_FAMILY_COUNT };
  static constexpr uint8_t BUILTIN_ARABIC_FONT_COUNT = ARABIC_FONT_FAMILY_COUNT;
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, EXTRA_WIDE = 3, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN {
    IGNORE = 0,
    SLEEP = 1,
    PAGE_TURN = 2,
    FORCE_REFRESH = 3,
    FOOTNOTES = 4,
    PWR_CONFIRM = 5,
    SHORT_PWRBTN_COUNT
  };

  // Long-press Confirm action while reading an EPUB. The setting cycles through these values.
  // Persisted in settings.json by index: any new function (e.g. dictionary, bookmark) MUST use a
  // value >= 2 and be appended at the END of the enumValues array in SettingsList.h, otherwise the
  // stored indices shift and existing saves are silently misinterpreted.
  enum LONG_PRESS_MENU_FUNCTION {
    LP_MENU_KOSYNC = 0,
    LP_MENU_DISABLED = 1,
    LP_MENU_BOOKMARK = 2,
    LP_MENU_DICTIONARY = 3,
    LP_MENU_READER_MENU = 4,
    LONG_PRESS_MENU_FUNCTION_COUNT
  };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // UI Theme
  enum UI_THEME { CLASSIC = 0, LYRA = 1, LYRA_3_COVERS = 2, ROUNDEDRAFF = 3, FOULAD = 4 };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_NVERTED = 2, TILT_PAGE_TURN_COUNT };

  enum TOUCH_READER_CONTROLS {
    TOUCH_READER_OFF = 0,
    TOUCH_READER_ON = 1,
    TOUCH_READER_SWIPE = 2,
    TOUCH_READER_INVERTED_TAP = 3,
    TOUCH_READER_CONTROLS_COUNT
  };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = DASHBOARD;
  // Night mode: inverted output polarity on the reading surfaces only
  // (resolved per render by ActivityManager via Activity::appliesNightMode).
  uint8_t screenInverted = 0;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings (statusBar retained for migration only)
  uint8_t statusBar = FULL;
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (X3 only, requires DS3231 RTC)
  uint8_t statusBarClock = STATUS_BAR_CLOCK_HIDE;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t textAntiAliasing = 1;
  // Short power button click behaviour
  uint8_t shortPwrBtn = IGNORE;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = BITTER;
  // Point size of the LATIN reading font. Only sizes the active family actually
  // ships are selectable in the UI (see ReaderFontSizes.h / SettingsList.h's
  // buildFontSizeSetting()); SdCardFontSystem::ensureLoaded() snaps this to the
  // nearest available size (and persists the snap) whenever the family changes.
  // The Arabic reading font size is unaffected -- see arabicFontSize below.
  uint8_t fontPointSize = DEFAULT_FONT_POINT_SIZE;
  uint8_t lineSpacing = NORMAL;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  static constexpr uint8_t SCREEN_MARGIN_MIN = 5;
  static constexpr uint8_t SCREEN_MARGIN_MAX = 40;
  static constexpr uint8_t SCREEN_MARGIN_STEP = 5;
  uint8_t screenMargin = SCREEN_MARGIN_MIN;
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // Long-press Confirm function in EPUB reader (cycles through LONG_PRESS_MENU_FUNCTION values).
  // Defaults to Disabled so shortcut-based bookmark toggling remains opt-in.
  uint8_t longPressMenuFunction = LP_MENU_DISABLED;
  // UI Theme
  uint8_t uiTheme = FOULAD;  // single-theme firmware: always Foulad
  // Sunlight fading compensation: power the panel off after each refresh so a
  // static image can't fade/darken while sitting ("background becomes dark
  // instead of clear white" -- reported on this fork's hardware). Default ON;
  // devices with an older saved settings.json keep their stored value and can
  // enable it under Settings -> Display -> Sunlight Fading Fix.
  uint8_t fadingFix = 1;
  // Global dark mode: the framebuffer is inverted at the panel-push points in
  // GfxRenderer (see setDarkMode), so the whole UI -- reader, Arabic text,
  // games, home theme -- renders white-on-black with zero per-activity code.
  uint8_t darkModeEnabled = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  // SD card font family name (empty = use built-in fontFamily)
  char sdFontFamilyName[32] = "";
  // SD card font family supplying Arabic glyphs for drawArabicText (empty = use the
  // built-in arabicFontFamily below). Independent of sdFontFamilyName/fontFamily --
  // see ArabicFontSystem.
  char sdArabicFontFamilyName[32] = "";
  // Built-in Arabic reading-font family/size (Settings -> Reader -> Arabic Font /
  // Arabic Font Size). Only used when sdArabicFontFamilyName is empty. Independent of
  // fontFamily/fontSize (the Latin reading font) -- see ArabicFontSystem.
  uint8_t arabicFontFamily = NOTONASKHARABIC;
  uint8_t arabicFontSize = MEDIUM;

  // Settings -> Apps -> Quran/Games/Tasbih/Stop Watch/Pomodoro/Gym/Midad BLE
  // and the Debug logging toggle all moved to MidadAppSettings (see
  // src/MidadAppSettings.h) -- none of them have a CrossPoint upstream
  // equivalent, so keeping them here meant every upstream sync touched this
  // file for no reason. See docs/upstream-sync-architecture.md's Phase B.

  // --- Per-book reading overrides (RAM ONLY -- never serialized) ---
  // Applied by EpubReaderActivity from the book's own settings file (see
  // BookReaderSettings) on open and cleared on exit, so every consumer of the
  // eff*() accessors below -- getReaderFontId(), section cache keys, the font
  // systems -- resolves per-book values without touching the global fields
  // that saveToFile() persists. BOOK_NO_OVERRIDE / an empty name = inherit the
  // global setting; BOOK_FORCE_BUILTIN_FAMILY as a name forces the built-in
  // font family even when the global setting points at an SD family.
  static constexpr uint8_t BOOK_NO_OVERRIDE = 0xFF;
  static constexpr char BOOK_FORCE_BUILTIN_FAMILY[2] = "\x01";
  // A point size (see fontPointSize above), NOT a FONT_SIZE enum slot, despite
  // the shared BOOK_NO_OVERRIDE sentinel shape. snapToNearestPointSize() at
  // render/load time keeps an override that the active family can't render
  // exactly (e.g. after switching to an SD family with a sparser size set) safe.
  uint8_t bookFontSize = BOOK_NO_OVERRIDE;
  // Arabic reading-font size override -- still a FONT_SIZE enum slot, unlike
  // bookFontSize above. Arabic sizing is untouched by the point-size migration.
  uint8_t bookArabicFontSize = BOOK_NO_OVERRIDE;
  // Which BUILT-IN Arabic family this book uses when the built-in path is
  // active (bookSdArabicFontFamilyName forced-builtin or global has no SD
  // family). The Quran defaults to UTHMANICHAFS via its extraction-time
  // sidecar (see QuranBook::writeDefaultSidecarIfMissing).
  uint8_t bookArabicFontFamily = BOOK_NO_OVERRIDE;
  // Which BUILT-IN Latin family this book uses when the built-in path is
  // active (bookSdFontFamilyName forced-builtin or global has no SD family) --
  // same role as bookArabicFontFamily above, added once there was a second
  // built-in Latin option (Bitter/Lexend Deca) to actually choose between.
  uint8_t bookFontFamily = BOOK_NO_OVERRIDE;
  uint8_t bookLineSpacing = BOOK_NO_OVERRIDE;
  uint8_t bookParagraphAlignment = BOOK_NO_OVERRIDE;
  char bookSdFontFamilyName[32] = "";
  char bookSdArabicFontFamilyName[32] = "";

  // Effective LATIN reading-font point size (per-book override, else the global
  // fontPointSize). Name kept as effFontSize() rather than effFontPointSize() --
  // it predates the point-size migration and every call site already expects
  // this name; only the returned value's meaning changed (slot -> point size).
  uint8_t effFontSize() const { return bookFontSize != BOOK_NO_OVERRIDE ? bookFontSize : fontPointSize; }
  uint8_t effArabicFontSize() const {
    return bookArabicFontSize != BOOK_NO_OVERRIDE ? bookArabicFontSize : arabicFontSize;
  }
  uint8_t effArabicFontFamily() const {
    return bookArabicFontFamily != BOOK_NO_OVERRIDE ? bookArabicFontFamily : arabicFontFamily;
  }
  uint8_t effFontFamily() const { return bookFontFamily != BOOK_NO_OVERRIDE ? bookFontFamily : fontFamily; }
  uint8_t effLineSpacing() const { return bookLineSpacing != BOOK_NO_OVERRIDE ? bookLineSpacing : lineSpacing; }
  uint8_t effParagraphAlignment() const {
    return bookParagraphAlignment != BOOK_NO_OVERRIDE ? bookParagraphAlignment : paragraphAlignment;
  }
  // Empty string means "no SD family": either forced built-in for this book, or
  // the global setting itself is built-in.
  const char* effSdFontFamilyName() const {
    if (bookSdFontFamilyName[0] == BOOK_FORCE_BUILTIN_FAMILY[0]) return "";
    return bookSdFontFamilyName[0] != '\0' ? bookSdFontFamilyName : sdFontFamilyName;
  }
  const char* effSdArabicFontFamilyName() const {
    if (bookSdArabicFontFamilyName[0] == BOOK_FORCE_BUILTIN_FAMILY[0]) return "";
    return bookSdArabicFontFamilyName[0] != '\0' ? bookSdArabicFontFamilyName : sdArabicFontFamilyName;
  }
  bool hasBookOverrides() const {
    return bookFontSize != BOOK_NO_OVERRIDE || bookArabicFontSize != BOOK_NO_OVERRIDE ||
           bookArabicFontFamily != BOOK_NO_OVERRIDE || bookFontFamily != BOOK_NO_OVERRIDE ||
           bookLineSpacing != BOOK_NO_OVERRIDE || bookParagraphAlignment != BOOK_NO_OVERRIDE ||
           bookSdFontFamilyName[0] != '\0' || bookSdArabicFontFamilyName[0] != '\0';
  }
  void clearBookOverrides() {
    bookFontSize = bookArabicFontSize = bookArabicFontFamily = bookFontFamily = bookLineSpacing =
        bookParagraphAlignment = BOOK_NO_OVERRIDE;
    bookSdFontFamilyName[0] = '\0';
    bookSdArabicFontFamilyName[0] = '\0';
  }
  // Track reading time/stats (session time, pace, streaks -- see
  // src/reading/ReadingStatsStore.h). 1 = on. Turning it off stops accumulation;
  // already-saved stats are kept.
  uint8_t trackReadingStats = 1;
  // Daily reading goal, as an index into DAILY_GOAL_MINUTES (default 30 minutes).
  // Drives the goal card on the stats screen and the heatmap's goal-met badges.
  uint8_t dailyReadingGoal = 1;
  static constexpr uint16_t DAILY_GOAL_MINUTES[6] = {15, 30, 45, 60, 90, 120};
  uint64_t getDailyGoalMs() const {
    const uint8_t index = dailyReadingGoal < 6 ? dailyReadingGoal : 1;
    return static_cast<uint64_t>(DAILY_GOAL_MINUTES[index]) * 60ULL * 1000ULL;
  }
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Settings -> System -> Pre-release: OtaUpdater checks GitHub's full
  // /releases list (newest-first, includes pre-releases) instead of
  // /releases/latest (which excludes them) when this is on, so "Check for
  // Update" can offer a pre-release/RC build if it's newer than the current
  // stable release. Off by default -- pre-release builds are for testers who
  // explicitly opt in, not the general OTA channel.
  uint8_t otaPrereleaseEnabled = 0;
  // Short press Back goes to file browser instead of home (0 = disabled, 1 = enabled)
  uint8_t backShortToFileBrowser = 0;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Touch screen reader zones/gestures on boards with a touch controller.
  uint8_t touchReaderControls = TOUCH_READER_ON;
  // Center-third tap opens the reader menu (0 = disabled, 1 = enabled). Only
  // surfaced on home-key boards, where the menu stays reachable without it.
  uint8_t tapForReaderMenu = 1;
  // Frontlight quick-panel state. Category-less SettingsList entries persist
  // these without adding them to the regular Settings screen.
  uint8_t frontlightBrightness = 60;
  uint8_t frontlightWarmth = 50;  // 0 = cool .. 100 = warm
  uint8_t frontlightOn = 0;
  // Restore the saved on/off state after a normal boot or wake. Brightness and
  // warmth are always remembered even when this is disabled.
  uint8_t frontlightRestoreOnWake = 1;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  static const char* getFilePath() { return "/.crosspoint/settings.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;

  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t pointSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 400;
  }
  int getReaderFontId() const;

  // Drop the SD font selection and fall back to the built-in family. The reader
  // point size comes back into BUILTIN_READER_POINT_SIZES with it, since that is
  // the only set a built-in family ships -- otherwise the settings UI would keep
  // offering a size nothing renders at. Both fields are persisted in one write.
  void clearSdFontFamily();

  // If count_only is true, returns the number of settings items that would be written.
  uint8_t writeSettings(HalFile& file, bool count_only = false) const;

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
  bool migrateLanguageBinaryFile();

 public:
  // Resolved status-bar composition. Consumers read the spec; only settings
  // editors read the raw fields.
  //
  // Deliberately NOT built under storeMutex: every field it reads is a single
  // byte, so a concurrent settings write can never produce a corrupt value —
  // only a snapshot mixing pre- and post-change fields. That costs at most one
  // e-ink frame drawn with a mixed status bar, which self-corrects on the next
  // refresh. Locking here would instead put a mutex on the render path and
  // stall it behind the SD write inside saveToFile(). Don't add one back.
  struct StatusBarSpec {
    bool showChapterPageCount = false;
    bool showBookProgressPercent = false;
    uint8_t titleMode = HIDE_TITLE;  // STATUS_BAR_TITLE
    bool showBattery = false;
    bool showBatteryPercent = false;
    uint8_t clockMode = STATUS_BAR_CLOCK_HIDE;  // STATUS_BAR_CLOCK_MODE
    bool clock12h = false;
    uint8_t clockUtcOffsetQ = 48;             // 48 = UTC+0
    uint8_t progressBarMode = HIDE_PROGRESS;  // STATUS_BAR_PROGRESS_BAR
    uint8_t progressBarHeightPx = 0;          // (thickness+1)*2; 0 when the bar is hidden
    uint8_t xtcMode = XTC_STATUS_BAR_HIDE;    // XTC_STATUS_BAR_MODE

    bool showsProgressBar() const { return progressBarMode != HIDE_PROGRESS; }
    bool showsTitle() const { return titleMode != HIDE_TITLE; }
    bool showsClock() const { return clockMode != STATUS_BAR_CLOCK_HIDE; }
    // Visibility of the text lane. Clock hardware presence is the caller's
    // concern: pass halClock.isAvailable(), or true for layout reservation.
    bool textLaneVisible(bool clockAvailable) const {
      return showChapterPageCount || showBookProgressPercent || showsTitle() || showBattery ||
             (showsClock() && clockAvailable);
    }
  };
  StatusBarSpec statusBarSpec() const;

  // Resolved text-rendering configuration for the Epub layout engine. The
  // viewport is renderer/orientation-derived, so the caller supplies it —
  // passing it in keeps a spec from ever existing in a half-filled state.
  // Unlocked for the same reason as statusBarSpec(); see the note above.
  ReaderRenderSpec readerRenderSpec(uint16_t viewportWidth, uint16_t viewportHeight) const;

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
