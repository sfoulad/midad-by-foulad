#pragma once

#include <HalStorage.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <memory>
#include <string>
#include <vector>

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    HalFile file;
    String fileName;
    String path = "/";
    size_t size = 0;
    bool success = false;
    String error = "";

    // Upload write buffer - batches small writes into larger SD card operations
    // 4KB is a good balance: large enough to reduce syscall overhead, small enough
    // to keep individual write times short and avoid watchdog issues
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    UploadState() { buffer.resize(UPLOAD_BUFFER_SIZE); }
  } upload;

  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }

  WsUploadStatus getWsUploadStatus() const;

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  NetworkUDP udp;
  bool udpActive = false;

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void abortWsUpload(const char* tag);

  // File scanning
  void scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const;
  String formatFileSize(size_t bytes) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleJszip() const;
  void handleNotFound() const;
  void handleStatus() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList() const;
  void handleFontUpload();
  void handleFontUploadData();
  void handleFontDelete();
  void handleFontConvert();
  void handleFontConvertUploadData();

  // Font upload state
  struct FontUploadState {
    HalFile file;
    std::string familyName;
    std::string filePath;
    bool valid = false;
    bool magicChecked = false;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    FontUploadState() { buffer.resize(BUFFER_SIZE); }
  } fontUpload;

  // Convert-font upload state: the raw TTF/OTF is staged to a scratch path
  // (not installed as a font directly) before being relayed to foulad-ebooks.
  // See handleFontConvertUploadData()/handleFontConvert().
  struct FontConvertUploadState {
    // Up to 4 style files arrive sequentially as multipart parts named
    // "font" (regular, required), "font_bold", "font_italic",
    // "font_bolditalic" -- matching the foulad-ebooks endpoint. Each saved
    // part is relayed onward by handleFontConvert(). Only one part streams at
    // a time, so a single write buffer is shared across them.
    static constexpr int MAX_STYLE_FILES = 4;
    static constexpr const char* PART_NAMES[MAX_STYLE_FILES] = {"font", "font_bold", "font_italic", "font_bolditalic"};
    HalFile file;                            // the part currently streaming
    int currentSlot = -1;                    // index into PART_NAMES for the streaming part
    std::string filePaths[MAX_STYLE_FILES];  // temp SD path per saved part ("" = absent)
    int partsSeen = 0;
    // family/language are NOT stored here: they're read straight from
    // server->arg() in handleFontConvert() after the full body is parsed, which
    // is order-independent. Storing them meant reading during the upload
    // callback, where fields after the file part aren't parsed yet.
    bool valid = false;
    // Specific reason valid==false, surfaced to the web UI so the user sees
    // *which* field was rejected instead of one opaque combined message (the
    // real reason was previously only in the serial log they can't see).
    std::string rejectReason;
    size_t bytesWritten = 0;  // total across all parts
    // Raw font files (esp. variable fonts) can run a few MB -- cap well below
    // available SD/heap headroom so a bad/huge upload fails cleanly instead of
    // filling the card or ballooning the relay's own read buffer needs.
    // Applied to the TOTAL across all style files.
    static constexpr size_t MAX_SIZE = 10 * 1024 * 1024;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    FontConvertUploadState() { buffer.resize(BUFFER_SIZE); }
  } fontConvertUpload;

  // OPDS server handlers
  void handleGetOpdsServers() const;
  void handlePostOpdsServer();
  void handleDeleteOpdsServer();

  // Wi-Fi credential handlers
  void handleGetWifiNetworks() const;
  void handlePostWifiNetwork();
  void handleDeleteWifiNetwork();
};
