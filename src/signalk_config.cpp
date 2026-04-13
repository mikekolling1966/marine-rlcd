#include "signalk_config.h"
#include "network_setup.h"
#include "screen_config_c_api.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include "esp_task_wdt.h"
#include <esp_heap_caps.h>
#include <map>
#include <set>
#include <vector>

extern "C" int ui_get_current_screen(void);

// STL allocator that places all nodes in PSRAM instead of iRAM.
template <typename T>
struct PsramStlAllocator {
    using value_type = T;
    PsramStlAllocator() = default;
    template <class U> PsramStlAllocator(const PsramStlAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!p) {
            p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
        }
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
};
template <class T, class U>
bool operator==(const PsramStlAllocator<T>&, const PsramStlAllocator<U>&) { return true; }
template <class T, class U>
bool operator!=(const PsramStlAllocator<T>&, const PsramStlAllocator<U>&) { return false; }

template <typename K, typename V>
using PsramMap = std::map<K, V, std::less<K>,
    PsramStlAllocator<std::pair<const K, V>>>;

// Custom ArduinoJson allocator that uses PSRAM instead of internal RAM.
// Saves ~4 KB of iRAM on every SK WebSocket message parse.
struct PsramAllocator {
    void* allocate(size_t size) {
        void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!p) {
            p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
        }
        return p;
    }
    void deallocate(void* ptr) {
        heap_caps_free(ptr);
    }
    void* reallocate(void* ptr, size_t new_size) {
        void* p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!p) {
            p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_8BIT);
        }
        return p;
    }
};

static ArduinoJson::Allocator* psram_json_allocator() {
    return ArduinoJson::detail::AllocatorAdapter<PsramAllocator>::instance();
}

// Global array to hold all sensor values (10 parameters)
float g_sensor_values[TOTAL_PARAMS] = {
    0,        // SCREEN1_RPM
    313.15,   // SCREEN1_COOLANT_TEMP
    0,        // SCREEN2_RPM
    50.0,     // SCREEN2_FUEL
    313.15,   // SCREEN3_COOLANT_TEMP
    373.15,   // SCREEN3_EXHAUST_TEMP
    50.0,     // SCREEN4_FUEL
    313.15,   // SCREEN4_COOLANT_TEMP
    2.0,      // SCREEN5_OIL_PRESSURE
    313.15    // SCREEN5_COOLANT_TEMP
};

// Mutex for thread-safe access to sensor variables
SemaphoreHandle_t sensor_mutex = NULL;

// Metadata storage for each parameter
String g_sensor_units[TOTAL_PARAMS];
String g_sensor_descriptions[TOTAL_PARAMS];

// Navigation globals for POSITION/COMPASS display types
volatile float g_nav_latitude  = NAN;
volatile float g_nav_longitude = NAN;
char g_nav_datetime[32]        = {0};
char g_sk_datetime[32]         = {0};  // SK writes here; RTC sync reads it

// Extended storage for paths beyond the gauge array (number displays, dual displays)
// Uses PSRAM allocator to keep map nodes out of iRAM.
static PsramMap<String, float> extended_sensor_values;
static PsramMap<String, String> extended_sensor_units;
static PsramMap<String, String> extended_sensor_descriptions;

// WiFi and HTTP client (static to this file)
static WebSocketsClient ws_client;
static String server_ip_str = "";
static uint16_t server_port_num = 0;
static String signalk_paths[TOTAL_PARAMS];  // Array of 10 paths
static TaskHandle_t signalk_task_handle = NULL;
static bool signalk_enabled = false;

// Set by HTTP handler (Core 1) before building/sending the config page.
// signalk_task (Core 0) sees this, disconnects the WS, and suspends reconnects
// until the flag is cleared on save — freeing the ~22KB WS receive buffer.
static volatile bool g_signalk_ws_paused = false;

// Set by resume_signalk_ws() to tell signalk_task to reconnect once iRAM > 18KB.
// signalk_task clears both this and g_signalk_ws_paused when the threshold is met.
static volatile bool g_signalk_ws_resume_when_ready = false;

