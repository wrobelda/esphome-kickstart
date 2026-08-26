#include "kickstart_slot_control.h"

#ifdef USE_ESP8266

#include <Esp.h>
extern "C" {
#include <user_interface.h>
}

namespace esphome::kickstart_slot_control {

static constexpr uint8_t V2_MAGIC = 0xEA;
static constexpr uint8_t V2_MARKER = 0x04;
static constexpr uint8_t V1_MAGIC = 0xE9;
static constexpr uint8_t UPGRADE_FLAG_FINISH_VALUE = 2;

static uint32_t read_u32_le(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 |
         static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
}

static bool advance(uint32_t *cursor, uint32_t amount, uint32_t limit) {
  if (*cursor > limit || amount > limit - *cursor)
    return false;
  *cursor += amount;
  return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
  for (size_t index = 0; index < length; index++) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320UL : 0);
  }
  return crc;
}

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

bool KickstartSlotControl::validate_v2_(const Slot &slot) const {
  const uint32_t limit = slot.offset + slot.size;
  if (limit < slot.offset || limit > this->flash_size_)
    return false;

  uint8_t outer[16];
  if (!ESP.flashRead(slot.offset, outer, sizeof(outer)) || outer[0] != V2_MAGIC || outer[1] != V2_MARKER)
    return false;
  const uint8_t mode = outer[2];
  const uint8_t size_frequency = outer[3];
  const uint32_t entrypoint = read_u32_le(outer + 4);

  uint32_t cursor = slot.offset + sizeof(outer);
  if (!advance(&cursor, read_u32_le(outer + 12), limit))
    return false;

  uint8_t inner[8];
  if (!ESP.flashRead(cursor, inner, sizeof(inner)) || inner[0] != V1_MAGIC || inner[1] == 0 ||
      inner[2] != mode || inner[3] != size_frequency || read_u32_le(inner + 4) != entrypoint)
    return false;
  const uint8_t segment_count = inner[1];
  if (!advance(&cursor, sizeof(inner), limit))
    return false;

  uint8_t checksum = 0xEF;
  uint8_t buffer[256];
  for (uint8_t segment = 0; segment < segment_count; segment++) {
    uint8_t header[8];
    if (!ESP.flashRead(cursor, header, sizeof(header)) || !advance(&cursor, sizeof(header), limit))
      return false;
    uint32_t remaining = read_u32_le(header + 4);
    if (remaining > limit - cursor)
      return false;
    while (remaining) {
      const size_t block = std::min(sizeof(buffer), static_cast<size_t>(remaining));
      if (!ESP.flashRead(cursor, buffer, block))
        return false;
      for (size_t index = 0; index < block; index++)
        checksum ^= buffer[index];
      cursor += block;
      remaining -= block;
      yield();
    }
  }

  const uint32_t checksum_offset = cursor | 0x0F;
  if (checksum_offset < cursor || checksum_offset > limit || limit - checksum_offset < 5)
    return false;
  uint8_t trailer[5];
  if (!ESP.flashRead(checksum_offset, trailer, sizeof(trailer)) || trailer[0] != checksum)
    return false;

  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t crc_cursor = slot.offset;
  const uint32_t crc_end = checksum_offset + 1;
  while (crc_cursor < crc_end) {
    const size_t block = std::min(sizeof(buffer), static_cast<size_t>(crc_end - crc_cursor));
    if (!ESP.flashRead(crc_cursor, buffer, block))
      return false;
    crc = crc32_update(crc, buffer, block);
    crc_cursor += block;
    yield();
  }
  crc ^= 0xFFFFFFFFUL;
  const uint32_t expected_crc = crc & 0x80000000UL ? crc ^ 0xFFFFFFFFUL : crc + 1;
  return read_u32_le(trailer + 1) == expected_crc;
}

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
  if (!this->validate_v2_(this->slots_[other])) {
    request->send(409, "application/json", "{\"error\":\"other_slot_invalid\"}");
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
