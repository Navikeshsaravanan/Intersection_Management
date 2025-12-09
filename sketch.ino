/****************************************************
 * SMART TRAFFIC LIGHT USING ESP32 + LCD
 * - NO millis() USED
 * - waitOneSecondWithButtons() used for timing
 * - Vehicle counts taken ONLY when road is RED
 * - During GREEN:
 *      Line1: NSG/EWG base+extra (e.g., "NSG 10+20s")
 *      Line2: Countdown + other road's count (e.g., "T=30 EW=14")
 * - Pedestrian request:
 *      Next inter-road phase is pedestrian green,
 *      then only opposite road's green.
 *
 * NEW GREEN-TIME ADAPTATION (for BOTH roads):
 *   Base green = 10 s
 *   +10 s for count >= 5
 *   +20 s for count >= 10
 *   +30 s for count >= 15
 ****************************************************/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// -------- LCD CONFIG (I2C on GPIO32=SDA, GPIO33=SCL) --------
LiquidCrystal_I2C lcd(0x27, 16, 2);   // Change address to 0x3F if needed

// ============= PIN DEFINITIONS =============

// North–South LEDs
const int PIN_NS_RED    = 2;
const int PIN_NS_YELLOW = 4;
const int PIN_NS_GREEN  = 5;

// East–West LEDs
const int PIN_EW_RED    = 18;
const int PIN_EW_YELLOW = 19;
const int PIN_EW_GREEN  = 21;

// Pedestrian LEDs
const int PIN_PED_RED   = 22;
const int PIN_PED_GREEN = 23;

// Push buttons
const int PIN_BTN_NS_TRAFFIC  = 12;   // NS vehicle count (when NS red)
const int PIN_BTN_EW_TRAFFIC  = 13;   // EW vehicle count (when EW red)
const int PIN_BTN_PED_REQUEST = 14;   // Pedestrian request

// ============= CONSTANTS =============

const int YELLOW_TIME_SEC   = 3;
const int PED_TIME_SEC      = 8;

const int BASE_GREEN_SEC    = 10;     // standard base green time

// ============= PHASE ENUM =============

enum Phase {
  PHASE_NS_GREEN,
  PHASE_NS_YELLOW,
  PHASE_EW_GREEN,
  PHASE_EW_YELLOW,
  PHASE_PED_GREEN
};

Phase currentPhase = PHASE_NS_GREEN;

// ============= GLOBAL VARIABLES =============

int trafficCountNS = 0;   // vehicles waiting on NS (when NS red)
int trafficCountEW = 0;   // vehicles waiting on EW (when EW red)

bool pedRequest = false;  // latched pedestrian request

bool lastNsBtnState  = HIGH;
bool lastEwBtnState  = HIGH;
bool lastPedBtnState = HIGH;

// ============= FUNCTION DECLARATIONS =============

void readButtons();
void waitOneSecondWithButtons();

void phaseNsGreen();
void phaseNsYellow();
void phaseEwGreen();
void phaseEwYellow();
void phasePedestrianIfRequested();

void setAllVehicleRed();
void setNsGreenState();
void setNsYellowState();
void setEwGreenState();
void setEwYellowState();
void setPedestrianGreenState();

bool isNsRed();
bool isEwRed();

int  computeNsGreenSeconds();
int  computeEwGreenSeconds();

void lcdShowTwoLines(const char* line1, const char* line2);

// ============= SETUP =============

void setup() {
  // Use GPIO32 as SDA and GPIO33 as SCL for I2C
  Wire.begin(32, 33);

  lcd.init();
  lcd.backlight();
  lcdShowTwoLines("Traffic System", "Starting...");
  delay(1000);

  pinMode(PIN_NS_RED, OUTPUT);
  pinMode(PIN_NS_YELLOW, OUTPUT);
  pinMode(PIN_NS_GREEN, OUTPUT);

  pinMode(PIN_EW_RED, OUTPUT);
  pinMode(PIN_EW_YELLOW, OUTPUT);
  pinMode(PIN_EW_GREEN, OUTPUT);

  pinMode(PIN_PED_RED, OUTPUT);
  pinMode(PIN_PED_GREEN, OUTPUT);

  pinMode(PIN_BTN_NS_TRAFFIC, INPUT_PULLUP);
  pinMode(PIN_BTN_EW_TRAFFIC, INPUT_PULLUP);
  pinMode(PIN_BTN_PED_REQUEST, INPUT_PULLUP);

  setAllVehicleRed();
  digitalWrite(PIN_PED_RED, HIGH);
  digitalWrite(PIN_PED_GREEN, LOW);

  lcdShowTwoLines("Traffic System", "Ready");
  delay(1000);
}