// Connection health and reconnection/backoff state
static unsigned long last_message_time = 0;
static unsigned long last_reconnect_attempt = 0;
static unsigned long next_reconnect_at = 0;
static unsigned long current_backoff_ms = 5000; // start 5s
static const unsigned long RECONNECT_BASE_MS = 5000;
static const unsigned long RECONNECT_MAX_MS = 60000;
static const unsigned long MESSAGE_TIMEOUT_MS = 30000; // 30s without messages => reconnect (ping is every 15s, so 30s means 2 missed pongs)
static const unsigned long PING_INTERVAL_MS = 15000; // send periodic ping
static unsigned long last_ping_sent_ms = 0;
static bool s_ws_begun = false;  // true between ws_begin_connection() and disconnect
static unsigned long s_ws_begin_time = 0;  // millis() when ws_begin_connection() was called
static const unsigned long WS_HANDSHAKE_TIMEOUT_MS = 15000; // 15s to complete WS handshake

// Forward declaration for active-screen path collection
static std::vector<String> get_active_screen_paths(int screen_1based);

// Outgoing message queue (simple ring buffer)
static SemaphoreHandle_t ws_queue_mutex = NULL;
static const int OUTGOING_QUEUE_SIZE = 8;
static String outgoing_queue[OUTGOING_QUEUE_SIZE];
static int queue_head = 0;
static int queue_tail = 0;
static int queue_count = 0;

static bool enqueue_outgoing(const String &msg) {
    if (ws_queue_mutex == NULL) return false;
    if (xSemaphoreTake(ws_queue_mutex, pdMS_TO_TICKS(100))) {
        if (queue_count >= OUTGOING_QUEUE_SIZE) {
            // Drop oldest to make room
            queue_head = (queue_head + 1) % OUTGOING_QUEUE_SIZE;
            queue_count--;
        }
        outgoing_queue[queue_tail] = msg;
        queue_tail = (queue_tail + 1) % OUTGOING_QUEUE_SIZE;
        queue_count++;
        xSemaphoreGive(ws_queue_mutex);
        return true;
    }
    return false;
}

static void flush_outgoing() {
    if (ws_queue_mutex == NULL) return;
    if (!ws_client.isConnected()) return;
    if (!xSemaphoreTake(ws_queue_mutex, pdMS_TO_TICKS(100))) return;
    while (queue_count > 0 && ws_client.isConnected()) {
        String &m = outgoing_queue[queue_head];
        ws_client.sendTXT(m);
        queue_head = (queue_head + 1) % OUTGOING_QUEUE_SIZE;
        queue_count--;
    }
    xSemaphoreGive(ws_queue_mutex);
}

// Public enqueue wrapper (declared in header)
void enqueue_signalk_message(const String &msg) {
    if (ws_queue_mutex == NULL) return;
    enqueue_outgoing(msg);
}

// Convert dot-delimited Signal K path to REST URL form
static String build_signalk_url(const String &path) {
    String cleaned = path;
    cleaned.trim();
    cleaned.replace(".", "/");
    return String("/signalk/v1/api/vessels/self/") + cleaned;
}

static void store_sensor_value_for_path(const String& path, float value) {
    bool found_in_gauge = false;
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i].length() > 0 && signalk_paths[i].equals(path)) {
            set_sensor_value(i, value);
            found_in_gauge = true;
        }
    }

    if (!found_in_gauge && sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        extended_sensor_values[path] = value;
        xSemaphoreGive(sensor_mutex);
    }
}

static void fetch_current_value_for_path(const String& path) {
    if (path.length() == 0) return;

    HTTPClient http;
    String url = "http://" + server_ip_str + ":" + String(server_port_num) + build_signalk_url(path);

    esp_task_wdt_reset();
    http.begin(url);
    http.setTimeout(1500);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc(psram_json_allocator());
        DeserializationError err = deserializeJson(doc, payload);
        if (!err && !doc["value"].isNull()) {
            store_sensor_value_for_path(path, doc["value"].as<float>());
        }
    }

    http.end();
}

