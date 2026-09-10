#include "esp8266_nonos_v2_slot_control.h"

#ifdef USE_ESP8266

#include <Esp.h>

#include <algorithm>
#include <cstring>

extern "C" {
#include <spi_flash.h>
#include <user_interface.h>
}

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::esp8266_nonos_v2_slot_control {

static const char *const TAG = "esp8266_nonos_v2_slot_control";
static const char *const STATUS_PATH = "/hub/slot_status";
static const char *const BOOT_OTHER_PATH = "/hub/boot_other";
static const char *const COPY_LOWER_TO_UPPER_SLOT_PATH = "/hub/copy_lower_to_upper_slot";
static constexpr uint8_t V2_MAGIC = 0xEA;
static constexpr uint8_t V2_MARKER = 0x04;
static constexpr uint8_t V1_MAGIC = 0xE9;
static constexpr uint8_t UPGRADE_FLAG_IDLE_VALUE = 0;
static constexpr uint8_t UPGRADE_FLAG_START_VALUE = 1;
static constexpr uint8_t UPGRADE_FLAG_FINISH_VALUE = 2;

static uint32_t read_u32_le(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 | static_cast<uint32_t>(data[2]) << 16 |
         static_cast<uint32_t>(data[3]) << 24;
}

static bool advance(uint32_t *cursor, uint32_t amount, uint32_t limit) {
  if (*cursor > limit || amount > limit - *cursor)
    return false;
  *cursor += amount;
  return true;
}

static bool valid_ram_segment_range(uint32_t address, uint32_t size) {
  if (size == 0 || address > UINT32_MAX - size)
    return false;
  const uint32_t end = address + size;
  return (address >= 0x3FFE8000 && end <= 0x40000000) || (address >= 0x40100000 && end <= 0x40108000);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
  for (size_t index = 0; index < length; index++) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320UL : 0);
  }
  return crc;
}

void Esp8266NonosV2SlotControl::setup() {
  // Relocate a lower-slot bridge before Wi-Fi starts, while the network stack
  // is still down.
  if (this->auto_copy_lower_to_upper_slot_ && ESP.getFlashChipRealSize() == this->migration_->get_flash_size() &&
      this->current_slot_() == 0) {
    this->relocation_status_ = RelocationStatus::RUNNING;
    ESP_LOGI(TAG, "Running from the lower slot; copying to the upper slot before WiFi setup");
    this->perform_lower_to_upper_copy_(true);
    if (this->relocation_status_ == RelocationStatus::RUNNING)
      return;
  }
  this->server_->init();
  this->server_->add_handler(this);
  // Populate the cached slot info from the main loop, not the HTTP callback.
  this->request_slot_scan_();
}

void Esp8266NonosV2SlotControl::request_slot_scan_() {
  if (this->slot_scan_pending_)
    return;
  this->slot_scan_pending_ = true;
  this->set_timeout("slot-info-scan", 0, [this]() {
    this->slot_scan_pending_ = false;
    this->refresh_slot_info(true);
  });
}

const char *Esp8266NonosV2SlotControl::relocation_status_name_(RelocationStatus status) {
  switch (status) {
    case RelocationStatus::IDLE:
      return "idle";
    case RelocationStatus::RUNNING:
      return "running";
    case RelocationStatus::LOWER_INVALID:
      return "lower_invalid";
    case RelocationStatus::COPY_FAILED:
      return "copy_failed";
    case RelocationStatus::UPPER_INVALID:
      return "upper_invalid";
  }
  return "unknown";
}

bool Esp8266NonosV2SlotControl::canHandle(AsyncWebServerRequest *request) const {
  if (request->method() == HTTP_GET)
    return request->url() == STATUS_PATH;
  return request->method() == HTTP_POST &&
         (request->url() == BOOT_OTHER_PATH || request->url() == COPY_LOWER_TO_UPPER_SLOT_PATH);
}

uint8_t Esp8266NonosV2SlotControl::current_slot_() const { return system_upgrade_userbin_check(); }

