#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>
#include <time.h>
#include <math.h>
#include <esp_heap_caps.h>

#include "driver/spi_master.h"
#include <Wire.h>
#include <TouchDrvCSTXXX.hpp>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <Preferences.h>
#include "mbedtls/base64.h"

// -----------------------------------------------------------------------------
// Fallbacks das Definições do Ficheiro .env
// -----------------------------------------------------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID "WIFI_INDEFINIDO"
#endif

#if !defined(WIFI_PASS) && defined(WIFI_PASSWORD)
#define WIFI_PASS WIFI_PASSWORD
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "SENHA_INDEFINIDA"
#endif

// -----------------------------------------------------------------------------
// Parâmetros Físicos do Ecrã AMOLED 2.16" e Pinagem QSPI
// -----------------------------------------------------------------------------
#define SCREEN_WIDTH   480
#define SCREEN_HEIGHT  480
#define CENTER_X       240
#define CENTER_Y       240

#define LCD_HOST       SPI2_HOST
#define LCD_PIN_CS     12
#define LCD_PIN_SCLK   38
#define LCD_PIN_DATA0  4
#define LCD_PIN_DATA1  5
#define LCD_PIN_DATA2  6
#define LCD_PIN_DATA3  7
#define LCD_PIN_RST    39

#define I2C_SDA        15
#define I2C_SCL        14
#define TOUCH_INT      11
#define TOUCH_RST      40
#define TOUCH_ADDR     0x5A

// Linhas por buffer: 480*30*2 = 28800 bytes, abaixo do limite de 32768 bytes
// por transação SPI do ESP32-S3.
#define DRAW_BUF_LINES 30

static spi_device_handle_t lcd_spi = NULL;
static lv_disp_drv_t *lcd_disp_drv = NULL;

// Buffers do LVGL
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_buf;

// -----------------------------------------------------------------------------
// Driver de Hardware: QSPI + painel AMOLED CO5300
// Comando: cmd 0x02, endereço = registo << 8, parâmetros em 1 linha
// Pixels : cmd 0x32, endereço = 0x2C00, dados em 4 linhas
// -----------------------------------------------------------------------------
static void co_cmd(uint8_t reg, const uint8_t *params, int n) {
    spi_transaction_t t = {};
    t.cmd = 0x02;
    t.addr = ((uint32_t)reg) << 8;
    if (n > 0) {
        t.flags = SPI_TRANS_USE_TXDATA;
        t.length = n * 8;
        for (int i = 0; i < n && i < 4; i++) t.tx_data[i] = params[i];
    }
    spi_device_polling_transmit(lcd_spi, &t);
}

static void co_cmd1(uint8_t reg, uint8_t v) { co_cmd(reg, &v, 1); }

static void lvgl_rounder_cb(lv_disp_drv_t *disp, lv_area_t *area) {
    // O CO5300 exige janelas com início par e fim ímpar
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void IRAM_ATTR lcd_pixels_done(spi_transaction_t *t) {
    // O post_cb também dispara nas transações de comando; só os pixels (0x32) terminam o flush
    if (t->cmd == 0x32) {
        lv_disp_flush_ready(lcd_disp_drv);
    }
}

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint8_t c[4] = { (uint8_t)(area->x1 >> 8), (uint8_t)area->x1, (uint8_t)(area->x2 >> 8), (uint8_t)area->x2 };
    uint8_t r[4] = { (uint8_t)(area->y1 >> 8), (uint8_t)area->y1, (uint8_t)(area->y2 >> 8), (uint8_t)area->y2 };
    co_cmd(0x2A, c, 4);
    co_cmd(0x2B, r, 4);

    static spi_transaction_t t;
    t = {};
    t.cmd = 0x32;
    t.addr = 0x2C00;
    t.flags = SPI_TRANS_MODE_QIO;
    t.tx_buffer = color_p;
    t.length = (size_t)lv_area_get_width(area) * lv_area_get_height(area) * 2 * 8;
    if (spi_device_queue_trans(lcd_spi, &t, portMAX_DELAY) != ESP_OK) {
        lv_disp_flush_ready(disp);
    }
}

void init_amoled_hardware(lv_disp_drv_t *disp_drv) {
    lcd_disp_drv = disp_drv;

    pinMode(LCD_PIN_RST, OUTPUT);
    digitalWrite(LCD_PIN_RST, HIGH); delay(10);
    digitalWrite(LCD_PIN_RST, LOW);  delay(200);
    digitalWrite(LCD_PIN_RST, HIGH); delay(200);

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = LCD_PIN_DATA0;
    buscfg.miso_io_num = LCD_PIN_DATA1;
    buscfg.sclk_io_num = LCD_PIN_SCLK;
    buscfg.quadwp_io_num = LCD_PIN_DATA2;
    buscfg.quadhd_io_num = LCD_PIN_DATA3;
    buscfg.max_transfer_sz = SCREEN_WIDTH * DRAW_BUF_LINES * 2 + 64;
    buscfg.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits = 8;
    devcfg.address_bits = 24;
    devcfg.mode = 0;
    devcfg.clock_speed_hz = 40 * 1000 * 1000;
    devcfg.spics_io_num = LCD_PIN_CS;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;
    devcfg.queue_size = 2;
    devcfg.post_cb = lcd_pixels_done;
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &lcd_spi));

    co_cmd(0x11, NULL, 0);          // Sleep out
    delay(120);
    co_cmd1(0xFE, 0x00);            // Comandos de utilizador
    co_cmd1(0xC4, 0x80);            // Modo QSPI
    co_cmd1(0x3A, 0x55);            // RGB565
    co_cmd1(0x53, 0x20);            // Controlo de brilho
    co_cmd1(0x63, 0xFF);            // Brilho HBM
    co_cmd1(0x36, 0x00);            // Orientação
    co_cmd(0x29, NULL, 0);          // Display ON
    co_cmd1(0x51, 0xD0);            // Brilho
    co_cmd1(0x58, 0x00);
    co_cmd(0x20, NULL, 0);          // Inversão desligada
    delay(10);
}

// -----------------------------------------------------------------------------
// Touch CST9220 (I2C) como dispositivo de entrada do LVGL
// -----------------------------------------------------------------------------
static TouchDrvCST92xx touch;
static bool touch_ok = false;

