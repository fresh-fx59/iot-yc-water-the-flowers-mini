#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <secret.h>

#define FIRMWARE_VERSION "0.9.0"

// GPIO (TODO: confirm against YD-ESP32-23 v1.3 silkscreen before flashing)
#define MOTOR_RELAY_PIN          5
#define SOIL_SENSOR_AOUT_PIN     4    // ADC1 only
#define OVERFLOW_SENSOR_DO_PIN   42
#define LED_PIN                  48
#define RTC_SDA_PIN              14
#define RTC_SCL_PIN              3

// DS3231 I2C (kept-from-mother headers reference these names)
#define I2C_SDA_PIN              RTC_SDA_PIN
#define I2C_SCL_PIN              RTC_SCL_PIN
#define DS3231_I2C_ADDRESS       0x68

// DS3231 Battery Measurement pins (used by DS3231RTC.h::getBatteryVoltage)
#define BATTERY_ADC_PIN          1
#define BATTERY_CONTROL_PIN      2
static const float BATTERY_VOLTAGE_CALIBRATION = 1.0695f;

// Motor relay polarity
static const bool MOTOR_RELAY_ACTIVE_HIGH = true;
inline int motorOnLevel()  { return MOTOR_RELAY_ACTIVE_HIGH ? HIGH : LOW; }
inline int motorOffLevel() { return MOTOR_RELAY_ACTIVE_HIGH ? LOW  : HIGH; }

// Schedule defaults
static const int      DEFAULT_INTERVAL_DAYS         = 4;
static const int      DEFAULT_SCHEDULE_HOUR         = 7;
static const int      DEFAULT_SCHEDULE_MINUTE       = 0;
static const uint32_t DEFAULT_MAX_RUNTIME_SEC       = 120;
static const int      DEFAULT_SOIL_THRESHOLD        = 1800;
static const int      DEFAULT_CALIBRATION_DRY       = 0;
static const int      DEFAULT_CALIBRATION_WET       = 0;

// Schedule grace
static const unsigned long SCHEDULE_GRACE_MS = 12UL * 3600UL * 1000UL;

// Sensor sampling
static const int          SOIL_AVG_SAMPLES                 = 8;
static const unsigned long SOIL_POLL_INTERVAL_MS           = 100;
static const int          OVERFLOW_DEBOUNCE_WINDOW         = 7;
static const int          OVERFLOW_DEBOUNCE_TRIP_THRESHOLD = 5;
static const unsigned long OVERFLOW_POLL_INTERVAL_MS       = 50;

// Safety
static const unsigned long GLOBAL_WATCHDOG_MARGIN_MS = 5000UL;

// Skip-wet escalation
static const int CONSECUTIVE_SKIPS_WET_ALERT_THRESHOLD = 2;

// WiFi (from mother)
static const int           WIFI_MAX_RETRY_ATTEMPTS         = 20;
static const unsigned long WIFI_RETRY_DELAY_MS             = 500;
static const int           WIFI_RECONNECT_MAX_ATTEMPTS     = 5;
static const unsigned long WIFI_RECONNECT_BACKOFF_INITIAL_MS = 5000UL;
static const unsigned long WIFI_RECONNECT_BACKOFF_MAX_MS    = 300000UL;
static const unsigned long WIFI_OUTAGE_NOTIFY_THRESHOLD_MS  = 60000UL;

// LittleFS files
static const char* SETTINGS_FILE = "/settings.json";
static const char* STATE_FILE    = "/state.json";

// Cross-core notification queue
static const int NOTIFICATION_QUEUE_SIZE = 16;

// ============================================
// Debug Configuration (kept-from-mother headers)
// ============================================
#define IS_DEBUG_TO_SERIAL_ENABLED   false
#define IS_DEBUG_TO_TELEGRAM_ENABLED true
#define DEBUG_SERIAL                 Serial

// ============================================
// Telegram Queue & Backoff (DebugHelper.h, TelegramNotifier.h)
// ============================================
static const int           TELEGRAM_QUEUE_SIZE                  = 20;
static const int           TELEGRAM_MAX_RETRY_ATTEMPTS          = 5;
static const unsigned long TELEGRAM_RETRY_DELAY_MS              = 2000UL;
static const unsigned long TELEGRAM_HTTP_TIMEOUT_MS             = 1500UL;
static const unsigned long TELEGRAM_PROXY_HTTP_TIMEOUT_MS       = 4000UL;
static const unsigned long TELEGRAM_FAILURE_COOLDOWN_INITIAL_MS = 5000UL;
static const unsigned long TELEGRAM_FAILURE_COOLDOWN_MAX_MS     = 300000UL;
static const unsigned long MESSAGE_GROUP_INTERVAL_MS            = 2000UL;
static const unsigned long MESSAGE_GROUP_MAX_AGE_MS             = 180000UL;

// Optional monitoring-server Telegram proxy. Concrete URL/token live in secret.h.
#ifndef TELEGRAM_PROXY_BASE_URL
#define TELEGRAM_PROXY_BASE_URL ""
#endif
#ifndef TELEGRAM_PROXY_AUTH_TOKEN
#define TELEGRAM_PROXY_AUTH_TOKEN ""
#endif

// Metrics proxy uses same base URL and auth as Telegram proxy.
#ifndef METRICS_PROXY_BASE_URL
#define METRICS_PROXY_BASE_URL TELEGRAM_PROXY_BASE_URL
#endif

// ============================================
// Metrics Log Routing (used by all logging headers)
// ============================================
// Global callback for routing log messages to MetricsPusher (Loki).
// Set by MetricsPusher::init(). Allows headers included before MetricsPusher.h
// to log without a compile-time dependency on MetricsPusher.
//
// NOTE: Declared `extern` here so that multiple translation units may include
// config.h without producing multiple-definition link errors. The single
// definition lives in src/main.cpp.
typedef void (*MetricsLogFn)(const String& level, const String& msg);
extern MetricsLogFn g_metricsLog;

// Global telegram failure counter (incremented by TelegramNotifier, read by MetricsPusher).
// See note above re: extern + single definition in src/main.cpp.
extern int g_telegramFailures;

// ============================================
// Metrics Push Configuration (MetricsPusher.h)
// ============================================
static const unsigned long METRICS_PUSH_INTERVAL_ACTIVE_MS = 10000UL;
static const unsigned long METRICS_PUSH_INTERVAL_IDLE_MS   = 60000UL;
static const int           METRICS_LOG_BUFFER_SIZE         = 64;
static const unsigned long METRICS_HTTP_TIMEOUT_MS         = 4000UL;

// RTC timezone: DS3231 stores Moscow local time (UTC+3).
// System time runs on local time; this offset converts to UTC for Loki/Prometheus.
static const long RTC_TIMEZONE_OFFSET_SEC = 3 * 3600;

// ============================================
// OTA Configuration (ota.h)
// ============================================
#define OTA_HOSTNAME "esp32-watering-mini"

#endif
