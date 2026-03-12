#include <driver/rtc_io.h>
#include <esp_sleep.h>

constexpr uint8_t GATE_SENSOR_PIN = 33;
constexpr uint8_t SILENCE_BUTTON_PIN = 19;
constexpr uint8_t GREEN_LED_PIN = 25;
constexpr uint8_t YELLOW_LED_PIN = 26;
constexpr uint8_t RED_LED_PIN = 27;
constexpr uint8_t BUZZER_PIN = 23;

constexpr unsigned long GATE_OPEN_TIMEOUT_MS = 10000;
constexpr unsigned long SILENCE_TIME_MS = 60000;
constexpr unsigned long DEBOUNCE_TIME_MS = 35;
constexpr unsigned long EVENT_BLINK_STEP_MS = 100;
constexpr unsigned long YELLOW_BLINK_INTERVAL_MS = 1000;
constexpr unsigned long YELLOW_LED_ON_TIME_MS = 250;
constexpr unsigned long ALARM_TOGGLE_INTERVAL_MS = 250;
constexpr unsigned long SLEEP_ARM_DELAY_MS = 500;

struct DebouncedInput {
  uint8_t pin;
  bool stablePressed;
  bool lastReading;
  unsigned long lastChangeAt;
};

struct BlinkSequence {
  bool active;
  uint8_t pin;
  uint8_t togglesRemaining;
  bool state;
  unsigned long lastToggleAt;
};

DebouncedInput gateSensor{GATE_SENSOR_PIN, false, false, 0};
DebouncedInput silenceButton{SILENCE_BUTTON_PIN, false, false, 0};

BlinkSequence greenLedBlink{false, GREEN_LED_PIN, 0, false, 0};
BlinkSequence redLedBlink{false, RED_LED_PIN, 0, false, 0};

bool isGateClosed = false;
bool wasGateClosed = false;
bool isSilenceButtonLatched = false;
bool isAlarmOutputOn = false;
bool hasCountdownStarted = false;
bool isSilencePending = false;
bool shouldAlarmImmediatelyAfterSilence = false;

unsigned long gateOpenedAt = 0;
unsigned long silenceUntil = 0;
unsigned long lastYellowBlinkAt = 0;
unsigned long lastAlarmToggleAt = 0;
unsigned long gateClosedAt = 0;

bool gateIsOpen() {
  return !isGateClosed;
}

void updateDebouncedInput(DebouncedInput &input, unsigned long now) {
  bool reading = digitalRead(input.pin) == LOW;

  if (reading != input.lastReading) {
    input.lastReading = reading;
    input.lastChangeAt = now;
  }

  if ((now - input.lastChangeAt) >= DEBOUNCE_TIME_MS) {
    input.stablePressed = reading;
  }
}

void startBlinkSequence(BlinkSequence &sequence, uint8_t pin, uint8_t blinkCount, unsigned long now) {
  sequence.active = true;
  sequence.pin = pin;
  sequence.togglesRemaining = blinkCount * 2;
  sequence.state = false;
  sequence.lastToggleAt = now - EVENT_BLINK_STEP_MS;
}

bool updateBlinkSequence(BlinkSequence &sequence, unsigned long now) {
  if (!sequence.active) {
    return false;
  }

  if ((now - sequence.lastToggleAt) >= EVENT_BLINK_STEP_MS) {
    sequence.lastToggleAt = now;
    sequence.state = !sequence.state;

    if (sequence.pin != RED_LED_PIN) {
      digitalWrite(sequence.pin, sequence.state ? HIGH : LOW);
    }

    if (sequence.togglesRemaining > 0) {
      sequence.togglesRemaining--;
    }

    if (sequence.togglesRemaining == 0) {
      sequence.active = false;
      sequence.state = false;

      if (sequence.pin != RED_LED_PIN) {
        digitalWrite(sequence.pin, LOW);
      }
    }
  }

  return sequence.active;
}

void setAlarmOutput(bool enabled) {
  digitalWrite(BUZZER_PIN, enabled ? HIGH : LOW);
}

void updateRedLed(bool alarmEnabled) {
  bool redLedOn = false;

  if (alarmEnabled) {
    redLedOn = isAlarmOutputOn;
  } else if (redLedBlink.active) {
    redLedOn = redLedBlink.state;
  }

  digitalWrite(RED_LED_PIN, redLedOn ? HIGH : LOW);
}

bool isSilenced(unsigned long now) {
  return isSilencePending || now < silenceUntil;
}

bool shouldAlarm(unsigned long now) {
  if (!gateIsOpen()) {
    return false;
  }

  if (!hasCountdownStarted) {
    return false;
  }

  if (isSilenced(now)) {
    return false;
  }

  return (now - gateOpenedAt) >= GATE_OPEN_TIMEOUT_MS;
}

void enterDeepSleep() {
  Serial.println("Entering deep sleep");
  Serial.flush();

  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  rtc_gpio_pullup_en(static_cast<gpio_num_t>(GATE_SENSOR_PIN));
  rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(GATE_SENSOR_PIN));
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(GATE_SENSOR_PIN), 1);

  delay(50);
  esp_deep_sleep_start();
}

