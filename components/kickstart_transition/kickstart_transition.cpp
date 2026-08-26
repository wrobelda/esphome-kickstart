#include "kickstart_transition.h"

#ifdef USE_ESP8266
#include <Esp.h>
extern "C" {
#include <user_interface.h>
}

namespace esphome::kickstart_transition {

static constexpr uint8_t V2_MAGIC = 0xEA;
static constexpr uint8_t UPGRADE_FLAG_FINISH_VALUE = 2;

void KickstartTransition::setup() {
  this->server_->init();
  this->server_->add_handler(this);
}

bool KickstartTransition::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() == HTTP_GET && request->url() == "/hub/status")
    return true;
  return request->method() == HTTP_POST && request->url() == "/hub/boot_other";
}

uint8_t KickstartTransition::current_slot_() const { return system_upgrade_userbin_check(); }

void KickstartTransition::handleRequest(AsyncWebServerRequest *request) {
  if (request->url() == "/hub/status")
    this->send_status_(request);
  else if (request->url() == "/hub/boot_other")
    this->boot_other_(request);
  else
    request->send(404, "application/json", "{\"error\":\"not_found\"}");
}

void KickstartTransition::send_status_(AsyncWebServerRequest *request) {
  const uint8_t current = this->current_slot_();
  const uint8_t other = current == 0 ? 1 : 0;
  const uint32_t actual_size = ESP.getFlashChipRealSize();
  char body[256];
  snprintf(body, sizeof(body),
           "{\"flash_size\":%u,\"flash_size_ok\":%s,\"current_slot\":%u,"
           "\"current_slot_offset\":%u,\"other_slot\":%u,\"other_slot_offset\":%u}",
           actual_size, actual_size == this->flash_size_ ? "true" : "false", current + 1,
           this->slots_[current].offset, other + 1, this->slots_[other].offset);
  request->send(200, "application/json", body);
}

void KickstartTransition::boot_other_(AsyncWebServerRequest *request) {
  if (!this->allow_boot_other_) {
    request->send(403, "application/json", "{\"error\":\"boot_switch_disabled\"}");
    return;
  }
  if (ESP.getFlashChipRealSize() != this->flash_size_) {
    request->send(409, "application/json", "{\"error\":\"unexpected_flash_size\"}");
    return;
  }
  if (!request->hasParam("confirm", true) || request->getParam("confirm", true)->value() != "boot-other") {
    request->send(400, "application/json", "{\"error\":\"confirmation_required\"}");
    return;
  }

  const uint8_t other = this->current_slot_() == 0 ? 1 : 0;
  uint8_t magic = 0;
  if (!ESP.flashRead(this->slots_[other].offset, &magic, 1) || magic != V2_MAGIC) {
    request->send(409, "application/json", "{\"error\":\"other_slot_not_v2\"}");
    return;
  }

  request->send(202, "application/json", "{\"status\":\"rebooting_to_other_slot\"}");
  this->set_timeout("boot-other", 500, []() {
    system_upgrade_flag_set(UPGRADE_FLAG_FINISH_VALUE);
    system_upgrade_reboot();
  });
}

}  // namespace esphome::kickstart_transition
#endif
