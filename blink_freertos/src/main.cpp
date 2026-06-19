/*
 * ============================================================
 *  MONITOR DE CONSUMO ELECTRICO -- FIRMWARE COMPLETO
 *  ESP32 + FreeRTOS + WiFi + MQTT (ThingsBoard)
 *  Taller de Sistemas Embebidos -- TEC, I Semestre 2026
 *  ODS 7 -- Energia Asequible y No Contaminante
 * ============================================================
 *
 *  ARQUITECTURA DE TAREAS (de mayor a menor prioridad):
 *    TaskSensor    (prioridad 5) -- muestrea los 4 canales ADC
 *    TaskControl   (prioridad 4) -- logica de umbral + control del rele
 *    TaskRPC       (prioridad 3) -- validacion de comandos remotos
 *    TaskMQTT      (prioridad 2) -- publica telemetria, recibe RPC,
 *                                    maneja reconexion MQTT
 *    TaskWatchdog  (prioridad 1) -- vigila el estado de WiFi
 *
 *  NOTA SOBRE PRIORIDADES: en FreeRTOS las prioridades NO van de
 *  0 a 255 (eso es el rango de un byte/8 bits, otro concepto).
 *  Aqui el rango valido es 0-24 (configMAX_PRIORITIES=25 por
 *  defecto en ESP32). 0 es la prioridad mas baja del sistema
 *  (la usa la tarea Idle). Usamos 1-5 a proposito, dejando
 *  mucho margen libre para el futuro.
 *
 *  IMPORTANTE: el SSID de WiFi tiene que ser el de 2.4GHz.
 *  El ESP32 clasico (ESP32-D0WD-V3) NO tiene radio de 5GHz,
 *  nunca va a poder conectarse a una red "_5GHz".
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "secrets.h"   // WIFI_SSID, WIFI_PASSWORD, MQTT_SERVER, MQTT_PORT, MQTT_TOKEN

/* ------------------------------------------------------------
 *  PINES
 * ------------------------------------------------------------ */
#define GPIO_V1     34   // ADC1_CH6 -- voltaje fase 1
#define GPIO_I1     35   // ADC1_CH7 -- corriente fase 1
#define GPIO_V2     32   // ADC1_CH4 -- voltaje fase 2
#define GPIO_I2     33   // ADC1_CH5 -- corriente fase 2
#define GPIO_RELAY  26

/* ------------------------------------------------------------
 *  BITS DEL EVENT GROUP DE WIFI
 * ------------------------------------------------------------ */
#define WIFI_CONNECTED_BIT  (1 << 0)

/* ------------------------------------------------------------
 *  ESTRUCTURAS DE DATOS
 * ------------------------------------------------------------ */
typedef struct {
    uint8_t  raw_v1, raw_i1, raw_v2, raw_i2;   // 8 bits mas significativos
    float    voltage_v1, current_a1, power_w1; // fase 1
    float    voltage_v2, current_a2, power_w2; // fase 2
    float    power_total;                       // power_w1 + power_w2
    float    energy_kwh;                        // energia acumulada
    uint32_t ts_ms;
    uint8_t  relay_state;                        // 0=OFF 1=ON
} TelemetryData_t;

typedef enum { CMD_SET_RELAY, CMD_SET_THRESHOLD } CmdType_t;

typedef struct {
    CmdType_t type;
    union {
        bool  relay_on;
        float threshold_w;
    };
} ControlCmd_t;

/* ------------------------------------------------------------
 *  HANDLES GLOBALES DE FreeRTOS
 * ------------------------------------------------------------ */
SemaphoreHandle_t   xMutexADC      = NULL;  // protege el acceso al ADC1
SemaphoreHandle_t   xMutexRelay    = NULL;  // protege la escritura del GPIO del rele
QueueHandle_t       xQueueSensor   = NULL;  // depth=1, xQueueOverwrite (ultimo dato fresco)
QueueHandle_t       xQueueRPC      = NULL;  // depth=5, xQueueSend (comandos, no se pierden)
EventGroupHandle_t  xWifiEventGroup = NULL; // bandera de estado de WiFi

WiFiClient   espClient;
PubSubClient mqttClient(espClient);

volatile float g_power_threshold_w = 1500.0f;  // umbral configurable por RPC (Watts)

