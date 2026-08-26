#ifdef USE_ARDUINO

#include "hub_api.h"
#include "esphome/core/application.h"

#ifdef USE_LIBRETINY
#include <Flash.h>
#elif defined(USE_ESP8266)
#include <Esp.h>
#endif

#undef min

namespace esphome {
namespace hub_api {

static const char *const TAG = "hub_api";

static uint32_t flashLength() {
#ifdef USE_LIBRETINY
  return FLASH_LENGTH;
#elif defined(USE_ESP8266)
  return ESP.getFlashChipRealSize();
#else
  return 0;
#endif
}

static bool flashRead(uint32_t offset, uint8_t *buffer, size_t length) {
#ifdef USE_LIBRETINY
  Flash.readBlock(offset, buffer, length);
  return true;
#elif defined(USE_ESP8266)
  return ESP.flashRead(offset, buffer, length);
#else
  return false;
#endif
}

void HubAPI::handleRequest(AsyncWebServerRequest *req) {
  if (req->url().substring(4) == "/flash_read") {
    uint32_t offset, length;

    if (req->hasParam("offset")) {
      offset = req->getParam("offset")->value().toInt();
    } else {
      offset = 0;
    }

    if (req->hasParam("length")) {
      length = req->getParam("length")->value().toInt();
    } else {
      length = flashLength();
    }

    const uint32_t flash_length = flashLength();
    if (offset > flash_length || length > flash_length - offset) {
      req->send(416, "application/json", "{\"error\":\"range_out_of_bounds\"}");
      return;
    }

    auto callback = [offset, length](uint8_t *buffer, size_t maxLen, size_t position) -> size_t {
      size_t blockStart = offset + position;
      size_t blockSize = std::min(static_cast<size_t>(maxLen), static_cast<size_t>(length - position));
      blockSize = std::min(blockSize, static_cast<size_t>(4096));

      if (blockSize) {
        ESP_LOGD(TAG, "Reading flash: offset=%06x, length=%u", blockStart, blockSize);
        if (!flashRead(blockStart, buffer, blockSize)) {
          ESP_LOGE(TAG, "Flash read failed at offset=%06x", blockStart);
          return 0;
        }
      }

      return blockSize;
    };

    req->send("application/octet-stream", length, callback);
    return;
  }

EMPTY:
#ifdef LT_BANNER_STR
  req->send(200, "text/plain", LT_BANNER_STR);
#elif defined(USE_LIBRETINY)
  req->send(200, "text/plain", "LibreTuya " LT_VERSION_STR);
#elif defined(USE_ESP8266)
  req->send(200, "text/plain", "ESP8266 Kickstart");
#else
  req->send(501, "text/plain", "Unsupported platform");
#endif
}

}  // namespace hub_api
}  // namespace esphome

#endif  // USE_ARDUINO