static void my_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static int16_t last_x = 0, last_y = 0;
    int16_t x[1], y[1];
    if (touch_ok && touch.getPoint(x, y, 1) > 0) {
        last_x = x[0];
        last_y = y[0];
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

void init_touch() {
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    touch.setPins(TOUCH_RST, TOUCH_INT);
    touch_ok = touch.begin(Wire, TOUCH_ADDR, I2C_SDA, I2C_SCL);
    Serial.printf("[TOUCH] %s\n", touch_ok ? "CST9220 OK" : "FALHA ao iniciar");
    touch.setMaxCoordinates(SCREEN_WIDTH, SCREEN_HEIGHT);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touch_read;
    lv_indev_drv_register(&indev_drv);
}

static void no_click(lv_obj_t *o) {
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// -----------------------------------------------------------------------------
// Assistente de IA por voz (microfone ES7210 -> Gemini)
// -----------------------------------------------------------------------------
#ifndef GEMINI_API_KEY
#define GEMINI_API_KEY ""
#endif
#ifndef GEMINI_MODEL
#define GEMINI_MODEL "gemini-3.5-flash-lite"
#endif

#define MIC_RATE 16000
#define MIC_MAX_SEC 10
#define MIC_MAX_SAMPLES (MIC_RATE * MIC_MAX_SEC)
#define PIN_I2S_MCLK 42
#define PIN_I2S_BCLK 9
#define PIN_I2S_WS 45
#define PIN_I2S_DIN 10
#define ES7210_ADDR 0x40
#define ES8311_ADDR 0x18
#define PIN_I2S_DOUT 8
#define PIN_PA_EN 46

// Botões físicos superiores (+ e -) conforme esquemático oficial Waveshare ESP32-S3-Touch-AMOLED-2.16:
// - Key3 (+ / KEY, serigrafia IO18): conectado ao GPIO 18 (pull-up R18=10K para VCC3V3, ativo em LOW, C22=NC)
// - Key2 (- / BOOT, serigrafia BOOT): conectado ao GPIO 0  (pull-up R9=10K para VCC3V3, ativo em LOW, C12=100nF)
// - Key1 (PWR central): conectado ao AXP2101 PWRON
#define PIN_BTN_VOL_UP   18
#define PIN_BTN_VOL_DOWN 0

static I2SClass i2s_mic;
static bool mic_ok = false;
static SemaphoreHandle_t wifi_mtx = NULL;
static Preferences prefs;
static volatile int ai_volume = 80; // Volume de 0 a 100% (padrão 80%)

enum { AI_IDLE = 0, AI_LISTENING, AI_THINKING, AI_SPEAKING, AI_DONE };
static volatile int ai_state = AI_IDLE;
static volatile bool ai_rec = false;
static volatile uint32_t ai_rec_ms = 0;
static char ai_text[1024] = "";
static volatile bool ai_dirty = true;
static portMUX_TYPE ai_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t ai_task_handle = NULL;

static void ai_set(int state, const char *text) {
    portENTER_CRITICAL(&ai_mux);
    if (text) {
        strlcpy(ai_text, text, sizeof(ai_text));
        ai_dirty = true;
    }
    ai_state = state;
    portEXIT_CRITICAL(&ai_mux);
}

// Certificados raiz da Google Trust Services para validação SSL/TLS estrita (GTS Root R1 e GTS Root R4)
static const char GOOGLE_ROOT_CAS[] PROGMEM =
"-----BEGIN CERTIFICATE-----\n"
"MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
"MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
"MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
"Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
"A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
"27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
"Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
"TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
"qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
"szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
"Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
"MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
"wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
"aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
"VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
"AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
"FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
"C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
"QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
"h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
"7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
"ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
"MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
"Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
"6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
"0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
"2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
"bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
"-----END CERTIFICATE-----\n"
"-----BEGIN CERTIFICATE-----\n"
"MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
"VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
"A1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAATzdHOnaItg\n"
"rkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyiQHY7qca4R9gq\n"
"55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvRHYqjQjBAMA4G\n"
"A1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBSATNbrdP9J\n"
"NqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D9J+uHXqnLrmv\n"
"T/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8p/SgguMh1YQd\n"
"c4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
"-----END CERTIFICATE-----\n";

// Validação TLS estrita usando o bundle Mozilla de CAs embutido no firmware (inclui GTS Root R1/R4 e GlobalSign)
static void google_tls(WiFiClientSecure &c) {
    c.useBuiltinCACertBundle();
}

// -----------------------------------------------------------------------------
// Google Agenda OAuth2 Device Flow & Calendar API v3 (100% Standalone)
// -----------------------------------------------------------------------------
#ifndef GOOGLE_CLIENT_ID
#define GOOGLE_CLIENT_ID ""
#endif
#ifndef GOOGLE_CLIENT_SECRET
#define GOOGLE_CLIENT_SECRET ""
#endif
#ifndef GOOGLE_REFRESH_TOKEN
#define GOOGLE_REFRESH_TOKEN ""
#endif

static String gcal_refresh_token = "";
static String gcal_access_token = "";
static uint32_t gcal_token_expires_at = 0;

static String get_now_formatted() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[64];
    if (t.tm_year < 120) { // Antes de 2020: relógio ainda não sincronizou
        return "Relogio ainda nao sincronizado via NTP.";
    }
    const char *dias[] = {"Domingo", "Segunda-feira", "Terca-feira", "Quarta-feira", "Quinta-feira", "Sexta-feira", "Sabado"};
    snprintf(buf, sizeof(buf), "%s, %02d/%02d/%04d %02d:%02d:%02d (Horario de Brasilia UTC-3)",
             dias[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
    return String(buf);
}

static void gcal_load_token() {
    // O refresh_token vem do .env (gerado por scripts/google_auth.py); a Flash e usada como alternativa
    gcal_refresh_token = String(GOOGLE_REFRESH_TOKEN);
    if (gcal_refresh_token.length() == 0) gcal_refresh_token = prefs.getString("gcal_refresh", "");
    if (gcal_refresh_token.length() > 0) {
        Serial.printf("[GCAL] Refresh Token carregado da Flash NVS (%u bytes)\n", gcal_refresh_token.length());
    } else {
        Serial.println("[GCAL] Nenhum Refresh Token salvo na Flash. Agenda nao vinculada.");
    }
}

static bool gcal_refresh_access_token() {
    if (gcal_refresh_token.length() == 0 || strlen(GOOGLE_CLIENT_ID) == 0 || strlen(GOOGLE_CLIENT_SECRET) == 0) {
        return false;
    }
    if (gcal_access_token.length() > 0 && millis() < gcal_token_expires_at) {
        return true; // Token atual ainda é válido
    }

    WiFiClientSecure client;
    google_tls(client);
    HTTPClient http;
    http.setTimeout(15000);
    http.begin(client, "https://oauth2.googleapis.com/token");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body = "client_id=" + String(GOOGLE_CLIENT_ID) +
                  "&client_secret=" + String(GOOGLE_CLIENT_SECRET) +
                  "&refresh_token=" + gcal_refresh_token +
                  "&grant_type=refresh_token";

    int code = http.POST(body);
    if (code == 200) {
        String resp = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, resp)) {
            gcal_access_token = doc["access_token"].as<String>();
            int exp = doc["expires_in"] | 3600;
            gcal_token_expires_at = millis() + (exp - 120) * 1000UL;
            Serial.println("[GCAL] Access token renovado com sucesso!");
            http.end();
            return true;
        }
    }
    Serial.printf("[GCAL] Erro ao renovar access token: HTTP %d\n", code);
    http.end();
    return false;
}

// O fluxo de dispositivo do Google nao permite o escopo da Agenda: o pareamento e feito no PC
// com scripts/google_auth.py, que grava GOOGLE_REFRESH_TOKEN no .env.
static bool gcal_start_device_auth() {
    if (gcal_refresh_token.length() > 0) {
        ai_set(AI_DONE, "A agenda ja esta vinculada.");
    } else {
        ai_set(AI_DONE, "Para vincular: no PC rode scripts/google_auth.py e regrave o firmware.");
    }
    return false;
}

// Consulta compromissos na Google Calendar API v3
static String gcal_list_events(const char *time_min, const char *time_max) {
    if (!gcal_refresh_access_token()) {
        return "Agenda nao conectada. Diga 'conectar agenda' para vincular sua conta.";
    }

    WiFiClientSecure client;
    google_tls(client);
    HTTPClient http;
    http.setTimeout(20000);

    String url = "https://www.googleapis.com/calendar/v3/calendars/primary/events?singleEvents=true&orderBy=startTime&maxResults=10";
    if (time_min && strlen(time_min) > 0) url += "&timeMin=" + String(time_min);
    if (time_max && strlen(time_max) > 0) url += "&timeMax=" + String(time_max);

    http.begin(client, url);
    http.addHeader("Authorization", "Bearer " + gcal_access_token);

    int code = http.GET();
    if (code != 200) {
        http.end();
        return "Erro ao consultar a Google Agenda (codigo " + String(code) + ").";
    }

    String resp = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, resp)) return "Erro ao processar dados da agenda.";

    JsonArray items = doc["items"].as<JsonArray>();
    if (items.size() == 0) return "Nenhum compromisso encontrado nesse periodo.";

    String result = "Compromissos encontrados:\\n";
    for (JsonObject it : items) {
        const char *summary = it["summary"] | "Sem titulo";
        const char *start_dt = it["start"]["dateTime"] | (it["start"]["date"] | "");
        result += "- ";
        result += summary;
        if (strlen(start_dt) >= 16) {
            // Extrai hora: HH:MM
            result += " as " + String(start_dt).substring(11, 16);
        }
        result += "\\n";
    }
    return result;
}

// Cria um novo compromisso na Google Calendar API v3
static String gcal_create_event(const char *summary, const char *start_iso, const char *end_iso) {
    if (!gcal_refresh_access_token()) {
        return "Agenda nao conectada. Diga 'conectar agenda' para vincular sua conta.";
    }

    WiFiClientSecure client;
    google_tls(client);
    HTTPClient http;
    http.setTimeout(20000);
    http.begin(client, "https://www.googleapis.com/calendar/v3/calendars/primary/events");
    http.addHeader("Authorization", "Bearer " + gcal_access_token);
    http.addHeader("Content-Type", "application/json");

    JsonDocument req;
    req["summary"] = summary;
    req["start"]["dateTime"] = start_iso;
    req["end"]["dateTime"] = end_iso;
    String body;
    serializeJson(req, body);

    int code = http.POST(body);
    http.end();

    if (code == 200 || code == 201) {
        return "Compromisso '" + String(summary) + "' agendado com sucesso na sua Google Agenda!";
    }
    return "Erro ao criar compromisso (codigo " + String(code) + ").";
}
#define MAX_MEM_FACTS 10
#define MAX_MEM_LEN   120
static String ai_memories[MAX_MEM_FACTS];
static int ai_memories_count = 0;

