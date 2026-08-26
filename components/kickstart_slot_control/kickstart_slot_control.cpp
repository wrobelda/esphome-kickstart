#include "kickstart_slot_control.h"

#ifdef USE_ESP8266

#include <Esp.h>
extern "C" {
#include <user_interface.h>
}

namespace esphome::kickstart_slot_control {

static constexpr uint8_t V2_MAGIC = 0xEA;
static constexpr uint8_t UPGRADE_FLAG_FINISH_VALUE = 2;

void KickstartSlotControl::setup() {
  this->server_->init();
  this->server_->add_handler(this);
}

bool KickstartSlotControl::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() == HTTP_GET)
    return request->url() == "/hub/slot_status";
  return request->method() == HTTP_POST && request->url() == "/hub/boot_other";
}

uint8_t KickstartSlotControl::current_slot_() const { return system_upgrade_userbin_check(); }

void KickstartSlotControl::handleRequest(AsyncWebServerRequest *request) {
  if (request->url() == "/hub/slot_status")
    this->send_status_(request);
  else if (request->url() == "/hub/boot_other")
    this->boot_other_(request);
  else
    request->send(404, "application/json", "{\"error\":\"not_found\"}");
}

void KickstartSlotControl::send_status_(AsyncWebServerRequest *request) {
  const uint8_t current = this->current_slot_();
  if (current > 1) {
    request->send(409, "application/json", "{\"error\":\"unexpected_current_slot\"}");
    return;
  }
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

void KickstartSlotControl::boot_other_(AsyncWebServerRequest *request) {
  if (ESP.getFlashChipRealSize() != this->flash_size_) {
    request->send(409, "application/json", "{\"error\":\"unexpected_flash_size\"}");
    return;
  }
  if (!request->hasParam("confirm", true) || request->getParam("confirm", true)->value() != "boot-other") {
    request->send(400, "application/json", "{\"error\":\"confirmation_required\"}");
    return;
  }
  const uint8_t current = this->current_slot_();
  if (current > 1) {
    request->send(409, "application/json", "{\"error\":\"unexpected_current_slot\"}");
    return;
  }
  const uint8_t other = current == 0 ? 1 : 0;
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

}  // namespace esphome::kickstart_slot_control

#endif
