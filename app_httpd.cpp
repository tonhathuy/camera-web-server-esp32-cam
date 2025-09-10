// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "esp32-hal-ledc.h"
#include "sdkconfig.h"
#include "camera_index.h"
#include "FS.h"
#include "SD_MMC.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#endif

// Enable LED FLASH setting
#define CONFIG_LED_ILLUMINATOR_ENABLED 1

// LED FLASH setup
#if CONFIG_LED_ILLUMINATOR_ENABLED

#define LED_LEDC_GPIO            22  //configure LED pin
#define CONFIG_LED_MAX_INTENSITY 255

int led_duty = 0;
bool isStreaming = false;

#endif

typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

typedef struct {
  size_t size;   //number of values used for filtering
  size_t index;  //current value index
  size_t count;  //value count
  int sum;
  int *values;  //array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size) {
  memset(filter, 0, sizeof(ra_filter_t));

  filter->values = (int *)malloc(sample_size * sizeof(int));
  if (!filter->values) {
    return NULL;
  }
  memset(filter->values, 0, sample_size * sizeof(int));

  filter->size = sample_size;
  return filter;
}

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
static int ra_filter_run(ra_filter_t *filter, int value) {
  if (!filter->values) {
    return value;
  }
  filter->sum -= filter->values[filter->index];
  filter->values[filter->index] = value;
  filter->sum += filter->values[filter->index];
  filter->index++;
  filter->index = filter->index % filter->size;
  if (filter->count < filter->size) {
    filter->count++;
  }
  return filter->sum / filter->count;
}
#endif

#if CONFIG_LED_ILLUMINATOR_ENABLED
bool led_enabled = false; // Global LED enable/disable flag for power saving

void enable_led(bool en) {  // Turn LED On or Off
  if (!led_enabled) return; // Don't turn on LED if globally disabled
  
  int duty = en ? led_duty : 0;
  if (en && isStreaming && (led_duty > CONFIG_LED_MAX_INTENSITY)) {
    duty = CONFIG_LED_MAX_INTENSITY;
  }
  ledcWrite(LED_LEDC_GPIO, duty);
  //ledc_set_duty(CONFIG_LED_LEDC_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL, duty);
  //ledc_update_duty(CONFIG_LED_LEDC_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL);
  log_i("Set flash LED intensity to %d (LED enabled: %s)", duty, led_enabled ? "yes" : "no");
}
#endif

// Status LED control (GPIO 33)
bool status_led_enabled = false;

void control_status_led(bool en) {
  digitalWrite(33, en ? HIGH : LOW);
  status_led_enabled = en;
  log_i("Status LED (GPIO 33): %s", en ? "ON" : "OFF");
}