// ============= MAIN LOOP =============

void loop() {
  // Full cycle: NS -> (Ped?) -> EW -> (Ped?) -> repeat

  phaseNsGreen();
  phaseNsYellow();
  phasePedestrianIfRequested();   // if pedRequest, MUST go now before EW green

  phaseEwGreen();
  phaseEwYellow();
  phasePedestrianIfRequested();   // if pedRequest, MUST go now before NS green
}

// ============= BUTTON HANDLING =============

void readButtons() {
  // NS vehicle count button
  bool nsBtn = digitalRead(PIN_BTN_NS_TRAFFIC);
  if (nsBtn == LOW && lastNsBtnState == HIGH) {      // just pressed
    if (isNsRed()) {                                 // NS must be red
      trafficCountNS++;                              // no upper limit

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("NS RED: Count");
      lcd.setCursor(0, 1);
      lcd.print("NS=");
      lcd.print(trafficCountNS);
    } else {
      lcdShowTwoLines("NS not RED", "No count");
    }
    delay(30);   // small debounce
  }
  lastNsBtnState = nsBtn;

  // EW vehicle count button
  bool ewBtn = digitalRead(PIN_BTN_EW_TRAFFIC);
  if (ewBtn == LOW && lastEwBtnState == HIGH) {      // just pressed
    if (isEwRed()) {                                 // EW must be red
      trafficCountEW++;                              // no upper limit

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("EW RED: Count");
      lcd.setCursor(0, 1);
      lcd.print("EW=");
      lcd.print(trafficCountEW);
    } else {
      lcdShowTwoLines("EW not RED", "No count");
    }
    delay(30);   // small debounce
  }
  lastEwBtnState = ewBtn;

  // Pedestrian request button
  bool pedBtn = digitalRead(PIN_BTN_PED_REQUEST);
  if (pedBtn == LOW && lastPedBtnState == HIGH) {    // just pressed
    pedRequest = true;                               // latched
    lcdShowTwoLines("Pedestrian Req", "Stored");
    delay(30);
  }
  lastPedBtnState = pedBtn;
}

// ============= TIMING HELPER (NO millis) =============

// 1 second = 50 × (readButtons + 20 ms)
void waitOneSecondWithButtons() {
  for (int i = 0; i < 50; i++) {
    readButtons();
    delay(20);
  }
}

// ============= PHASE FUNCTIONS =============

void phaseNsGreen() {
  currentPhase = PHASE_NS_GREEN;

  // Total green time based on NS traffic count
  int totalSecs = computeNsGreenSeconds();
  int baseSecs  = BASE_GREEN_SEC;
  int extraSecs = totalSecs - baseSecs;
  if (extraSecs < 0) extraSecs = 0;

  setNsGreenState();

  // Countdown loop (GREEN duration) – syncs exactly with signal
  for (int remaining = totalSecs; remaining > 0; remaining--) {
    lcd.clear();
    lcd.setCursor(0, 0);
    // Line 1 example: "NSG 10+20s"
    lcd.print("NSG ");
    lcd.print(baseSecs);
    lcd.print("+");
    lcd.print(extraSecs);
    lcd.print("s");

    // Line 2 example: "T=30 EW=14"
    lcd.setCursor(0, 1);
    lcd.print("T=");
    lcd.print(remaining);
    lcd.print(" EW=");
    lcd.print(trafficCountEW);   // vehicles currently waiting on EW (red)

    waitOneSecondWithButtons();  // 1-second tick with frequent button checks
  }

  // After NS green is served, reset its own old queue
  trafficCountNS = 0;
}

void phaseNsYellow() {
  currentPhase = PHASE_NS_YELLOW;

  // Yellow phase – show NSY + EW count
  for (int remaining = YELLOW_TIME_SEC; remaining > 0; remaining--) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("NSY T=");
    lcd.print(remaining);
    lcd.print("s");

    lcd.setCursor(0, 1);
    lcd.print("EW=");
    lcd.print(trafficCountEW);

    setNsYellowState();
    waitOneSecondWithButtons();
  }
}

void phaseEwGreen() {
  currentPhase = PHASE_EW_GREEN;

  int totalSecs = computeEwGreenSeconds();
  int baseSecs  = BASE_GREEN_SEC;
  int extraSecs = totalSecs - baseSecs;
  if (extraSecs < 0) extraSecs = 0;

  setEwGreenState();

  // Countdown loop for EW green
  for (int remaining = totalSecs; remaining > 0; remaining--) {
    lcd.clear();
    lcd.setCursor(0, 0);
    // Line 1: "EWG 10+20s"
    lcd.print("EWG ");
    lcd.print(baseSecs);
    lcd.print("+");
    lcd.print(extraSecs);
    lcd.print("s");

    // Line 2: "T=30 NS=7"
    lcd.setCursor(0, 1);
    lcd.print("T=");
    lcd.print(remaining);
    lcd.print(" NS=");
    lcd.print(trafficCountNS);   // vehicles currently waiting on NS (red)

    waitOneSecondWithButtons();
  }

  trafficCountEW = 0;
}

