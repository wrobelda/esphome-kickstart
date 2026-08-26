#pragma once

#ifdef USE_ESP8266

#include <ESPAsyncWebServer.h>
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

namespace esphome::kickstart_transition {

struct Slot {
  uint32_t offset;
  uint32_t size;
};

class KickstartTransition : public AsyncWebHandler, public Component {
 public:
  explicit KickstartTransition(web_server_base::WebServerBase *server) : server_(server) {}
  void set_flash_size(uint32_t size) { this->flash_size_ = size; }
  void set_slot(uint8_t index, uint32_t offset, uint32_t size) { this->slots_[index] = {offset, size}; }
  void set_allow_boot_other(bool allow) { this->allow_boot_other_ = allow; }
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void setup() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

 protected:
  uint8_t current_slot_() const;
  void send_status_(AsyncWebServerRequest *request);
  void boot_other_(AsyncWebServerRequest *request);
  web_server_base::WebServerBase *server_;
  uint32_t flash_size_{0};
  Slot slots_[2]{};
  bool allow_boot_other_{false};
};

}  // namespace esphome::kickstart_transition
#endif
