/*
 * CYD (ESP32-2432S028R) GPU health dashboard.
 *
 * Polls gpu_stats_server.py (NVML-based, Ollama-independent) and shows:
 *   - status dot (green=busy, amber=idle, red=unreachable)
 *   - top VRAM consumer (process name + GB) in the header slot
 *   - busiest-GPU % readout (top-right)
 *   - one chart line per GPU (up to MAX_GPUS)
 *   - total VRAM bar (summed across GPUs)
 *   - llama-server context bar (ctx %, green→amber→red) + tok/s
 *   - per-GPU temp + power in the footer
 *
 * Assumes you already have the standard CYD LVGL + TFT_eSPI display init
 * boilerplate in your project (lv_init, display driver registration, touch
 * driver, lv_timer_handler in loop()). This file only covers the
 * app-specific parts: Wi-Fi, polling the stats server, and updating the UI.
 *
 * Libraries needed (Library Manager): lvgl, TFT_eSPI, ArduinoJson
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#define LV_CONF_INCLUDE_SIMPLE
#include <lvgl.h>
#include <TFT_eSPI.h>

// ---- Display driver glue (LVGL <-> TFT_eSPI) ----
static const uint16_t SCREEN_W = 320;
static const uint16_t SCREEN_H = 240;
static const int BACKLIGHT_PIN = 21;

TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCREEN_W * 20];  // partial buffer, 20 rows at a time

void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t*)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

void display_init() {
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  tft.begin();
  tft.setRotation(1);  // 1 = landscape; try 3 if image is upside down

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_W * 20);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
}

// ---- Config ----
const char* WIFI_SSID = "<YOUR_WIFI_SSID>";
const char* WIFI_PASS = "<YOUR_WIFI_PASS>";
const char* STATS_URL = "http://<GPU_HOST_LAN_IP>:8090/stats"; // LAN IP of the GPU host (must be routable from the CYD's Wi-Fi network)
const uint32_t POLL_INTERVAL_MS = 3000;
const int MAX_GPUS = 2;
static const uint32_t GPU_COLORS[MAX_GPUS] = { 0xF0997B, 0x5DCAA5 };

// ---- UI objects (created once in ui_init()) ----
static lv_obj_t* status_dot;
static lv_obj_t* proc_label;
static lv_obj_t* gpu_pct_label;
static lv_obj_t* gpu_chart;
static lv_chart_series_t* chart_series[MAX_GPUS];
static lv_obj_t* vram_bar;
static lv_obj_t* vram_label;
static lv_obj_t* ctx_bar;
static lv_obj_t* ctx_label;
static lv_obj_t* footer_label;

static uint32_t last_poll = 0;
static uint32_t last_tick_ms = 0;

void ui_init() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

  // ---- Header row: status dot + top process (left), GPU% (right, large) ----
  status_dot = lv_obj_create(scr);
  lv_obj_set_size(status_dot, 14, 14);
  lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(status_dot, 0, 0);
  lv_obj_set_style_bg_color(status_dot, lv_color_hex(0x555555), 0);
  lv_obj_align(status_dot, LV_ALIGN_TOP_LEFT, 10, 12);

  proc_label = lv_label_create(scr);
  lv_obj_set_style_text_color(proc_label, lv_color_hex(0x5DCAA5), 0);
  lv_obj_set_style_text_font(proc_label, &lv_font_montserrat_20, 0);
  lv_obj_align(proc_label, LV_ALIGN_TOP_LEFT, 32, 8);
  lv_label_set_text(proc_label, "connecting...");

  gpu_pct_label = lv_label_create(scr);
  lv_obj_set_style_text_color(gpu_pct_label, lv_color_hex(0xF0997B), 0);
  lv_obj_set_style_text_font(gpu_pct_label, &lv_font_montserrat_24, 0);
  lv_obj_align(gpu_pct_label, LV_ALIGN_TOP_RIGHT, -10, 4);
  lv_label_set_text(gpu_pct_label, "--%");

  // ---- Chart: one line per GPU ----
  gpu_chart = lv_chart_create(scr);
  lv_obj_set_size(gpu_chart, 300, 74);
  lv_obj_align(gpu_chart, LV_ALIGN_TOP_MID, 0, 40);
  lv_chart_set_type(gpu_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_range(gpu_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_point_count(gpu_chart, 60);   // fills in 3 min at 3s/poll, scrolls after
  lv_obj_set_style_bg_color(gpu_chart, lv_color_black(), 0);
  lv_obj_set_style_border_width(gpu_chart, 0, 0);
  lv_obj_set_style_line_width(gpu_chart, 3, LV_PART_MAIN);   // thicker line, easier to read
  lv_obj_set_style_size(gpu_chart, 0, LV_PART_INDICATOR);      // hide point markers, cleaner line
  for (int i = 0; i < MAX_GPUS; i++) {
    chart_series[i] = lv_chart_add_series(gpu_chart, lv_color_hex(GPU_COLORS[i]),
                                           LV_CHART_AXIS_PRIMARY_Y);
  }

  // ---- VRAM row: label + full-width bar (summed across all GPUs) ----
  vram_label = lv_label_create(scr);
  lv_obj_set_style_text_color(vram_label, lv_color_hex(0x5DCAA5), 0);
  lv_obj_set_style_text_font(vram_label, &lv_font_montserrat_20, 0);
  lv_obj_align(vram_label, LV_ALIGN_TOP_LEFT, 10, 120);
  lv_label_set_text(vram_label, "vram --");

  vram_bar = lv_bar_create(scr);
  lv_obj_set_size(vram_bar, 300, 16);
  lv_obj_align(vram_bar, LV_ALIGN_TOP_MID, 0, 142);
  lv_bar_set_range(vram_bar, 0, 100);
  lv_obj_set_style_bg_color(vram_bar, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
  lv_obj_set_style_bg_color(vram_bar, lv_color_hex(0x5DCAA5), LV_PART_INDICATOR);

  // ---- Context row: llama-server context window usage ----
  ctx_label = lv_label_create(scr);
  lv_obj_set_style_text_color(ctx_label, lv_color_hex(0x5DCAA5), 0);
  lv_obj_set_style_text_font(ctx_label, &lv_font_montserrat_20, 0);
  lv_obj_align(ctx_label, LV_ALIGN_TOP_LEFT, 10, 164);
  lv_label_set_text(ctx_label, "ctx --");

  ctx_bar = lv_bar_create(scr);
  lv_obj_set_size(ctx_bar, 300, 16);
  lv_obj_align(ctx_bar, LV_ALIGN_TOP_MID, 0, 186);
  lv_bar_set_range(ctx_bar, 0, 100);
  lv_obj_set_style_bg_color(ctx_bar, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
  lv_obj_set_style_bg_color(ctx_bar, lv_color_hex(0x5DCAA5), LV_PART_INDICATOR);

  // ---- Footer: per-GPU temp + power, small text bottom-right ----
  footer_label = lv_label_create(scr);
  lv_obj_set_style_text_color(footer_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(footer_label, &lv_font_montserrat_14, 0);
  lv_obj_align(footer_label, LV_ALIGN_TOP_LEFT, 10, 210);
  lv_label_set_text(footer_label, "");
}

void poll_stats() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[poll_stats] WiFi not connected, skipping");
    return;
  }

  HTTPClient http;
  http.begin(STATS_URL);
  http.setTimeout(4000);
  int code = http.GET();

  Serial.printf("[poll_stats] URL: %s\n", STATS_URL);
  Serial.printf("[poll_stats] HTTP code: %d\n", code);

  if (code != 200) {
    Serial.printf("[poll_stats] HTTPClient error string: %s\n", http.errorToString(code).c_str());
    lv_label_set_text(proc_label, "unreachable");
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(0xE24B4A), 0); // red
    http.end();
    return;
  }

  String payload = http.getString();
  Serial.printf("[poll_stats] payload: %s\n", payload.c_str());
  http.end();

  StaticJsonDocument<3072> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    lv_label_set_text(proc_label, "bad json");
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(0xE24B4A), 0);
    return;
  }

  bool ok = doc["ok"] | false;
  const char* activity = doc["activity"] | "unreachable";

  // status dot: red = unreachable/no data, green = busy, amber = idle
  if (!ok || strcmp(activity, "unreachable") == 0) {
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(0xE24B4A), 0);
    lv_label_set_text(proc_label, ok ? "gpu read err" : "unreachable");
    return;
  }
  if (strcmp(activity, "busy") == 0) {
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(0x5DCAA5), 0); // green
  } else {
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(0xEF9F27), 0); // amber
  }

  // Header: top VRAM consumer (server pre-sorts processes[] desc), or "idle"
  JsonArray procs = doc["processes"];
  char top_buf[40];
  if (procs.size() == 0) {
    snprintf(top_buf, sizeof(top_buf), "idle");
  } else {
    JsonObject top = procs[0];
    const char* name = top["name"] | "unknown";
    float vram_gb = (top["vram_mb"] | 0.0f) / 1024.0f;
    snprintf(top_buf, sizeof(top_buf), "%s %.1fGB", name, vram_gb);
  }
  lv_label_set_text(proc_label, top_buf);

  // Busiest-GPU % readout
  float gpu_pct = doc["gpu_pct"] | 0.0f;
  char gpu_buf[8];
  snprintf(gpu_buf, sizeof(gpu_buf), "%.0f%%", gpu_pct);
  lv_label_set_text(gpu_pct_label, gpu_buf);

  // One chart line per GPU (0 for GPUs not present in the payload)
  JsonArray gpus = doc["gpus"];
  for (int i = 0; i < MAX_GPUS; i++) {
    float util = 0.0f;
    if ((size_t)i < gpus.size()) {
      util = gpus[i]["util_pct"] | 0.0f;
    }
    lv_chart_set_next_value(gpu_chart, chart_series[i], (lv_coord_t)util);
  }

  // VRAM row: summed across all GPUs
  float vram_pct = doc["vram_pct"] | 0.0f;
  float vram_gb = doc["vram_gb"] | 0.0f;
  char vram_buf[32];
  snprintf(vram_buf, sizeof(vram_buf), "vram %.1f%% (%.1fgb)", vram_pct, vram_gb);
  lv_label_set_text(vram_label, vram_buf);
  lv_bar_set_value(vram_bar, (int32_t)vram_pct, LV_ANIM_ON);

  // Context row: llama-server context window (hottest slot)
  if (!(doc["llama_ok"] | false)) {
    lv_label_set_text(ctx_label, "llama off");
    lv_bar_set_value(ctx_bar, 0, LV_ANIM_ON);
    lv_obj_set_style_bg_color(ctx_bar, lv_color_hex(0x333333), LV_PART_INDICATOR);
  } else {
    bool llama_busy = doc["llama_busy"] | false;
    float ctx_pct = doc["ctx_pct"] | 0.0f;
    float ctx_used = doc["ctx_used"] | 0.0f;
    float ctx_total = doc["ctx_total"] | 0.0f;
    float tok_s = doc["tok_s"] | 0.0f;
    char ctx_buf[40];
    if (llama_busy && tok_s > 0.0f) {
      snprintf(ctx_buf, sizeof(ctx_buf), "ctx %.0f%% %.0fk/%.0fk %.0ft/s",
               ctx_pct, ctx_used / 1000.0f, ctx_total / 1000.0f, tok_s);
    } else {
      snprintf(ctx_buf, sizeof(ctx_buf), "ctx %.0f%% %.0fk/%.0fk",
               ctx_pct, ctx_used / 1000.0f, ctx_total / 1000.0f);
    }
    lv_label_set_text(ctx_label, ctx_buf);
    lv_bar_set_value(ctx_bar, (int32_t)ctx_pct, LV_ANIM_ON);
    uint32_t ctx_col = ctx_pct > 85.0f ? 0xE24B4A : ctx_pct > 60.0f ? 0xEF9F27 : 0x5DCAA5;
    lv_obj_set_style_bg_color(ctx_bar, lv_color_hex(ctx_col), LV_PART_INDICATOR);
  }

  // Footer: per-GPU temp + power
  char footer_buf[48];
  int n = gpus.size();
  if (n > MAX_GPUS) n = MAX_GPUS;
  int pos = 0;
  for (int i = 0; i < n && pos < (int)sizeof(footer_buf) - 1; i++) {
    int temp = gpus[i]["temp_c"] | -1;
    float power = gpus[i]["power_w"] | 0.0f;
    char part[16];
    if (temp < 0) {
      snprintf(part, sizeof(part), "g%d --C %.0fW", i, power);
    } else {
      snprintf(part, sizeof(part), "g%d %dC %.0fW", i, temp, power);
    }
    pos += snprintf(footer_buf + pos, sizeof(footer_buf) - (size_t)pos,
                    i ? " | %s" : "%s", part);
  }
  lv_label_set_text(footer_label, footer_buf);
}

void setup() {
  Serial.begin(115200);

  display_init();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t wifi_start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - wifi_start > 15000) {
      Serial.println("\nWiFi failed to connect after 15s - check credentials");
      break;  // proceed anyway so the screen at least shows something
    }
  }

  ui_init();
  poll_stats();
  last_poll = millis();
  last_tick_ms = millis();
}

void loop() {
  uint32_t now = millis();
  lv_tick_inc(now - last_tick_ms);   // tell LVGL how much time has passed
  last_tick_ms = now;

  lv_timer_handler();

  if (now - last_poll > POLL_INTERVAL_MS) {
    poll_stats();
    last_poll = now;
  }

  delay(5);
}