static void mem_load() {
    ai_memories_count = 0;
    int total = prefs.getInt("mem_count", 0);
    total = constrain(total, 0, MAX_MEM_FACTS);
    for (int i = 0; i < total; i++) {
        char key[16];
        snprintf(key, sizeof(key), "mem_%d", i);
        String fact = prefs.getString(key, "");
        if (fact.length() > 0) {
            ai_memories[ai_memories_count++] = fact;
        }
    }
    Serial.printf("[MEM] %d fatos carregados da memoria Flash (NVS)\n", ai_memories_count);
    for (int i = 0; i < ai_memories_count; i++) {
        Serial.printf("  - %s\n", ai_memories[i].c_str());
    }
}

static void mem_save() {
    prefs.putInt("mem_count", ai_memories_count);
    for (int i = 0; i < ai_memories_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "mem_%d", i);
        prefs.putString(key, ai_memories[i]);
    }
}

static void mem_learn(const char *fact) {
    if (!fact || !strlen(fact)) return;
    String new_fact = String(fact);
    new_fact.trim();
    if (new_fact.length() < 3) return;

    // Se já existir fato igual ou muito parecido, ignora para não duplicar
    for (int i = 0; i < ai_memories_count; i++) {
        if (ai_memories[i].equalsIgnoreCase(new_fact)) return;
    }

    if (ai_memories_count < MAX_MEM_FACTS) {
        ai_memories[ai_memories_count++] = new_fact;
    } else {
        // Rotaciona: descarta a memória mais antiga e adiciona a nova no final
        for (int i = 0; i < MAX_MEM_FACTS - 1; i++) {
            ai_memories[i] = ai_memories[i + 1];
        }
        ai_memories[MAX_MEM_FACTS - 1] = new_fact;
    }
    mem_save();
    Serial.printf("[MEM] Nova memoria aprendida e salva na Flash: \"%s\"\n", new_fact.c_str());
}

static String mem_build_prompt() {
    if (ai_memories_count == 0) {
        return "Nenhuma memoria anterior ainda.";
    }
    String s = "";
    for (int i = 0; i < ai_memories_count; i++) {
        s += "- ";
        s += ai_memories[i];
        if (i < ai_memories_count - 1) s += "\\n";
    }
    return s;
}

static bool es7210_w(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES7210_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

// Sequência de inicialização baseada no driver ES7210 da Espressif (Apache-2.0):
// 16 kHz, MCLK 4,096 MHz (256 x fs), I2S 16 bits, microfones 1/2 ligados.
static bool es7210_init_16k() {
    bool ok = es7210_w(0x00, 0xFF);
    es7210_w(0x00, 0x32);
    es7210_w(0x09, 0x30);
    es7210_w(0x0A, 0x30);
    es7210_w(0x23, 0x2A);
    es7210_w(0x22, 0x0A);
    es7210_w(0x21, 0x2A);
    es7210_w(0x20, 0x0A);
    es7210_w(0x11, 0x00 | 0x60);
    es7210_w(0x12, 0x00);
    es7210_w(0x40, 0xC3);
    es7210_w(0x41, 0x70);
    es7210_w(0x42, 0x70);
    for (uint8_t r = 0x43; r <= 0x46; r++) es7210_w(r, 0x10 | 12);  // ~34,5 dB
    for (uint8_t r = 0x47; r <= 0x4A; r++) es7210_w(r, 0x08);
    es7210_w(0x02, 0x01 | (0x01 << 6) | (0x01 << 7));  // adc_div 1, doubler, dll
    es7210_w(0x07, 0x20);
    es7210_w(0x03, 0x00);
    es7210_w(0x04, 0x01);
    es7210_w(0x05, 0x00);
    es7210_w(0x06, 0x04);
    es7210_w(0x4B, 0x0F);
    es7210_w(0x4C, 0x0F);
    es7210_w(0x00, 0x71);
    es7210_w(0x00, 0x41);
    return ok;
}

static void es8311_w(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

// Baseado no driver ES8311 da Espressif (Apache-2.0): 16 kHz, MCLK 4,096 MHz, I2S 16 bits, escravo
static void es8311_init_16k() {
    es8311_w(0x00, 0x1F);
    delay(20);
    es8311_w(0x00, 0x00);
    es8311_w(0x00, 0x80);
    es8311_w(0x01, 0x3F);
    es8311_w(0x02, 0x00);
    es8311_w(0x03, 0x10);
    es8311_w(0x04, 0x10);
    es8311_w(0x05, 0x00);
    es8311_w(0x06, 0x03);
    es8311_w(0x07, 0x00);
    es8311_w(0x08, 0xFF);
    es8311_w(0x09, 0x0C);
    es8311_w(0x0A, 0x0C);
    es8311_w(0x0D, 0x01);
    es8311_w(0x0E, 0x02);
    es8311_w(0x12, 0x00);
    es8311_w(0x13, 0x10);
    es8311_w(0x1C, 0x6A);
    es8311_w(0x37, 0x08);
    es8311_w(0x31, 0x00);
    es8311_w(0x32, 0xD0);  // Ganho de referência limpo do DAC (+8.5 dB); volume de 0-100% escalado em tempo real via software
}

static void init_audio() {
    i2s_mic.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, LOW);
    if (!i2s_mic.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
        Serial.println("[MIC] Falha ao iniciar I2S");
        return;
    }
    delay(20);
    if (!es7210_init_16k()) {
        Serial.println("[MIC] ES7210 nao respondeu");
        return;
    }
    es8311_init_16k();
    i2s_mic.setTimeout(200);
    mic_ok = true;
    Serial.println("[MIC] ES7210 OK");
}

// A fonte padrão só tem ASCII: converte acentos do UTF-8 e descarta o resto
static void to_ascii(const char *src, char *dst, size_t dst_len) {
    static const char *map_c3 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)src; *p && o + 1 < dst_len; p++) {
        if (*p < 0x80) {
            dst[o++] = (*p == '\n' || *p >= 32) ? *p : ' ';
        } else if (*p == 0xC3 && p[1] >= 0x80 && p[1] < 0xC0) {
            dst[o++] = map_c3[p[1] - 0x80];
            p++;
        } else if (*p == 0xE2 && p[1] == 0x80) {
            uint8_t c = p[2];
            dst[o++] = (c == 0x93 || c == 0x94) ? '-' : (c == 0x98 || c == 0x99) ? '\'' : (c == 0x9C || c == 0x9D) ? '"' : (c == 0xA2) ? '*' : ' ';
            p += 2;
        } else {
            int skip = (*p >= 0xF0) ? 3 : (*p >= 0xE0) ? 2 : (*p >= 0xC0) ? 1 : 0;
            for (int i = 0; i < skip && p[1]; i++) p++;
        }
    }
    dst[o] = 0;
}

// Sincroniza o relógio via NTP (essencial para validar certificados TLS). Retorna true se o relógio é válido.
static bool ntp_sync(uint32_t timeout_ms) {
    static bool sntp_inited = false;
    if (!sntp_inited) {
        configTzTime("<-03>3", "pool.ntp.org", "time.google.com", "a.st1.ntp.br");
        sntp_inited = true;
    }
    uint32_t start = millis();
    time_t now = time(nullptr);
    while (now < 1700000000 && millis() - start < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(250));
        now = time(nullptr);
    }
    bool ok = now >= 1700000000;
    Serial.printf("[NTP] %s | epoch=%ld | %u ms\n", ok ? "Relogio sincronizado" : "FALHA ao sincronizar", (long)now, (unsigned)(millis() - start));
    return ok;
}

static bool ai_wifi_up() {
    if (WiFi.status() == WL_CONNECTED) {
        if (time(nullptr) < 1700000000) ntp_sync(10000);
        return true;
    }
    WiFi.persistent(false);       // NUNCA grava a senha do Wi-Fi na memória Flash NVS
    WiFi.setAutoReconnect(false); // Evita tentativas de reconexão automáticas em background
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(400));
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Falha ao conectar");
        return false;
    }
    Serial.printf("[WIFI] Conectado, IP %s\n", WiFi.localIP().toString().c_str());
    // Sempre sincroniza o relógio logo após conectar (necessário para o handshake TLS)
    if (time(nullptr) < 1700000000) ntp_sync(15000);
    return true;
}

