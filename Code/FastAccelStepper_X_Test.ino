/*
 * FastAccelStepper - X Axis Test (continuous rotation + torque/smoothness tuned)
 * Board: ESP32 (Arduino IDE)
 *
 * Wiring:
 *   STEP -> GPIO32
 *   DIR  -> GPIO33
 *   EN   -> GPIO25 (active LOW, typical for A4988/DRV8825/TMC2209)
 *
 * Library: FastAccelStepper by gin66
 *   Install via Arduino IDE > Library Manager > search "FastAccelStepper"
 *
 * Behavior:
 *   - On boot: optional test cycle (4 revolutions forward, pause, back).
 *   - Waits for serial commands (115200 baud):
 *       f  = move +200 microsteps (short)
 *       b  = move -200 microsteps (short)
 *       F  = move +TEST_DISTANCE
 *       B  = move -TEST_DISTANCE
 *       a  = rotate CONTINUOUSLY counterclockwise (until 's')
 *       d  = rotate CONTINUOUSLY clockwise        (until 's')
 *       s  = stop (decelerate to halt)
 *       +  = increase speed by 200 microsteps/s
 *       -  = decrease speed by 200 microsteps/s
 *       h  = go back to position 0
 *       e  = force full holding torque now (EN on, hotter)
 *       x  = force driver rest now        (EN off, coolest, no torque)
 *       r  = return to automatic hold mode (see HOLD_MODE below)
 *       p  = print position + speed
 *
 * IMPORTANT - microstepping:
 *   Smoothness comes mostly from the DRIVER's microstepping setting
 *   (MS1/MS2/MS3 pins on the driver board). For DRV8825:
 *     MS1=LOW,  MS2=LOW,  MS3=LOW  -> full step
 *     MS1=HIGH, MS2=HIGH, MS3=HIGH -> 1/32 step (smoothest)
 *   MICROSTEPPING below MUST match that setting.
 *
 * DRIVER: DRV8825 -> set Vref BEFORE connecting the motor (see notes).
 */

#include <FastAccelStepper.h>

#define PIN_STEP 32
#define PIN_DIR  33
#define PIN_EN   25

// --- Motor/driver configuration ---
#define MICROSTEPPING   32      // DRV8825: set MS1=MS2=MS3=HIGH for 1/32 step
#define STEPS_PER_REV   (200 * MICROSTEPPING)   // full steps * microstepping

// Speed/accel are in MICROSTEPS per second (and per second^2).
// Start conservative, raise with '+' once it runs clean.
#define MAX_SPEED_HZ    (2 * STEPS_PER_REV)     // 2 rev/s
#define MIN_SPEED_HZ    100
#define ACCEL_STEPS_S2  (5 * STEPS_PER_REV)     // 5 rev/s^2 ramp, smooth & safe

#define TEST_DISTANCE   (4 * STEPS_PER_REV)     // 4 revolutions
#define SHORT_DISTANCE  (1 * STEPS_PER_REV)     // 1 revolution
#define SPEED_STEP      200                     // Hz added/removed by + and -

// Holding behavior when the motor is IDLE. During motion EN is always
// fully active (full torque) - this only affects standstill holding.
//
//   0 = EN off at idle          -> coolest, but NO holding torque at rest
//   1 = EN chopped at idle      -> ~IDLE_HOLD_DUTY of the heat/torque of mode 2
//                                  (DRV8825 has no real hold-current setting,
//                                   so we chop the enable pin via hardware
//                                   LEDC PWM at 22 kHz, above hearing range)
//   2 = EN always on            -> max holding torque, most heat
//
// On DRV8825 this duty trick reduces I^2R losses roughly proportionally,
// but during the OFF part of the cycle there is zero coil torque
// (only the motor's detent torque remains). Not for heavily loaded axes.
// NOTE: chopping EN at 22 kHz adds a little switching loss in the driver's
// output stage; if the DRV8825 gets hot in this mode, go HOLD_MODE 0 or 2.
#define HOLD_MODE 1
#define IDLE_HOLD_DUTY 0.4f          // 0.4 = outputs on 40% of each idle period
#define EN_PWM_FREQ 22000            // Hz, hardware LEDC (>20 kHz = inaudible for most)
#define EN_PWM_RES_BITS 10           // 10-bit duty resolution

// Boot self-test on every reset. Opening the serial port resets the ESP32,
// so this would also run every time the keyboard bridge connects.
// Set to true again only for standalone bring-up tests.
#define RUN_BOOT_TEST false

FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper *stepper = nullptr;

uint32_t currentSpeedHz = MAX_SPEED_HZ;

// Runtime hold-mode override via serial commands:
// 0 = AUTO (follow HOLD_MODE), 1 = force EN on, 2 = force EN off
uint8_t holdOverride = 0;

// --- EN pin driven by hardware LEDC PWM (pin stays on LEDC instead of GPIO) ---
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2   // assume legacy core 2.x API
#endif

const uint32_t EN_PWM_MAX = (1u << EN_PWM_RES_BITS) - 1;
float lastEnFrac = -1.0f;             // fraction of time EN is HIGH (disabled)

void ledcEnInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_EN, EN_PWM_FREQ, EN_PWM_RES_BITS);
#else
  ledcSetup(0, EN_PWM_FREQ, EN_PWM_RES_BITS);
  ledcAttachPin(PIN_EN, 0);
#endif
}

void enSetHighFraction(float frac) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  if (fabs(frac - lastEnFrac) < 0.001f) return;  // avoid redundant writes
  lastEnFrac = frac;
  uint32_t duty = (uint32_t)(frac * EN_PWM_MAX + 0.5f);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_EN, duty);
#else
  ledcWrite(0, duty);
#endif
}

// Keep EN at the level required by the current hold mode. Called in loop().
// During motion / mode 2 / forced-on: EN held LOW (outputs always on).
// During idle in mode 1: EN chopped by hardware PWM at IDLE_HOLD_DUTY.
void driveHoldPin() {
  bool moving = stepper->isRunning();

  if (holdOverride == 1 || (holdOverride == 0 && HOLD_MODE == 2) || moving) {
    enSetHighFraction(0.0f);   // outputs always ON
  } else if (holdOverride == 2 || (holdOverride == 0 && HOLD_MODE == 0)) {
    enSetHighFraction(1.0f);   // outputs always OFF
  } else {
    enSetHighFraction(1.0f - IDLE_HOLD_DUTY);  // outputs on for IDLE_HOLD_DUTY
  }
}

void printStatus() {
  Serial.print(F("Speed: "));
  Serial.print(currentSpeedHz);
  Serial.print(F(" usteps/s ("));
  Serial.print((float)currentSpeedHz / STEPS_PER_REV, 2);
  Serial.print(F(" rev/s) | Pos: "));
  Serial.println(stepper->getCurrentPosition());
}

void applySpeed() {
  stepper->setSpeedInHz(currentSpeedHz);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { /* wait for USB serial, max 3s */ }

  engine.init();

  stepper = engine.stepperConnectToPin(PIN_STEP);
  if (stepper == nullptr) {
    Serial.println(F("ERROR: could not connect STEP pin. Is the pin usable?"));
    while (true) { delay(1000); }
  }

  stepper->setDirectionPin(PIN_DIR);

  // Most drivers enable when EN is LOW -> lowToEnable = true.
  // We drive EN ourselves from driveHoldPin(), so tell the library
  // not to auto-enable/disable it.
  stepper->setEnablePin(PIN_EN, /*lowToEnable=*/true);
  stepper->setAutoEnable(false);
  // Attach EN to LEDC PWM AFTER the library has configured the pin,
  // then set the requested idle state.
  ledcEnInit();
  driveHoldPin();

  stepper->setSpeedInHz(currentSpeedHz);
  stepper->setAcceleration(ACCEL_STEPS_S2);
  stepper->setCurrentPosition(0);

  Serial.println(F("FastAccelStepper X-axis ready"));
  Serial.println(F("Commands: f b F B a d s + - h e x r p"));
  // 'e' = force EN on, 'x' = force EN off, 'r' = automatic hold mode.
  Serial.print(F("HOLD_MODE="));
  Serial.print(HOLD_MODE);
  Serial.print(F(" duty="));
  Serial.println(IDLE_HOLD_DUTY, 2);
  printStatus();

#if RUN_BOOT_TEST
  Serial.println(F("Boot test: +4 rev then -4 rev..."));
  stepper->move(TEST_DISTANCE);
  while (stepper->isRunning()) { delay(1); }
  delay(500);
  stepper->move(-TEST_DISTANCE);
  while (stepper->isRunning()) { delay(1); }
  stepper->setCurrentPosition(0);
  Serial.println(F("Boot test done."));
#endif
}

void loop() {
  driveHoldPin();

  if (Serial.available()) {
    char cmd = (char)Serial.read();

    switch (cmd) {
      case 'f': stepper->move(SHORT_DISTANCE);  break;
      case 'b': stepper->move(-SHORT_DISTANCE); break;
      case 'F': stepper->move(TEST_DISTANCE);   break;
      case 'B': stepper->move(-TEST_DISTANCE);  break;

      case 'a': stepper->runBackward();         break;  // continuous CCW
      case 'd': stepper->runForward();          break;  // continuous CW
      case 's': stepper->stopMove();            break;  // stop (decel ramp)

      case '+':
        currentSpeedHz += SPEED_STEP;
        applySpeed();
        printStatus();
        break;
      case '-':
        currentSpeedHz = (currentSpeedHz > MIN_SPEED_HZ + SPEED_STEP)
                             ? currentSpeedHz - SPEED_STEP
                             : MIN_SPEED_HZ;
        applySpeed();
        printStatus();
        break;

      case 'h': stepper->moveTo(0);             break;
      case 'e':
        holdOverride = 1;
        Serial.println(F("Hold override: EN forced ON"));
        break;
      case 'x':
        holdOverride = 2;
        Serial.println(F("Hold override: EN forced OFF"));
        break;
      case 'r':
        holdOverride = 0;
        Serial.println(F("Hold override: automatic (HOLD_MODE)"));
        break;
      case 'p': printStatus();                  break;

      case '\n':
      case '\r':
        break;
      default:
        Serial.print(F("Unknown command: "));
        Serial.println(cmd);
        break;
    }
  }
}
