#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <time.h>          // for NTP time
#include "Cart.h"          // our robot control class
#include "Website.h"       // our index_html

// ------------------------- JSON & Instruction Storage -------------------------
// We keep these mostly for compatibility with our previous structure,
// but now we FILL them from our clock-drawing code instead of WebSocket.

StaticJsonDocument<500> doc;

const int MAX_NUM_INSTRUCTIONS = 200;     // bumped up a bit for circle etc.
int instructions[MAX_NUM_INSTRUCTIONS][2];
int numInstructions = 0;                  // how many instructions are valid
int currInstruction = 0;                  // which instruction we are currently executing
bool playing = false;                     // not really used right now

// instructions[i][0]: 0 = move, 1 = rotate
// instructions[i][1]: distance in mm OR angle in degrees (integer)

// ---------------------------- Robot / Kinematics -----------------------------
// CHANGE THESE VALUES TO BEST SUIT OUR ROBOT
const int rightStepPin = 11;
const int rightDirPin  = 10;
const int leftStepPin  = 7;
const int leftDirPin   = 6;

const int MAX_SPEED   = 400;
const int ACCEL       = 400;
const int MICROSTEPS  = 1;     // microstepping mode valid values (1,2,4,8,16)
const int WHEEL_RADIUS = 40;   // radius of wheels in mm
const int AXLE_LENGTH  = 273;  // distance between wheel centers in mm

Cart drawingRobot(
  leftStepPin, leftDirPin,
  rightStepPin, rightDirPin,
  MAX_SPEED,
  MICROSTEPS, MICROSTEPS,
  WHEEL_RADIUS, AXLE_LENGTH
);

// ------------------------------- Pen Control ---------------------------------
// If our plotter has a servo or solenoid for pen up/down, define it here.
// If we do NOT have such a control, we can:
// - ignore these functions, OR
// - remove them and the calls to penUp()/penDown() below.

const int PEN_PIN = 5;          // CHANGE if needed
const int PEN_UP_LEVEL = LOW;
const int PEN_DOWN_LEVEL = HIGH;

void penUp() {
  digitalWrite(PEN_PIN, PEN_UP_LEVEL);
  delay(200); // simple settle delay; tune as needed
}

void penDown() {
  digitalWrite(PEN_PIN, PEN_DOWN_LEVEL);
  delay(200); // simple settle delay; tune as needed
}

// --------------------------- WiFi / Web Server ------------------------------

const char* ssid     = "MAKERSPACE";
const char* password = "12345678";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Simple WiFi connect helper
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(1000);
  }
  Serial.println();
  Serial.print("Connected, IP: ");
  Serial.println(WiFi.localIP());
}

// WebSocket handling is kept, but we do NOT currently use it to set instructions.
// we can extend this later if we want a mode switch between "clock" and "remote".

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    String message = (char*)data;
    Serial.println("message: " + (String)message);

    // we could parse JSON here to change mode, etc.
    // For now we just print it.
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n",
                    client->id(), client->remoteIP().toString().c_str());
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA:
      handleWebSocketMessage(arg, data, len);
      break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

void initWebSocket() {
  ws.onEvent(onEvent);
  server.addHandler(&ws);
}

// ------------------------------- NTP / Time ----------------------------------
// We'll get time from an NTP server and redraw the clock once per minute.

const char* ntpServer = "pool.ntp.org";

// Offset is seconds from UTC; set this for wer local timezone if we like.
// For example: UTC-5 = -5 * 3600 = -18000
const long gmtOffset_sec       = 0;
const int  daylightOffset_sec  = 0;

unsigned long lastRedrawMillis = 0;
const unsigned long REDRAW_INTERVAL_MS = 60UL * 1000UL;   // redraw every minute

int lastDrawnMinute = -1;
int lastDrawnHour   = -1;

// -------------------------- Instruction Helpers ------------------------------
// These functions add instructions to our instruction buffer in a safe way.

void addMoveInstruction(int distMm) {
  if (numInstructions >= MAX_NUM_INSTRUCTIONS) return;

  instructions[numInstructions][0] = 0;      // 0 = move
  instructions[numInstructions][1] = distMm; // distance in mm (we treat sign as direction)
  numInstructions++;
}

