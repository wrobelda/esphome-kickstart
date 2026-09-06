#pragma once

#ifdef USE_ESP8266

#include <array>

#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

namespace esphome::esp8266_nonos_v2_to_eboot_v1 {

struct Slot {
  uint32_t offset;
  uint32_t size;
};

class Esp8266NonosV2ToEbootV1 : public AsyncWebHandler, public Component {
 public:
  explicit Esp8266NonosV2ToEbootV1(web_server_base::WebServerBase *server) : server_(server) {}

  void set_flash_size(uint32_t size) { this->flash_size_ = size; }
  void set_irom_range(uint32_t address, uint32_t size) {
    this->irom_address_ = address;
    this->irom_size_ = size;
  }
  void set_slot(size_t index, uint32_t offset, uint32_t size) {
    if (index < this->slots_.size())
      this->slots_[index] = {offset, size};
  }
  uint32_t get_flash_size() const { return this->flash_size_; }
  const Slot &get_slot(size_t index) const { return this->slots_[index]; }

  void setup() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }
  bool canHandle(AsyncWebServerRequest *request) const override;
  bool isRequestHandlerTrivial() const override { return false; }
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len,
                    bool final) override;

 protected:
  enum class UploadResult : uint8_t {
    IDLE,
    RECEIVING,
    READY,
    SUCCESS,
    CONFIRMATION_REQUIRED,
    BUSY,
    WRONG_FLASH_SIZE,
    UNEXPECTED_SLOT,
    UPPER_SLOT_REQUIRED,
    OUT_OF_BOUNDS,
    OUT_OF_ORDER,
    FLASH_WRITE_FAILED,
    INVALID_FACTORY_IMAGE,
    READBACK_FAILED,
    BOOTLOADER_WRITE_FAILED,
  };

  static constexpr size_t SECTOR_SIZE = 0x1000;
  static constexpr uint32_t FACTORY_CRC_SIZE_OFFSET = SECTOR_SIZE + 16;
  static constexpr uint32_t FACTORY_CRC_VALUE_OFFSET = SECTOR_SIZE + 20;

  void reset_upload_();
  void fail_(UploadResult result);
  bool begin_upload_(AsyncWebServerRequest *request);
  bool consume_(size_t index, const uint8_t *data, size_t len);
  bool flush_application_sector_(bool final);
  bool validate_factory_image_();
  bool validate_factory_crc_();
  bool verify_readback_();
  bool commit_bootloader_();
  bool validate_e9_(uint32_t offset, uint32_t limit, bool bootloader, uint32_t *image_end);
  bool valid_segment_range_(uint32_t address, uint32_t size) const;
  bool read_(uint32_t offset, uint8_t *data, size_t len, bool include_uncommitted_bootloader) const;
  static bool read_flash_(uint32_t offset, uint8_t *data, size_t len);
  static uint32_t crc32_update_(uint32_t crc, const uint8_t *data, size_t len);
  static uint32_t eboot_crc32_update_(uint32_t crc, const uint8_t *data, size_t len);
  static const char *result_name_(UploadResult result);

  web_server_base::WebServerBase *server_;
  std::array<Slot, 2> slots_{};
  uint32_t flash_size_{0};
  uint32_t irom_address_{0};
  uint32_t irom_size_{0};
  uint32_t destination_limit_{0};
  uint32_t image_size_{0};
  uint32_t upload_crc_{0xFFFFFFFFUL};
  uint32_t buffer_address_{SECTOR_SIZE};
  size_t buffer_length_{0};
  UploadResult result_{UploadResult::IDLE};
  alignas(uint32_t) std::array<uint8_t, SECTOR_SIZE> bootloader_{};
  alignas(uint32_t) std::array<uint8_t, SECTOR_SIZE> buffer_{};
};

}  // namespace esphome::esp8266_nonos_v2_to_eboot_v1

#endif