bool Esp8266NonosV2SlotControl::validate_v2_(const esp8266_nonos_v2_to_eboot_v1::Slot &slot,
                                             uint32_t *image_size) const {
  const uint32_t limit = slot.offset + slot.size;
  if (limit < slot.offset || limit > this->migration_->get_flash_size())
    return false;

  uint8_t outer[16];
  if (!ESP.flashRead(slot.offset, outer, sizeof(outer)) || outer[0] != V2_MAGIC || outer[1] != V2_MARKER)
    return false;
  const uint8_t mode = outer[2];
  const uint8_t size_frequency = outer[3];
  const uint32_t entrypoint = read_u32_le(outer + 4);
  if (entrypoint < 0x40100000 || entrypoint >= 0x40110000)
    return false;
  if (read_u32_le(outer + 8) != 0)
    return false;

  uint32_t cursor = slot.offset + sizeof(outer);
  if (!advance(&cursor, read_u32_le(outer + 12), limit))
    return false;

  uint8_t inner[8];
  if (!ESP.flashRead(cursor, inner, sizeof(inner)) || inner[0] != V1_MAGIC || inner[1] == 0 || inner[2] != mode ||
      inner[3] != size_frequency || read_u32_le(inner + 4) != entrypoint)
    return false;
  const uint8_t segment_count = inner[1];
  if (segment_count > 16)
    return false;
  if (!advance(&cursor, sizeof(inner), limit))
    return false;

  uint8_t checksum = 0xEF;
  uint8_t buffer[256];
  std::array<std::array<uint32_t, 2>, 16> ranges{};
  for (uint8_t segment = 0; segment < segment_count; segment++) {
    uint8_t header[8];
    if (!ESP.flashRead(cursor, header, sizeof(header)) || !advance(&cursor, sizeof(header), limit))
      return false;
    const uint32_t address = read_u32_le(header);
    uint32_t remaining = read_u32_le(header + 4);
    if (!valid_ram_segment_range(address, remaining))
      return false;
    const uint32_t end = address + remaining;
    for (uint8_t previous = 0; previous < segment; previous++) {
      if (address < ranges[previous][1] && ranges[previous][0] < end)
        return false;
    }
    ranges[segment] = {address, end};
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
      App.feed_wdt();
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
    App.feed_wdt();
  }
  crc ^= 0xFFFFFFFFUL;
  const uint32_t expected_crc = crc & 0x80000000UL ? crc ^ 0xFFFFFFFFUL : crc + 1;
  if (read_u32_le(trailer + 1) != expected_crc)
    return false;
  if (image_size != nullptr)
    *image_size = checksum_offset + sizeof(trailer) - slot.offset;
  return true;
}

