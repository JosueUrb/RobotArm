#!/usr/bin/env python3
"""
X-axis hold-to-move keyboard controller (bridges to Arduino sketch over serial).

Controls:
    HOLD A   -> motor rotates continuously counterclockwise
    HOLD D   -> motor rotates continuously clockwise
    Release  -> motor decelerates and stops
    + / =    -> speed up
    - / _    -> speed down
    ESC      -> quit (motor stopped on exit)

Notes:
    * The Arduino sketch must already be flashed (FastAccelStepper_X_Test.ino).
    * Close the Arduino IDE Serial Monitor first — one program can own the
      port at a time.  Use the port of your ESP32 board (e.g. /dev/ttyUSB0).
    * Linux deps:        pip3 install pyserial pynput
    * Linux permissions: sudo usermod -a -G dialout $USER  (then log out/in)
    * pynput needs an X11 session (or XWayland). On pure Wayland it may not
      receive global key events.
"""

import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("Missing dependency. Install it with:  pip3 install pyserial")

try:
    from pynput import keyboard
except ImportError:
    sys.exit("Missing dependency. Install it with:  pip3 install pynput")

BAUD = 115200


def find_port():
    candidates = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
                        + glob.glob("/dev/ttyAMA*") + glob.glob("/dev/cu.*"))
    if not candidates:
        return None
    return candidates[0]


def main():
    parser = argparse.ArgumentParser(description="Hold-to-move X-axis keyboard controller")
    parser.add_argument("-p", "--port", help="Serial port (default: auto-detect /dev/ttyUSB* /dev/ttyACM*)")
    parser.add_argument("-b", "--baud", type=int, default=BAUD, help="Baud rate (default: 115200)")
    args = parser.parse_args()

    port = args.port or find_port()
    if port is None:
        sys.exit("No serial port found. Plug in the ESP32 or pass --port /dev/ttyUSB0")

    print(f"Opening {port} @ {args.baud} ...")
    ser = serial.Serial(port, args.baud, timeout=0.1)
    time.sleep(2.0)  # ESP32 resets when the port opens; let it boot

    held = set()  # currently held motion keys: 'a' and/or 'd'

    def send(cmd: str):
        ser.write(cmd.encode())
        ser.flush()

    def update_motion():
        a_held = "a" in held
        d_held = "d" in held
        if a_held and not d_held:
            send("a")
        elif d_held and not a_held:
            send("d")
        else:
            # nothing held, or both held (unsafe) -> stop
            send("s")

    def char_of(key):
        try:
            return key.char.lower()
        except AttributeError:
            return None

    def on_press(key):
        if key == keyboard.Key.esc:
            return False  # stop the listener
        ch = char_of(key)
        if ch == "a":
            if "a" not in held:
                held.add("a")
                update_motion()
        elif ch == "d":
            if "d" not in held:
                held.add("d")
                update_motion()
        elif ch in ("+", "="):
            send("+")
            print("speed +")
        elif ch in ("-", "_"):
            send("-")
            print("speed -")

    def on_release(key):
        ch = char_of(key)
        if ch in ("a", "d"):
            held.discard(ch)
            update_motion()

    print("\nX-axis controller running.")
    print("  HOLD 'a' = CCW   HOLD 'd' = CW   release = stop")
    print("  '+'/'=' speed up   '-'/'_' speed down   ESC quit\n")

    listener = keyboard.Listener(on_press=on_press, on_release=on_release)
    listener.start()
    try:
        listener.join()
    except KeyboardInterrupt:
        pass
    finally:
        print("\nStopping motor and closing port...")
        try:
            send("s")
        except Exception:
            pass
        ser.close()


if __name__ == "__main__":
    main()
