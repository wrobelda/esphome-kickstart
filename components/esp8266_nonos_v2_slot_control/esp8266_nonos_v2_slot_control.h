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

  void set_auto_copy_lower_to_upper_slot(bool enabled) { this->auto_copy_lower_to_upper_slot_ = enabled; }

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
  alignas(4) std::array<uint8_t, 0x1000> copy_buffer_{};
};

}  // namespace esphome::esp8266_nonos_v2_slot_control

#endif
