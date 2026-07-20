#include "DictionaryDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include "DictionaryStore.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

DictionaryDownloadActivity::DictionaryDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("DictionaryDownload", renderer, mappedInput) {}

// --- Lifecycle (mirrors FontDownloadActivity) ---

void DictionaryDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void DictionaryDownloadActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void DictionaryDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }
  {
    RenderLock lock(*this);
    state_ = LOADING_CATALOG;
  }
  requestUpdateAndWait();

  if (!fetchAndParseCatalog()) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }
  {
    RenderLock lock(*this);
    state_ = DICT_LIST;
    selectedIndex_ = 0;
  }
}

// --- Catalog ---

bool DictionaryDownloadActivity::fetchAndParseCatalog() {
  static constexpr const char* CATALOG_TMP = "/dicts_catalog.tmp";

  auto result = HttpDownloader::downloadToFile(FOULAD_DICTS_CATALOG_URL, CATALOG_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("DICT", "Failed to fetch catalog from %s", FOULAD_DICTS_CATALOG_URL);
    errorMessage_ = "Failed to fetch dictionary list";
    Storage.remove(CATALOG_TMP);
    return false;
  }

  HalFile catalogFile;
  if (!Storage.openFileForRead("DICT", CATALOG_TMP, catalogFile)) {
    Storage.remove(CATALOG_TMP);
    errorMessage_ = "Failed to read dictionary list";
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, catalogFile);
  catalogFile.close();
  Storage.remove(CATALOG_TMP);

  if (err || (doc["version"] | 0) != 1) {
    LOG_ERR("DICT", "Catalog parse error or bad version");
    errorMessage_ = "Invalid dictionary catalog";
    return false;
  }

  baseUrl_ = doc["baseUrl"] | "";
  dicts_.clear();

  for (JsonObject dObj : doc["dicts"].as<JsonArray>()) {
    CatalogDict dict;
    dict.slug = dObj["slug"] | "";
    dict.name = dObj["name"] | "";
    dict.description = dObj["description"] | "";
    dict.direction = dObj["direction"] | "";
    dict.totalSize = dObj["totalSize"] | 0;
    bool valid = !dict.slug.empty();
    for (JsonObject fObj : dObj["files"].as<JsonArray>()) {
      CatalogFile file;
      file.name = fObj["name"] | "";
      file.size = fObj["size"] | 0;
      if (!fObj["crc32"].is<uint32_t>() || file.name.rfind(dict.slug + ".", 0) != 0) {
        valid = false;
        break;
      }
      file.crc32 = fObj["crc32"].as<uint32_t>();
      dict.files.push_back(std::move(file));
    }
    if (!valid || dict.files.empty()) {
      LOG_ERR("DICT", "Skipping malformed catalog entry: %s", dict.slug.c_str());
      continue;
    }
    dicts_.push_back(std::move(dict));
  }

  refreshInstalledFlags();
  LOG_DBG("DICT", "Catalog loaded: %zu dictionaries", dicts_.size());
  return true;
}

std::string DictionaryDownloadActivity::installDir(const CatalogDict& dict) const {
  // /dictionaries/<direction>/ -- the directory doubles as the group label in
  // the Dictionary app's scan (DictionaryStore::DICTIONARY_ROOT).
  std::string dir = DictionaryStore::DICTIONARY_ROOT;
  dir += "/";
  dir += dict.direction.empty() ? dict.slug : dict.direction;
  return dir;
}

void DictionaryDownloadActivity::refreshInstalledFlags() {
  for (auto& dict : dicts_) {
    const std::string dir = installDir(dict);
    dict.installed = !dict.files.empty();
    for (const auto& file : dict.files) {
      const std::string path = dir + "/" + file.name;
      HalFile f;
      bool ok = false;
      if (Storage.openFileForRead("DICT", path.c_str(), f)) {
        // Size match doubles as the update check, like the font store.
        ok = f.fileSize() == file.size;
        f.close();
      }
      if (!ok) {
        dict.installed = false;
        break;
      }
    }
  }
}

// --- Download / delete ---

bool DictionaryDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  HalFile f;
  if (!Storage.openFileForRead("DICT", path, f)) return false;
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, BUF_SIZE);
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  outCrc = crc;
  return true;
}