bool Esp8266NonosV2SlotControl::copy_slot_(const esp8266_nonos_v2_to_eboot_v1::Slot &source,
                                           const esp8266_nonos_v2_to_eboot_v1::Slot &destination, uint32_t image_size) {
  static constexpr uint32_t SECTOR_SIZE = 0x1000;
  if (image_size <= SECTOR_SIZE || image_size > source.size || image_size > destination.size ||
      source.offset % SECTOR_SIZE != 0 || destination.offset % SECTOR_SIZE != 0)
    return false;

  const uint32_t sectors = (image_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
  // Invalidate any old image first. Sector zero contains the V2 header and is
  // written last, so an interrupted copy cannot leave a bootable destination.
  if (spi_flash_erase_sector(destination.offset / SECTOR_SIZE) != SPI_FLASH_RESULT_OK)
    return false;
  for (uint32_t sector = 1; sector < sectors; sector++) {
    const uint32_t source_address = source.offset + sector * SECTOR_SIZE;
    const uint32_t destination_address = destination.offset + sector * SECTOR_SIZE;
    if (!ESP.flashRead(source_address, this->copy_buffer_.data(), SECTOR_SIZE))
      return false;
    if (sector == sectors - 1 && image_size % SECTOR_SIZE != 0)
      std::fill(this->copy_buffer_.begin() + image_size % SECTOR_SIZE, this->copy_buffer_.end(), 0xFF);
    if (spi_flash_erase_sector(destination_address / SECTOR_SIZE) != SPI_FLASH_RESULT_OK ||
        spi_flash_write(destination_address, reinterpret_cast<uint32_t *>(this->copy_buffer_.data()), SECTOR_SIZE) !=
            SPI_FLASH_RESULT_OK)
      return false;
    App.feed_wdt();
  }

  if (!ESP.flashRead(source.offset, this->copy_buffer_.data(), SECTOR_SIZE) ||
      spi_flash_write(destination.offset, reinterpret_cast<uint32_t *>(this->copy_buffer_.data()), SECTOR_SIZE) !=
          SPI_FLASH_RESULT_OK)
    return false;
  return true;
}

void Esp8266NonosV2SlotControl::request_copy_lower_to_upper() {
  if (ESP.getFlashChipRealSize() != this->migration_->get_flash_size()) {
    ESP_LOGW(TAG, "Relocation refused: unexpected flash size");
    return;
  }
  if (this->current_slot_() != 0) {
    ESP_LOGW(TAG, "Relocation refused: not running from the lower slot");
    return;
  }
  if (this->relocation_status_ == RelocationStatus::RUNNING) {
    ESP_LOGW(TAG, "Relocation refused: already running");
    return;
  }
  this->relocation_status_ = RelocationStatus::RUNNING;
  this->set_timeout("copy-lower-to-upper-slot", 100, [this]() { this->perform_lower_to_upper_copy_(); });
}

void Esp8266NonosV2SlotControl::request_boot_other() {
  if (ESP.getFlashChipRealSize() != this->migration_->get_flash_size()) {
    ESP_LOGW(TAG, "Slot switch refused: unexpected flash size");
    return;
  }
  const uint8_t current = this->current_slot_();
  if (current > 1) {
    ESP_LOGW(TAG, "Slot switch refused: unexpected running slot");
    return;
  }
  const uint8_t other = current == 0 ? 1 : 0;
  if (!this->validate_v2_(this->migration_->get_slot(other))) {
    ESP_LOGW(TAG, "Slot switch refused: the other slot is not a valid V2 image");
    return;
  }
  this->set_timeout("boot-other", 500, []() {
    system_upgrade_flag_set(UPGRADE_FLAG_FINISH_VALUE);
    system_upgrade_reboot();
  });
}

void Esp8266NonosV2SlotControl::handleRequest(AsyncWebServerRequest *request) {
  if (request->url() == STATUS_PATH)
    this->send_status_(request);
  else if (request->url() == BOOT_OTHER_PATH)
    this->boot_other_(request);
  else if (request->url() == COPY_LOWER_TO_UPPER_SLOT_PATH)
    this->copy_lower_to_upper_slot_(request);
  else
    request->send(404, "application/json", "{\"error\":\"not_found\"}");
}

void Esp8266NonosV2SlotControl::copy_lower_to_upper_slot_(AsyncWebServerRequest *request) {
  if (ESP.getFlashChipRealSize() != this->migration_->get_flash_size()) {
    request->send(409, "application/json", "{\"error\":\"unexpected_flash_size\"}");
    return;
  }
  if (!request->hasParam("confirm") || request->getParam("confirm")->value() != "copy-lower-to-upper-slot") {
    request->send(400, "application/json", "{\"error\":\"confirmation_required\"}");
    return;
  }
  if (this->current_slot_() != 0) {
    request->send(409, "application/json", "{\"error\":\"lower_slot_required\"}");
    return;
  }
  if (this->relocation_status_ == RelocationStatus::RUNNING) {
    request->send(409, "application/json", "{\"error\":\"relocation_in_progress\"}");
    return;
  }

  this->relocation_status_ = RelocationStatus::RUNNING;
  request->send(202, "application/json", "{\"status\":\"relocation_started\"}");

  // ESPAsyncWebServer invokes handlers from the SDK/lwIP context. Arduino's
  // yield() cannot run there, so defer validation and copying to ESPHome's
  // normal component context.
  this->set_timeout("copy-lower-to-upper-slot", 100, [this]() { this->perform_lower_to_upper_copy_(); });
}

void Esp8266NonosV2SlotControl::perform_lower_to_upper_copy_(bool reboot_immediately) {
  // Force the cached slot info to refresh after this attempt (a failure leaves
  // the slots unchanged but the status should not stay stale).
  this->last_slot_scan_ms_ = 0;
  const auto &lower = this->migration_->get_slot(0);
  const auto &upper = this->migration_->get_slot(1);
  uint32_t image_size;
  ESP_LOGI(TAG, "Validating lower slot before relocation");
  if (!this->validate_v2_(lower, &image_size)) {
    this->relocation_status_ = RelocationStatus::LOWER_INVALID;
    ESP_LOGE(TAG, "Lower slot validation failed");
    return;
  }
  ESP_LOGI(TAG, "Lower slot is valid (%u bytes); copying to upper slot", image_size);
  system_upgrade_flag_set(UPGRADE_FLAG_START_VALUE);
  if (!this->copy_slot_(lower, upper, image_size)) {
    system_upgrade_flag_set(UPGRADE_FLAG_IDLE_VALUE);
    this->relocation_status_ = RelocationStatus::COPY_FAILED;
    ESP_LOGE(TAG, "Lower-to-upper copy failed");
    return;
  }
  ESP_LOGI(TAG, "Copy finished; validating upper slot");
  if (!this->validate_v2_(upper)) {
    system_upgrade_flag_set(UPGRADE_FLAG_IDLE_VALUE);
    this->relocation_status_ = RelocationStatus::UPPER_INVALID;
    ESP_LOGE(TAG, "Upper slot validation failed");
    return;
  }

  ESP_LOGI(TAG, "Upper slot is valid; rebooting to upper slot");
  if (reboot_immediately) {
    system_upgrade_flag_set(UPGRADE_FLAG_FINISH_VALUE);
    system_upgrade_reboot();
    return;
  }
  this->set_timeout("reboot-to-upper-slot", 500, []() {
    system_upgrade_flag_set(UPGRADE_FLAG_FINISH_VALUE);
    system_upgrade_reboot();
  });
}

static bool read_flash_span(uint32_t offset, uint8_t *data, size_t len) {
  while (len != 0) {
    const uint32_t aligned = offset & ~3U;
    uint32_t word;
    if (!ESP.flashRead(aligned, &word, sizeof(word)))
      return false;
    const size_t skip = offset - aligned;
    const size_t block = std::min(len, sizeof(word) - skip);
    memcpy(data, reinterpret_cast<const uint8_t *>(&word) + skip, block);
    offset += block;
    data += block;
    len -= block;
  }
  return true;
}

static uint32_t slot_entry_of(const esp8266_nonos_v2_to_eboot_v1::Slot &slot) {
  uint8_t header[16];
  if (!read_flash_span(slot.offset, header, sizeof(header)) || header[0] != V2_MAGIC || header[1] != V2_MARKER)
    return 0;
  return read_u32_le(header + 4);
}

static constexpr uint32_t SLOT_SCAN_INTERVAL_MS = 60000;

void Esp8266NonosV2SlotControl::refresh_slot_info(bool force) {
  const uint32_t now = millis();
  if (!force && this->last_slot_scan_ms_ != 0 && now - this->last_slot_scan_ms_ < SLOT_SCAN_INTERVAL_MS)
    return;
  this->last_slot_scan_ms_ = now;
  const uint8_t current = this->current_slot_();
  if (current > 1) {
    this->active_number_ = 0;
    this->active_valid_ = false;
    this->inactive_valid_ = false;
    this->active_entry_ = 0;
    this->inactive_entry_ = 0;
    return;
  }
  const uint8_t other = current == 0 ? 1 : 0;
  const auto &active_slot = this->migration_->get_slot(current);
  const auto &inactive_slot = this->migration_->get_slot(other);
  this->active_number_ = current + 1;
  this->active_valid_ = this->validate_v2_(active_slot);
  this->inactive_valid_ = this->validate_v2_(inactive_slot);
  this->active_entry_ = slot_entry_of(active_slot);
  this->inactive_entry_ = slot_entry_of(inactive_slot);
}

void Esp8266NonosV2SlotControl::send_status_(AsyncWebServerRequest *request) {
  const uint8_t current = this->current_slot_();
  if (current > 1) {
    request->send(409, "application/json", "{\"error\":\"unexpected_current_slot\"}");
    return;
  }
  const uint8_t other = current == 0 ? 1 : 0;
  const uint32_t actual_size = ESP.getFlashChipRealSize();
  const auto &current_slot = this->migration_->get_slot(current);
  const auto &other_slot = this->migration_->get_slot(other);
  // Never scan in the HTTP callback; report the cache and request a rescan.
  const bool scan_pending = this->last_slot_scan_ms_ == 0;
  if (scan_pending)
    this->request_slot_scan_();
  char body[1024];
  snprintf(body, sizeof(body),
           "{\"flash_size\":%u,\"flash_size_ok\":%s,\"current_slot\":%u,"
           "\"current_slot_offset\":%u,\"current_valid\":%s,\"current_entry\":\"0x%08X\","
           "\"other_slot\":%u,\"other_slot_offset\":%u,\"other_valid\":%s,\"other_entry\":\"0x%08X\","
           "\"scan_pending\":%s,\"relocation_status\":\"%s\"}",
           actual_size, actual_size == this->migration_->get_flash_size() ? "true" : "false", current + 1,
           current_slot.offset, this->active_valid_ ? "true" : "false", this->active_entry_, other + 1, other_slot.offset,
           this->inactive_valid_ ? "true" : "false", this->inactive_entry_, scan_pending ? "true" : "false",
           relocation_status_name_(this->relocation_status_));
  request->send(200, "application/json", body);
}

void Esp8266NonosV2SlotControl::boot_other_(AsyncWebServerRequest *request) {
  if (ESP.getFlashChipRealSize() != this->migration_->get_flash_size()) {
    request->send(409, "application/json", "{\"error\":\"unexpected_flash_size\"}");
    return;
  }
  if (!request->hasParam("confirm") || request->getParam("confirm")->value() != "boot-other") {
    request->send(400, "application/json", "{\"error\":\"confirmation_required\"}");
    return;
  }

  const uint8_t current = this->current_slot_();
  if (current > 1) {
    request->send(409, "application/json", "{\"error\":\"unexpected_current_slot\"}");
    return;
  }
  const uint8_t other = current == 0 ? 1 : 0;
  if (!this->validate_v2_(this->migration_->get_slot(other))) {
    request->send(409, "application/json", "{\"error\":\"other_slot_invalid\"}");
    return;
  }

  request->send(202, "application/json", "{\"status\":\"rebooting_to_other_slot\"}");
  this->set_timeout("boot-other", 500, []() {
    system_upgrade_flag_set(UPGRADE_FLAG_FINISH_VALUE);
    system_upgrade_reboot();
  });
}

}  // namespace esphome::esp8266_nonos_v2_slot_control

#endif
