/*
 * AI-POWERED SMART CANE - DUAL-MODE CONTROLLER (3-sensor build)
 *
 * Mode 1: Standalone autonomous - forward ultrasonic adaptive braking.
 * Mode 2: AI-guided (PictoBlox / laptop over HC-05 Bluetooth).
 *   Steering protocol: receives single characters F / L / R / S.
 *   Distance query:    on 'U', replies with LEFT distance, then a short
 *                      pause, then RIGHT distance, so the AI program can
 *                      compare both sides and choose the safer turn.
 *
 * Local safety always overrides the AI: if the forward sensor sees an
 * obstacle closer than SAFETY_STOP_DIST, the motors stop no matter what
 * command Bluetooth sent.
 */

#include <SoftwareSerial.h>

// --- SYSTEM CONSTANTS ---
const int SAFETY_STOP_DIST    = 20;   // cm -> emergency halt
const int SLOWDOWN_START_DIST = 100;  // cm -> begin proportional slowdown
const int BUZZER_PIN          = 13;   // status buzzer / LED

// --- BLUETOOTH (HC-05) ---
// Arduino D11 (RX) <- HC-05 TX  |  Arduino D12 (TX) -> HC-05 RX
SoftwareSerial BTSerial(11, 12);

// --- MOTOR PINS (L298N) ---
const int ENA = 3;   // Left motor speed (PWM)
const int IN1 = 4;   // Left motor direction 1
const int IN2 = 5;   // Left motor direction 2
const int IN3 = 7;   // Right motor direction 1
const int IN4 = 8;   // Right motor direction 2
const int ENB = 9;   // Right motor speed (PWM)

// --- ULTRASONIC SENSORS (3x HC-SR04) ---
// IMPORTANT: the forward pins match the original build (A0/A1).
// Make sure the LEFT and RIGHT sensors are plugged into exactly these
// four pins before flashing (or edit the four values to match the cane).
const int TRIG_F = A0;  // forward sensor
const int ECHO_F = A1;
const int TRIG_L = A2;  // left sensor
const int ECHO_L = A3;
const int TRIG_R = A4;  // right sensor
const int ECHO_R = A5;

// --- STATE VARIABLES ---
char currentCmd = 'F';        // default: move forward in autonomous mode
int  currentSpeed = 0;
bool bluetoothActive = false;
unsigned long lastBtTime = 0;

void setup() {
  BTSerial.begin(9600);
  Serial.begin(9600);

  pinMode(ENA, OUTPUT); pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(ENB, OUTPUT); pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  pinMode(TRIG_F, OUTPUT); pinMode(ECHO_F, INPUT);
  pinMode(TRIG_L, OUTPUT); pinMode(ECHO_L, INPUT);
  pinMode(TRIG_R, OUTPUT); pinMode(ECHO_R, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  // System-initialized tone
  digitalWrite(BUZZER_PIN, HIGH);
  delay(300);
  digitalWrite(BUZZER_PIN, LOW);

  Serial.println("=========================================");
  Serial.println(" SYSTEM RUNNING: DUAL-MODE SMART CANE v2 ");
  Serial.println("=========================================");
  stopMotors();
}

void loop() {
  // 1. Bluetooth command handling (AI / app control)
  if (BTSerial.available()) {
    char incoming = BTSerial.read();

    if (incoming == 'F' || incoming == 'L' || incoming == 'R' || incoming == 'S') {
      currentCmd = incoming;
      bluetoothActive = true;
      lastBtTime = millis();
      Serial.print("Mode: [AI/BT] | Command received: ");
      Serial.println(currentCmd);

    } else if (incoming == 'U') {
      // AI asks for side distances to decide the turn direction.
      // A timeout (999) means "nothing in range", which reads as clear.
      long dl = readDistance(TRIG_L, ECHO_L);
      long dr = readDistance(TRIG_R, ECHO_R);
      BTSerial.println(dl);   // PictoBlox stores this as leftDist
      delay(150);             // gap so the two values arrive as two reads
      BTSerial.println(dr);   // PictoBlox stores this as RightDist
      bluetoothActive = true;
      lastBtTime = millis();
      Serial.print("U-request | Left: "); Serial.print(dl);
      Serial.print(" cm | Right: ");      Serial.println(dr);
    }
  }

  // Watchdog: revert to autonomous if Bluetooth is idle for 5 seconds
  if (bluetoothActive && (millis() - lastBtTime > 5000)) {
    bluetoothActive = false;
    currentCmd = 'F';
    Serial.println("Mode: [AUTONOMOUS] | BT idle. Reverting to auto-safety.");
  }

  // 2. Forward-sensor adaptive braking (always overrides the AI)
  long objDist = readDistance(TRIG_F, ECHO_F);

  if (objDist <= 0 || objDist == 999) {
    currentSpeed = 0;                    // sensor error / timeout -> stop
    digitalWrite(BUZZER_PIN, HIGH);
  } else if (objDist < SAFETY_STOP_DIST) {
    currentSpeed = 0;                    // EMERGENCY STOP
    digitalWrite(BUZZER_PIN, HIGH);
  } else if (objDist < SLOWDOWN_START_DIST) {
    currentSpeed = map(objDist, SAFETY_STOP_DIST, SLOWDOWN_START_DIST, 110, 255);
    digitalWrite(BUZZER_PIN, LOW);
  } else {
    currentSpeed = 255;                  // path clear
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Telemetry to the USB serial monitor
  Serial.print("Distance: ");
  if (objDist == 999 || objDist <= 0) Serial.print("TIMEOUT/ERROR");
  else { Serial.print(objDist); Serial.print(" cm"); }
  Serial.print(" | Mode: ");
  Serial.print(bluetoothActive ? "AI-BT" : "STANDALONE");
  Serial.print(" | Cmd: ");
  Serial.print(currentCmd);
  Serial.print(" | Speed: ");
  Serial.println(currentSpeed);

  // 3. Drive execution
  if (currentSpeed == 0 || currentCmd == 'S') {
    stopMotors();
  } else {
    moveCane(currentCmd, currentSpeed);
  }

  delay(100);
}

// Fires one sonic pulse on the given sensor and returns distance in cm.
// 30 ms timeout (~5 m range); returns 999 when no echo comes back.
long readDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return 999;
  return duration * 0.034 / 2;
}

void stopMotors() {
  analogWrite(ENA, 0); analogWrite(ENB, 0);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void moveCane(char directionChar, int targetSpeed) {
  int leftSpeed  = targetSpeed;
  int rightSpeed = targetSpeed;

  if (directionChar == 'L') {            // pivot left
    digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);   // left motor reverse
    digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);    // right motor forward
    leftSpeed  = targetSpeed * 0.75;
    rightSpeed = targetSpeed * 0.75;
  } else if (directionChar == 'R') {     // pivot right
    digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);    // left motor forward
    digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);   // right motor reverse
    leftSpeed  = targetSpeed * 0.75;
    rightSpeed = targetSpeed * 0.75;
  } else if (directionChar == 'F') {     // straight forward
    digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
    digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  }

  analogWrite(ENA, leftSpeed);
  analogWrite(ENB, rightSpeed);
}