static void ai_wifi_down() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// Na inicialização: conecta ao Wi-Fi, atualiza o relógio via NTP, testa o TLS com o Gemini e desliga o rádio
static void boot_net_task(void *arg) {
    if (xSemaphoreTake(wifi_mtx, pdMS_TO_TICKS(60000)) == pdTRUE) {
        if (ai_wifi_up()) {
            WiFiClientSecure c;
            google_tls(c);
            c.setTimeout(10);
            bool ok = c.connect("generativelanguage.googleapis.com", 443);
            char err[100] = "";
            if (!ok) c.lastError(err, sizeof(err));
            Serial.printf("[BOOT-NET] Teste TLS Gemini: %s %s | epoch=%ld\n", ok ? "OK" : "FALHOU", err, (long)time(nullptr));
            c.stop();
        }
        ai_wifi_down();
        xSemaphoreGive(wifi_mtx);
    }
    vTaskDelete(NULL);
}

static void ai_put_le(uint8_t *p, uint32_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

#ifndef GEMINI_TTS_MODEL
#define GEMINI_TTS_MODEL "gemini-3.1-flash-tts-preview"
#endif
#define TTS_MAX_BYTES (24000 * 2 * 25)

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Pede a fala ao Gemini e devolve PCM 16 bit 24 kHz mono (buffer em PSRAM) ou NULL
static uint8_t *ai_tts_fetch(const char *text, size_t *out_bytes) {
    JsonDocument req;
    String prompt = String("Diga de forma natural e simpatica: ") + text;
    req["contents"][0]["parts"][0]["text"] = prompt;
    req["generationConfig"]["responseModalities"][0] = "AUDIO";
    req["generationConfig"]["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] = "Kore";
    String body;
    serializeJson(req, body);

    uint8_t *pcm = (uint8_t *)heap_caps_malloc(TTS_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!pcm) return NULL;
    size_t n = 0;

    WiFiClientSecure client;
    google_tls(client);
    HTTPClient http;
    http.useHTTP10(true);  // sem chunked: o corpo pode ser lido direto do stream
    http.setTimeout(45000);
    http.begin(client, String("https://generativelanguage.googleapis.com/v1beta/models/") + GEMINI_TTS_MODEL + ":generateContent");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-goog-api-key", GEMINI_API_KEY);
    int code = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        code = http.POST(body);
        if (code != 503 && code != 500) break;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    Serial.printf("[TTS] HTTP %d\n", code);
    if (code == 200) {
        WiFiClient *st = http.getStreamPtr();
        const char key[] = "\"data\":";
        int ki = 0, state = 0;  // 0 procura chave, 1 procura aspas, 2 le base64
        int q[4], qn = 0;
        uint32_t last = millis();
        while ((http.connected() || st->available()) && millis() - last < 20000) {
            if (!st->available()) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            int c = st->read();
            last = millis();
            if (state == 0) {
                ki = (c == key[ki]) ? ki + 1 : (c == key[0] ? 1 : 0);
                if (key[ki] == 0) state = 1;
            } else if (state == 1) {
                if (c == '"') state = 2;
            } else {
                if (c == '"') break;
                int v = b64v(c);
                if (v < 0) continue;
                q[qn++] = v;
                if (qn == 4) {
                    if (n + 3 > TTS_MAX_BYTES) break;
                    pcm[n++] = (q[0] << 2) | (q[1] >> 4);
                    pcm[n++] = ((q[1] & 15) << 4) | (q[2] >> 2);
                    pcm[n++] = ((q[2] & 3) << 6) | q[3];
                    qn = 0;
                }
            }
        }
    }
    http.end();
    if (n < 4800) {
        free(pcm);
        return NULL;
    }
    *out_bytes = n & ~1u;
    return pcm;
}

// Reamostra 24 kHz -> 16 kHz (linear) e toca pelo ES8311 + amplificador com controle de volume em tempo real
static void ai_play_pcm24(const int16_t *in, size_t n) {
    digitalWrite(PIN_PA_EN, HIGH);
    static int16_t out[256 * 2];
    size_t out_n = n * 2 / 3, i = 0;
    while (i < out_n) {
        size_t cnt = min<size_t>(256, out_n - i);
        float vol_scale = (ai_volume <= 0) ? 0.0f : (ai_volume / 100.0f);
        for (size_t k = 0; k < cnt; k++) {
            float pos = (i + k) * 1.5f;
            size_t i0 = (size_t)pos;
            float f = pos - i0;
            int a = in[i0], b = (i0 + 1 < n) ? in[i0 + 1] : a;
            int32_t sample = (int32_t)((a + (b - a) * f) * vol_scale);
            int16_t v = (int16_t)constrain(sample, -32768, 32767);
            out[2 * k] = v;
            out[2 * k + 1] = v;
        }
        i2s_mic.write((uint8_t *)out, cnt * 4);
        i += cnt;
    }
    memset(out, 0, sizeof(out));
    for (int k = 0; k < 4; k++) i2s_mic.write((uint8_t *)out, sizeof(out));
    digitalWrite(PIN_PA_EN, LOW);
}

static void ai_query(int16_t *pcm, size_t n) {
    // Monta o prompt do sistema injetando dinamicamente a data/hora oficial, memórias e ferramentas da agenda
    String prompt_text =
        "Voce e um robo assistente de voz inteligente em um display de mesa com memoria persistente e integracao com a Google Agenda. "
        "Responda em portugues do Brasil ao que foi dito no audio, em no maximo 3 frases curtas "
        "(ate 160 caracteres), sem markdown e sem emojis. "
        "[DATA E HORA ATUAL]: " + get_now_formatted() + ". "
        "[FATOS QUE VOCE LEMBRA (SEU NOME, DO USUARIO, PREFERENCIAS E AMBIENTE)]: " + mem_build_prompt() + ". "
        "SE a mensagem do usuario trouxer algum fato novo importante (como seu nome proprio de robo, o nome dele, gostos, rotina, cidade ou algo para lembrar), "
        "ao final da sua resposta, adicione exatamente: MEMORIA: <fato curto e claro a ser salvo>. "
        "FERRAMENTAS DA GOOGLE AGENDA: "
        "- Se o usuario pedir para conectar ou vincular a agenda, responda apenas: GCAL_AUTH "
        "- Se o usuario perguntar sobre compromissos ou eventos da agenda, responda apenas: "
        "GCAL_LIST: <data/hora inicio RFC3339> | <data/hora fim RFC3339> "
        "- Se o usuario pedir para marcar ou agendar um compromisso, responda apenas: "
        "GCAL_ADD: <titulo> | <inicio RFC3339> | <fim RFC3339>";

    prompt_text.replace("\"", "\\\"");
    prompt_text.replace("\n", " ");
    prompt_text.replace("\r", "");

    String prefix = "{\"contents\":[{\"parts\":[{\"text\":\"" + prompt_text + "\"},{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
    static const char suffix[] = "\"}}]}]}";

    size_t wav_len = 44 + n * 2;
    uint8_t *wav = (uint8_t *)heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM);
    size_t b64_cap = 4 * ((wav_len + 2) / 3) + 1;
    size_t body_cap = prefix.length() + b64_cap + sizeof(suffix) + 32;
    char *body = (char *)heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM);
    if (!wav || !body) {
        free(wav);
        free(body);
        ai_set(AI_DONE, "Memoria insuficiente.");
        return;
    }
    memcpy(wav, "RIFF", 4);
    ai_put_le(wav + 4, wav_len - 8, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    ai_put_le(wav + 16, 16, 4);
    ai_put_le(wav + 20, 1, 2);
    ai_put_le(wav + 22, 1, 2);
    ai_put_le(wav + 24, MIC_RATE, 4);
    ai_put_le(wav + 28, MIC_RATE * 2, 4);
    ai_put_le(wav + 32, 2, 2);
    ai_put_le(wav + 34, 16, 2);
    memcpy(wav + 36, "data", 4);
    ai_put_le(wav + 40, n * 2, 4);
    memcpy(wav + 44, pcm, n * 2);

    size_t pl = prefix.length(), olen = 0;
    memcpy(body, prefix.c_str(), pl);
    mbedtls_base64_encode((uint8_t *)body + pl, b64_cap, &olen, wav, wav_len);
    memcpy(body + pl + olen, suffix, sizeof(suffix));
    size_t body_len = pl + olen + sizeof(suffix) - 1;
    memset(wav, 0, wav_len);
    free(wav);

    if (xSemaphoreTake(wifi_mtx, pdMS_TO_TICKS(40000)) != pdTRUE) {
        free(body);
        ai_set(AI_DONE, "Wi-Fi ocupado, tente de novo.");
        return;
    }
    if (!ai_wifi_up()) {
        ai_wifi_down();
        xSemaphoreGive(wifi_mtx);
        free(body);
        ai_set(AI_DONE, "Sem Wi-Fi.");
        return;
    }

    String resp;
    int code;
    {
        WiFiClientSecure client;
        google_tls(client);
        HTTPClient http;
        http.setTimeout(45000);
        http.begin(client, String("https://generativelanguage.googleapis.com/v1beta/models/") + GEMINI_MODEL + ":generateContent");
        http.addHeader("Content-Type", "application/json");
        http.addHeader("x-goog-api-key", GEMINI_API_KEY);
        for (int attempt = 0; attempt < 4; attempt++) {
            code = http.POST((uint8_t *)body, body_len);
            if (code > 0) {
                resp = http.getString();
                // 503/500 = modelo temporariamente sobrecarregado: tenta de novo com espera crescente
                if (code != 503 && code != 500) break;
                Serial.printf("[AI] HTTP %d (sobrecarga temporaria), tentativa %d/4\n", code, attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(2000 * (attempt + 1)));
                continue;
            }
            char err_buf[64];
            client.lastError(err_buf, sizeof(err_buf));
            Serial.printf("[HTTP-ERR] Tentativa %d falhou: HTTP %d | TLS erro: %s\n", attempt, code, err_buf);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        http.end();
    }
    memset(body, 0, body_cap);
    free(body);

    char raw[1024], out[1024];
    raw[0] = 0;
    bool ok = false;
    if (code == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, resp)) {
            const char *t = doc["candidates"][0]["content"]["parts"][0]["text"] | "";
            strlcpy(raw, t, sizeof(raw));
        }
        ok = raw[0] != 0;
        if (!ok) strlcpy(raw, "Resposta vazia da IA.", sizeof(raw));
    } else if (code == 429) {
        strlcpy(raw, "Limite gratuito da IA atingido. Aguarde um pouco.", sizeof(raw));
    } else if (code == 400 || code == 403) {
        snprintf(raw, sizeof(raw), "Erro %d: verifique GEMINI_API_KEY no .env.", code);
    } else {
        snprintf(raw, sizeof(raw), "Falha na consulta (codigo %d).", code);
    }
    Serial.printf("[AI] HTTP %d: %s\n", code, raw);

    char speech_text[1024];
    strlcpy(speech_text, raw, sizeof(speech_text));

    // Trata intenções da Google Agenda detectadas pelo Gemini
    if (strstr(raw, "GCAL_AUTH")) {
        gcal_start_device_auth();
        ai_wifi_down();
        xSemaphoreGive(wifi_mtx);
        return;
    } else if (strstr(raw, "GCAL_LIST:")) {
        char *p = strstr(raw, "GCAL_LIST:") + 10;
        while (*p == ' ') p++;
        char *sep = strchr(p, '|');
        String t_min = "", t_max = "";
        if (sep) {
            *sep = 0;
            char *p2 = sep + 1;
            while (*p2 == ' ') p2++;
            char *nl = strchr(p2, '\n'); if (nl) *nl = 0;
            t_min = String(p); t_min.trim();
            t_max = String(p2); t_max.trim();
        } else {
            char *nl = strchr(p, '\n'); if (nl) *nl = 0;
            t_min = String(p); t_min.trim();
        }
        String g_res = gcal_list_events(t_min.c_str(), t_max.c_str());
        strlcpy(speech_text, g_res.c_str(), sizeof(speech_text));
    } else if (strstr(raw, "GCAL_ADD:")) {
        char *p = strstr(raw, "GCAL_ADD:") + 9;
        while (*p == ' ') p++;
        char *sep1 = strchr(p, '|');
        if (sep1) {
            *sep1 = 0;
            char *p2 = sep1 + 1;
            while (*p2 == ' ') p2++;
            char *sep2 = strchr(p2, '|');
            if (sep2) {
                *sep2 = 0;
                char *p3 = sep2 + 1;
                while (*p3 == ' ') p3++;
                char *nl = strchr(p3, '\n'); if (nl) *nl = 0;
                String title = String(p); title.trim();
                String st = String(p2); st.trim();
                String en = String(p3); en.trim();
                String g_res = gcal_create_event(title.c_str(), st.c_str(), en.c_str());
                strlcpy(speech_text, g_res.c_str(), sizeof(speech_text));
            }
        }
    }

    size_t tts_bytes = 0;
    uint8_t *tts = ok ? ai_tts_fetch(speech_text, &tts_bytes) : NULL;
    ai_wifi_down();
    xSemaphoreGive(wifi_mtx);

    if (tts) {
        to_ascii(speech_text, out, sizeof(out));
        ai_set(AI_SPEAKING, out);
        ai_play_pcm24((int16_t *)tts, tts_bytes / 2);
        memset(tts, 0, tts_bytes); // Limpeza segura do áudio sintetizado na PSRAM
        free(tts);
        ai_set(AI_DONE, out);
    } else {
        to_ascii(speech_text, out, sizeof(out));
        ai_set(AI_DONE, out);
    }
}