/* ============================================================
 *  TAREA 1 -- SENSOR  (prioridad 5, la mas alta)
 * ============================================================ */
void TaskSensor(void *pv) {
    TelemetryData_t data = {0};
    static float energia_acum = 0.0f;
    TickType_t xLast = xTaskGetTickCount();

    for (;;) {
        if (xSemaphoreTake(xMutexADC, pdMS_TO_TICKS(10)) == pdTRUE) {
            int rv1 = analogRead(GPIO_V1);
            int ri1 = analogRead(GPIO_I1);
            int rv2 = analogRead(GPIO_V2);
            int ri2 = analogRead(GPIO_I2);
            xSemaphoreGive(xMutexADC);

            data.raw_v1 = (uint8_t)(rv1 >> 4);
            data.raw_i1 = (uint8_t)(ri1 >> 4);
            data.raw_v2 = (uint8_t)(rv2 >> 4);
            data.raw_i2 = (uint8_t)(ri2 >> 4);

            data.voltage_v1 = data.raw_v1 / 255.0f * 240.0f;
            data.current_a1 = data.raw_i1 / 255.0f * 10.0f;
            data.voltage_v2 = data.raw_v2 / 255.0f * 240.0f;
            data.current_a2 = data.raw_i2 / 255.0f * 10.0f;

            data.power_w1    = data.voltage_v1 * data.current_a1;
            data.power_w2    = data.voltage_v2 * data.current_a2;
            data.power_total = data.power_w1 + data.power_w2;

            energia_acum   += data.power_total * 0.5f / 3600000.0f;
            data.energy_kwh = energia_acum;
            data.ts_ms       = millis();
            data.relay_state = digitalRead(GPIO_RELAY);

            xQueueOverwrite(xQueueSensor, &data);
        }
        vTaskDelayUntil(&xLast, pdMS_TO_TICKS(500));
    }
}

/* ============================================================
 *  TAREA 2 -- CONTROL  (prioridad 4)
 * ============================================================ */