void phaseEwYellow() {
  currentPhase = PHASE_EW_YELLOW;

  // Yellow phase – show EWY + NS count
  for (int remaining = YELLOW_TIME_SEC; remaining > 0; remaining--) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("EWY T=");
    lcd.print(remaining);
    lcd.print("s");

    lcd.setCursor(0, 1);
    lcd.print("NS=");
    lcd.print(trafficCountNS);

    setEwYellowState();
    waitOneSecondWithButtons();
  }
}

void phasePedestrianIfRequested() {
  if (!pedRequest) return;   // No request → skip

  currentPhase = PHASE_PED_GREEN;

  setPedestrianGreenState();

  // Pedestrian green with countdown
  for (int remaining = PED_TIME_SEC; remaining > 0; remaining--) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PEDESTRIAN");
    lcd.setCursor(0, 1);
    lcd.print("T=");
    lcd.print(remaining);
    lcd.print(" WALK");

    waitOneSecondWithButtons();
  }

  // End pedestrian phase: all roads red, ped to red
  setAllVehicleRed();
  digitalWrite(PIN_PED_RED, HIGH);
  digitalWrite(PIN_PED_GREEN, LOW);

  lcdShowTwoLines("PEDESTRIAN", "STOP");
  delay(500);

  // This request is now fully served
  pedRequest = false;
}

// ============= RED-STATUS HELPERS =============

// NS is considered "red period" when NS is not green or yellow
bool isNsRed() {
  return (currentPhase == PHASE_EW_GREEN ||
          currentPhase == PHASE_EW_YELLOW ||
          currentPhase == PHASE_PED_GREEN);
}

// EW is considered "red period" when EW is not green or yellow
bool isEwRed() {
  return (currentPhase == PHASE_NS_GREEN ||
          currentPhase == PHASE_NS_YELLOW ||
          currentPhase == PHASE_PED_GREEN);
}

// ============= LED STATE HELPERS =============

void setAllVehicleRed() {
  digitalWrite(PIN_NS_RED, HIGH);
  digitalWrite(PIN_NS_YELLOW, LOW);
  digitalWrite(PIN_NS_GREEN, LOW);

  digitalWrite(PIN_EW_RED, HIGH);
  digitalWrite(PIN_EW_YELLOW, LOW);
  digitalWrite(PIN_EW_GREEN, LOW);
}

void setNsGreenState() {
  setAllVehicleRed();
  digitalWrite(PIN_NS_RED, LOW);
  digitalWrite(PIN_NS_GREEN, HIGH);
}

void setNsYellowState() {
  setAllVehicleRed();
  digitalWrite(PIN_NS_RED, LOW);
  digitalWrite(PIN_NS_YELLOW, HIGH);
}

void setEwGreenState() {
  setAllVehicleRed();
  digitalWrite(PIN_EW_RED, LOW);
  digitalWrite(PIN_EW_GREEN, HIGH);
}

void setEwYellowState() {
  setAllVehicleRed();
  digitalWrite(PIN_EW_RED, LOW);
  digitalWrite(PIN_EW_YELLOW, HIGH);
}

void setPedestrianGreenState() {
  setAllVehicleRed();
  digitalWrite(PIN_PED_RED, LOW);
  digitalWrite(PIN_PED_GREEN, HIGH);
}

// ============= GREEN TIME COMPUTATION (NEW LOGIC) =============

int computeNsGreenSeconds() {
  // New logic:
  // count < 5   -> 10
  // 5–9         -> 20
  // 10–14       -> 30
  // >= 15       -> 40
  int extra = 0;
  if (trafficCountNS >= 15) {
    extra = 30;
  } else if (trafficCountNS >= 10) {
    extra = 20;
  } else if (trafficCountNS >= 5) {
    extra = 10;
  } else {
    extra = 0;
  }
  return BASE_GREEN_SEC + extra;
}

int computeEwGreenSeconds() {
  int extra = 0;
  if (trafficCountEW >= 15) {
    extra = 30;
  } else if (trafficCountEW >= 10) {
    extra = 20;
  } else if (trafficCountEW >= 5) {
    extra = 10;
  } else {
    extra = 0;
  }
  return BASE_GREEN_SEC + extra;
}

// ============= LCD HELPER =============

void lcdShowTwoLines(const char* line1, const char* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