static void ai_record_and_ask() {
    int16_t *pcm = (int16_t *)heap_caps_malloc(MIC_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) {
        ai_set(AI_DONE, "Memoria insuficiente.");
        return;
    }
    static int16_t tmp[512 * 2];
    // Descarta áudio acumulado antes do toque
    for (int i = 0; i < 4; i++) i2s_mic.readBytes((char *)tmp, sizeof(tmp));

    size_t n = 0;
    int dry = 0;
    ai_rec_ms = millis();
    while (ai_rec && n < MIC_MAX_SAMPLES - 512) {
        size_t got = i2s_mic.readBytes((char *)tmp, sizeof(tmp));
        if (got == 0) {
            if (++dry > 10) break;
            continue;
        }
        dry = 0;
        for (size_t i = 0; i + 1 < got / 2; i += 2) pcm[n++] = ((int)tmp[i] + (int)tmp[i + 1]) / 2;
    }
    ai_rec = false;

    int peak = 1;
    for (size_t i = 0; i < n; i++) peak = max(peak, abs((int)pcm[i]));
    Serial.printf("[AI] %u amostras, pico %d\n", (unsigned)n, peak);

    if (n < MIC_RATE / 2) {
        ai_set(AI_DONE, "Toque curto demais. Segure a tela enquanto fala.");
    } else if (peak < 150) {
        ai_set(AI_DONE, "Nao captei som. Fale mais perto do aparelho.");
    } else {
        float gain = min(16.0f, 24000.0f / peak);
        for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)constrain((int)(pcm[i] * gain), -32768, 32767);
        ai_set(AI_THINKING, NULL);
        ai_query(pcm, n);
    }
    memset(pcm, 0, MIC_MAX_SAMPLES * sizeof(int16_t));
    free(pcm);
}

static void ai_task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ai_record_and_ask();
    }
}

static lv_obj_t *lbl_ai_text, *lbl_ai_status, *ai_box;
static int ai_shown_state = -1;
extern uint32_t talk_until;

static void ai_btn_cb(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_LONG_PRESSED) {
        if (!mic_ok) {
            ai_set(AI_DONE, "Microfone nao iniciou.");
        } else if (!strlen(GEMINI_API_KEY)) {
            ai_set(AI_DONE, "Defina GEMINI_API_KEY no .env (chave gratuita em aistudio.google.com).");
        } else if (ai_state != AI_LISTENING && ai_state != AI_THINKING && ai_state != AI_SPEAKING) {
            ai_rec = true;
            ai_set(AI_LISTENING, "");
            xTaskNotifyGive(ai_task_handle);
        }
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        ai_rec = false;
    }
}

static void ai_timer_cb(lv_timer_t *t) {
    if (ai_dirty) {
        char buf[1024];
        portENTER_CRITICAL(&ai_mux);
        strlcpy(buf, ai_text, sizeof(buf));
        ai_dirty = false;
        portEXIT_CRITICAL(&ai_mux);
        lv_label_set_text(lbl_ai_text, buf);
        if (buf[0]) lv_obj_clear_flag(ai_box, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ai_box, LV_OBJ_FLAG_HIDDEN);
    }
    int st = ai_state;
    if (st == AI_LISTENING) {
        lv_label_set_text_fmt(lbl_ai_status, "Ouvindo... %us\nSolte a tela quando terminar", (unsigned)((millis() - ai_rec_ms) / 1000));
    } else if (st != ai_shown_state) {
        lv_label_set_text(lbl_ai_status, st == AI_THINKING ? "Pensando...\nConsultando a IA" : st == AI_SPEAKING ? "Falando...\nOuca a resposta" : "Segure a tela e fale");
    }
    if (st == AI_DONE && ai_shown_state != AI_DONE && lv_label_get_text(lbl_ai_text)[0]) talk_until = millis() + min<uint32_t>(8000, strlen(lv_label_get_text(lbl_ai_text)) * 55);
    ai_shown_state = st;
}

