#include "esp8266_nonos_v2_to_eboot_v1.h"

#ifdef USE_ESP8266

#include <Esp.h>
extern "C" {
#include <spi_flash.h>
#include <user_interface.h>
}

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::esp8266_nonos_v2_to_eboot_v1 {

static const char *const TAG = "kickstart.migration";
static const char *const PATH = "/hub/migrate";
static const char *const BLOCKED_OTA_PATH = "/update";
static const char *const CONFIRMATION = "replace-vendor-bootloader";
static constexpr uint8_t E9_MAGIC = 0xE9;

static uint32_t read_u32_le(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 | static_cast<uint32_t>(data[2]) << 16 |
         static_cast<uint32_t>(data[3]) << 24;
}

void Esp8266NonosV2ToEbootV1::setup() {
  this->server_->init();
  this->server_->add_handler(this);
}

bool Esp8266NonosV2ToEbootV1::canHandle(AsyncWebServerRequest *request) const {
  if (request->url() == BLOCKED_OTA_PATH)
    return request->method() == HTTP_GET || request->method() == HTTP_POST;
  return request->url() == PATH && (request->method() == HTTP_GET || request->method() == HTTP_POST);
}

void Esp8266NonosV2ToEbootV1::reset_upload_() {
  this->destination_limit_ = 0;
  this->image_size_ = 0;
  this->upload_crc_ = 0xFFFFFFFFUL;
  this->buffer_address_ = SECTOR_SIZE;
  this->buffer_length_ = 0;
  this->result_ = UploadResult::IDLE;
}

void Esp8266NonosV2ToEbootV1::fail_(UploadResult result) {
  ESP_LOGE(TAG, "Migration upload failed: %s", result_name_(result));
  this->result_ = result;
}

bool Esp8266NonosV2ToEbootV1::begin_upload_(AsyncWebServerRequest *request) {
  if (this->result_ == UploadResult::RECEIVING) {
    this->fail_(UploadResult::BUSY);
    return false;
  }
  this->reset_upload_();
  if (!request->hasParam("confirm") || request->getParam("confirm")->value() != CONFIRMATION) {
    this->fail_(UploadResult::CONFIRMATION_REQUIRED);
    return false;
  }
  if (ESP.getFlashChipRealSize() != this->flash_size_) {
    this->fail_(UploadResult::WRONG_FLASH_SIZE);
    return false;
  }
  const uint8_t current = system_upgrade_userbin_check();
  if (current > 1) {
    this->fail_(UploadResult::UNEXPECTED_SLOT);
    return false;
  }
  this->destination_limit_ = this->slots_[current].offset;
  if (this->destination_limit_ <= SECTOR_SIZE) {
    this->fail_(UploadResult::UPPER_SLOT_REQUIRED);
    return false;
  }
  this->result_ = UploadResult::RECEIVING;
  ESP_LOGI(TAG, "Receiving factory image below running slot at 0x%06x", this->destination_limit_);
  return true;
}

bool Esp8266NonosV2ToEbootV1::consume_(size_t index, const uint8_t *data, size_t len) {
  if (index != this->image_size_) {
    this->fail_(UploadResult::OUT_OF_ORDER);
    return false;
  }
  if (this->image_size_ > this->destination_limit_ || len > this->destination_limit_ - this->image_size_) {
    this->fail_(UploadResult::OUT_OF_BOUNDS);
    return false;
  }
  this->upload_crc_ = crc32_update_(this->upload_crc_, data, len);
  while (len != 0) {
    if (this->image_size_ < SECTOR_SIZE) {
      const size_t block = std::min(len, SECTOR_SIZE - this->image_size_);
      memcpy(this->bootloader_.data() + this->image_size_, data, block);
      this->image_size_ += block;
      data += block;
      len -= block;
      continue;
    }
    const size_t block = std::min(len, SECTOR_SIZE - this->buffer_length_);
    memcpy(this->buffer_.data() + this->buffer_length_, data, block);
    this->buffer_length_ += block;
    this->image_size_ += block;
    data += block;
    len -= block;
    if (this->buffer_length_ == SECTOR_SIZE && !this->flush_application_sector_(false))
      return false;
  }
  return true;
}

bool Esp8266NonosV2ToEbootV1::flush_application_sector_(bool final) {
  if (this->buffer_length_ == 0)
    return true;
  if (this->buffer_address_ >= this->destination_limit_ ||
      SECTOR_SIZE > this->destination_limit_ - this->buffer_address_) {
    this->fail_(UploadResult::OUT_OF_BOUNDS);
    return false;
  }
  if (final)
    std::fill(this->buffer_.begin() + this->buffer_length_, this->buffer_.end(), 0xFF);
  if (spi_flash_erase_sector(this->buffer_address_ / SECTOR_SIZE) != SPI_FLASH_RESULT_OK ||
      spi_flash_write(this->buffer_address_, reinterpret_cast<uint32_t *>(this->buffer_.data()), SECTOR_SIZE) !=
          SPI_FLASH_RESULT_OK) {
    this->fail_(UploadResult::FLASH_WRITE_FAILED);
    return false;
  }
  this->buffer_address_ += SECTOR_SIZE;
  this->buffer_length_ = 0;
  App.feed_wdt();
  return true;
}

bool Esp8266NonosV2ToEbootV1::read_(uint32_t offset, uint8_t *data, size_t len,
                                    bool include_uncommitted_bootloader) const {
  if (include_uncommitted_bootloader && offset < SECTOR_SIZE) {
    const size_t block = std::min(len, SECTOR_SIZE - offset);
    memcpy(data, this->bootloader_.data() + offset, block);
    if (block == len)
      return true;
    offset += block;
    data += block;
    len -= block;
  }
  return read_flash_(offset, data, len);
}

bool Esp8266NonosV2ToEbootV1::read_flash_(uint32_t offset, uint8_t *data, size_t len) {
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

bool Esp8266NonosV2ToEbootV1::valid_segment_range_(uint32_t address, uint32_t size) const {
  if (size == 0 || address > UINT32_MAX - size)
    return false;
  const uint32_t end = address + size;
  const bool dram = address >= 0x3FFE8000 && end <= 0x40000000;
  const bool iram = address >= 0x40100000 && end <= 0x40110000;
  const bool irom = this->irom_size_ != 0 && this->irom_address_ <= UINT32_MAX - this->irom_size_ &&
                    address >= this->irom_address_ && end <= this->irom_address_ + this->irom_size_;
  return dram || iram || irom;
}

bool Esp8266NonosV2ToEbootV1::validate_e9_(uint32_t offset, uint32_t limit, bool bootloader, uint32_t *image_end) {
  uint8_t header[8];
  if (!this->read_(offset, header, sizeof(header), true) || header[0] != E9_MAGIC || header[1] == 0 || header[1] > 16)
    return false;
  const uint32_t entry = read_u32_le(header + 4);
  if (entry < 0x40100000 || entry >= 0x40110000)
    return false;
  uint32_t cursor = offset + sizeof(header);
  uint8_t checksum = 0xEF;
  uint8_t block[256];
  std::array<std::array<uint32_t, 2>, 16> ranges{};
  for (uint8_t segment = 0; segment < header[1]; segment++) {
    uint8_t segment_header[8];
    if (cursor > limit || sizeof(segment_header) > limit - cursor ||
        !this->read_(cursor, segment_header, sizeof(segment_header), true))
      return false;
    cursor += sizeof(segment_header);
    const uint32_t address = read_u32_le(segment_header);
    uint32_t remaining = read_u32_le(segment_header + 4);
    if (!this->valid_segment_range_(address, remaining))
      return false;
    const uint32_t end = address + remaining;
    for (uint8_t previous = 0; previous < segment; previous++) {
      if (address < ranges[previous][1] && ranges[previous][0] < end)
        return false;
    }
    ranges[segment] = {address, end};
    if (remaining > limit - cursor)
      return false;
    while (remaining != 0) {
      const size_t length = std::min(sizeof(block), static_cast<size_t>(remaining));
      if (!this->read_(cursor, block, length, true))
        return false;
      for (size_t index = 0; index < length; index++) {
        const uint32_t address = cursor + index;
        // Arduino's elf2bin patches its whole-image size and CRC into these
        // application bytes after calculating the E9 segment checksum.
        if (address < FACTORY_CRC_SIZE_OFFSET || address >= FACTORY_CRC_VALUE_OFFSET + sizeof(uint32_t))
          checksum ^= block[index];
      }
      cursor += length;
      remaining -= length;
    }
  }
  const uint32_t checksum_offset = cursor | 0x0F;
  uint8_t stored_checksum;
  if (checksum_offset >= limit || !this->read_(checksum_offset, &stored_checksum, 1, true) ||
      stored_checksum != checksum)
    return false;
  *image_end = checksum_offset + 1;
  return !bootloader || *image_end <= SECTOR_SIZE;
}

bool Esp8266NonosV2ToEbootV1::validate_factory_image_() {
  if (this->image_size_ <= SECTOR_SIZE)
    return false;
  uint32_t bootloader_end;
  uint32_t application_end;
  return validate_e9_(0, SECTOR_SIZE, true, &bootloader_end) &&
         validate_e9_(SECTOR_SIZE, this->image_size_, false, &application_end) &&
         application_end == this->image_size_ && this->validate_factory_crc_();
}

bool Esp8266NonosV2ToEbootV1::validate_factory_crc_() {
  uint8_t word[sizeof(uint32_t)];
  if (!this->read_(FACTORY_CRC_SIZE_OFFSET, word, sizeof(word), true) || read_u32_le(word) != this->image_size_ ||
      !this->read_(FACTORY_CRC_VALUE_OFFSET, word, sizeof(word), true))
    return false;
  const uint32_t stored_crc = read_u32_le(word);
  uint32_t crc = 0xFFFFFFFFUL;
  uint8_t block[256];
  for (uint32_t cursor = 0; cursor < this->image_size_;) {
    const size_t length = std::min(sizeof(block), static_cast<size_t>(this->image_size_ - cursor));
    if (!this->read_(cursor, block, length, true))
      return false;
    for (size_t index = 0; index < length; index++) {
      const uint32_t address = cursor + index;
      if (address >= FACTORY_CRC_SIZE_OFFSET && address < FACTORY_CRC_VALUE_OFFSET + sizeof(uint32_t))
        block[index] = 0;
    }
    crc = eboot_crc32_update_(crc, block, length);
    cursor += length;
    App.feed_wdt();
  }
  return crc == stored_crc;
}

uint32_t Esp8266NonosV2ToEbootV1::crc32_update_(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t index = 0; index < len; index++) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320UL : 0);
  }
  return crc;
}