void handleGateTransitions(unsigned long now) {
  if (isGateClosed == wasGateClosed) {
    return;
  }

  if (isGateClosed) {
    hasCountdownStarted = false;
    shouldAlarmImmediatelyAfterSilence = false;
    gateClosedAt = now;
    startBlinkSequence(greenLedBlink, GREEN_LED_PIN, 2, now);
    digitalWrite(YELLOW_LED_PIN, LOW);
    setAlarmOutput(false);
    isAlarmOutputOn = false;
  } else {
    hasCountdownStarted = false;
    startBlinkSequence(redLedBlink, RED_LED_PIN, 2, now);
  }

  wasGateClosed = isGateClosed;
}

void handleSilenceButton(unsigned long now) {
  if (silenceButton.stablePressed && !isSilenceButtonLatched) {
    isSilenceButtonLatched = true;
    isSilencePending = true;
    shouldAlarmImmediatelyAfterSilence = gateIsOpen();
    silenceUntil = 0;
    setAlarmOutput(false);
    isAlarmOutputOn = false;

    if (gateIsOpen()) {
      startBlinkSequence(greenLedBlink, GREEN_LED_PIN, 3, now);
    } else {
      isSilencePending = false;
      silenceUntil = now + SILENCE_TIME_MS;
    }
  } else if (!silenceButton.stablePressed) {
    isSilenceButtonLatched = false;
  }
}

void updateYellowLed(unsigned long now, bool alarmEnabled) {
  if (!gateIsOpen() || alarmEnabled || !hasCountdownStarted || greenLedBlink.active || isSilencePending) {
    digitalWrite(YELLOW_LED_PIN, LOW);
    return;
  }

  unsigned long elapsed = now - lastYellowBlinkAt;
  if (elapsed >= YELLOW_BLINK_INTERVAL_MS) {
    lastYellowBlinkAt = now;
    elapsed = 0;
  }

  digitalWrite(YELLOW_LED_PIN, elapsed < YELLOW_LED_ON_TIME_MS ? HIGH : LOW);
}

void updateAlarm(unsigned long now) {
  bool alarmEnabled = shouldAlarm(now);

  if (!alarmEnabled) {
    setAlarmOutput(false);
    isAlarmOutputOn = false;
    updateRedLed(false);
    return;
  }

  if ((now - lastAlarmToggleAt) >= ALARM_TOGGLE_INTERVAL_MS) {
    lastAlarmToggleAt = now;
    isAlarmOutputOn = !isAlarmOutputOn;
    setAlarmOutput(isAlarmOutputOn);
  }

  updateRedLed(true);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(GATE_SENSOR_PIN, INPUT_PULLUP);
  pinMode(SILENCE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  unsigned long now = millis();
  gateSensor.lastReading = digitalRead(gateSensor.pin) == LOW;
  gateSensor.stablePressed = gateSensor.lastReading;
  gateSensor.lastChangeAt = now;

  silenceButton.lastReading = digitalRead(silenceButton.pin) == LOW;
  silenceButton.stablePressed = silenceButton.lastReading;
  silenceButton.lastChangeAt = now;

  isGateClosed = gateSensor.stablePressed;
  wasGateClosed = isGateClosed;
  gateClosedAt = now;

  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  if (wakeupCause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke up from deep sleep because the gate opened");
  } else {
    Serial.println("Normal boot");
  }

  if (gateIsOpen()) {
    hasCountdownStarted = false;
    startBlinkSequence(redLedBlink, RED_LED_PIN, 2, now);
  }
}

void loop() {
  unsigned long now = millis();

  updateDebouncedInput(gateSensor, now);
  updateDebouncedInput(silenceButton, now);

  isGateClosed = gateSensor.stablePressed;

  handleGateTransitions(now);
  handleSilenceButton(now);
  updateAlarm(now);

  bool alarmEnabled = shouldAlarm(now);
  updateYellowLed(now, alarmEnabled);

  bool greenBlinkWasActive = greenLedBlink.active;
  bool greenBlinkIsActive = updateBlinkSequence(greenLedBlink, now);
  if (greenBlinkWasActive && !greenBlinkIsActive && isSilencePending) {
    isSilencePending = false;
    silenceUntil = now + SILENCE_TIME_MS;

    if (shouldAlarmImmediatelyAfterSilence && gateIsOpen()) {
      hasCountdownStarted = true;
      gateOpenedAt = silenceUntil - GATE_OPEN_TIMEOUT_MS;
      lastYellowBlinkAt = now;
    }
  }

  if (!alarmEnabled) {
    bool redBlinkWasActive = redLedBlink.active;
    bool redBlinkIsActive = updateBlinkSequence(redLedBlink, now);

    if (redBlinkWasActive && !redBlinkIsActive && gateIsOpen() && !hasCountdownStarted) {
      gateOpenedAt = now;
      lastYellowBlinkAt = now;
      hasCountdownStarted = true;
    }
  } else {
    redLedBlink.active = false;
    redLedBlink.state = false;
  }

  updateRedLed(alarmEnabled);

  if (isGateClosed && !greenLedBlink.active && !redLedBlink.active && !alarmEnabled &&
      !isSilencePending && !silenceButton.stablePressed &&
      (now - gateClosedAt) >= SLEEP_ARM_DELAY_MS) {
    enterDeepSleep();
  }
}