void addRotateInstruction(int angleDeg) {
  if (numInstructions >= MAX_NUM_INSTRUCTIONS) return;

  instructions[numInstructions][0] = 1;       // 1 = rotate
  instructions[numInstructions][1] = angleDeg; // angle in degrees (sign = direction)
  numInstructions++;
}

// Small helper to move OUT and BACK without using negative distances,
// in case Cart::move() doesn't like negative values.
//
// Strategy:
//  - rotate 0 (no-op, but here for conceptual clarity)
//  - move forward d
//  - pen action if needed (outside this function)
//  - rotate 180, move back d, rotate 180 to face original
//
void moveOutAndBack(int distMm) {
  // go out
  addMoveInstruction(distMm);
  // come back
  addRotateInstruction(180);
  addMoveInstruction(distMm);
  addRotateInstruction(180);
}

// --------------------------- Clock Drawing Params ----------------------------
// These define the visual size of the clock on our Buddha paper.

const int CLOCK_RADIUS_MM   = 80;  // distance from center to outer circle
const int TICK_LENGTH_MM    = 8;   // length of hour ticks
const int HAND_LENGTH_H_MM  = 45;  // hour hand length
const int HAND_LENGTH_M_MM  = 70;  // minute hand length

// VERY IMPORTANT: we must know how rotate(angle) behaves.
//
// Assumption for this sketch (adjust if our Cart is opposite):
//  - rotate(+angle) turns COUNTER-CLOCKWISE
//  - rotate(-angle) turns CLOCKWISE
//
// For a typical "math-style" polar angle where
//   0 degrees = "straight ahead" (12 o'clock),
//   positive angles turn to the LEFT (CCW),
// this matches the usual robot convention.

// Draw a circle around the center using a polygon approximation.
void drawCircle(int radiusMm) {
  // We'll approximate the circle with a regular polygon.
  // More segments = smoother but more moves.
  const int SEGMENTS = 36;             // 10 degrees per segment
  const float anglePerSeg = 360.0 / SEGMENTS;

  // Circumference of circle
  const float circumference = 2.0 * 3.1415926 * radiusMm;
  const float segLength = circumference / SEGMENTS;

  // We assume we start at the CENTER, facing "12 o'clock".
  // 1) Move to the top of the circle
  penUp();
  addMoveInstruction(radiusMm);  // go from center to top of circle
  penDown();

  // 2) Go around the circle as small straight segments.
  // We will:
  //    - rotate half of the internal angle (tangent-ish)
  //    - move forward a small chord
  //    - rotate the other half to line up for next
  //
  // This is a hacky but simple way to get a tangent-like path.
  for (int i = 0; i < SEGMENTS; i++) {
    addRotateInstruction((int)round(anglePerSeg / 2.0));      // turn left a bit
    addMoveInstruction((int)round(segLength));                // forward
    addRotateInstruction((int)round(anglePerSeg / 2.0));      // line up
  }

  // 3) Lift pen and return approximately to the center,
  // by going straight back inward and restoring orientation.
  penUp();
  addRotateInstruction(180);      // turn around
  addMoveInstruction(radiusMm);   // back to center
  addRotateInstruction(180);      // face "12 o'clock" again
}

// Draw 12 hour ticks around the circle. We use a simple "fan" approach:
// for each hour h:
//   - rotate to angle for h,
//   - move from center to near the edge,
//   - draw a short line outwards,
//   - move back to center,
//   - rotate back.
//
void drawHourTicks(int radiusMm, int tickLenMm) {
  // We'll consider 0° as "12 o'clock", increasing CCW.
  for (int h = 0; h < 12; h++) {
    int angleDeg = h * 30;  // 360/12 = 30° between hour marks

    // Rotate to the hour direction from our reference "12 o'clock".
    // (We always return to this reference after each tick.)
    addRotateInstruction(angleDeg);

    // Move outward so that drawing the tick reaches the circle.
    penUp();
    addMoveInstruction(radiusMm - tickLenMm);
    penDown();
    addMoveInstruction(tickLenMm);  // draw tick outward
    penUp();

    // Move back to center
    addRotateInstruction(180);
    addMoveInstruction(radiusMm);
    addRotateInstruction(180);

    // Rotate back to "12 o'clock" reference
    addRotateInstruction(-angleDeg);
  }
}