// Robô em pixel art (grade 64x64 ampliada) desenhado em um canvas
#define SPR_W 64
#define SPR_H 64
static lv_color_t spr_buf[SPR_W * SPR_H];
static lv_obj_t *spr_canvas;
uint32_t talk_until = 0;
static int spr_dy = 0;

enum : uint32_t {
    C_OUT = 0x2A3F8F, C_LIGHT = 0xE4F0FF, C_MID = 0xAFCFF5, C_SHADE = 0x7BA3DD,
    C_SCREEN = 0x0A1630, C_CYAN = 0x5CF0F0, C_CHEEK = 0xF27A8A, C_GLOW = 0x7CFFD8,
    C_PANEL = 0x3A6FD0, C_DIM = 0x7777B0
};

static inline void spr_px(int x, int y, uint32_t c) {
    y += spr_dy;
    if (x >= 0 && x < SPR_W && y >= 0 && y < SPR_H) spr_buf[y * SPR_W + x] = lv_color_hex(c);
}

static void spr_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) spr_px(x + i, y + j, c);
}

static void spr_ell(float cx, float cy, float rx, float ry, uint32_t c) {
    for (int y = (int)(cy - ry - 1); y <= (int)(cy + ry + 1); y++)
        for (int x = (int)(cx - rx - 1); x <= (int)(cx + rx + 1); x++) {
            float ux = (x - cx) / rx, uy = (y - cy) / ry;
            if (ux * ux + uy * uy <= 1.0f) spr_px(x, y, c);
        }
}

// Elipse com contorno e sombreamento (luz vinda de cima à esquerda)
static void spr_shell(float cx, float cy, float rx, float ry) {
    spr_ell(cx, cy, rx + 1, ry + 1, C_OUT);
    for (int y = (int)(cy - ry - 1); y <= (int)(cy + ry + 1); y++)
        for (int x = (int)(cx - rx - 1); x <= (int)(cx + rx + 1); x++) {
            float ux = (x - cx) / rx, uy = (y - cy) / ry;
            if (ux * ux + uy * uy > 1.0f) continue;
            float n = -0.5f * ux - 0.7f * uy;
            spr_px(x, y, n > 0.25f ? C_LIGHT : (n < -0.35f ? C_SHADE : C_MID));
        }
}

static void spr_rrect(int x, int y, int w, int h, int r, uint32_t c, int ymin = -999) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            if (y + j < ymin) continue;
            int dx = i < r ? r - i : (i >= w - r ? i - (w - r - 1) : 0);
            int dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
            if (dx * dx + dy * dy <= r * r) spr_px(x + i, y + j, c);
        }
}

static void spr_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int n = max(abs(x1 - x0), abs(y1 - y0));
    for (int i = 0; i <= n; i++) spr_px(x0 + (x1 - x0) * i / max(n, 1), y0 + (y1 - y0) * i / max(n, 1), c);
}

static void spr_gear(float cx, float cy, float r, float ang, uint32_t c) {
    for (int y = (int)(cy - r - 2); y <= (int)(cy + r + 2); y++)
        for (int x = (int)(cx - r - 2); x <= (int)(cx + r + 2); x++) {
            float dx = x - cx, dy = y - cy, d = sqrtf(dx * dx + dy * dy);
            if (d < r * 0.35f) continue;
            if (d <= r - 0.3f || (d <= r + 1.3f && cosf(8 * (atan2f(dy, dx) + ang)) > 0.1f)) spr_px(x, y, c);
        }
}

static void spr_note(int x, int y, uint32_t c) {
    spr_ell(x, y + 5, 1.8f, 1.4f, c);
    spr_rect(x + 1, y - 1, 1, 6, c);
    spr_px(x + 2, y, c);
    spr_px(x + 3, y + 1, c);
}

static void spr_ring_eye(int cx, int cy, float r, float pdx, float pdy) {
    spr_ell(cx, cy, r, r, C_CYAN);
    spr_ell(cx + pdx, cy + pdy, r - 1.4f, r - 1.4f, C_SCREEN);
    spr_px(cx - 1, cy - 1, 0xFFFFFF);
}

static void spr_arm(float cx, float cy, float rx, float ry, int hx, int hy) {
    spr_shell(cx, cy, rx, ry);
    spr_shell(hx, hy, 2.6f, 2.6f);
}