void DictionaryDownloadActivity::downloadDict(CatalogDict& dict) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingIndex_ = static_cast<int>(&dict - dicts_.data());
    currentFileIndex_ = 0;
    currentFileTotal_ = dict.files.size();
    fileProgress_ = 0;
    fileTotal_ = 0;
    cancelRequested_ = false;
  }
  requestUpdateAndWait();

  const std::string dir = installDir(dict);
  Storage.mkdir(DictionaryStore::DICTIONARY_ROOT);
  Storage.mkdir(dir.c_str());

  auto abortCleanup = [&] {
    for (const auto& file : dict.files) {
      Storage.remove((dir + "/" + file.name).c_str());
    }
    dict.installed = false;
  };

  for (size_t i = 0; i < dict.files.size(); i++) {
    const auto& file = dict.files[i];
    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    const std::string destPath = dir + "/" + file.name;
    const std::string url = baseUrl_ + file.name;

    auto result = HttpDownloader::downloadToFile(
        url, destPath,
        [this](size_t downloaded, size_t total) {
          fileProgress_ = downloaded;
          fileTotal_ = total;
          mappedInput.update();
          if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
              mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            cancelRequested_ = true;
          }
          requestUpdate(true);
        },
        &cancelRequested_);

    if (result == HttpDownloader::ABORTED) {
      abortCleanup();
      RenderLock lock(*this);
      state_ = DICT_LIST;
      return;
    }
    if (result != HttpDownloader::OK) {
      LOG_ERR("DICT", "Download failed: %s (%d)", file.name.c_str(), result);
      abortCleanup();
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Download failed: " + file.name;
      return;
    }

    uint32_t actualCrc = 0;
    if (!computeFileCrc32(destPath.c_str(), actualCrc) || actualCrc != file.crc32) {
      LOG_ERR("DICT", "CRC mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
      abortCleanup();
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Checksum mismatch: " + file.name;
      return;
    }
  }

  // A re-downloaded (updated) set invalidates any previously built lookup
  // cache; the Dictionary app rebuilds it on next activation.
  Storage.remove((dir + "/" + dict.slug + ".cpridx").c_str());

  dict.installed = true;
  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void DictionaryDownloadActivity::deleteDict(const CatalogDict& dict) {
  const std::string dir = installDir(dict);
  DICTIONARIES.clearActiveIfMatches(dir + "/" + dict.slug + ".ifo");
  for (const auto& file : dict.files) {
    Storage.remove((dir + "/" + file.name).c_str());
  }
  // Device-generated artifacts that never appear in the catalog.
  Storage.remove((dir + "/" + dict.slug + ".cpridx").c_str());
  Storage.remove((dir + "/" + dict.slug + ".syn").c_str());
  Storage.rmdir(dir.c_str());  // no-op if other sets share the directory
}

void DictionaryDownloadActivity::promptDeleteSelected() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(dicts_.size())) return;
  const auto& dict = dicts_[selectedIndex_];
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), dict.name),
      [this](const ActivityResult& result) {
        if (!result.isCancelled && selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(dicts_.size())) {
          deleteDict(dicts_[selectedIndex_]);
          refreshInstalledFlags();
        }
        requestUpdate();
      });
}

// --- Input ---

void DictionaryDownloadActivity::loop() {
  if (state_ == DICT_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    const int listSize = static_cast<int>(dicts_.size());
    buttonNavigator_.onScrollNextRelease([this, listSize] {
      if (listSize > 0) {
        selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
        requestUpdate();
      }
    });
    buttonNavigator_.onScrollPreviousRelease([this, listSize] {
      if (listSize > 0) {
        selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
        requestUpdate();
      }
    });
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !dicts_.empty()) {
      auto& dict = dicts_[selectedIndex_];
      if (dict.installed) {
        promptDeleteSelected();
        return;
      }
      downloadDict(dict);
      requestUpdateAndWait();
      return;
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = DICT_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = DICT_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingIndex_ >= 0 && downloadingIndex_ < static_cast<int>(dicts_.size())) {
        downloadDict(dicts_[downloadingIndex_]);
        requestUpdateAndWait();
        return;
      }
      RenderLock lock(*this);
      state_ = DICT_LIST;
      requestUpdate();
    }
  }
}

// --- Rendering ---

std::string DictionaryDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void DictionaryDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_DOWNLOAD_DICTIONARIES));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_CATALOG) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING));
  } else if (state_ == DICT_LIST) {
    if (dicts_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_DICTIONARIES));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          static_cast<int>(dicts_.size()), selectedIndex_,
          [this](int index) { return dicts_[index].name + " (" + formatSize(dicts_[index].totalSize) + ")"; },
          [this](int index) { return dicts_[index].description; }, nullptr,
          [this](int index) { return dicts_[index].installed ? std::string(tr(STR_INSTALLED)) : std::string(); }, true,
          [this](int index) { return dicts_[index].installed; });

      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK),
                                (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(dicts_.size()) &&
                                 dicts_[selectedIndex_].installed)
                                    ? tr(STR_DELETE)
                                    : tr(STR_DOWNLOAD),
                                tr(STR_DIR_UP), tr(STR_DIR_DOWN), /*rtlSwap=*/false);
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    const auto& dict = dicts_[downloadingIndex_];
    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + dict.name + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    const int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_DICTIONARY_READY), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DOWNLOAD_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