uint32_t Esp8266NonosV2ToEbootV1::eboot_crc32_update_(uint32_t crc, const uint8_t *data, size_t len) {
  for (size_t index = 0; index < len; index++) {
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
      const bool invert = ((crc & 0x80000000UL) != 0) != ((data[index] & mask) != 0);
      crc <<= 1;
      if (invert)
        crc ^= 0x04C11DB7UL;
    }
  }
  return crc;
}

bool Esp8266NonosV2ToEbootV1::verify_readback_() {
  uint32_t crc = 0xFFFFFFFFUL;
  uint8_t block[256];
  uint32_t cursor = 0;
  while (cursor < this->image_size_) {
    const size_t length = std::min(sizeof(block), static_cast<size_t>(this->image_size_ - cursor));
    if (!this->read_(cursor, block, length, true))
      return false;
    crc = crc32_update_(crc, block, length);
    cursor += length;
    App.feed_wdt();
  }
  return crc == this->upload_crc_;
}

bool Esp8266NonosV2ToEbootV1::commit_bootloader_() {
  if (spi_flash_erase_sector(0) != SPI_FLASH_RESULT_OK ||
      spi_flash_write(0, reinterpret_cast<uint32_t *>(this->bootloader_.data()), SECTOR_SIZE) != SPI_FLASH_RESULT_OK)
    return false;
  uint8_t block[256];
  for (uint32_t offset = 0; offset < SECTOR_SIZE; offset += sizeof(block)) {
    if (!read_flash_(offset, block, sizeof(block)) ||
        memcmp(block, this->bootloader_.data() + offset, sizeof(block)) != 0)
      return false;
  }
  return true;
}

