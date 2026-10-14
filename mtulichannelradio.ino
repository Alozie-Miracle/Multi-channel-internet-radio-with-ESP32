#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include "MP3DecoderHelix.h"

using namespace libhelix;

// ===== WIFI CONFIG =====
const char* ssid = "Marc4@Airtel_6407";
const char* password = "@callme20";

// ===== RELIABLE DIRECT MP3 STATIONS ARRAY (No redirects/playlists) =====
const char* radioStations[][2] = {
  { "Classic FM", "http://media-ice.musicradio.com/ClassicFMMP3" },
  { "SomaFM Groove Salad", "http://ice2.somafm.com/groovesalad-128-mp3" },
  { "SomaFM Secret Agent", "http://ice4.somafm.com/secretagent-128-mp3" },
  { "BBC World Service", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service" }
};
const int totalStations = 4;
volatile int currentStation = 0;

// ===== I2S PINS =====
#define I2S_BCLK  27
#define I2S_LRC   26
#define I2S_DOUT  25

// ===== BUTTON PINS =====
#define BTN_NEXT 32
#define BTN_PREV 33

// ===== BUFFER CONFIG =====
#define NET_BUFFER_SIZE 16384
uint8_t netBuffer[NET_BUFFER_SIZE];
volatile size_t bufHead = 0, bufTail = 0;

void pushToBuffer(uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    size_t next = (bufHead + 1) % NET_BUFFER_SIZE;
    if (next != bufTail) {
      netBuffer[bufHead] = data[i];
      bufHead = next;
    }
  }
}

size_t popFromBuffer(uint8_t *out, size_t maxLen) {
  size_t count = 0;
  while (bufTail != bufHead && count < maxLen) {
    out[count++] = netBuffer[bufTail];
    bufTail = (bufTail + 1) % NET_BUFFER_SIZE;
  }
  return count;
}

void clearBuffer() {
  bufHead = 0;
  bufTail = 0;
}

// ===== I2S INIT =====
void i2sInit() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 16,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
}

// ===== MP3 CALLBACK =====
void audioCallback(MP3FrameInfo &info, int16_t *pcm, size_t len, void*) {
  static int currentRate = 44100;
  if (info.samprate != currentRate) {
    i2s_set_sample_rates(I2S_NUM_0, info.samprate);
    currentRate = info.samprate;
  }

  // --- Volume gain ---
  float gain = 2.0f;  // 200% volume
  for (size_t i = 0; i < len; i++) {
    int32_t sample = (int32_t)(pcm[i] * gain);
    if (sample > INT16_MAX) sample = INT16_MAX;
    if (sample < INT16_MIN) sample = INT16_MIN;
    pcm[i] = (int16_t)sample;
  }

  size_t written;
  i2s_write(I2S_NUM_0, pcm, len * sizeof(int16_t), &written, portMAX_DELAY);
}

MP3DecoderHelix mp3(audioCallback);

// ===== DECODER TASK =====
void decodeTask(void *param) {
  uint8_t temp[1024];
  while (true) {
    size_t got = popFromBuffer(temp, sizeof(temp));
    if (got > 0) {
      mp3.write(temp, got);
    } else {
      vTaskDelay(1);
    }
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);

  // Configure push buttons with internal pull-ups
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  i2sInit();
  mp3.begin();

  xTaskCreatePinnedToCore(decodeTask, "decodeTask", 8192, NULL, 1, NULL, 0);
}

// ===== MAIN LOOP =====
unsigned long lastPressTime = 0;
const unsigned long debounceDelay = 300;

void loop() {
  // Check Next Station Button
  if (digitalRead(BTN_NEXT) == LOW) {
    if (millis() - lastPressTime > debounceDelay) {
      currentStation = (currentStation + 1) % totalStations;
      Serial.printf("Switching forward to [%d]: %s\n", currentStation, radioStations[currentStation][0]);
      clearBuffer();
      lastPressTime = millis();
      return; 
    }
  }

  // Check Previous Station Button
  if (digitalRead(BTN_PREV) == LOW) {
    if (millis() - lastPressTime > debounceDelay) {
      currentStation = (currentStation - 1 + totalStations) % totalStations;
      Serial.printf("Switching backward to [%d]: %s\n", currentStation, radioStations[currentStation][0]);
      clearBuffer();
      lastPressTime = millis();
      return; 
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    const char* url = radioStations[currentStation][1];
    Serial.printf("Connecting to stream: %s\n", url);
    
    http.begin(url);
    // Force HTTPClient to follow 301/302 redirects automatically
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
      WiFiClient *stream = http.getStreamPtr();
      uint8_t buffer[1024];
      
      while (stream->connected()) {
        // Poll buttons inside stream loop for instant response time
        if (digitalRead(BTN_NEXT) == LOW) {
          if (millis() - lastPressTime > debounceDelay) {
            currentStation = (currentStation + 1) % totalStations;
            Serial.printf("Switching forward to [%d]: %s\n", currentStation, radioStations[currentStation][0]);
            clearBuffer();
            lastPressTime = millis();
            http.end();
            return;
          }
        }
        if (digitalRead(BTN_PREV) == LOW) {
          if (millis() - lastPressTime > debounceDelay) {
            currentStation = (currentStation - 1 + totalStations) % totalStations;
            Serial.printf("Switching backward to [%d]: %s\n", currentStation, radioStations[currentStation][0]);
            clearBuffer();
            lastPressTime = millis();
            http.end();
            return;
          }
        }

        int len = stream->read(buffer, sizeof(buffer));
        if (len > 0) {
          pushToBuffer(buffer, len);
        } else {
          delay(1);
        }
      }
    } else {
      Serial.printf("HTTP GET failed, error code: %d\n", httpCode);
      delay(2000);
    }
    http.end();
  } else {
    delay(1000);
  }
}