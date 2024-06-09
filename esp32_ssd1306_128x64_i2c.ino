#include <ESPAsyncWebServer.h>
#include "FS.h"
#include "SD_MMC.h"
#include "lcdgfx.h"
#include "esp_camera.h"
#include "camera_index.h"
#include <TJpg_Decoder.h>
#include <TFT_eSPI.h>
TFT_eSPI tft = TFT_eSPI();

AsyncWebServer webserver(80);
AsyncWebSocket ws("/ws");

const char* ssid = "maxhome";
const char* password = "MaxMax256";

const int trigger_button_pin = 12;
const int left_button_pin = 1;
const int right_button_pin = 3;
int visible_image_id;
long trigger_button_millis = 0;
long direction_button_millis = 0;

int x = 160;
int y = 120;
byte  gray_array [161][121];
byte  resize_array [161][121];
uint8_t display_array [10][128] {};
int bitmap_row = 0;
int bitmap_column = 0;

#define SD_CS 5

DisplaySSD1306_128x64_I2C display(-1, { -1, 0x3C, 13, 16, 0}); // 16 when psram off

camera_fb_t *fb = NULL;
static esp_err_t cam_err;
static esp_err_t card_err;
int file_number = 0;

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

void setup()
{
  Serial.begin(115200);

  pinMode(4, OUTPUT);// initialize io4 as an output for LED flash.
  digitalWrite(4, LOW); // flash off

  display.begin();
  delay(2000); // just in case pinmode below stops serial comms

  init_wifi();

  pinMode(trigger_button_pin, INPUT_PULLUP);
  pinMode(left_button_pin, INPUT_PULLUP);
  pinMode(right_button_pin, INPUT_PULLUP);

  display.setFixedFont( ssd1306xled_font6x8 );
  display.clear();
  display.printFixed(0,  8, "IP ADDRESS", STYLE_NORMAL);
  display.printFixed(0,  16, WiFi.localIP().toString().c_str(), STYLE_NORMAL);
  delay(4000);
  display.clear();

  TJpgDec.setCallback(tft_output);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  //init with high specs to pre-allocate larger buffers
  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  // camera init
  cam_err = esp_camera_init(&config);
  if (cam_err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", cam_err);
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QQVGA);
  s->set_hmirror(s, 1);
  s->set_vflip(s, 1);
  init_sdcard_arduino_stylie();

  ws.onEvent(onEvent);
  webserver.addHandler(&ws);

  webserver.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    Serial.print("wtf here");
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_ov2640_html_gz, sizeof(index_ov2640_html_gz));
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  webserver.on("/image", HTTP_GET, [](AsyncWebServerRequest * request) {
    Serial.println("requesting image from SD");
    if (request->hasParam("id")) {
      AsyncWebParameter* p = request->getParam("id");
      Serial.printf("GET[%s]: %s\n", p->name().c_str(), p->value().c_str());
      String imagefile = p->value();
      imagefile = imagefile.substring(4); // remove img_
      imagefile = "full_" + imagefile + ".jpg";
      request->send(SD_MMC, "/" + imagefile, "image/jpeg");
    }
  });

  webserver.begin();

  file_number = latest_file_number(SD_MMC, "/", 0);

}
bool init_wifi()
{
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
  return true;
}

void init_sdcard_arduino_stylie()
{
  if (!SD_MMC.begin("/sd", true)) {
    Serial.println("Card Mount Failed");
    return;
  }
  uint8_t cardType = SD_MMC.cardType();

  if (cardType == CARD_NONE) {
    Serial.println("No SD_MMC card attached");
    return;
  }

  Serial.print("SD_MMC Card Type: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("SD_MMC Card Size: %lluMB\n", cardSize);

  Serial.printf("Total space: %lluMB\n", SD_MMC.totalBytes() / (1024 * 1024));
  Serial.printf("Used space: %lluMB\n", SD_MMC.usedBytes() / (1024 * 1024));
}

void onEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len)
{
  // String incoming = String((char *)data); No idea why.. gave extra characters in data for short names.
  // so ....

  // websocket events
}

