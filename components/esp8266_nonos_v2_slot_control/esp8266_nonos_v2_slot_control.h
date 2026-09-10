#pragma once

#ifdef USE_ESP8266

#include <ESPAsyncWebServer.h>
#include <array>

#include "esphome/components/esp8266_nonos_v2_to_eboot_v1/esp8266_nonos_v2_to_eboot_v1.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

namespace esphome::esp8266_nonos_v2_slot_control {

class Esp8266NonosV2SlotControl : public AsyncWebHandler, public Component {
 public:
  Esp8266NonosV2SlotControl(web_server_base::WebServerBase *server,
                            esp8266_nonos_v2_to_eboot_v1::Esp8266NonosV2ToEbootV1 *migration)
      : server_(server), migration_(migration) {}

  /** Relocate a lower-slot bridge to the upper slot during setup. Off by
   * default; the copy route below is the manual alternative. */
  void set_auto_copy_lower_to_upper_slot(bool enabled) { this->auto_copy_lower_to_upper_slot_ = enabled; }

  /** Validate the other V2 slot and reboot into it. Callable from a button or
   * automation; mirrors POST /hub/boot_other. */
  void request_boot_other();
  /** Validate the running lower slot and relocate it to the upper slot.
   * Callable from a button or automation; mirrors
   * POST /hub/copy_lower_to_upper_slot. */
  void request_copy_lower_to_upper();

  /** Rescan both slots' validity and entry point and cache it. Cheap to call
   * repeatedly: rescanning is rate-limited unless *force* is set. */
  void refresh_slot_info(bool force = false);
  uint8_t active_slot_number() const { return this->active_number_; }
  bool active_slot_valid() const { return this->active_valid_; }
  bool inactive_slot_valid() const { return this->inactive_valid_; }
  uint32_t active_slot_entry() const { return this->active_entry_; }
  uint32_t inactive_slot_entry() const { return this->inactive_entry_; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void setup() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  enum class RelocationStatus : uint8_t {
    IDLE,
    RUNNING,
    LOWER_INVALID,
    COPY_FAILED,
    UPPER_INVALID,
  };

  uint8_t current_slot_() const;
  bool validate_v2_(const esp8266_nonos_v2_to_eboot_v1::Slot &slot, uint32_t *image_size = nullptr) const;
  /** Schedule a slot-info rescan in the main loop. The scan reads up to 1 MiB
   * per slot, so it must not run in an ESPAsyncWebServer callback. */
  void request_slot_scan_();
  bool copy_slot_(const esp8266_nonos_v2_to_eboot_v1::Slot &source,
                  const esp8266_nonos_v2_to_eboot_v1::Slot &destination, uint32_t image_size);
  void send_status_(AsyncWebServerRequest *request);
  void boot_other_(AsyncWebServerRequest *request);
  void copy_lower_to_upper_slot_(AsyncWebServerRequest *request);
  void perform_lower_to_upper_copy_(bool reboot_immediately = false);
  static const char *relocation_status_name_(RelocationStatus status);

  web_server_base::WebServerBase *server_;
  esp8266_nonos_v2_to_eboot_v1::Esp8266NonosV2ToEbootV1 *migration_;
  bool auto_copy_lower_to_upper_slot_{false};
  RelocationStatus relocation_status_{RelocationStatus::IDLE};
  uint8_t active_number_{0};
  bool active_valid_{false};
  bool inactive_valid_{false};
  uint32_t active_entry_{0};
  uint32_t inactive_entry_{0};
  uint32_t last_slot_scan_ms_{0};
  bool slot_scan_pending_{false};
  alignas(4) std::array<uint8_t, 0x1000> copy_buffer_{};
};

}  // namespace esphome::esp8266_nonos_v2_slot_control

#endif