static void spr_draw(uint32_t now, int st, bool talking) {
    for (int i = 0; i < SPR_W * SPR_H; i++) spr_buf[i] = lv_color_black();
    enum { M_IDLE, M_LISTEN, M_THINK, M_SPEAK } mode =
        talking ? M_SPEAK : (st == AI_LISTENING ? M_LISTEN : (st == AI_THINKING ? M_THINK : M_IDLE));
    float t = now / 1000.0f;

    // Brilhos de fundo (só repouso e fala)
    spr_dy = 0;
    if (mode == M_IDLE || mode == M_SPEAK) {
        static const int8_t sp[5][2] = {{5, 20}, {58, 26}, {6, 50}, {57, 48}, {10, 8}};
        for (int i = 0; i < 5; i++) {
            int ph = (now / 300 + i * 4) % 8;
            if (ph > 3) continue;
            bool big = ph == 1 || ph == 2;
            spr_px(sp[i][0], sp[i][1], big ? 0xFFFFFF : C_DIM);
            if (big) {
                spr_px(sp[i][0] - 1, sp[i][1], C_DIM);
                spr_px(sp[i][0] + 1, sp[i][1], C_DIM);
                spr_px(sp[i][0], sp[i][1] - 1, C_DIM);
                spr_px(sp[i][0], sp[i][1] + 1, C_DIM);
            }
        }
    }
    spr_ell(32, 61, 15, 2, 0x1A1A38);

    int bob = (int)lroundf(sinf(t * (mode == M_SPEAK ? 5.0f : 2.2f)) * 1.5f);
    spr_dy = bob;

    // Pés
    spr_shell(25, 56, 3.5f, 2.2f);
    spr_shell(39, 56, 3.5f, 2.2f);

    // Braços
    float sway = sinf(t * 2.2f) * 1.2f;
    bool waving = mode == M_IDLE && (now % 8000) < 1600;
    float osc = sinf(t * 9.0f);
    if (mode == M_IDLE) {
        spr_arm(14, 43 + sway, 2.8f, 6, 13, 49 + sway);
        if (waving) spr_arm(51, 36, 2.8f, 6, 54 + osc * 2, 28);
        else spr_arm(50, 43 - sway, 2.8f, 6, 51, 49 - sway);
    } else if (mode == M_LISTEN) {
        spr_arm(12, 36, 2.8f, 6, 12, 27);
        spr_arm(50, 43, 2.8f, 6, 51, 49);
    } else if (mode == M_THINK) {
        spr_arm(14, 44, 2.8f, 5, 20, 47);
    } else {
        spr_arm(12, 36, 2.8f, 6, 10, 28 + osc);
        spr_arm(52, 36, 2.8f, 6, 54, 28 - osc);
    }

    // Corpo
    spr_shell(32, 45, 13, 11);
    spr_rrect(29, 46, 6, 3, 1, C_PANEL);

    // Antenas
    float pulse = (sinf(t * 4) + 1) * 0.5f;
    uint32_t glow = pulse > 0.5f ? 0xC8FFF0 : C_GLOW;
    spr_line(24, 11, 22, 7, C_OUT);
    spr_line(40, 11, 42, 7, C_OUT);
    spr_shell(22, 5.5f, 1.6f, 1.6f);
    spr_shell(42, 5.5f, 1.6f, 1.6f);
    spr_ell(22, 5.5f, 1.6f, 1.6f, glow);
    spr_ell(42, 5.5f, 1.6f, 1.6f, glow);

    // Cabeça
    spr_rrect(14, 10, 36, 32, 14, C_OUT);
    spr_rrect(15, 11, 34, 30, 13, C_LIGHT);
    spr_rrect(15, 11, 34, 30, 13, C_MID, 33);
    spr_ell(14, 27, 2, 4, C_OUT);
    spr_ell(50, 27, 2, 4, C_OUT);
    spr_ell(14, 27, 1, 3, C_SHADE);
    spr_ell(50, 27, 1, 3, C_SHADE);
    spr_rrect(17, 14, 30, 21, 9, C_OUT);
    spr_rrect(18, 15, 28, 19, 8, C_SCREEN);

    bool blink = mode != M_LISTEN && mode != M_SPEAK && (now % 3600) < 140;
    int ex = 26, ex2 = 38, ey = 23;
    if (mode == M_IDLE) {
        float look = sinf(t * 0.8f);
        float pdx = look > 0.6f ? 1 : (look < -0.6f ? -1 : 0);
        if (blink) { spr_rect(ex - 2, ey, 5, 1, C_CYAN); spr_rect(ex2 - 2, ey, 5, 1, C_CYAN); }
        else { spr_ring_eye(ex, ey, 3.6f, pdx * 0.6f, 0); spr_ring_eye(ex2, ey, 3.6f, pdx * 0.6f, 0); }
        spr_px(30, 30, C_CYAN); spr_rect(31, 31, 3, 1, C_CYAN); spr_px(34, 30, C_CYAN);
    } else if (mode == M_LISTEN) {
        spr_ring_eye(ex, ey, 4.5f, 0, 0);
        spr_ring_eye(ex2, ey, 4.5f, 0, 0);
        spr_ell(32, 31, 2, 2, C_CYAN);
        spr_ell(32, 31, 0.9f, 0.9f, C_SCREEN);
    } else if (mode == M_THINK) {
        if (blink) { spr_rect(ex - 2, ey, 5, 1, C_CYAN); spr_rect(ex2 - 2, ey, 5, 1, C_CYAN); }
        else { spr_ring_eye(ex, ey, 3.8f, 1.3f, -1.5f); spr_ring_eye(ex2, ey, 3.8f, 1.3f, -1.5f); }
        spr_px(30, 31, C_CYAN); spr_rect(31, 30, 3, 1, C_CYAN); spr_px(34, 31, C_CYAN);
    } else {
        for (int k = 0; k < 2; k++) {
            int cx = k ? ex2 : ex;
            for (int dx = -3; dx <= 3; dx++) {
                int y = ey + 1 - (int)lroundf(2.4f - dx * dx / 3.8f);
                spr_px(cx + dx, y, C_CYAN);
                spr_px(cx + dx, y + 1, C_CYAN);
            }
        }
        float open = (sinf(now / 75.0f) + 1.0f) * 0.5f;
        spr_ell(32, 30, 4, 1.2f + open * 2.4f, C_CYAN);
        spr_ell(32, 30.5f, 2.5f, 0.2f + open * 1.4f, 0x1B6E86);
    }
    spr_ell(22, 29, 2.6f, 1.6f, C_CHEEK);
    spr_ell(42, 29, 2.6f, 1.6f, C_CHEEK);

    // Mão no queixo (pensando) fica à frente da cabeça
    if (mode == M_THINK) spr_arm(40, 40, 2.8f, 5, 36, 37);

    // Efeitos
    spr_dy = 0;
    if (mode == M_LISTEN) {
        int ph = (now / 220) % 3;
        for (int k = 0; k <= ph; k++) {
            spr_line(7 - k * 2, 21, 7 - k * 2, 27, C_CYAN);
            spr_line(57 + k * 2, 21, 57 + k * 2, 27, C_CYAN);
        }
        if ((now / 350) & 1) {
            for (int k = 0; k < 2; k++) {
                spr_rect(54 + k * 4, 2, 2, 5, 0xBFD4FF);
                spr_rect(54 + k * 4, 8, 2, 2, 0xBFD4FF);
            }
        }
    } else if (mode == M_THINK) {
        float a = t * 1.5f;
        spr_gear(34, 4, 3.5f, a, C_SHADE);
        spr_gear(25, 2, 2, -a * 1.6f, C_DIM);
        spr_gear(45, 1, 2, -a * 1.6f, C_DIM);
        spr_ell(55, 8, 7, 5, C_LIGHT);
        spr_px(50, 14, C_LIGHT);
        spr_px(49, 16, C_LIGHT);
        spr_gear(55, 8, 3, a, C_SHADE);
    } else if (mode == M_SPEAK) {
        int f = (now / 400) & 1;
        spr_note(5, 14 - f, C_DIM);
        spr_note(57, 10 + f, C_DIM);
        for (int k = 0; k < 5; k++) {
            int h = 2 + (int)((sinf(now / 90.0f + k * 1.7f) + 1.0f) * 3);
            spr_rect(58 + k, 36 - h / 2, 1, h, 0x3AD0C0);
        }
    }
    lv_obj_invalidate(spr_canvas);
}

static void face_timer_cb(lv_timer_t *t) {
    uint32_t now = millis();
    int st = ai_state;
    bool talking = (st == AI_SPEAKING) || (st == AI_DONE && now < talk_until);
    spr_draw(now, st, talking);
    lv_obj_align(spr_canvas, LV_ALIGN_CENTER, 0, 40);
}

// -----------------------------------------------------------------------------
// Controle de Volume Físico (Botões + e - nas portas GPIO 0 e GPIO 18) + HUD na Tela
// -----------------------------------------------------------------------------
static lv_obj_t *vol_box = NULL;
static lv_obj_t *lbl_vol = NULL;
static lv_obj_t *vol_bar_bg = NULL;
static lv_obj_t *vol_bar_fill = NULL;
static uint32_t vol_hud_until = 0;

static volatile uint32_t btn_up_clicks = 0;
static volatile uint32_t btn_down_clicks = 0;
static volatile uint32_t last_up_isr_us = 0;
static volatile uint32_t last_down_isr_us = 0;
static bool vol_nvs_dirty = false;

static void IRAM_ATTR on_btn_vol_up_isr() {
    uint32_t now_us = micros();
    if (now_us - last_up_isr_us >= 80000) { // 80 ms debounce por software (Key3 / GPIO 18 tem C22=NC)
        last_up_isr_us = now_us;
        btn_up_clicks++;
    }
}

static void IRAM_ATTR on_btn_vol_down_isr() {
    uint32_t now_us = micros();
    if (now_us - last_down_isr_us >= 80000) { // 80 ms debounce por software (Key2 / GPIO 0)
        last_down_isr_us = now_us;
        btn_down_clicks++;
    }
}

static void play_volume_beep(int vol_pct, bool is_up) {
    if (!mic_ok || vol_pct <= 0 || ai_rec) return;
    if (ai_state != AI_IDLE && ai_state != AI_DONE) return;

    digitalWrite(PIN_PA_EN, HIGH);
    static int16_t tone_buf[256 * 2];
    const float freq = is_up ? 880.0f : 660.0f;
    const float amp = (vol_pct / 100.0f) * 8000.0f;
    // Gera um bip curto e suave de 256 amostras (16 ms @ 16 kHz) com envelope de fade-in/out
    for (int i = 0; i < 256; i++) {
        float env = (i < 48) ? (i / 48.0f) : ((i > 208) ? ((255 - i) / 48.0f) : 1.0f);
        int16_t s = (int16_t)(sinf(2.0f * 3.14159265f * freq * i / (float)MIC_RATE) * amp * env);
        tone_buf[2 * i] = s;
        tone_buf[2 * i + 1] = s;
    }
    i2s_mic.write((uint8_t *)tone_buf, sizeof(tone_buf));
    memset(tone_buf, 0, sizeof(tone_buf));
    i2s_mic.write((uint8_t *)tone_buf, sizeof(tone_buf));
    digitalWrite(PIN_PA_EN, LOW);
}

static void show_volume_hud(int vol_pct) {
    if (!vol_box) return;
    lv_label_set_text_fmt(lbl_vol, "Volume: %d%%", vol_pct);
    int fill_w = (200 * constrain(vol_pct, 0, 100)) / 100;
    lv_obj_set_width(vol_bar_fill, max(fill_w, 4));
    lv_obj_set_style_bg_color(vol_bar_fill, vol_pct == 0 ? lv_color_hex(0xFF4545) : lv_color_hex(0x5CF0F0), 0);
    lv_obj_clear_flag(vol_box, LV_OBJ_FLAG_HIDDEN);
    vol_hud_until = millis() + 2000;
}

static void adjust_volume(int delta, const char *source_name) {
    int new_vol = constrain((int)ai_volume + delta, 0, 100);
    ai_volume = new_vol;
    vol_nvs_dirty = true;
    show_volume_hud(new_vol);
    play_volume_beep(new_vol, delta > 0);
    Serial.printf("[VOL] %s -> Volume ajustado para %d%%\n", source_name, new_vol);
}

