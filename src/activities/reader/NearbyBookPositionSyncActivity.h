#pragma once

#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "KOReaderCredentialStore.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

/**
 * Shares the position in the currently open EPUB with another reader over
 * ESP-NOW -- no WiFi network, no server, no account.
 *
 * Both readers open this screen with the same book. One presses Share; the
 * other is shown the incoming position next to its own and confirms before it
 * is applied. The wire format is CrossInk's ('CIBP', protocol 1), so this also
 * talks to a CrossInk X3/X4/X4 Pro.
 *
 * The screen writes the accepted position to progress.bin itself and then
 * silent-reboots back into the book: bringing the WiFi radio up fragments the
 * heap badly enough that resuming rendering in-place is not safe on the C3.
 */
class NearbyBookPositionSyncActivity final : public Activity {
 public:
  explicit NearbyBookPositionSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                          std::shared_ptr<Epub> epub, const std::string& epubPath,
                                          int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                          KOReaderPosition localKoPos, std::string localChapterName,
                                          DocumentMatchMethod matchMethod,
                                          std::optional<uint16_t> currentParagraphIndex = std::nullopt,
                                          std::optional<uint16_t> currentListItemIndex = std::nullopt);
  ~NearbyBookPositionSyncActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool isReaderActivity() const override { return true; }
  // isReaderActivity() would otherwise keep a paired BLE remote alive; the radio
  // and the ~56KB the NimBLE stack holds are both needed by ESP-NOW instead.
  bool keepsBluetoothActive() const override { return false; }
  bool skipLoopDelay() override { return state_ == State::DISCOVERING || state_ == State::SYNCING; }

  // Called from the ESP-NOW receive callback (WiFi task context).
  void enqueueEspNowPacket(const uint8_t* sourceMac, const uint8_t* data, int length);

  static constexpr size_t DOCUMENT_HASH_BYTES = 32;
  static constexpr size_t MAX_DEVICE_NAME_BYTES = 20;
  static constexpr size_t MIN_DEVICE_NAME_BYTES = 2;
  static constexpr size_t MAX_ANCHOR_BYTES = 48;
  static constexpr size_t MAX_XPATH_BYTES = 120;

  struct CompactPosition {
    std::array<char, DOCUMENT_HASH_BYTES + 1> documentHash = {};
    uint32_t percentageQ = 0;
    uint16_t spineIndex = 0;
    uint16_t pageNumber = 0;
    uint16_t totalPages = 1;
    uint16_t paragraphIndex = 0;
    uint16_t liIndex = 0;
    bool hasParagraphIndex = false;
    bool hasLiIndex = false;
    std::array<char, MAX_ANCHOR_BYTES + 1> anchor = {};
    std::array<char, MAX_XPATH_BYTES + 1> xpath = {};
  };

 private:
  enum class State { STARTING, READY, DISCOVERING, SYNCING, SHOWING_RESULT, SYNCED, ERROR };
  enum class PacketType : uint8_t { HELLO = 1, POSITION = 2, APPLY = 3, ACK = 4, NAME = 5, INVALID = 0xFF };

  static constexpr size_t MAX_SYNC_EVENTS = 8;

  struct SyncEvent {
    PacketType type = PacketType::HELLO;
    std::array<uint8_t, 6> sourceMac = {};
    std::array<uint8_t, 6> deviceMac = {};
    CompactPosition position = {};
    std::array<char, MAX_DEVICE_NAME_BYTES + 1> deviceName = {};
  };

  State state_ = State::STARTING;
  SemaphoreHandle_t eventMutex_ = nullptr;
  std::array<SyncEvent, MAX_SYNC_EVENTS> events_ = {};
  uint8_t eventHead_ = 0;
  uint8_t eventCount_ = 0;
  bool eventOverflow_ = false;
  bool espNowStarted_ = false;
  bool radioActivated_ = false;
  bool localPrepared_ = false;
  bool peerSeen_ = false;
  bool peerPositionReceived_ = false;
  bool sourceMode_ = false;
  bool localPositionSent_ = false;
  bool localPositionAcked_ = false;

  std::shared_ptr<Epub> epub_;
  std::string epubPath_;
  std::string localChapterName_;
  std::string localDeviceName_;
  std::string peerId_;
  std::string peerName_;
  std::string errorMessage_;
  int currentSpineIndex_ = 0;
  int currentPage_ = 0;
  int totalPagesInSpine_ = 1;
  std::optional<uint16_t> currentParagraphIndex_;
  std::optional<uint16_t> currentListItemIndex_;
  KOReaderPosition localKoPosition_;
  DocumentMatchMethod matchMethod_ = DocumentMatchMethod::FILENAME;
  CompactPosition localPosition_ = {};
  CompactPosition peerPosition_ = {};
  CrossPointPosition peerCrossPoint_ = {};

  std::array<uint8_t, 6> localDeviceMac_ = {};
  std::array<uint8_t, 6> peerSourceMac_ = {};
  std::array<uint8_t, 6> peerDeviceMac_ = {};

  uint32_t syncStartedMs_ = 0;
  uint32_t lastHelloMs_ = 0;
  uint32_t lastPositionSendMs_ = 0;

  bool prepareLocalPosition();
  bool beginEspNow();
  void endEspNow();
  void startSync();
  void processEvents();
  void handleEvent(const SyncEvent& event);
  bool sendPacket(PacketType type, const uint8_t* peerMac);
  bool sendHello(const uint8_t* peerMac);
  bool sendDeviceName(const uint8_t* peerMac);
  bool sendLocalPosition();
  bool sendAck(const uint8_t* peerMac);
  static bool addPeer(const uint8_t* peerMac);
  bool applyPeerPosition();
  bool mapPeerPosition();
  void updateSyncProgress();
  void returnToReader();
  void setState(State state);
  void setError(const std::string& error);
  void renderStatus(const char* primary, const std::string& detail, const std::string& detailSecondary) const;
  void renderComparison() const;
};