static esp_err_t bmp_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  uint64_t fr_start = esp_timer_get_time();
#endif
  fb = esp_camera_fb_get();
  if (!fb) {
    log_e("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/x-windows-bmp");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.bmp");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char ts[32];
  snprintf(ts, 32, "%lld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

  uint8_t *buf = NULL;
  size_t buf_len = 0;
  bool converted = frame2bmp(fb, &buf, &buf_len);
  esp_camera_fb_return(fb);
  if (!converted) {
    log_e("BMP Conversion failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  res = httpd_resp_send(req, (const char *)buf, buf_len);
  free(buf);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  uint64_t fr_end = esp_timer_get_time();
#endif
  log_i("BMP: %llums, %uB", (uint64_t)((fr_end - fr_start) / 1000), buf_len);
  return res;
}

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if (!index) {
    j->len = 0;
  }
  if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
    return 0;
  }
  j->len += len;
  return len;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  int64_t fr_start = esp_timer_get_time();
#endif

#if CONFIG_LED_ILLUMINATOR_ENABLED
  enable_led(true);
  vTaskDelay(150 / portTICK_PERIOD_MS);  // The LED needs to be turned on ~150ms before the call to esp_camera_fb_get()
  fb = esp_camera_fb_get();              // or it won't be visible in the frame. A better way to do this is needed.
  enable_led(false);
#else
  fb = esp_camera_fb_get();
#endif

  if (!fb) {
    log_e("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char ts[32];
  snprintf(ts, 32, "%lld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  size_t fb_len = 0;
#endif
  if (fb->format == PIXFORMAT_JPEG) {
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    fb_len = fb->len;
#endif
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    jpg_chunking_t jchunk = {req, 0};
    res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    fb_len = jchunk.len;
#endif
  }
  esp_camera_fb_return(fb);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  int64_t fr_end = esp_timer_get_time();
#endif
  log_i("JPG: %uB %ums", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start) / 1000));
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  struct timeval _timestamp;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char *part_buf[128];

  static int64_t last_frame = 0;
  if (!last_frame) {
    last_frame = esp_timer_get_time();
  }

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

#if CONFIG_LED_ILLUMINATOR_ENABLED
  isStreaming = true;
  enable_led(true);
#endif

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      log_e("Camera capture failed");
      res = ESP_FAIL;
    } else {
      _timestamp.tv_sec = fb->timestamp.tv_sec;
      _timestamp.tv_usec = fb->timestamp.tv_usec;
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) {
          log_e("JPEG compression failed");
          res = ESP_FAIL;
        }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) {
      log_e("Send frame failed");
      break;
    }
    int64_t fr_end = esp_timer_get_time();

    int64_t frame_time = fr_end - last_frame;
    last_frame = fr_end;

    frame_time /= 1000;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
#endif
    log_i(
      "MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)", (uint32_t)(_jpg_buf_len), (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time, avg_frame_time,
      1000.0 / avg_frame_time
    );
  }

#if CONFIG_LED_ILLUMINATOR_ENABLED
  isStreaming = false;
  enable_led(false);
#endif

  return res;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf) {
  char *buf = NULL;
  size_t buf_len = 0;

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (!buf) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      *obuf = buf;
      return ESP_OK;
    }
    free(buf);
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char *buf = NULL;
  char variable[32];
  char value[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK || httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int val = atoi(value);
  log_i("%s = %d", variable, val);
  sensor_t *s = esp_camera_sensor_get();
  int res = 0;

  if (!strcmp(variable, "framesize")) {
    if (s->pixformat == PIXFORMAT_JPEG) {
      res = s->set_framesize(s, (framesize_t)val);
    }
  } else if (!strcmp(variable, "quality")) {
    res = s->set_quality(s, val);
  } else if (!strcmp(variable, "contrast")) {
    res = s->set_contrast(s, val);
  } else if (!strcmp(variable, "brightness")) {
    res = s->set_brightness(s, val);
  } else if (!strcmp(variable, "saturation")) {
    res = s->set_saturation(s, val);
  } else if (!strcmp(variable, "gainceiling")) {
    res = s->set_gainceiling(s, (gainceiling_t)val);
  } else if (!strcmp(variable, "colorbar")) {
    res = s->set_colorbar(s, val);
  } else if (!strcmp(variable, "awb")) {
    res = s->set_whitebal(s, val);
  } else if (!strcmp(variable, "agc")) {
    res = s->set_gain_ctrl(s, val);
  } else if (!strcmp(variable, "aec")) {
    res = s->set_exposure_ctrl(s, val);
  } else if (!strcmp(variable, "hmirror")) {
    res = s->set_hmirror(s, val);
  } else if (!strcmp(variable, "vflip")) {
    res = s->set_vflip(s, val);
  } else if (!strcmp(variable, "awb_gain")) {
    res = s->set_awb_gain(s, val);
  } else if (!strcmp(variable, "agc_gain")) {
    res = s->set_agc_gain(s, val);
  } else if (!strcmp(variable, "aec_value")) {
    res = s->set_aec_value(s, val);
  } else if (!strcmp(variable, "aec2")) {
    res = s->set_aec2(s, val);
  } else if (!strcmp(variable, "dcw")) {
    res = s->set_dcw(s, val);
  } else if (!strcmp(variable, "bpc")) {
    res = s->set_bpc(s, val);
  } else if (!strcmp(variable, "wpc")) {
    res = s->set_wpc(s, val);
  } else if (!strcmp(variable, "raw_gma")) {
    res = s->set_raw_gma(s, val);
  } else if (!strcmp(variable, "lenc")) {
    res = s->set_lenc(s, val);
  } else if (!strcmp(variable, "special_effect")) {
    res = s->set_special_effect(s, val);
  } else if (!strcmp(variable, "wb_mode")) {
    res = s->set_wb_mode(s, val);
  } else if (!strcmp(variable, "ae_level")) {
    res = s->set_ae_level(s, val);
  }
#if CONFIG_LED_ILLUMINATOR_ENABLED
  else if (!strcmp(variable, "led_intensity")) {
    led_duty = val;
    if (isStreaming) {
      enable_led(true);
    }
  }
  else if (!strcmp(variable, "led_enabled")) {
    led_enabled = (val == 1);
    log_i("Flash LED globally %s", led_enabled ? "enabled" : "disabled");
    if (!led_enabled) {
      // Force turn off LED when disabled
      ledcWrite(LED_LEDC_GPIO, 0);
    }
  }
#endif
  else if (!strcmp(variable, "status_led")) {
    control_status_led(val == 1);
  }
  else {
    log_i("Unknown command: %s", variable);
    res = -1;
  }

  if (res < 0) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static int print_reg(char *p, sensor_t *s, uint16_t reg, uint32_t mask) {
  return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

static esp_err_t status_handler(httpd_req_t *req) {
  static char json_response[1024];

  sensor_t *s = esp_camera_sensor_get();
  char *p = json_response;
  *p++ = '{';

  if (s->id.PID == OV5640_PID || s->id.PID == OV3660_PID) {
    for (int reg = 0x3400; reg < 0x3406; reg += 2) {
      p += print_reg(p, s, reg, 0xFFF);  //12 bit
    }
    p += print_reg(p, s, 0x3406, 0xFF);

    p += print_reg(p, s, 0x3500, 0xFFFF0);  //16 bit
    p += print_reg(p, s, 0x3503, 0xFF);
    p += print_reg(p, s, 0x350a, 0x3FF);   //10 bit
    p += print_reg(p, s, 0x350c, 0xFFFF);  //16 bit

    for (int reg = 0x5480; reg <= 0x5490; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }

    for (int reg = 0x5380; reg <= 0x538b; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }

    for (int reg = 0x5580; reg < 0x558a; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }
    p += print_reg(p, s, 0x558a, 0x1FF);  //9 bit
  } else if (s->id.PID == OV2640_PID) {
    p += print_reg(p, s, 0xd3, 0xFF);
    p += print_reg(p, s, 0x111, 0xFF);
    p += print_reg(p, s, 0x132, 0xFF);
  }

  p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
  p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
  p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p += sprintf(p, "\"quality\":%u,", s->status.quality);
  p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
  p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
  p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
  p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
  p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
  p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
  p += sprintf(p, "\"awb\":%u,", s->status.awb);
  p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
  p += sprintf(p, "\"aec\":%u,", s->status.aec);
  p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
  p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
  p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
  p += sprintf(p, "\"agc\":%u,", s->status.agc);
  p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
  p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
  p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
  p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
  p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
  p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
  p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
  p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
  p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
#if CONFIG_LED_ILLUMINATOR_ENABLED
  p += sprintf(p, ",\"led_intensity\":%u", led_duty);
  p += sprintf(p, ",\"led_enabled\":%d", led_enabled ? 1 : 0);
#else
  p += sprintf(p, ",\"led_intensity\":%d", -1);
  p += sprintf(p, ",\"led_enabled\":0");
#endif
  p += sprintf(p, ",\"status_led\":%d", status_led_enabled ? 1 : 0);
  *p++ = '}';
  *p++ = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t xclk_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _xclk[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "xclk", _xclk, sizeof(_xclk)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int xclk = atoi(_xclk);
  log_i("Set XCLK: %d MHz", xclk);

  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_xclk(s, LEDC_TIMER_0, xclk);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t reg_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _reg[32];
  char _mask[32];
  char _val[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK || httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK
      || httpd_query_key_value(buf, "val", _val, sizeof(_val)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int reg = atoi(_reg);
  int mask = atoi(_mask);
  int val = atoi(_val);
  log_i("Set Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, val);

  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_reg(s, reg, mask, val);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t greg_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _reg[32];
  char _mask[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK || httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int reg = atoi(_reg);
  int mask = atoi(_mask);
  sensor_t *s = esp_camera_sensor_get();
  int res = s->get_reg(s, reg, mask);
  if (res < 0) {
    return httpd_resp_send_500(req);
  }
  log_i("Get Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, res);

  char buffer[20];
  const char *val = itoa(res, buffer, 10);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, val, strlen(val));
}

static int parse_get_var(char *buf, const char *key, int def) {
  char _int[16];
  if (httpd_query_key_value(buf, key, _int, sizeof(_int)) != ESP_OK) {
    return def;
  }
  return atoi(_int);
}

static esp_err_t pll_handler(httpd_req_t *req) {
  char *buf = NULL;

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  int bypass = parse_get_var(buf, "bypass", 0);
  int mul = parse_get_var(buf, "mul", 0);
  int sys = parse_get_var(buf, "sys", 0);
  int root = parse_get_var(buf, "root", 0);
  int pre = parse_get_var(buf, "pre", 0);
  int seld5 = parse_get_var(buf, "seld5", 0);
  int pclken = parse_get_var(buf, "pclken", 0);
  int pclk = parse_get_var(buf, "pclk", 0);
  free(buf);

  log_i("Set Pll: bypass: %d, mul: %d, sys: %d, root: %d, pre: %d, seld5: %d, pclken: %d, pclk: %d", bypass, mul, sys, root, pre, seld5, pclken, pclk);
  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_pll(s, bypass, mul, sys, root, pre, seld5, pclken, pclk);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t win_handler(httpd_req_t *req) {
  char *buf = NULL;

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  int startX = parse_get_var(buf, "sx", 0);
  int startY = parse_get_var(buf, "sy", 0);
  int endX = parse_get_var(buf, "ex", 0);
  int endY = parse_get_var(buf, "ey", 0);
  int offsetX = parse_get_var(buf, "offx", 0);
  int offsetY = parse_get_var(buf, "offy", 0);
  int totalX = parse_get_var(buf, "tx", 0);
  int totalY = parse_get_var(buf, "ty", 0);  // codespell:ignore totaly
  int outputX = parse_get_var(buf, "ox", 0);
  int outputY = parse_get_var(buf, "oy", 0);
  bool scale = parse_get_var(buf, "scale", 0) == 1;
  bool binning = parse_get_var(buf, "binning", 0) == 1;
  free(buf);

  log_i(
    "Set Window: Start: %d %d, End: %d %d, Offset: %d %d, Total: %d %d, Output: %d %d, Scale: %u, Binning: %u", startX, startY, endX, endY, offsetX, offsetY,
    totalX, totalY, outputX, outputY, scale, binning  // codespell:ignore totaly
  );
  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_res_raw(s, startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);  // codespell:ignore totaly
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t gallery_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  // Parse pagination parameters
  int page = 1;
  int perPage = 40; // Images per page (increased for smaller thumbnails)
  
  char *buf = NULL;
  size_t buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (buf && httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      char pageStr[10];
      if (httpd_query_key_value(buf, "page", pageStr, sizeof(pageStr)) == ESP_OK) {
        page = atoi(pageStr);
        if (page < 1) page = 1;
      }
      char perPageStr[10];
      if (httpd_query_key_value(buf, "per", perPageStr, sizeof(perPageStr)) == ESP_OK) {
        perPage = atoi(perPageStr);
        if (perPage < 5) perPage = 5;
        if (perPage > 50) perPage = 50;
      }
    }
    if (buf) free(buf);
  }
  
  // Send HTML header in chunks to reduce stack usage
  httpd_resp_send_chunk(req, 
    "<!DOCTYPE html><html><head>"
    "<title>ESP32-CAM Gallery</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body{font-family:Arial;margin:20px;background:#f0f0f0;}"
    ".container{max-width:1200px;margin:0 auto;}"
    ".header{text-align:center;margin-bottom:30px;}", -1);
    
  httpd_resp_send_chunk(req,
    ".gallery{display:grid;grid-template-columns:repeat(auto-fill,minmax(100px,1fr));gap:8px;margin-bottom:20px;}"
    ".image-card{background:white;border-radius:6px;padding:6px;box-shadow:0 2px 4px rgba(0,0,0,0.1);transition:transform 0.2s;position:relative;}"
    ".image-card:hover{transform:scale(1.05);}"
    ".image-card img{width:100%;height:70px;object-fit:cover;border-radius:3px;cursor:pointer;background:#f0f0f0;transition:all 0.3s;filter:brightness(0.9);}"
    ".image-card img:hover{filter:brightness(1);}"
    ".image-card img.loading{opacity:0.5;background:linear-gradient(90deg,#f0f0f0 25%,#e0e0e0 50%,#f0f0f0 75%);background-size:200% 100%;animation:loading 1.5s infinite;}"
    ".image-info{margin-top:4px;font-size:10px;color:#666;text-align:center;}"
    ".new-badge{position:absolute;top:2px;right:2px;background:#ff4444;color:white;padding:1px 4px;border-radius:8px;font-size:8px;font-weight:bold;}"
    "@keyframes loading{0%{background-position:200% 0;}100%{background-position:-200% 0;}}", -1);
    
  httpd_resp_send_chunk(req,
    ".pagination{display:flex;justify-content:center;align-items:center;margin:30px 0;gap:10px;flex-wrap:wrap;}"
    ".page-btn{padding:8px 12px;margin:2px;background:white;border:1px solid #ddd;border-radius:4px;text-decoration:none;color:#333;transition:all 0.3s;}"
    ".page-btn:hover{background:#4CAF50;color:white;border-color:#4CAF50;}"
    ".page-btn.active{background:#4CAF50;color:white;border-color:#4CAF50;font-weight:bold;}"
    ".page-info{color:#666;margin:0 15px;font-size:14px;}"
    ".no-images{text-align:center;color:#666;margin-top:50px;}"
    ".refresh-btn{background:#4CAF50;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;margin:10px;}"
    ".refresh-btn:hover{background:#45a049;}"
    "</style>"
    "</head><body>"
    "<div class='container'>", -1);
    
  httpd_resp_send_chunk(req,
    "<div class='header'>"
    "<h1>📷 ESP32-CAM Gallery</h1>"
    "<button class='refresh-btn' onclick='location.reload()'>🔄 Refresh</button>"
    "<button class='refresh-btn' onclick='location.href=\"/camera\"'>📹 Camera Controls</button>"
    "</div>", -1);
  
  // Collect all image files first for sorting and pagination
  String* imageFiles = nullptr;
  size_t* imageSizes = nullptr;
  int totalImages = 0;
  
  File root = SD_MMC.open("/");
  if (!root) {
    httpd_resp_send_chunk(req, "<div class='no-images'>❌ Cannot access SD card</div>", -1);
  } else {
    // First pass: count images
    File file = root.openNextFile();
    while (file) {
      String fileName = file.name();
      if (!file.isDirectory() && (fileName.endsWith(".jpg") || fileName.endsWith(".JPG"))) {
        totalImages++;
      }
      file = root.openNextFile();
    }
    root.close();
    
    if (totalImages == 0) {
      httpd_resp_send_chunk(req, "<div class='no-images'>📷 No images found on SD card</div>", -1);
    } else {
      // Allocate arrays for image data
      imageFiles = new String[totalImages];
      imageSizes = new size_t[totalImages];
      
      // Second pass: collect image data
      root = SD_MMC.open("/");
      file = root.openNextFile();
      int index = 0;
      while (file && index < totalImages) {
        String fileName = file.name();
        if (!file.isDirectory() && (fileName.endsWith(".jpg") || fileName.endsWith(".JPG"))) {
          imageFiles[index] = fileName;
          imageSizes[index] = file.size();
          index++;
        }
        file = root.openNextFile();
      }
      root.close();
      
      // Sort images by filename (newest first - assuming img_xxx.jpg format)
      for (int i = 0; i < totalImages - 1; i++) {
        for (int j = i + 1; j < totalImages; j++) {
          if (imageFiles[i] < imageFiles[j]) { // Reverse order for newest first
            String tempFile = imageFiles[i];
            size_t tempSize = imageSizes[i];
            imageFiles[i] = imageFiles[j];
            imageSizes[i] = imageSizes[j];
            imageFiles[j] = tempFile;
            imageSizes[j] = tempSize;
          }
        }
      }
      
      // Calculate pagination
      int totalPages = (totalImages + perPage - 1) / perPage;
      if (page > totalPages) page = totalPages;
      
      int startIndex = (page - 1) * perPage;
      int endIndex = startIndex + perPage;
      if (endIndex > totalImages) endIndex = totalImages;
      
      // Display page info
      char pageInfo[200];
      snprintf(pageInfo, sizeof(pageInfo),
        "<div class='page-info' style='text-align:center;margin-bottom:20px;'>"
        "📷 Showing %d-%d of %d images (Page %d of %d) - Newest first"
        "</div>",
        startIndex + 1, endIndex, totalImages, page, totalPages);
      httpd_resp_send_chunk(req, pageInfo, strlen(pageInfo));
      
      // Start gallery
      httpd_resp_send_chunk(req, "<div class='gallery'>", -1);
      
      // Display images for current page
      for (int i = startIndex; i < endIndex; i++) {
        String fileName = imageFiles[i];
        size_t fileSize = imageSizes[i];
        
        // Ensure filename starts with /
        String fullPath = fileName;
        if (!fullPath.startsWith("/")) {
          fullPath = "/" + fullPath;
        }
        
        // Check if this is a new image (last 10 images are considered "new")
        bool isNew = (i < 10);
        
        char imageCard[550];
        snprintf(imageCard, sizeof(imageCard),
          "<div class='image-card'>"
          "%s"
          "<img src='/image%s?thumb=1' alt='%s' onclick='window.open(\"/image%s\", \"_blank\")' loading='lazy'>"
          "<div class='image-info'>%s<br>%.1f KB</div>"
          "</div>",
          isNew ? "<div class='new-badge'>NEW</div>" : "",
          fullPath.c_str(), fileName.c_str(), fullPath.c_str(), 
          fileName.c_str(), fileSize / 1024.0);
        
        httpd_resp_send_chunk(req, imageCard, strlen(imageCard));
      }
      
      httpd_resp_send_chunk(req, "</div>", -1); // Close gallery
      
      // Generate pagination controls
      if (totalPages > 1) {
        httpd_resp_send_chunk(req, "<div class='pagination'>", -1);
        
        // Previous button
        if (page > 1) {
          char prevBtn[100];
          snprintf(prevBtn, sizeof(prevBtn), 
            "<a href='/gallery?page=%d&per=%d' class='page-btn'>« Previous</a>", 
            page - 1, perPage);
          httpd_resp_send_chunk(req, prevBtn, strlen(prevBtn));
        }
        
        // Page numbers
        int startPage = (page > 3) ? page - 2 : 1;
        int endPage = (page + 2 < totalPages) ? page + 2 : totalPages;
        
        if (startPage > 1) {
          char firstBtn[80];
          snprintf(firstBtn, sizeof(firstBtn), 
            "<a href='/gallery?page=1&per=%d' class='page-btn'>1</a>", perPage);
          httpd_resp_send_chunk(req, firstBtn, strlen(firstBtn));
          if (startPage > 2) {
            httpd_resp_send_chunk(req, "<span class='page-btn' style='border:none;'>...</span>", -1);
          }
        }
        
        for (int p = startPage; p <= endPage; p++) {
          char pageBtn[100];
          snprintf(pageBtn, sizeof(pageBtn), 
            "<a href='/gallery?page=%d&per=%d' class='page-btn%s'>%d</a>", 
            p, perPage, (p == page) ? " active" : "", p);
          httpd_resp_send_chunk(req, pageBtn, strlen(pageBtn));
        }
        
        if (endPage < totalPages) {
          if (endPage < totalPages - 1) {
            httpd_resp_send_chunk(req, "<span class='page-btn' style='border:none;'>...</span>", -1);
          }
          char lastBtn[80];
          snprintf(lastBtn, sizeof(lastBtn), 
            "<a href='/gallery?page=%d&per=%d' class='page-btn'>%d</a>", 
            totalPages, perPage, totalPages);
          httpd_resp_send_chunk(req, lastBtn, strlen(lastBtn));
        }
        
        // Next button
        if (page < totalPages) {
          char nextBtn[100];
          snprintf(nextBtn, sizeof(nextBtn), 
            "<a href='/gallery?page=%d&per=%d' class='page-btn'>Next »</a>", 
            page + 1, perPage);
          httpd_resp_send_chunk(req, nextBtn, strlen(nextBtn));
        }
        
        httpd_resp_send_chunk(req, "</div>", -1); // Close pagination
      }
      
      // Cleanup
      delete[] imageFiles;
      delete[] imageSizes;
    }
  }
  
  // HTML footer with JavaScript
  httpd_resp_send_chunk(req,
    "</div>"
    "<div style='text-align:center;margin-top:30px;color:#666;'>"
    "<p>📷 Gallery with pagination for fast browsing. Newest images shown first. Click any image to view full size.</p>"
    "<p style='font-size:12px;'>💡 Tip: Use ?per=10 or ?per=30 in URL to change images per page</p>"
    "</div>"
    "</div>"
    "<script>"
    "document.querySelectorAll('img').forEach(img=>{"
    "img.classList.add('loading');"
    "img.onload=()=>{img.classList.remove('loading');img.style.opacity='1';};"
    "img.onerror=()=>{img.classList.remove('loading');img.style.opacity='0.3';};"
    "});"
    "let loaded=0,total=document.querySelectorAll('img').length;"
    "document.querySelectorAll('img').forEach(img=>{"
    "img.addEventListener('load',()=>{"
    "loaded++;if(loaded===total)console.log('Page images loaded: '+loaded+'/'+total);"
    "});"
    "});"
    "</script>"
    "</body></html>", -1);
  
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t image_handler(httpd_req_t *req) {
  // Extract filename from URI (e.g., /image/img_001.jpg -> /img_001.jpg)
  const char* uri = req->uri;
  
  log_i("Requested URI: %s", uri);
  
  // Check if this is a thumbnail request
  bool isThumbnail = (strstr(uri, "?thumb=1") != NULL);
  
  // Find the filename part after /image
  const char* filename = NULL;
  if (strncmp(uri, "/image/", 7) == 0) {
    filename = uri + 7; // Skip "/image/" part
  } else if (strncmp(uri, "/image", 6) == 0) {
    filename = uri + 6; // Skip "/image" part
  } else {
    log_e("Invalid image URI: %s", uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  
  // Remove query parameters from filename
  char cleanFilename[32];
  const char* queryStart = strchr(filename, '?');
  if (queryStart) {
    size_t nameLen = queryStart - filename;
    strncpy(cleanFilename, filename, nameLen);
    cleanFilename[nameLen] = '\0';
    filename = cleanFilename;
  }
  
  log_i("Extracted filename: %s, thumbnail: %s", filename, isThumbnail ? "yes" : "no");
  
  // Build full path
  char fullPath[64];
  if (filename[0] == '/') {
    strncpy(fullPath, filename, sizeof(fullPath) - 1);
  } else {
    snprintf(fullPath, sizeof(fullPath), "/%s", filename);
  }
  fullPath[sizeof(fullPath) - 1] = '\0';
  
  log_i("Opening file: %s", fullPath);
  
  File file = SD_MMC.open(fullPath);
  if (!file) {
    log_e("File not found: %s", fullPath);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  
  size_t fileSize = file.size();
  log_i("File opened successfully: %s, size: %d", fullPath, fileSize);
  
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000");
  
  // For thumbnails, we need to send the complete JPEG file but with reduced quality
  // The approach of cutting off bytes doesn't work for JPEG format
  
  // Set content length for better performance  
  char contentLength[16];
  snprintf(contentLength, sizeof(contentLength), "%u", (unsigned int)fileSize);
  httpd_resp_set_hdr(req, "Content-Length", contentLength);
  
  // Use appropriate buffer size based on request type
  const size_t bufferSize = isThumbnail ? 1024 : 4096; // Smaller buffer for thumbs to reduce memory
  uint8_t* buffer = (uint8_t*)malloc(bufferSize);
  if (!buffer) {
    log_e("Failed to allocate buffer");
    file.close();
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  size_t bytesRead;
  size_t totalSent = 0;
  
  // Send the complete file (JPEG needs to be complete to display properly)
  while ((bytesRead = file.read(buffer, bufferSize)) > 0) {
    if (httpd_resp_send_chunk(req, (const char*)buffer, bytesRead) != ESP_OK) {
      log_e("Failed to send chunk");
      free(buffer);
      file.close();
      return ESP_FAIL;
    }
    totalSent += bytesRead;
  }
  
  free(buffer);
  file.close();
  httpd_resp_send_chunk(req, NULL, 0);
  
  log_i("Image sent successfully: %d bytes (%s)", totalSent, isThumbnail ? "thumbnail" : "full");
  return ESP_OK;
}

static esp_err_t debug_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  // Send header first
  httpd_resp_send_chunk(req, "=== SD CARD DEBUG INFO ===\n", -1);
  
  File root = SD_MMC.open("/");
  if (!root) {
    httpd_resp_send_chunk(req, "ERROR: Cannot open SD root directory\n", -1);
  } else {
    httpd_resp_send_chunk(req, "SD Card accessible\nFiles found:\n", -1);
    
    File file = root.openNextFile();
    int fileCount = 0;
    char line[128];
    
    while (file && fileCount < 50) { // Limit to prevent overflow
      String fileName = file.name();
      snprintf(line, sizeof(line), "%d. '%s' (%d bytes)\n", 
        ++fileCount, fileName.c_str(), file.size());
      httpd_resp_send_chunk(req, line, strlen(line));
      file = root.openNextFile();
    }
    root.close();
    
    if (fileCount == 0) {
      httpd_resp_send_chunk(req, "No files found!\n", -1);
    } else {
      snprintf(line, sizeof(line), "\nTotal: %d files\n", fileCount);
      httpd_resp_send_chunk(req, line, strlen(line));
    }
  }
  
  httpd_resp_send_chunk(req, NULL, 0); // End response
  return ESP_OK;
}

static esp_err_t home_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  // Send HTML in chunks to reduce stack usage
  httpd_resp_send_chunk(req, 
    "<!DOCTYPE html><html><head>"
    "<title>ESP32-CAM Control</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body{font-family:Arial;text-align:center;margin:50px;background:#f0f0f0;}"
    ".container{max-width:600px;margin:0 auto;background:white;padding:30px;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,0.1);}"
    "h1{color:#333;margin-bottom:30px;}", -1);
    
  httpd_resp_send_chunk(req,
    ".btn{display:inline-block;padding:15px 30px;margin:10px;background:#4CAF50;color:white;text-decoration:none;border-radius:5px;font-size:16px;transition:background 0.3s;}"
    ".btn:hover{background:#45a049;}"
    ".btn-secondary{background:#2196F3;}"
    ".btn-secondary:hover{background:#0b7dda;}"
    ".btn-danger{background:#f44336;}"
    ".btn-danger:hover{background:#da190b;}", -1);
    
  httpd_resp_send_chunk(req,
    ".info{background:#e7f3ff;padding:15px;border-radius:5px;margin:20px 0;}"
    "</style>"
    "</head><body>"
    "<div class='container'>"
    "<h1>📷 ESP32-CAM Control Panel</h1>"
    "<div class='info'>"
    "<p>🔴 Camera is automatically capturing images every second and saving to SD card</p>"
    "</div>", -1);
    
  httpd_resp_send_chunk(req,
    "<a href='/stream' class='btn btn-secondary' target='_blank'>📹 Live Stream</a>"
    "<a href='/capture' class='btn' target='_blank'>📸 Take Photo</a>"
    "<a href='/gallery' class='btn btn-secondary'>🖼️ View Gallery</a>"
    "<a href='/debug' class='btn btn-danger' target='_blank'>🔧 Debug SD</a>"
    "<br>"
    "<button id='flashToggle' class='btn' onclick='toggleFlashLED()' style='background:#ff6b35;'>🔦 Flash: Loading...</button>"
    "<button id='statusToggle' class='btn' onclick='toggleStatusLED()' style='background:#ff6b35;'>🔴 Status: Loading...</button>"
    "<br><br>"
    "<p style='color:#666;font-size:14px;'>Use the buttons above to access camera functions</p>"
    "<script>"
    "let flashEnabled = false, statusEnabled = false;"
    "function toggleFlashLED() {"
    "  flashEnabled = !flashEnabled;"
    "  fetch('/control?var=led_enabled&val=' + (flashEnabled ? 1 : 0))"
    "    .then(r => {"
    "      document.getElementById('flashToggle').textContent = '🔦 Flash: ' + (flashEnabled ? 'ON' : 'OFF');"
    "      document.getElementById('flashToggle').style.background = flashEnabled ? '#4CAF50' : '#666';"
    "    }).catch(e => console.error('Flash LED failed:', e));"
    "}"
    "function toggleStatusLED() {"
    "  statusEnabled = !statusEnabled;"
    "  fetch('/control?var=status_led&val=' + (statusEnabled ? 1 : 0))"
    "    .then(r => {"
    "      document.getElementById('statusToggle').textContent = '🔴 Status: ' + (statusEnabled ? 'ON' : 'OFF');"
    "      document.getElementById('statusToggle').style.background = statusEnabled ? '#4CAF50' : '#666';"
    "    }).catch(e => console.error('Status LED failed:', e));"
    "}"
    "fetch('/status').then(r=>r.json()).then(d=>{"
    "  flashEnabled = d.led_enabled === 1;"
    "  statusEnabled = d.status_led === 1;"
    "  document.getElementById('flashToggle').textContent = '🔦 Flash: ' + (flashEnabled ? 'ON' : 'OFF');"
    "  document.getElementById('flashToggle').style.background = flashEnabled ? '#4CAF50' : '#666';"
    "  document.getElementById('statusToggle').textContent = '🔴 Status: ' + (statusEnabled ? 'ON' : 'OFF');"
    "  document.getElementById('statusToggle').style.background = statusEnabled ? '#4CAF50' : '#666';"
    "}).catch(e=>console.error('Status load failed:', e));"
    "</script>"
    "</div>"
    "</body></html>", -1);
  
  httpd_resp_send_chunk(req, NULL, 0); // End response
  return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req) {
  // Redirect root to home page
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/home");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t camera_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    if (s->id.PID == OV3660_PID) {
      return httpd_resp_send(req, (const char *)index_ov3660_html_gz, index_ov3660_html_gz_len);
    } else if (s->id.PID == OV5640_PID) {
      return httpd_resp_send(req, (const char *)index_ov5640_html_gz, index_ov5640_html_gz_len);
    } else {
      return httpd_resp_send(req, (const char *)index_ov2640_html_gz, index_ov2640_html_gz_len);
    }
  } else {
    log_e("Camera sensor not found");
    return httpd_resp_send_500(req);
  }
}


// Custom error handler for 404 - handle image requests
esp_err_t custom_404_handler(httpd_req_t *req, httpd_err_code_t err) {
  const char* uri = req->uri;
  log_i("404 Handler called for: %s", uri);
  
  // Check if this is an image request
  if (strncmp(uri, "/image/", 7) == 0) {
    log_i("Handling image request in 404 handler: %s", uri);
    return image_handler(req);
  }
  
  // Default 404 response
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 20;
  config.stack_size = 8192; // Increase stack size to prevent overflow
  config.task_priority = 5;

  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = status_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t cmd_uri = {
    .uri = "/control",
    .method = HTTP_GET,
    .handler = cmd_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t stream_uri = {
    .uri = "/stream",
    .method = HTTP_GET,
    .handler = stream_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t bmp_uri = {
    .uri = "/bmp",
    .method = HTTP_GET,
    .handler = bmp_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t xclk_uri = {
    .uri = "/xclk",
    .method = HTTP_GET,
    .handler = xclk_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t reg_uri = {
    .uri = "/reg",
    .method = HTTP_GET,
    .handler = reg_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t greg_uri = {
    .uri = "/greg",
    .method = HTTP_GET,
    .handler = greg_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t pll_uri = {
    .uri = "/pll",
    .method = HTTP_GET,
    .handler = pll_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t win_uri = {
    .uri = "/resolution",
    .method = HTTP_GET,
    .handler = win_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t gallery_uri = {
    .uri = "/gallery",
    .method = HTTP_GET,
    .handler = gallery_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t image_uri = {
    .uri = "/image",
    .method = HTTP_GET,
    .handler = image_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t home_uri = {
    .uri = "/home",
    .method = HTTP_GET,
    .handler = home_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t camera_uri = {
    .uri = "/camera",
    .method = HTTP_GET,
    .handler = camera_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };

  httpd_uri_t debug_uri = {
    .uri = "/debug",
    .method = HTTP_GET,
    .handler = debug_handler,
    .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,
    .is_websocket = true,
    .handle_ws_control_frames = false,
    .supported_subprotocol = NULL
#endif
  };


  ra_filter_init(&ra_filter, 20);

  log_i("Starting web server on port: '%d'", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &home_uri);
    httpd_register_uri_handler(camera_httpd, &camera_uri);
    httpd_register_uri_handler(camera_httpd, &debug_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &bmp_uri);
    httpd_register_uri_handler(camera_httpd, &gallery_uri);
    httpd_register_uri_handler(camera_httpd, &image_uri);

    httpd_register_uri_handler(camera_httpd, &xclk_uri);
    httpd_register_uri_handler(camera_httpd, &reg_uri);
    httpd_register_uri_handler(camera_httpd, &greg_uri);
    httpd_register_uri_handler(camera_httpd, &pll_uri);
    httpd_register_uri_handler(camera_httpd, &win_uri);
    
    // Register custom 404 error handler for image requests
    httpd_register_err_handler(camera_httpd, HTTPD_404_NOT_FOUND, custom_404_handler);
  }

  config.server_port += 1;
  config.ctrl_port += 1;
  log_i("Starting stream server on port: '%d'", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setupLedFlash(int pin) {
#if CONFIG_LED_ILLUMINATOR_ENABLED
  ledcAttach(pin, 5000, 8);
#else
  log_i("LED flash is disabled -> CONFIG_LED_ILLUMINATOR_ENABLED = 0");
#endif
}