static void fetch_current_values_for_paths(const std::vector<String>& paths) {
    std::set<String> seen;
    for (const String& path : paths) {
        if (path.length() == 0 || seen.find(path) != seen.end()) continue;
        seen.insert(path);
        fetch_current_value_for_path(path);
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

// Thread-safe getter for any sensor value
float get_sensor_value(int index) {
    if (index < 0 || index >= TOTAL_PARAMS) return 0;
    
    float val = 0;
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        val = g_sensor_values[index];
        xSemaphoreGive(sensor_mutex);
    }
    return val;
}

// Thread-safe setter for any sensor value
void set_sensor_value(int index, float value) {
    if (index < 0 || index >= TOTAL_PARAMS) return;
    
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        float old = g_sensor_values[index];
        if (old != value) {
            g_sensor_values[index] = value;
        } else {
            // No change; keep as-is
        }
        xSemaphoreGive(sensor_mutex);
    }
}

// Metadata getters (thread-safe)
String get_sensor_unit(int index) {
    if (index < 0 || index >= TOTAL_PARAMS) return "";
    String unit = "";
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        unit = g_sensor_units[index];
        xSemaphoreGive(sensor_mutex);
    }
    return unit;
}

String get_sensor_description(int index) {
    if (index < 0 || index >= TOTAL_PARAMS) return "";
    String desc = "";
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        desc = g_sensor_descriptions[index];
        xSemaphoreGive(sensor_mutex);
    }
    return desc;
}

void set_sensor_metadata(int index, const char* unit, const char* description) {
    if (index < 0 || index >= TOTAL_PARAMS) return;
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        if (unit) g_sensor_units[index] = String(unit);
        if (description) g_sensor_descriptions[index] = String(description);
        xSemaphoreGive(sensor_mutex);
    }
}

// Get sensor value by path (for number and dual displays that may use non-gauge paths)
float get_sensor_value_by_path(const String& path) {
    if (path.length() == 0) return NAN;
    
    // First check if it's in the gauge paths array
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i] == path) {
            return get_sensor_value(i);
        }
    }
    
    // Check extended storage
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = extended_sensor_values.find(path);
        float val = (it != extended_sensor_values.end()) ? it->second : NAN;
        xSemaphoreGive(sensor_mutex);
        return val;
    }
    
    return NAN;
}

// Get sensor unit by path
String get_sensor_unit_by_path(const String& path) {
    if (path.length() == 0) return "";
    
    // First check gauge paths
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i] == path) {
            return get_sensor_unit(i);
        }
    }
    
    // Check extended storage
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = extended_sensor_units.find(path);
        String unit = (it != extended_sensor_units.end()) ? it->second : "";
        xSemaphoreGive(sensor_mutex);
        return unit;
    }
    
    return "";
}

// Get sensor description by path
String get_sensor_description_by_path(const String& path) {
    if (path.length() == 0) return "";
    
    // First check gauge paths
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i] == path) {
            return get_sensor_description(i);
        }
    }
    
    // Check extended storage
    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
        auto it = extended_sensor_descriptions.find(path);
        String desc = (it != extended_sensor_descriptions.end()) ? it->second : "";
        xSemaphoreGive(sensor_mutex);
        return desc;
    }
    
    return "";
}

// Fetch metadata from SignalK REST API for a specific path
static void fetch_metadata_for_path(int index, const String &path) {
    if (path.length() == 0) return;
    
    // Convert dots to slashes for REST API path
    String rest_path = path;
    rest_path.replace(".", "/");
    
    HTTPClient http;
    String url = "http://" + server_ip_str + ":" + String(server_port_num) + "/signalk/v1/api/vessels/self/" + rest_path;
    
    esp_task_wdt_reset(); // prevent WDT during HTTP fetch
    http.begin(url);
    http.setTimeout(1500); // 1.5s — fast LAN; long enough for SK, short enough to avoid WDT
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        JsonDocument doc(psram_json_allocator());
        DeserializationError err = deserializeJson(doc, payload);
        
        if (!err) {
            if (doc["meta"].is<JsonObject>()) {
                JsonObject meta = doc["meta"].as<JsonObject>();
                const char* unit = nullptr;
                const char* description = nullptr;
                
                if (meta["units"].is<const char*>()) {
                    unit = meta["units"];
                }
                if (meta["description"].is<const char*>()) {
                    description = meta["description"];
                }
                
                if (unit || description) {
                    set_sensor_metadata(index, unit, description);
                }
            } else {
            }
        } else {
            Serial.printf("[SIGNALK] JSON parse error for %s: %s\n", path.c_str(), err.c_str());
        }
    } else {
        Serial.printf("[SIGNALK] HTTP GET failed for %s: code %d\n", path.c_str(), httpCode);
    }
    
    http.end();
}

