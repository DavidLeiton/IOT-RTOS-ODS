#include "Arduino.h"   // REQUERIDO en PlatformIO (no en Arduino IDE)

#define PIN_LED 2      // GPIO2 = D2 en tu placa

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  Serial.println("Sistema iniciado");
}

void loop() {
  digitalWrite(PIN_LED, HIGH);
  Serial.println("LED encendido");
  delay(500);

  digitalWrite(PIN_LED, LOW);
  Serial.println("LED apagado");
  delay(500);
}