int latest_file_number(fs::FS &fs, const char * dirname, uint8_t levels)
{
  String file_name, file_id;
  int until_dot, from_underscore;
  File root = fs.open(dirname);

  File file = root.openNextFile();
  while (file) {
    file_name = file.name();
    // Serial.println(file_name);
    from_underscore = file_name.lastIndexOf('_') + 1;
    until_dot = file_name.lastIndexOf('.');
    file_id = file_name.substring(from_underscore, until_dot);
    // Serial.println(file_id);
    file = root.openNextFile();
  }
  return file_id.toInt();
}

static bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap)
{
  if ( y >= 120 ) return 0;
  //  Serial.print("x"); Serial.print(" "); Serial.println(x);
  //  Serial.print("y"); Serial.print(" "); Serial.println(y);
  for (int p = 0; p < w * h; p++) { //pixels in current tile
    uint16_t R = (((bitmap[p]) >> 11) & 0x1F) << 3; // extract R component
    uint16_t G = (((bitmap[p]) >> 5) & 0x3F) << 2; // extract G component
    uint16_t B = ((bitmap[p]) & 0x1F) << 3; // extract B component
    uint16_t gray = (R * 30 + G * 59 + B * 11) / 100; // formula from the link above

    if (p % 16 == 0 && p > 0) { // 16 x 16 tile
      bitmap_row++ ;
    }

    bitmap_column = p - (bitmap_row * 16);

    gray_array[x + bitmap_column][y + bitmap_row] = gray;

    // if (y <= 96 && p == 255) { // 16x16 tile
    //  bitmap_row = 0;
    // }
    if (p == 127) {  // 16x8 tile
      bitmap_row = 0;
    }
  }
  return 1;
}

esp_err_t display_image_from_sd(int fileno)
{
  fs::FS &fs = SD_MMC;
  char *filename = (char*)malloc(11 + sizeof(fileno));
  sprintf(filename, "/thumb_%d.jpg", fileno);
  Serial.println(filename);
  File file = fs.open(filename);
  if (!file) {
    ESP_LOGE(TAG, "file open failed");
    return ESP_FAIL;
  }
  TJpgDec.drawSdJpg(0, 0, file);
  file.close();
  Serial.println("Start dithering");
  resize_dither_display(160);
  char *id4html = (char*)malloc(10 + sizeof(fileno));
  sprintf(id4html, "viewid:%d", fileno);
  ws.textAll((char*)id4html);// image id for slide
}

esp_err_t resize_dither_display(int res) {
  // resize
  int new_x = 0;
  int new_y = 0;
  //  if (res = 800) {
  //    for (int y = 0; y < 800; y++) {
  //      if (y % 7 == 0  && x > 0) {
  //        y++;
  //      }
  //
  //      for (int x = 0; x < 600; x++) {
  //        if (x % 10 == 0  && x > 0) {
  //          x++;
  //        }
  //        if (x == 0) {
  //          new_x = 0;
  //        }
  //        resize_array[new_x][new_y] = gray_array[x][y];
  //        new_x++;
  //      }
  //      new_y++;
  //    }
  //  } else {
  for (int y = 0; y < 120; y++) {
    if (y != 14 && y != 29 && y != 42 && y != 57 && y != 60 && y != 75 && y != 88 && y != 103) {
      y++;
    }

    for (int x = 0; x < 160; x++) {
      if (x % 4 == 0  && x > 0) {
        x++;
      }
      if (x == 0) {
        new_x = 0;
      }
      resize_array[new_x][new_y] = gray_array[x][y];
      new_x++;
    }
    new_y++;
  }
  //  }

  // dither
  for (int j = 0; j < new_y; j++) {
    for (int i = 0; i < new_x; i++) {

      int oldpixel = resize_array[i][j];
      int newpixel  = (oldpixel > 128) ? 255 : 0;
      resize_array[i][j] = newpixel;
      int quant_error   = oldpixel - newpixel;

      resize_array[i + 1][j    ] = resize_array[i + 1][j    ] + (quant_error * 7 / 16);
      resize_array[i - 1][j + 1] = resize_array[i - 1][j + 1] + (quant_error * 3 / 16);
      resize_array[i    ][j + 1] = resize_array[i    ][j + 1] + (quant_error * 5 / 16);
      resize_array[i + 1][j + 1] = resize_array[i + 1][j + 1] + (quant_error * 1 / 16);

    }
  }

  //display
  for (int x = 0; x < new_x; x++) {
    for (int y = 0; y < new_y; y++) {
      int black_white = 128 < resize_array[x][y] ? 1 : 0;
      display_array[y / 8][x] |= black_white ? (1 << (y & 7)) : 0;
    }
  }

  display.drawBuffer1(10, 0, 128, 64, &display_array[0][0]);

  memset(gray_array, 0, sizeof(gray_array));
  memset(resize_array, 0, sizeof(resize_array));
  memset(display_array, 0, sizeof(display_array));
  return ESP_OK;
}