// Fetch metadata for all configured paths (gauges, number displays, dual displays)
void fetch_all_metadata() {
    // Fetch for gauge paths (stored by index)
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        if (signalk_paths[i].length() > 0) {
            esp_task_wdt_reset(); // prevent WDT across multi-path loop
            fetch_metadata_for_path(i, signalk_paths[i]);
            vTaskDelay(pdMS_TO_TICKS(50)); // Small delay between requests
        }
    }
    
    // Fetch for additional paths (number and dual displays) not in gauge slots
    std::vector<String> all_paths = get_all_signalk_paths();
    for (const String& path : all_paths) {
        if (path.length() == 0) continue;
        
        // Skip if already in gauge paths
        bool in_gauge = false;
        for (int i = 0; i < TOTAL_PARAMS; i++) {
            if (signalk_paths[i] == path) {
                in_gauge = true;
                break;
            }
        }
        
        if (!in_gauge) {
            // Fetch metadata and store in extended map
            String api_path = path;
            api_path.replace('.', '/');
            String url = "http://" + server_ip_str + ":" + String(server_port_num) + 
                         "/signalk/v1/api/vessels/self/" + api_path;
            
            esp_task_wdt_reset(); // prevent WDT across multi-path loop
            HTTPClient http;
            http.setTimeout(1500);
            http.begin(url);
            int httpCode = http.GET();
            
            if (httpCode == 200) {
                String payload = http.getString();
                JsonDocument doc(psram_json_allocator());
                DeserializationError err = deserializeJson(doc, payload);
                
                if (!err && doc["meta"].is<JsonObject>()) {
                    JsonObject meta = doc["meta"].as<JsonObject>();
                    String unit = meta["units"].is<const char*>() ? String(meta["units"].as<const char*>()) : String("");
                    String description = meta["description"].is<const char*>() ? String(meta["description"].as<const char*>()) : String("");
                    
                    if (sensor_mutex != NULL && xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(50))) {
                        if (unit.length() > 0) extended_sensor_units[path] = unit;
                        if (description.length() > 0) extended_sensor_descriptions[path] = description;
                        xSemaphoreGive(sensor_mutex);
                    }
                }
            }
            http.end();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// Initialize mutex
void init_sensor_mutex() {
    if (sensor_mutex == NULL) {
        sensor_mutex = xSemaphoreCreateMutex();
    }
}

// WebSocket event handler
static void wsEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_CONNECTED) {
        Serial.println("Signal K: WebSocket connected");
        last_message_time = millis();
        last_ping_sent_ms = last_message_time;
        // reset backoff on successful connect
        current_backoff_ms = RECONNECT_BASE_MS;
        // Subscribe only to paths for the active screen (+ background graph screens)
        // Manual string build avoids DynamicJsonDocument's 2048B iRAM alloc in the WS connect handler.
        int active_scr = ui_get_current_screen();  // 1-based
        std::vector<String> all_conn_paths = get_active_screen_paths(active_scr);
        String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
        bool first_conn = true;
        for (const String& p : all_conn_paths) {
            if (p.length() > 0) {
                if (!first_conn) out += ",";
                out += "{\"path\":\"";
                out += p;
                out += "\",\"period\":0}";
                first_conn = false;
            }
        }
        out += "]}";
        ws_client.sendTXT(out);
        // flush any queued outgoing messages (resubscribe, etc)
        flush_outgoing();
        // Pull one snapshot for the active paths so a newly changed path doesn't
        // keep showing the previously cached value until its next delta arrives.
        fetch_current_values_for_paths(all_conn_paths);
        
        return;
    }

    if (type == WStype_TEXT) {
        last_message_time = millis();

        // Parse only the pieces we actually consume from Signal K updates to
        // keep memory use low on large delta frames.
        JsonDocument filter;
        filter["updates"][0]["values"][0]["path"] = true;
        filter["updates"][0]["values"][0]["value"] = true;

        size_t doc_capacity = length + 1024;
        if (doc_capacity < 8192) doc_capacity = 8192;
        if (doc_capacity > 24576) doc_capacity = 24576;

        JsonDocument doc(psram_json_allocator());
        DeserializationError err = deserializeJson(
            doc,
            payload,
            length,
            DeserializationOption::Filter(filter)
        );
        if (err) {
            Serial.printf("[SIGNALK] JSON parse error: %s (len=%u cap=%u)\n",
                          err.c_str(), (unsigned)length, (unsigned)doc_capacity);
            return;
        }

        if (doc["updates"].is<JsonArray>()) {
            JsonArray updates = doc["updates"].as<JsonArray>();
            for (JsonVariant update : updates) {
                if (!update["values"].is<JsonArray>()) continue;
                JsonArray values = update["values"].as<JsonArray>();
                for (JsonVariant val : values) {
                    if (!val["path"].is<const char*>() || val["value"].isNull()) continue;
                    const char* path = val["path"];

                    // navigation.position arrives as a JSON object {latitude,longitude}
                    if (strcmp(path, "navigation.position") == 0) {
                        if (val["value"].is<JsonObject>()) {
                            JsonObject pos = val["value"].as<JsonObject>();
                            if (!pos["latitude"].isNull())  g_nav_latitude  = pos["latitude"].as<float>();
                            if (!pos["longitude"].isNull()) g_nav_longitude = pos["longitude"].as<float>();
                        }
                        continue;
                    }
                    // navigation.datetime arrives as an ISO-8601 string
                    if (strcmp(path, "navigation.datetime") == 0) {
                        const char* dt = val["value"].as<const char*>();
                        if (dt) {
                            strncpy(g_sk_datetime, dt, 31);
                            g_sk_datetime[31] = '\0';
                            strncpy(g_nav_datetime, dt, 31);
                            g_nav_datetime[31] = '\0';
                        }
                        continue;
                    }

                    float value = val["value"].as<float>();

                    store_sensor_value_for_path(String(path), value);
                }
            }
        }
    }
    if (type == WStype_PONG) {
        last_message_time = millis();
        last_ping_sent_ms = last_message_time;
    }

    if (type == WStype_DISCONNECTED) {
        Serial.println("[SK] WebSocket disconnected — will reconnect");
        s_ws_begun = false;  // stop calling loop() until next probe-gated reconnect
        last_ping_sent_ms = 0;
        current_backoff_ms = RECONNECT_BASE_MS;
        next_reconnect_at = millis() + RECONNECT_BASE_MS;
    }

    if (type == WStype_ERROR) {
        Serial.println("[SK] WebSocket error — will reconnect");
        s_ws_begun = false;  // stop calling loop() until next probe-gated reconnect
        last_ping_sent_ms = 0;
        current_backoff_ms = RECONNECT_BASE_MS;
        next_reconnect_at = millis() + RECONNECT_BASE_MS;
    }
}

