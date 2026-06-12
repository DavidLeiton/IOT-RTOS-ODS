#include "Arduino.h"

#define PIN_LED 2

// ── Tarea FreeRTOS ──────────────────────────────────────
// Toda tarea recibe un void* y NUNCA debe retornar
void tareaBlink(void *pvParameters) {
  pinMode(PIN_LED, OUTPUT);

  // xLastWakeTime guarda el momento exacto de la última ejecución
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xPeriodo = pdMS_TO_TICKS(500); // 500ms en ticks

  for (;;) {  // bucle infinito obligatorio
    digitalWrite(PIN_LED, !digitalRead(PIN_LED)); // toggle
    Serial.println(digitalRead(PIN_LED) ? "ON" : "OFF");

    // vTaskDelayUntil = determinista: garantiza período exacto
    // No importa cuánto tardó el toggle, siempre espera hasta t+500ms
    vTaskDelayUntil(&xLastWakeTime, xPeriodo);
  }
  // Si la tarea llegara acá (no debe), se elimina a sí misma
  vTaskDelete(NULL);
}

// ── Setup: solo crea tareas ──────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("FreeRTOS iniciado");

  // xTaskCreate(función, nombre, stack en bytes, parámetro, prioridad, handle)
  xTaskCreate(
    tareaBlink,   // función de la tarea
    "Blink",      // nombre para debug
    2048,         // tamaño de pila en bytes
    NULL,         // parámetro (no usamos)
    2,            // prioridad (1=baja, 5=alta)
    NULL          // handle (no necesitamos referencia)
  );
}

void loop() {
  // Vacío — FreeRTOS scheduler toma el control
  vTaskDelay(portMAX_DELAY);
}