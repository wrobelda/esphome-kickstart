#pragma once

#ifdef USE_ESP8266

#include <ESPAsyncWebServer.h>
#include <array>

#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome::esp8266_nonos_v2_to_eboot_v1 {

struct Slot {
  uint32_t offset;
  uint32_t size;
};

/** Move a running non-OS V2 bridge to the eboot V1 layout.
 *
 * Some ESP8266 vendor firmwares use Espressif's paired non-OS SDK V2 user-bin
 * layout. Normal ESPHome firmware uses the eboot V1 layout, so the bootloader
 * and the application packaging must change. This component owns the layout
 * metadata and rewrites the bridge's own application as an eboot V1 E9 image at
 * flash 0x1000, installing the embedded eboot bootloader at sector zero last.
 * After the reboot the same application runs under eboot, so ordinary ESPHome
 * OTA and dashboard import work with the stock backend.
 *
 * The application bytes are not recompiled: the V2 user-bin and the V1 E9 image
 * are two packagings of the same linked application. The component's only
 * precondition is that it runs from the upper slot; a V1 factory image starts
 * at flash zero and extends through the lower slot, so converting from the
 * lower slot would overwrite the running code. Relocation and any backup are
 * the caller's responsibility.
 *
 * Conversion runs in one of two ways:
 *   - this component's `auto_convert: true`, which converts on the first
 *     upper-slot boot (relocation is slot control's own automatic option);
 *   - an explicit trigger (the `request_conversion()` method, or an
 *     authenticated POST to `/hub/convert`) sets a persistent request flag and
 *     reboots, and the next boot performs the conversion before Wi-Fi starts.
 *
 * While the V2 layout is still active the optional native OTA component is kept
 * from setting up, so ordinary upload paths are not reachable until eboot is
 * running.
 */
class Esp8266NonosV2ToEbootV1 : public AsyncWebHandler, public Component {
 public:
  static constexpr size_t SECTOR_SIZE = 0x1000;
  static constexpr uint8_t E9_MAGIC = 0xE9;
  static constexpr uint8_t V2_MAGIC = 0xEA;
  static constexpr uint8_t V2_MARKER = 0x04;
  static constexpr uint32_t V2_IROM_PAYLOAD_OFFSET = 16;
  static constexpr uint32_t FACTORY_PATCH_SIZE = 8;
  static constexpr uint32_t FACTORY_SIZE_OFFSET = SECTOR_SIZE + 16;
  static constexpr uint32_t FACTORY_CRC_OFFSET = SECTOR_SIZE + 20;
  // eboot's first IRAM segment loads at 0x4010F000; the vendor boot_v1.x loads
  // at 0x40100000, so this distinguishes an eboot sector zero structurally.
  static constexpr uint32_t EBOOT_IRAM_BASE = 0x4010F000;
  static constexpr const char *CONVERSION_PATH = "/hub/convert";
  static constexpr const char *CONVERSION_CONFIRMATION = "convert-v2-to-eboot";
  static constexpr uint32_t CONVERSION_REQUEST_MAGIC = 0x434F4E56;  // "CONV"

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
  /** Reference the native OTA component the profile enables (its id, wired
   * through the `ota_id` option). While the V2 layout is active this component
   * calls mark_failed() on it before its setup, so the OTA socket is never
   * created; after the conversion reboot the component is rebuilt and starts
   * normally. */
  void set_native_ota(Component *ota) { this->native_ota_ = ota; }
  /** Convert automatically on the first upper-slot boot that finds the V2
   * layout, at most once per power cycle. Relocation stays the slot-control
   * component's own automatic option. Off by default. */
  void set_auto_convert(bool auto_convert) { this->auto_convert_ = auto_convert; }

  uint32_t get_flash_size() const { return this->flash_size_; }
  const Slot &get_slot(size_t index) const { return this->slots_[index]; }
  uint32_t get_irom_vma() const { return this->irom_address_; }
  uint32_t get_irom_size() const { return this->irom_size_; }