static void init_volume_buttons() {
    prefs.begin("robot_ai", false);
    ai_volume = constrain(prefs.getInt("vol", 80), 0, 100);

    pinMode(PIN_BTN_VOL_UP, INPUT_PULLUP);
    pinMode(PIN_BTN_VOL_DOWN, INPUT_PULLUP);
    delay(10);

    last_up_isr_us = micros();
    last_down_isr_us = micros();
    btn_up_clicks = 0;
    btn_down_clicks = 0;

    attachInterrupt(digitalPinToInterrupt(PIN_BTN_VOL_UP), on_btn_vol_up_isr, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN_VOL_DOWN), on_btn_vol_down_isr, FALLING);

    Serial.printf("[BTN] Botoes de volume prontos: [+] GPIO %d (Key3/IO18), [-] GPIO %d (Key2/BOOT), Volume=%d%%\n",
                  PIN_BTN_VOL_UP, PIN_BTN_VOL_DOWN, (int)ai_volume);
}

static void vol_btn_timer_cb(lv_timer_t *t) {
    static uint32_t up_hold_start = 0, down_hold_start = 0;
    static uint32_t last_repeat_ms = 0;

    uint32_t now = millis();

    // 1. Processa cliques capturados via interrupção de hardware
    if (btn_up_clicks > 0) {
        btn_up_clicks = 0;
        up_hold_start = now;
        last_repeat_ms = now;
        adjust_volume(+10, "Botao + (GPIO 18)");
    }
    if (btn_down_clicks > 0) {
        btn_down_clicks = 0;
        down_hold_start = now;
        last_repeat_ms = now;
        adjust_volume(-10, "Botao - (GPIO 0)");
    }

    // 2. Suporte a manter pressionado (auto-repeat após 500 ms segurando continuamente em LOW)
    if (digitalRead(PIN_BTN_VOL_UP) == LOW) {
        if (up_hold_start == 0) up_hold_start = now;
        if ((now - up_hold_start > 500) && (now - last_repeat_ms > 180)) {
            last_repeat_ms = now;
            last_up_isr_us = micros();
            btn_up_clicks = 0;
            adjust_volume(+10, "Botao + (Hold)");
        }
    } else {
        up_hold_start = 0;
    }

    if (digitalRead(PIN_BTN_VOL_DOWN) == LOW) {
        if (down_hold_start == 0) down_hold_start = now;
        if ((now - down_hold_start > 500) && (now - last_repeat_ms > 180)) {
            last_repeat_ms = now;
            last_down_isr_us = micros();
            btn_down_clicks = 0;
            adjust_volume(-10, "Botao - (Hold)");
        }
    } else {
        down_hold_start = 0;
    }

    // 3. Oculta o HUD de volume após 2 segundos e salva na memória flash (NVS) sem travar os cliques
    if (vol_box && vol_hud_until != 0 && now >= vol_hud_until) {
        lv_obj_add_flag(vol_box, LV_OBJ_FLAG_HIDDEN);
        vol_hud_until = 0;
        if (vol_nvs_dirty) {
            prefs.putInt("vol", (int)ai_volume);
            vol_nvs_dirty = false;
        }
    }
}

void build_ai_ui(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Personagem
    spr_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(spr_canvas, spr_buf, SPR_W, SPR_H, LV_IMG_CF_TRUE_COLOR);
    lv_img_set_antialias(spr_canvas, false);
    lv_img_set_zoom(spr_canvas, 1600);
    lv_obj_align(spr_canvas, LV_ALIGN_CENTER, 0, 40);
    no_click(spr_canvas);

    // Balão de fala
    lv_obj_t *box = ai_box = lv_obj_create(parent);
    lv_obj_set_size(box, 320, 130);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -45);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x101014), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x2A2A33), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 24, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    no_click(box);

    lbl_ai_text = lv_label_create(box);
    lv_obj_set_width(lbl_ai_text, 290);
    lv_label_set_long_mode(lbl_ai_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl_ai_text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ai_text, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl_ai_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_ai_text, ai_text);
    lv_obj_align(lbl_ai_text, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_event_cb(parent, ai_btn_cb, LV_EVENT_ALL, NULL);

    lbl_ai_status = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_ai_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_ai_status, lv_color_hex(0xA0A0A8), 0);
    lv_label_set_text(lbl_ai_status, "Segure a tela e fale");
    lv_obj_set_style_text_align(lbl_ai_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_ai_status, LV_ALIGN_TOP_MID, 0, 34);

    // Overlay de Volume (aparece ao apertar os botões físicos + e -)
    vol_box = lv_obj_create(parent);
    lv_obj_set_size(vol_box, 240, 68);
    lv_obj_align(vol_box, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(vol_box, lv_color_hex(0x141A29), 0);
    lv_obj_set_style_bg_opa(vol_box, LV_OPA_90, 0);
    lv_obj_set_style_border_color(vol_box, lv_color_hex(0x5CF0F0), 0);
    lv_obj_set_style_border_width(vol_box, 2, 0);
    lv_obj_set_style_radius(vol_box, 20, 0);
    lv_obj_set_style_pad_all(vol_box, 8, 0);
    lv_obj_add_flag(vol_box, LV_OBJ_FLAG_HIDDEN);
    no_click(vol_box);

    lbl_vol = lv_label_create(vol_box);
    lv_obj_set_style_text_font(lbl_vol, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_vol, lv_color_white(), 0);
    lv_label_set_text_fmt(lbl_vol, "Volume: %d%%", (int)ai_volume);
    lv_obj_align(lbl_vol, LV_ALIGN_TOP_MID, 0, 2);

    vol_bar_bg = lv_obj_create(vol_box);
    lv_obj_set_size(vol_bar_bg, 200, 12);
    lv_obj_align(vol_bar_bg, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(vol_bar_bg, lv_color_hex(0x2A3248), 0);
    lv_obj_set_style_bg_opa(vol_bar_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vol_bar_bg, 0, 0);
    lv_obj_set_style_radius(vol_bar_bg, 6, 0);
    lv_obj_set_style_pad_all(vol_bar_bg, 0, 0);
    no_click(vol_bar_bg);

    vol_bar_fill = lv_obj_create(vol_bar_bg);
    lv_obj_set_size(vol_bar_fill, (200 * ai_volume) / 100, 12);
    lv_obj_align(vol_bar_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(vol_bar_fill, lv_color_hex(0x5CF0F0), 0);
    lv_obj_set_style_bg_opa(vol_bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vol_bar_fill, 0, 0);
    lv_obj_set_style_radius(vol_bar_fill, 6, 0);
    no_click(vol_bar_fill);

    lv_timer_create(ai_timer_cb, 100, NULL);
    lv_timer_create(face_timer_cb, 40, NULL);
    lv_timer_create(vol_btn_timer_cb, 30, NULL);
}

// -----------------------------------------------------------------------------
// Inicialização Principal do Sistema
// -----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    Serial.println("[BOOT] Robot AI iniciado");

    // 1. Inicializar o núcleo do LVGL
    lv_init();

    // 2. Alocar buffer de desenho em RAM interna compatível com DMA
    disp_buf = (lv_color_t *)heap_caps_malloc(SCREEN_WIDTH * DRAW_BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!disp_buf) {
        Serial.println("[ERRO] Falha critica: Memoria interna insuficiente para alocar o buffer do ecrã!");
        while (1) { delay(1000); }
    }
    lv_disp_draw_buf_init(&draw_buf, disp_buf, NULL, SCREEN_WIDTH * DRAW_BUF_LINES);

    // 3. Inicializar a estrutura do driver de exibição
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.draw_buf = &draw_buf;

    // 4. Inicializar o barramento e o circuito de hardware do AMOLED
    init_amoled_hardware(&disp_drv);

    // 5. Concluir o registo do display no motor LVGL
    lv_disp_drv_register(&disp_drv);
    init_touch();
    wifi_mtx = xSemaphoreCreateMutex();
    init_volume_buttons();
    mem_load(); // Carrega os fatos aprendidos salvos na Flash NVS
    gcal_load_token(); // Carrega o refresh_token da Google Agenda se já emparelhado
    init_audio();
    xTaskCreatePinnedToCore(ai_task, "AI_Task", 20480, NULL, 1, &ai_task_handle, 0);
    xTaskCreatePinnedToCore(boot_net_task, "BootNet", 12288, NULL, 1, NULL, 0);

    // 6. Construir a interface do Agente de IA na tela principal
    build_ai_ui(lv_scr_act());
}

void loop() {
    lv_timer_handler();
    delay(5);
}
