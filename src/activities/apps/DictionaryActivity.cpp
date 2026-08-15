#include "DictionaryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "DictionaryStore.h"
#include "MappedInputManager.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "activities/reader/DictionaryHistoryActivity.h"
#include "activities/reader/DictionarySuggestionsActivity.h"
#include "activities/settings/DictionaryDownloadActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Foulad eInk hub layout: lookup + history + store first, then vcodex's
// text-size/clear-history actions, then the dictionary list.
constexpr int DICTIONARY_ACTION_COUNT = 5;
constexpr int ACTION_LOOKUP_WORD = 0;
constexpr int ACTION_LOOKUP_HISTORY = 1;
constexpr int ACTION_DOWNLOAD_DICTS = 2;
constexpr int ACTION_DEFINITION_TEXT_SIZE = 3;
constexpr int ACTION_CLEAR_HISTORY = 4;
}  // namespace

void DictionaryActivity::onEnter() {
  Activity::onEnter();
  DICTIONARIES.ensureScanned();
  const int activeIndex = DICTIONARIES.getActiveIndex();
  selectedIndex = activeIndex >= 0 ? activeIndex + DICTIONARY_ACTION_COUNT : 0;
  requestUpdate();
}

void DictionaryActivity::lookupTypedWord(const std::string& word) {
  const auto lookup = DICTIONARIES.lookup(word, true);
  const int readerFontId = SETTINGS.getReaderFontId();
  if (lookup.status == DictionaryLookupResult::Status::Found) {
    // No page/background: this is the standalone app, not the reader overlay.
    startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                               renderer, mappedInput, nullptr, lookup.headword, lookup.definition, lookup.truncated,
                               readerFontId, DICTIONARIES.getDefinitionFontId(readerFontId), 0, 0,
                               /*renderPageBackground=*/false),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  if (lookup.status == DictionaryLookupResult::Status::NotFound && !lookup.suggestions.empty()) {
    startActivityForResult(std::make_unique<DictionarySuggestionsActivity>(renderer, mappedInput, nullptr, lookup.query,
                                                                           lookup.suggestions, readerFontId, 0, 0),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  const char* msg;
  switch (lookup.status) {
    case DictionaryLookupResult::Status::NoDictionary:
      msg = tr(STR_NO_DICTIONARIES);
      break;
    case DictionaryLookupResult::Status::NotReady:
      msg = tr(STR_DICTIONARY_PREPARE_FAILED);
      break;
    case DictionaryLookupResult::Status::LowMemory:
      msg = tr(STR_DICTIONARY_LOW_MEMORY);
      break;
    case DictionaryLookupResult::Status::DecompressError:
      msg = tr(STR_DICTIONARY_DECOMPRESS_ERROR);
      break;
    case DictionaryLookupResult::Status::ReadError:
      msg = tr(STR_DICTIONARY_READ_ERROR);
      break;
    default:
      msg = tr(STR_DEFINITION_NOT_FOUND);
      break;
  }
  GUI.drawPopup(renderer, msg);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  delay(900);
  requestUpdate();
}

void DictionaryActivity::selectCurrent() {
  if (selectedIndex == ACTION_LOOKUP_WORD) {
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_LOOKUP_WORD), "", 63, InputType::Text),
        [this](const ActivityResult& result) {
          if (result.isCancelled) {
            requestUpdate();
            return;
          }
          const auto& kb = std::get<KeyboardResult>(result.data);
          if (kb.text.empty()) {
            requestUpdate();
            return;
          }
          lookupTypedWord(kb.text);
        });
    return;
  }
  if (selectedIndex == ACTION_LOOKUP_HISTORY) {
    startActivityForResult(
        std::make_unique<DictionaryHistoryActivity>(renderer, mappedInput, nullptr, SETTINGS.getReaderFontId(), 0, 0),
        [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  if (selectedIndex == ACTION_DOWNLOAD_DICTS) {
    startActivityForResult(std::make_unique<DictionaryDownloadActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) {
                             // Newly downloaded sets need a rescan to appear in the list.
                             DICTIONARIES.scan();
                             requestUpdate();
                           });
    return;
  }
  if (selectedIndex == ACTION_DEFINITION_TEXT_SIZE) {
    const uint8_t nextSize =
        static_cast<uint8_t>((DICTIONARIES.getDefinitionTextSize() + 1) % DictionaryStore::DEF_TEXT_SIZE_COUNT);
    DICTIONARIES.setDefinitionTextSize(nextSize);
    requestUpdate();
    return;
  }
  if (selectedIndex == ACTION_CLEAR_HISTORY) {
    DICTIONARIES.clearHistory();
    GUI.drawPopup(renderer, tr(STR_CLEAR_HISTORY));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    delay(650);
    requestUpdate();
    return;
  }

  const auto& entries = DICTIONARIES.getEntries();
  const int dictionaryIndex = selectedIndex - DICTIONARY_ACTION_COUNT;
  if (dictionaryIndex < 0 || dictionaryIndex >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[dictionaryIndex];
  if (entry.missingFiles || entry.unsupportedFormat) {
    // unsupportedFormat = .ifo declared idxoffsetbits=64. Shares the "missing
    // files" wording rather than adding a string every translation would have to
    // carry: from the user's side both mean "this folder isn't usable", and the
    // 64-bit variant is rare enough not to warrant its own message.
    GUI.drawPopup(renderer, tr(STR_DICTIONARY_MISSING_FILES));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    delay(1100);
    requestUpdate();
    return;
  }

  DICTIONARIES.setActiveIndex(dictionaryIndex);
  Rect popup;
  {
    RenderLock lock(*this);
    popup = GUI.drawPopup(renderer, tr(STR_DICTIONARY_PREPARING));
  }
  const bool ready = DICTIONARIES.prepareActive([this, &popup](int percent) {
    RenderLock lock(*this);
    GUI.fillPopupProgress(renderer, popup, percent);
  });
  GUI.drawPopup(renderer, ready ? tr(STR_DICTIONARY_READY) : tr(STR_DICTIONARY_PREPARE_FAILED));
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  delay(900);
  requestUpdate();
}

void DictionaryActivity::loop() {
  const auto& entries = DICTIONARIES.getEntries();
  const int totalItems = static_cast<int>(entries.size()) + DICTIONARY_ACTION_COUNT;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);

  // wasPressed, NOT vcodex's wasReleased: our SettingsActivity dispatches on
  // the Confirm PRESS, so the leftover RELEASE landed here and instantly
  // opened row 0 ("Look up a word") on entry. All dictionary activities act
  // on presses so edges can't leak across the chain (same convention as
  // FontDownloadActivity, launched from the same menu).
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    selectCurrent();
    return;
  }

  buttonNavigator.onNext([this, totalItems] {
    if (totalItems > 0) {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
      requestUpdate();
    }
  });
  buttonNavigator.onPrevious([this, totalItems] {
    if (totalItems > 0) {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
      requestUpdate();
    }
  });
  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    if (totalItems > 0) {
      selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
      requestUpdate();
    }
  });
  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    if (totalItems > 0) {
      selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
      requestUpdate();
    }
  });
}

void DictionaryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& entries = DICTIONARIES.getEntries();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  // vcodex draws a date in the header (their fork's HeaderDateUtils); ours
  // shows the active dictionary label as the plain header subtitle instead.
  const std::string activeLabel = DICTIONARIES.getActiveLabel();
  std::string headerTitle = tr(STR_DICTIONARY);
  if (!activeLabel.empty()) headerTitle += " - " + activeLabel;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int activeIndex = DICTIONARIES.getActiveIndex();

  auto textSizeLabel = []() -> const char* {
    switch (DICTIONARIES.getDefinitionTextSize()) {
      case DictionaryStore::DEF_TEXT_SMALL:
        return tr(STR_SMALL);
      case DictionaryStore::DEF_TEXT_LARGE:
        return tr(STR_LARGE);
      default:
        return tr(STR_SMALL);
    }
  };

  if (entries.empty()) {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, metrics.listRowHeight * DICTIONARY_ACTION_COUNT},
        DICTIONARY_ACTION_COUNT, selectedIndex,
        [](int index) {
          if (index == ACTION_LOOKUP_WORD) return std::string(tr(STR_LOOKUP_WORD));
          if (index == ACTION_LOOKUP_HISTORY) return std::string(tr(STR_LOOKUP_HISTORY));
          if (index == ACTION_DOWNLOAD_DICTS) return std::string(tr(STR_DOWNLOAD_DICTIONARIES));
          if (index == ACTION_DEFINITION_TEXT_SIZE) return std::string(tr(STR_DEFINITION_TEXT_SIZE));
          return std::string(tr(STR_CLEAR_HISTORY));
        },
        nullptr, nullptr,
        [&textSizeLabel](int index) {
          if (index == ACTION_DEFINITION_TEXT_SIZE) return std::string(textSizeLabel());
          return std::string();
        },
        true);
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + metrics.listRowHeight * DICTIONARY_ACTION_COUNT + 22,
                              tr(STR_NO_DICTIONARIES));
    renderer.drawCenteredText(SMALL_FONT_ID, contentTop + metrics.listRowHeight * DICTIONARY_ACTION_COUNT + 48,
                              "/dictionaries/<language>/");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        static_cast<int>(entries.size()) + DICTIONARY_ACTION_COUNT, selectedIndex,
        [&entries](int index) {
          if (index == ACTION_LOOKUP_WORD) return std::string(tr(STR_LOOKUP_WORD));
          if (index == ACTION_LOOKUP_HISTORY) return std::string(tr(STR_LOOKUP_HISTORY));
          if (index == ACTION_DOWNLOAD_DICTS) return std::string(tr(STR_DOWNLOAD_DICTIONARIES));
          if (index == ACTION_DEFINITION_TEXT_SIZE) return std::string(tr(STR_DEFINITION_TEXT_SIZE));
          if (index == ACTION_CLEAR_HISTORY) return std::string(tr(STR_CLEAR_HISTORY));
          return entries[index - DICTIONARY_ACTION_COUNT].languageId;
        },
        [&entries](int index) {
          if (index < DICTIONARY_ACTION_COUNT) return std::string();
          return entries[index - DICTIONARY_ACTION_COUNT].name;
        },
        nullptr,
        [&entries, activeIndex, &textSizeLabel](int index) {
          if (index == ACTION_DEFINITION_TEXT_SIZE) return std::string(textSizeLabel());
          if (index < DICTIONARY_ACTION_COUNT) return std::string();
          const int entryIndex = index - DICTIONARY_ACTION_COUNT;
          // Active takes priority: a compressed dictionary can now be active (readDefinition
          // handles .dz), so the "ZIP" format badge must not hide that state.
          if (entryIndex == activeIndex) return std::string(tr(STR_DICTIONARY_ACTIVE));
          if (entries[entryIndex].missingFiles || entries[entryIndex].unsupportedFormat) return std::string("!");
          if (entries[entryIndex].compressed) return std::string("ZIP");
          return std::string();
        },
        true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