  void setup() override;
  void dump_config() override;
  // Runs before esp8266_nonos_v2_slot_control (setup_priority::HARDWARE), so
  // its RTC preference allocation order is deterministic.
  float get_setup_priority() const override { return setup_priority::HARDWARE + 1.0f; }
  bool canHandle(AsyncWebServerRequest *request) const override;
  bool isRequestHandlerTrivial() const override { return false; }
  void handleRequest(AsyncWebServerRequest *request) override;

  bool convert();
  /** Persist a conversion request and reboot; the next boot converts before
   * Wi-Fi starts. Callable from an automation or a template button. */
  bool request_conversion();
  bool is_converted() const { return this->converted_; }
  uint8_t last_result() const { return this->last_result_; }
  const char *last_result_name() const { return result_name_(static_cast<Result>(this->last_result_)); }
  /** True once a conversion has been attempted (or completed) since the RTC
   * state was last cleared. The automatic flow uses it to make at most one
   * attempt per power cycle. */
  bool conversion_attempted() const { return this->last_result_ != static_cast<uint8_t>(Result::IDLE); }

 protected:
  enum class Result : uint8_t {
    IDLE,
    IN_PROGRESS,
    SUCCESS,
    ALREADY_CONVERTED,
    WRONG_FLASH_SIZE,
    UPPER_SLOT_REQUIRED,
    NOT_A_V2_IMAGE,
    V2_IMAGE_INVALID,
    OUT_OF_BOUNDS,
    FLASH_WRITE_FAILED,
    FLASH_READ_FAILED,
    READBACK_FAILED,
    BOOTLOADER_WRITE_FAILED,
  };

  static constexpr size_t MAX_SEGMENTS = 8;

  /** One V1 application segment and where its bytes live in the running V2 image. */
  struct SegmentSource {
    uint32_t vma;
    uint32_t size;
    uint32_t source_offset;  // flash offset of the payload in the running image
    bool irom;               // IROM payload is prefixed by the 8-byte patch
  };

  bool layout_is_eboot_() const;
  bool load_segments_(const Slot &slot);
  bool compute_app_layout_();
  uint8_t app_byte_(uint32_t offset);
  bool write_application_(uint32_t *app_size, uint32_t *factory_crc);
  bool verify_app_(uint32_t app_size, uint32_t factory_crc) const;
  bool commit_bootloader_();
  void erase_stale_upper_slot_();
  void gate_native_ota_();
  bool attempt_conversion_();
  void ensure_preferences_();
  void store_result_(Result result);
  void fail_(Result result);
  static const char *result_name_(Result result);
  static bool read_flash_(uint32_t offset, uint8_t *data, size_t len);
  static bool read_flash_block_(uint32_t offset, uint8_t *data, size_t len);
  static uint32_t eboot_crc32_update_(uint32_t crc, const uint8_t *data, size_t len);
  static uint32_t sdk_crc32_update_(uint32_t crc, const uint8_t *data, size_t len);
  static uint32_t read_u32_le_(const uint8_t *data);
  static int ram_region_(uint32_t vma, uint32_t size);

  web_server_base::WebServerBase *server_;
  std::array<Slot, 2> slots_{};
  std::array<SegmentSource, MAX_SEGMENTS> segments_{};
  size_t segment_count_{0};
  uint32_t flash_size_{0};
  uint32_t irom_address_{0};
  uint32_t irom_size_{0};
  uint32_t v2_irom_size_{0};
  uint32_t app_size_{0};
  uint32_t checksum_offset_{0};
  uint8_t app_checksum_{0};
  uint8_t flash_mode_{0};
  uint8_t flash_size_freq_{0};
  uint32_t entry_{0};
  bool converted_{false};
  bool auto_convert_{false};
  Result result_{Result::IDLE};
  Component *native_ota_{nullptr};
  ESPPreferenceObject conversion_request_;
  ESPPreferenceObject result_pref_;
  bool preferences_ready_{false};
  uint8_t previous_result_{0};
  uint8_t last_result_{0};
  alignas(uint32_t) std::array<uint8_t, SECTOR_SIZE> eboot_{};
  // Source block cache so app_byte_() reads flash in bounded blocks.
  alignas(uint32_t) std::array<uint8_t, 256> src_cache_{};
  uint32_t src_cache_offset_{0};
  size_t src_cache_len_{0};
};

}  // namespace esphome::esp8266_nonos_v2_to_eboot_v1

#endif