void Esp8266NonosV2ToEbootV1::handleUpload(AsyncWebServerRequest *request, const String &filename, size_t index,
                                           uint8_t *data, size_t len, bool final) {
  if (request->url() == BLOCKED_OTA_PATH)
    return;
  if (index == 0 && len != 0 && !this->begin_upload_(request))
    return;
  if (this->result_ != UploadResult::RECEIVING)
    return;
  if (len != 0 && !this->consume_(index, data, len))
    return;
  if (!final)
    return;
  if (!this->flush_application_sector_(true))
    return;
  if (!this->validate_factory_image_()) {
    this->fail_(UploadResult::INVALID_FACTORY_IMAGE);
    return;
  }
  if (!this->verify_readback_()) {
    this->fail_(UploadResult::READBACK_FAILED);
    return;
  }
  this->result_ = UploadResult::READY;
  if (!this->commit_bootloader_()) {
    this->fail_(UploadResult::BOOTLOADER_WRITE_FAILED);
    return;
  }
  this->result_ = UploadResult::SUCCESS;
  ESP_LOGI(TAG, "Factory image committed; rebooting into eboot layout");
}

const char *Esp8266NonosV2ToEbootV1::result_name_(UploadResult result) {
  switch (result) {
    case UploadResult::IDLE:
      return "idle";
    case UploadResult::RECEIVING:
      return "receiving";
    case UploadResult::READY:
      return "ready";
    case UploadResult::SUCCESS:
      return "success";
    case UploadResult::CONFIRMATION_REQUIRED:
      return "confirmation_required";
    case UploadResult::BUSY:
      return "busy";
    case UploadResult::WRONG_FLASH_SIZE:
      return "wrong_flash_size";
    case UploadResult::UNEXPECTED_SLOT:
      return "unexpected_slot";
    case UploadResult::UPPER_SLOT_REQUIRED:
      return "upper_slot_required";
    case UploadResult::OUT_OF_BOUNDS:
      return "out_of_bounds";
    case UploadResult::OUT_OF_ORDER:
      return "out_of_order";
    case UploadResult::FLASH_WRITE_FAILED:
      return "flash_write_failed";
    case UploadResult::INVALID_FACTORY_IMAGE:
      return "invalid_factory_image";
    case UploadResult::READBACK_FAILED:
      return "readback_failed";
    case UploadResult::BOOTLOADER_WRITE_FAILED:
      return "bootloader_write_failed";
  }
  return "unknown";
}