esp_err_t display_stream()
{
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    ESP_LOGE(TAG, "Camera Capture Failed");
    return ESP_FAIL;
  }

  TJpgDec.drawJpg(0, 0, (const uint8_t*)fb->buf, (uint32_t)fb->len);
  resize_dither_display(160);
  esp_camera_fb_return(fb);
  fb = NULL;
  return ESP_OK;
}

esp_err_t frame_capture()
{
  fs::FS &fs = SD_MMC;
  file_number++;
  digitalWrite(4, HIGH); // flash on
  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_SVGA);
  delay(300);
  camera_fb_t * fb = esp_camera_fb_get();
  digitalWrite(4, LOW);
  char *full_filename = (char*)malloc(10 + sizeof(file_number));
  sprintf(full_filename, "/full_%d.jpg", file_number);
  Serial.println(full_filename);
  File file = fs.open(full_filename, FILE_WRITE);
  if (file != NULL)  {
    file.write(fb->buf, fb->len);
    Serial.println("saved file");
  }  else  {
    Serial.println("Could not open file");
  }
  file.close();
  free(full_filename);
  esp_camera_fb_return(fb);


  s->set_framesize(s, FRAMESIZE_QQVGA);
  delay(300);
  fb = esp_camera_fb_get();

  char *thumb_filename  = (char*)malloc(13 + sizeof(file_number));
  sprintf(thumb_filename , "/thumb_%d.jpg", file_number);
  Serial.println(thumb_filename);
  file = fs.open(thumb_filename , FILE_WRITE);
  if (file != NULL)  {
    file.write(fb->buf, fb->len);
    Serial.println("saved file");
  }  else  {
    Serial.println("Could not open file");
  }
  file.close();
  free(thumb_filename);
  esp_camera_fb_return(fb);
}

void loop()
{
  display_stream();

  // add in a delay to avoid repeat presses
  if (digitalRead(trigger_button_pin) == LOW && millis() - trigger_button_millis > 1000) {
    trigger_button_millis = millis();
    frame_capture();
    display_image_from_sd(file_number);
    delay(2000); // Hold image on OLED
  }

  if (millis() - direction_button_millis > 2000) { // after two seconds of no directions, reset visible to current
    visible_image_id = file_number;
  }

  while (digitalRead(left_button_pin) == LOW) {
    visible_image_id--;
    display_image_from_sd(visible_image_id);
    delay(1000); // allow browser to catch up
  }

  while (digitalRead(right_button_pin) == LOW) {
    visible_image_id++;
    display_image_from_sd(visible_image_id);
    delay(1000); // allow browser to catch up
  }

  //  display.clear();
  //    display_image_from_sd(file_number);
  //    delay(4000);
  //    display_image_from_sd(file_number-1);
  //    delay(4000);
  //    display_image_from_sd(file_number-2);
  //    delay(4000);
  //    display_image_from_sd(file_number-3);
  //    delay(4000);
  //    display_image_from_sd(file_number-4);
  //    delay(4000);
  //    display_image_from_sd(file_number-5);
  //    delay(4000);
  //    display_image_from_sd(file_number-4);
  //    delay(4000);
  //    display_image_from_sd(file_number-3);
  //    delay(4000);
  //    display_image_from_sd(file_number-2);
  //    delay(4000);
  //   display_image_from_sd(file_number-1);
  //    //    delay(2000);
  //    //    display_image_from_sd(file_number - 4);
  //    //    delay(2000);
  //    //    display_image_from_sd(file_number - 3);
  //    //    delay(2000);
  //    //    display_image_from_sd(file_number - 2);
  //    delay(10000);

}