// Draw a single hand (hour or minute).
//  angleDeg: 0 = "12 o'clock", positive CCW
//  lengthMm: how long the hand is.
void drawHand(int angleDeg, int lengthMm) {
  // Rotate from "12 o'clock" to desired angle
  addRotateInstruction(angleDeg);

  // Draw from center outward
  penUp();
  // (We are already at center)
  penDown();
  addMoveInstruction(lengthMm);
  penUp();

  // Move back to center (reverse)
  addRotateInstruction(180);
  addMoveInstruction(lengthMm);
  addRotateInstruction(180);

  // Rotate back so we face "12 o'clock" again
  addRotateInstruction(-angleDeg);
}

// ------------------- High-level: Generate Clock Instructions -----------------
//
// This function wipes the current instructions and fills them so that
// the robot draws a clock showing the given hour:minute.
//
void generateClockForTime(int hour, int minute) {
  // Normalize hour from 24h to 12h
  int h12 = hour % 12;

  // Clear instruction list
  numInstructions = 0;
  currInstruction = 0;

  // Reset robot internal state if needed
  drawingRobot.resetMotors();

  // Optional: assume we are physically at the clock center
  // and facing "12 o'clock" at the beginning of every redraw.

  // 1) Outer circle
  drawCircle(CLOCK_RADIUS_MM);

  // 2) Hour ticks (instead of full numbers; easier and faster)
  drawHourTicks(CLOCK_RADIUS_MM, TICK_LENGTH_MM);

  // 3) Compute hand angles in degrees
  //
  //  - minute hand: 360° over 60 min = 6° per minute
  //  - hour hand:   each hour is 30°, plus 0.5° per minute
  float minuteAngle = minute * 6.0;                   // [0, 360)
  float hourAngle   = (h12 * 30.0) + (minute * 0.5); // [0, 360) within 12h

  int minuteAngleInt = (int)round(minuteAngle);
  int hourAngleInt   = (int)round(hourAngle);

  // 4) Draw hands
  drawHand(hourAngleInt, HAND_LENGTH_H_MM);
  drawHand(minuteAngleInt, HAND_LENGTH_M_MM);

  Serial.print("Generated instructions: ");
  Serial.println(numInstructions);
}

// ------------------------- Arduino Setup / Loop ------------------------------

void setup() {
  Serial.begin(115200);

  // Pen output
  pinMode(PEN_PIN, OUTPUT);
  penUp();

  // WiFi + Web
  initWiFi();
  initWebSocket();

  // Root page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("HTTP GET /");
    request->send(200, "text/html", index_html);
  });

  server.begin();

  // Motor setup
  drawingRobot.setupMotors();

  // NTP time configuration
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Waiting for NTP time...");

  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time, retrying...");
    delay(1000);
  }
  Serial.println("Time synchronized.");

  lastRedrawMillis = millis();

  // Draw initial time immediately
  int currHour = timeinfo.tm_hour;
  int currMin  = timeinfo.tm_min;
  lastDrawnHour   = currHour;
  lastDrawnMinute = currMin;

  Serial.printf("Initial clock draw for %02d:%02d\n", currHour, currMin);
  generateClockForTime(currHour, currMin);
}

void loop() {
  unsigned long nowMs = millis();

  // 1) Periodically check current time and regenerate clock once per minute.
  if (nowMs - lastRedrawMillis >= REDRAW_INTERVAL_MS) {
    lastRedrawMillis = nowMs;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      int currHour = timeinfo.tm_hour;
      int currMin  = timeinfo.tm_min;

      if (currMin != lastDrawnMinute || currHour != lastDrawnHour) {
        Serial.printf("Redrawing clock for %02d:%02d\n", currHour, currMin);
        lastDrawnMinute = currMin;
        lastDrawnHour   = currHour;

        generateClockForTime(currHour, currMin);
      }
    } else {
      Serial.println("Failed to get current time.");
    }
  }

  // 2) Execute the motion instructions one by one, just like Bobby's original code.
  if (drawingRobot.isDone() && currInstruction < numInstructions) {
    int type  = instructions[currInstruction][0];
    int value = instructions[currInstruction][1];

    if (type == 0) {
      // Move instruction
      drawingRobot.move(value);
    } else {
      // Rotate instruction
      drawingRobot.rotate(value);
    }

    currInstruction++;
  }

  // Let the robot's internal stepper logic run
  drawingRobot.run();

  // Keep WebSocket clients maintained
  ws.cleanupClients();
}