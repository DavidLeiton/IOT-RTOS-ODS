# Monitor de Consumo Eléctrico — ESP32 + FreeRTOS + ThingsBoard

> **ODS 7 — Energía Asequible y No Contaminante**  
> Taller de Sistemas Embebidos · TEC, I Semestre 2026

Sistema embebido de monitoreo y control de consumo eléctrico en tiempo real. Mide voltaje y corriente en dos fases independientes, publica telemetría a ThingsBoard vía MQTT y permite control remoto del relay de corte desde el dashboard.

---

## Características

- Muestreo simultáneo de **2 fases** (voltaje + corriente) a 2 Hz
- Publicación de telemetría a **ThingsBoard** cada 1 segundo vía MQTT
- Control remoto del **relay de corte** mediante RPC desde el dashboard
- **Umbral de potencia configurable** en tiempo real (por defecto 1500 W)
- Reconexión automática a WiFi y MQTT ante pérdida de señal
- Arquitectura multitarea con **FreeRTOS** (5 tareas concurrentes)

---

## Arquitectura de tareas

| Tarea | Prioridad | Stack | Función |
|---|---|---|---|
| `TaskSensor` | 5 (más alta) | 4096 B | Muestrea los 4 canales ADC cada 500 ms |
| `TaskControl` | 4 | 3072 B | Lógica de umbral y control del relay |
| `TaskRPC` | 3 | 3072 B | Validación de comandos remotos (skeleton) |
| `TaskMQTT` | 2 | 6144 B | Publica telemetría, recibe RPC, reconexión |
| `TaskWatchdog` | 1 (más baja) | 2048 B | Vigila el estado de WiFi cada 5 s |

La comunicación entre tareas usa primitivas FreeRTOS:
- `xQueueSensor` — queue de profundidad 1 con `xQueueOverwrite` (siempre el dato más fresco)
- `xQueueRPC` — queue de profundidad 5 con `xQueueSend` (comandos no se pierden)
- `xMutexADC` / `xMutexRelay` — mutexes para acceso exclusivo al ADC1 y al GPIO del relay
- `xWifiEventGroup` — event group con bit `WIFI_CONNECTED_BIT`

---

## Hardware

| Pin ESP32 | Señal | Descripción |
|---|---|---|
| GPIO 34 (ADC1_CH6) | V1 | Voltaje fase 1 |
| GPIO 35 (ADC1_CH7) | I1 | Corriente fase 1 |
| GPIO 32 (ADC1_CH4) | V2 | Voltaje fase 2 |
| GPIO 33 (ADC1_CH5) | I2 | Corriente fase 2 |
| GPIO 26 | RELAY | Control relay de corte |

> **Importante:** el ESP32 clásico (ESP32-D0WD-V3) solo soporta WiFi 2.4 GHz. Asegurarse de conectarse a una red 2.4 GHz.

---

## Telemetría publicada

Cada segundo se publica a `v1/devices/me/telemetry`:

```json
{
  "voltage_v1": 120.5,
  "current_a1": 2.30,
  "power_w1": 277.2,
  "voltage_v2": 119.8,
  "current_a2": 1.85,
  "power_w2": 221.6,
  "power_total": 498.8,
  "energy_kwh": 0.0014,
  "relay": 1
}
```

---

## Comandos RPC disponibles

| Método | Parámetro | Descripción |
|---|---|---|
| `setRelay` | `true` / `false` | Activa o desactiva el relay manualmente (hold 8 s) |
| `setThreshold` | `float` (Watts) | Cambia el umbral de potencia para corte automático |
| `getThreshold` | — | Devuelve el umbral actual |

---

## Configuración

### 1. Credenciales (`secrets.h`)

Crear el archivo `include/secrets.h` con las credenciales de red y ThingsBoard:

```cpp
#define WIFI_SSID      "tu_red_2.4GHz"
#define WIFI_PASSWORD  "tu_contraseña"
#define MQTT_SERVER    "thingsboard.cloud"  // o IP local
#define MQTT_PORT      1883
#define MQTT_TOKEN     "tu_token_de_dispositivo"
```

> Este archivo está en `.gitignore` para no exponer credenciales.

### 2. Puerto de carga

Ajustar el puerto en `platformio.ini` según el sistema operativo:

```ini
; Linux
upload_port = /dev/ttyUSB0
; macOS
upload_port = /dev/cu.usbserial-0001
; Windows
upload_port = COM3
```

---

## Instalación y uso

### Requisitos

- [PlatformIO](https://platformio.org/) (VSCode extension o CLI)
- ESP32 Dev Module
- Cuenta en [ThingsBoard](https://thingsboard.io/) (Cloud o self-hosted)

### Pasos

```bash
# 1. Clonar el repositorio
git clone https://github.com/DavidLeiton/IOT-RTOS-ODS.git
cd IOT-RTOS-ODS/blink_freertos

# 2. Crear el archivo de credenciales
cp include/README include/secrets.h   # y editar con tus datos

# 3. Compilar y cargar
pio run --target upload

# 4. Monitorear salida serial
pio device monitor
```

---

## Dependencias

Declaradas en `platformio.ini`, se instalan automáticamente:

- [`knolleary/PubSubClient`](https://github.com/knolleary/pubsubclient) — cliente MQTT
- [`bblanchon/ArduinoJson`](https://arduinojson.org/) — serialización/deserialización JSON

---

## Relación con los ODS

Este proyecto contribuye al **ODS 7 (Energía Asequible y No Contaminante)** al proveer una herramienta de bajo costo para:

- Monitorear el consumo energético en tiempo real
- Activar cortes automáticos al superar un umbral configurable
- Generar datos históricos de consumo para identificar ineficiencias

---

## Licencia

MIT
