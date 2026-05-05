#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[mini] v0.1.0 boot — placeholder firmware (no logic wired yet)");
}

void loop() {
    delay(1000);
}