void TaskControl(void *pv) {
    TelemetryData_t data;
    ControlCmd_t    cmd;

    bool     manual_override  = false;
    bool     manual_relay_on  = false;
    uint32_t manual_until_ms  = 0;
    const uint32_t MANUAL_HOLD_MS = 8000;  // sostiene el comando RPC 8s antes de soltar el control

    for (;;) {
        while (xQueueReceive(xQueueRPC, &cmd, 0) == pdTRUE) {
            if (cmd.type == CMD_SET_THRESHOLD) {
                g_power_threshold_w = cmd.threshold_w;
            } else if (cmd.type == CMD_SET_RELAY) {
                manual_override = true;
                manual_relay_on = cmd.relay_on;
                manual_until_ms = millis() + MANUAL_HOLD_MS;
                if (xSemaphoreTake(xMutexRelay, pdMS_TO_TICKS(50))) {
                    digitalWrite(GPIO_RELAY, manual_relay_on);
                    xSemaphoreGive(xMutexRelay);
                }
            }
        }

        if (manual_override && millis() > manual_until_ms) {
            manual_override = false;   // ventana vencida, vuelve el control automatico
        }

        if (!manual_override &&
            xQueuePeek(xQueueSensor, &data, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool debe_estar_conectada = (data.power_total <= g_power_threshold_w);
            if (xSemaphoreTake(xMutexRelay, pdMS_TO_TICKS(50))) {
                digitalWrite(GPIO_RELAY, debe_estar_conectada ? HIGH : LOW);
                xSemaphoreGive(xMutexRelay);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ============================================================
 *  TAREA 3 -- RPC  (prioridad 3) -- esqueleto reservado
 * ============================================================ */
void TaskRPC(void *pv) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

/* ------------------------------------------------------------
 *  CALLBACK MQTT -- procesa RPC entrante y responde al dashboard
 * ------------------------------------------------------------ */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    StaticJsonDocument<200> doc;
    deserializeJson(doc, payload, length);

    const char* method = doc["method"];
    if (method == nullptr) return;   // mensaje sin campo "method": lo ignoramos, evita crash
    ControlCmd_t cmd;

    // Extraer el request_id del topic (v1/devices/me/rpc/request/8 -> "8")
    String topicStr     = String(topic);
    String requestId     = topicStr.substring(topicStr.lastIndexOf('/') + 1);
    String responseTopic = "v1/devices/me/rpc/response/" + requestId;

    if (strcmp(method, "setRelay") == 0) {
        cmd.type     = CMD_SET_RELAY;
        cmd.relay_on = doc["params"];
        xQueueSend(xQueueRPC, &cmd, 0);
        mqttClient.publish(responseTopic.c_str(), "{\"result\":\"ok\"}");

    } else if (strcmp(method, "setThreshold") == 0) {
        cmd.type        = CMD_SET_THRESHOLD;
        cmd.threshold_w = doc["params"];
        xQueueSend(xQueueRPC, &cmd, 0);
        char attr[48];
        snprintf(attr, sizeof(attr), "{\"power_threshold\":%.1f}", (float)doc["params"]);
        mqttClient.publish("v1/devices/me/attributes", attr);

    } else if (strcmp(method, "getThreshold") == 0) {
        char resp[32];
        snprintf(resp, sizeof(resp), "%.1f", g_power_threshold_w);
        mqttClient.publish(responseTopic.c_str(), resp);
    }
}

/* ============================================================
 *  TAREA 4 -- MQTT  (prioridad 2)
 * ============================================================ */
void TaskMQTT(void *pv) {
    TelemetryData_t data;
    char payload[256];
    uint32_t last_publish_ms = 0;
    const uint32_t PUBLISH_INTERVAL_MS = 1000;

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);

    for (;;) {
        EventBits_t bits = xEventGroupGetBits(xWifiEventGroup);

        if (!(bits & WIFI_CONNECTED_BIT)) {
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        if (!mqttClient.connected()) {
            if (mqttClient.connect("ESP32Client", MQTT_TOKEN, "")) {
                mqttClient.subscribe("v1/devices/me/rpc/request/+");
            }
        } else {
            mqttClient.loop();   // procesa RPC entrante -- ahora corre cada 200ms, no cada 1s

            uint32_t now = millis();
            if (now - last_publish_ms >= PUBLISH_INTERVAL_MS) {
                last_publish_ms = now;
                if (xQueuePeek(xQueueSensor, &data, 0) == pdTRUE) {
                    snprintf(payload, sizeof(payload),
                        "{\"voltage_v1\":%.1f,\"current_a1\":%.2f,\"power_w1\":%.1f,"
                        "\"voltage_v2\":%.1f,\"current_a2\":%.2f,\"power_w2\":%.1f,"
                        "\"power_total\":%.1f,\"energy_kwh\":%.4f,\"relay\":%d}",
                        data.voltage_v1, data.current_a1, data.power_w1,
                        data.voltage_v2, data.current_a2, data.power_w2,
                        data.power_total, data.energy_kwh, data.relay_state);

                    mqttClient.publish("v1/devices/me/telemetry", payload);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ============================================================
 *  TAREA 5 -- WATCHDOG  (prioridad 1, la mas baja)
 * ============================================================ */
void TaskWatchdog(void *pv) {
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            xEventGroupSetBits(xWifiEventGroup, WIFI_CONNECTED_BIT);
        } else {
            xEventGroupClearBits(xWifiEventGroup, WIFI_CONNECTED_BIT);
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ============================================================
 *  SETUP
 * ============================================================ */
void setup() {
    Serial.begin(115200);
    delay(500);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    pinMode(GPIO_RELAY, OUTPUT);
    digitalWrite(GPIO_RELAY, LOW);

    xMutexADC       = xSemaphoreCreateMutex();
    xMutexRelay     = xSemaphoreCreateMutex();
    xQueueSensor    = xQueueCreate(1, sizeof(TelemetryData_t));
    xQueueRPC       = xQueueCreate(5, sizeof(ControlCmd_t));
    xWifiEventGroup = xEventGroupCreate();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    xTaskCreate(TaskSensor,   "Sensor",   4096, NULL, 5, NULL);
    xTaskCreate(TaskControl,  "Control",  3072, NULL, 4, NULL);
    xTaskCreate(TaskRPC,      "RPC",      3072, NULL, 3, NULL);
    xTaskCreate(TaskMQTT,     "MQTT",     6144, NULL, 2, NULL);
    xTaskCreate(TaskWatchdog, "Watchdog", 2048, NULL, 1, NULL);

    Serial.println("Sistema iniciado -- 5 tareas creadas");
}

void loop() {
    vTaskDelete(NULL);
}