// Helper: begin WS connection
static void ws_begin_connection() {
    // subscribe=none prevents server from firehosing all data on connect
    ws_client.begin(server_ip_str.c_str(), server_port_num, "/signalk/v1/stream?subscribe=none");
    ws_client.onEvent(wsEvent);
}

// FreeRTOS task for Signal K WebSocket updates (runs on core 0)
//
// Socket-safety design: ws_client.loop() is ONLY called when s_ws_begun is
// true (i.e. after a TCP reachability probe confirmed the server is up and
// ws_begin_connection() was called).  This prevents the library from opening
// sockets against an unreachable server, which previously exhausted LWIP's
// socket pool (errno 11) — especially after SD_MMC mount takes ~30KB iRAM
// for DMA, leaving less room for network buffers.
static void signalk_task(void *parameter) {
    Serial.println("Signal K task started (WebSocket)");
    vTaskDelay(pdMS_TO_TICKS(500));

    // Schedule the first connection attempt after a short delay
    next_reconnect_at = millis() + 2000;

    while (signalk_enabled) {
        // ---- Config UI pause ----
        if (g_signalk_ws_paused) {
            if (ws_client.isConnected() || s_ws_begun) {
                ws_client.disconnect();
                s_ws_begun = false;
                current_backoff_ms = RECONNECT_BASE_MS;
                Serial.println("[SK] Config UI active - WS disconnected to free iRAM");
            }
            if (g_signalk_ws_resume_when_ready) {
                g_signalk_ws_resume_when_ready = false;
                g_signalk_ws_paused = false;
                next_reconnect_at = millis() + 1000;
                Serial.println("[SK] WS unpaused, reconnecting in 1s");
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ---- Only pump the WS library when we have an active connection ----
        // CRITICAL: ws_client.loop() may fire wsEvent() which sets
        // last_message_time = millis(). We MUST sample `now` AFTER loop()
        // so that `now - last_message_time` doesn't underflow to ~4 billion.
        if (s_ws_begun) {
            ws_client.loop();
            flush_outgoing();
        }

        unsigned long now = millis();

        // ---- State 1: Connected and active ----
        if (s_ws_begun && ws_client.isConnected()) {
            if (last_ping_sent_ms == 0 || now - last_ping_sent_ms >= PING_INTERVAL_MS) {
                ws_client.sendPing();
                last_ping_sent_ms = now;
            }
            if (now - last_message_time >= MESSAGE_TIMEOUT_MS) {
                Serial.println("Signal K: connection idle timeout, forcing disconnect");
                ws_client.disconnect();
                s_ws_begun = false;
                last_ping_sent_ms = 0;
                unsigned int jitter = (esp_random() & 0x7FF) % 1000;
                next_reconnect_at = now + current_backoff_ms + jitter;
                last_reconnect_attempt = now;
                current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
            }
        }
        // ---- State 2: Handshake in progress (begun but not yet connected) ----
        else if (s_ws_begun && !ws_client.isConnected()) {
            // ws_client.loop() above is driving the TCP+WS handshake.
            // If it takes too long, give up and go back to probe-gated retry.
            if (now - s_ws_begin_time >= WS_HANDSHAKE_TIMEOUT_MS) {
                Serial.println("[SK] WS handshake timed out, disconnecting");
                ws_client.disconnect();
                s_ws_begun = false;
                last_ping_sent_ms = 0;
                unsigned int jitter = (esp_random() & 0x7FF) % 1000;
                next_reconnect_at = now + current_backoff_ms + jitter;
                current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
            }
        }
        // ---- State 3: Idle — wait for reconnect timer, then probe + connect ----
        else {
            if (next_reconnect_at == 0) {
                next_reconnect_at = now + current_backoff_ms;
            }
            if (now >= next_reconnect_at) {
                // Quick TCP reachability check before attempting WebSocket.
                // This prevents socket exhaustion when the server is unreachable,
                // which is critical when SD_MMC DMA reduces available iRAM.
                {
                    WiFiClient probe;
                    probe.setTimeout(2); // 2 second timeout
                    bool reachable = probe.connect(server_ip_str.c_str(), server_port_num);
                    probe.stop();
                    if (!reachable) {
                        Serial.printf("[SK] Server %s:%d unreachable, skipping reconnect\n",
                                      server_ip_str.c_str(), server_port_num);
                        last_reconnect_attempt = now;
                        unsigned int jitter = (esp_random() & 0x7FF) % 1000;
                        next_reconnect_at = now + current_backoff_ms + jitter;
                        current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        continue;
                    }
                }
                // Server is reachable — safe to open a WebSocket connection
                ws_client.disconnect(); // clean any stale state
                vTaskDelay(pdMS_TO_TICKS(100));
                ws_begin_connection();
                s_ws_begun = true;
                s_ws_begin_time = now;
                last_ping_sent_ms = 0;
                last_reconnect_attempt = now;
                unsigned int jitter = (esp_random() & 0x7FF) % 1000;
                next_reconnect_at = now + current_backoff_ms + jitter;
                current_backoff_ms = min(current_backoff_ms * 2, RECONNECT_MAX_MS);
                Serial.printf("[SK] Connection attempt started (backoff=%lums)\n", current_backoff_ms);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    Serial.println("Signal K task ended");
    vTaskDelete(NULL);
}

// Enable Signal K with WiFi credentials
void enable_signalk(const char* ssid, const char* password, const char* server_ip, uint16_t server_port) {
    if (signalk_enabled) {
        Serial.println("Signal K already enabled");
        return;
    }
    
    signalk_enabled = true;
    server_ip_str = server_ip;
    server_port_num = server_port;
    
    // Get all paths from configuration including gauges, number displays, and dual displays
    std::vector<String> all_paths = get_all_signalk_paths();
    
    // First, load the traditional gauge paths into signalk_paths array
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        signalk_paths[i] = get_signalk_path_by_index(i);
    }
    
    // Initialize mutex first
    init_sensor_mutex();
    // create ws queue mutex
    if (ws_queue_mutex == NULL) {
        ws_queue_mutex = xSemaphoreCreateMutex();
    }
    
    // WiFi should already be connected from setup_sensESP()
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Signal K: WiFi not connected, aborting");
        signalk_enabled = false;
        return;
    }

    Serial.println("Signal K: Starting WebSocket client...");
    // Do NOT call ws_begin_connection() here — the task will handle it
    // after a TCP reachability probe to prevent socket exhaustion when
    // SD_MMC DMA reduces available iRAM for network buffers.
    ws_client.setReconnectInterval(0);

    // Create task to pump connection loop
    xTaskCreatePinnedToCore(signalk_task, "SignalKWS", 8192, NULL, 3, &signalk_task_handle, 0);

    Serial.println("Signal K task created successfully");
    Serial.flush();
}

// Disable Signal K
void disable_signalk() {
    signalk_enabled = false;
    if (signalk_task_handle != NULL) {
        vTaskDelete(signalk_task_handle);
        signalk_task_handle = NULL;
    }
    ws_client.disconnect();
    s_ws_begun = false;
    Serial.println("Signal K disabled");
}

// Returns true if the WS is currently paused.
bool is_signalk_ws_paused() {
    return g_signalk_ws_paused;
}

// Pause the WebSocket connection while the config UI is open.
// Sets the pause flag and yields 300ms so signalk_task (Core 0) sees it,
// calls ws_client.disconnect(), and the ~22KB WS receive buffer is freed
// before the HTTP handler builds and sends the large config page.
void pause_signalk_ws() {
    if (!signalk_enabled) return;
    // Cancel any pending or in-flight resume so the signalk_task doesn't
    // unpause itself while we're serving config fragments.
    g_signalk_ws_resume_when_ready = false;
    g_signalk_ws_resume_pending = false;

    if (g_signalk_ws_paused) {
        // Already paused — WS is disconnected and buffers are freed.
        // Skip the 300ms wait; just print current iRAM for diagnostics.
        Serial.printf("[SK] WS already paused, iRAM now %u B\n",
                      heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return;
    }
    g_signalk_ws_paused = true;
    // 6 × 50ms = 300ms: task runs every 10ms so it sees the flag in <10ms;
    // remaining 290ms is for lwIP to actually free the TCP socket buffers.
    for (int i = 0; i < 6; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_task_wdt_reset();
    }
    Serial.printf("[SK] WS paused for config UI, iRAM now %u B\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// Set when handle_save_gauges() wants WS resumed but LVGL apply is still pending.
// The main loop calls resume_signalk_ws() after apply_all_screen_visuals() completes.
volatile bool g_signalk_ws_resume_pending = false;


// Resume the WebSocket connection after the config save completes.
// Keeps g_signalk_ws_paused=true and sets g_signalk_ws_resume_when_ready so the
// signalk_task will reconnect once iRAM > 18KB (lwIP TIME_WAIT PCBs have cleared).
void resume_signalk_ws() {
    if (!signalk_enabled) return;
    g_signalk_ws_resume_pending = false;
    // Keep paused — signalk_task will clear the pause once iRAM recovers
    g_signalk_ws_resume_when_ready = true;
    Serial.println("[SK] WS resume requested - waiting for iRAM > 18KB before reconnecting");
}

// Schedule a WS resume to happen after the next apply_all_screen_visuals() completes.
// Call this from HTTP handlers instead of resume_signalk_ws() directly, so that
// LVGL image SD reads happen while iRAM is still free (WS still paused).
void schedule_signalk_ws_resume() {
    if (!signalk_enabled) return;
    g_signalk_ws_resume_pending = true;
    Serial.println("[SK] WS resume deferred until after screen rebuild");
}

// Helper: collect paths for active screen + background graph screens
static std::vector<String> get_active_screen_paths(int screen_1based) {
    std::set<String> seen_paths;
    std::vector<String> result;
    int active_idx = screen_1based - 1;
    if (active_idx < 0) active_idx = 0;

    auto merge = [&](const std::vector<String>& src) {
        for (const String& p : src) {
            if (p.length() > 0 && seen_paths.find(p) == seen_paths.end()) {
                seen_paths.insert(p);
                result.push_back(p);
            }
        }
    };

    // Active screen paths
    merge(get_signalk_paths_for_screen(active_idx));

    // Background graph screens still need data collection
    for (int s = 0; s < NUM_SCREENS; s++) {
        if (s == active_idx) continue;
        if (screen_configs[s].display_type == DISPLAY_TYPE_GRAPH) {
            merge(get_signalk_paths_for_screen(s));
        }
    }

    // If the active screen has no configured paths, fall back to every
    // configured path so the device can still show data while the user is
    // switching screens or recovering from a blank default screen.
    if (result.empty()) {
        merge(get_all_signalk_paths());
    }

    return result;
}

// Subscribe to only the given screen's paths (+ background graph screens)
void subscribe_to_active_screen(int screen_1based) {
    std::vector<String> paths = get_active_screen_paths(screen_1based);

    // First unsubscribe from everything
    String unsub = "{\"context\":\"vessels.self\",\"unsubscribe\":[{\"path\":\"*\"}]}";
    enqueue_outgoing(unsub);

    // Then subscribe to only what we need
    String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
    bool first = true;
    for (const String& p : paths) {
        if (!first) out += ",";
        out += "{\"path\":\"";
        out += p;
        out += "\",\"period\":0}";
        first = false;
    }
    out += "]}";
    enqueue_outgoing(out);
    Serial.printf("[SK] Subscribed to %d paths for screen %d\n", (int)paths.size(), screen_1based);
    if (paths.empty()) {
        Serial.println("[SK] No Signal K paths configured yet");
    }
}

// Rebuild the subscription list from current configuration and (re)send it
// over the active WebSocket connection if connected. If the WS is not
// connected, the updated paths will be used when connection is (re)established.
void refresh_signalk_subscriptions() {
    // Reload gauge paths from configuration
    for (int i = 0; i < TOTAL_PARAMS; i++) {
        signalk_paths[i] = get_signalk_path_by_index(i);
    }

    // Subscribe to active screen paths (not all paths)
    int active = ui_get_current_screen();
    std::vector<String> all_paths = get_active_screen_paths(active);

    // Build subscription JSON manually to avoid DynamicJsonDocument allocating
    // 2048 bytes from internal iRAM on every save. DynamicJsonDocument uses malloc()
    // which draws from the internal heap; on a device with ~10 KB iRAM headroom this
    // fragments the heap and leaves the SDMMC DMA layer without a contiguous block.
    // Manual string building uses PSRAM-backed Arduino String objects instead.
    // Unsubscribe first, then subscribe to active paths only.
    String unsub = "{\"context\":\"vessels.self\",\"unsubscribe\":[{\"path\":\"*\"}]}";
    enqueue_outgoing(unsub);

    String out = "{\"context\":\"vessels.self\",\"subscribe\":[";
    bool first = true;
    for (const String& path : all_paths) {
        if (path.length() > 0) {
            if (!first) out += ",";
            out += "{\"path\":\"";
            out += path;
            out += "\",\"period\":0}";
            first = false;
        }
    }
    out += "]}";

    // Always queue — never call ws_client.sendTXT() directly here.
    // refresh_signalk_subscriptions() may be called from Core 1 (HTTP handler)
    // while signalk_task on Core 0 is inside ws_client.loop(). Calling sendTXT()
    // from two cores simultaneously is an unprotected race that crashes the device.
    // flush_outgoing() inside signalk_task will drain the queue safely from Core 0.
    enqueue_outgoing(out);
}