void Esp8266NonosV2ToEbootV1::handleRequest(AsyncWebServerRequest *request) {
  if (request->url() == BLOCKED_OTA_PATH) {
    request->send(409, "application/json", "{\"error\":\"standard_ota_disabled_during_migration\"}");
    return;
  }
  if (request->method() == HTTP_GET) {
    static const char PAGE[] PROGMEM = "<!doctype html><meta name=viewport "
                                       "content='width=device-width'><title>ESP8266 migration</title>"
                                       "<h1>Install final ESPHome firmware</h1>"
                                       "<p>This replaces the vendor bootloader and cannot be undone without a "
                                       "flash backup.</p>"
                                       "<form method=post enctype=multipart/form-data "
                                       "action='/hub/migrate?confirm=replace-vendor-bootloader'>"
                                       "<input type=file name=firmware accept=.bin required>"
                                       "<button type=submit>Replace vendor firmware</button></form>";
    request->send_P(200, "text/html", PAGE);
    return;
  }
  char body[96];
  snprintf(body, sizeof(body), "{\"result\":\"%s\",\"image_size\":%u}", result_name_(this->result_), this->image_size_);
  const bool success = this->result_ == UploadResult::SUCCESS;
  request->send(success ? 202 : 400, "application/json", body);
  if (success)
    this->set_timeout("migration-reboot", 750, []() { App.safe_reboot(); });
}

}  // namespace esphome::esp8266_nonos_v2_to_eboot_v1

#endif
