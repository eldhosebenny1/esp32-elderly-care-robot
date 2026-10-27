# Elderly Care Robot

An assistive robot built around two ESP32 boards. A wearable band monitors heart
rate, body temperature and movement, and a line-following robot that carries a
medicine compartment drives itself to the room when a reading crosses a limit or
the wearer presses nothing after a fall.

Built as a college exhibition project. The robot base and the wearable are two
separate sketches but they only make sense as a pair.

## How the two boards work together

Both boards join the same Wi-Fi network. The wearable builds a query string every
two seconds and sends it to the robot's web server:

```
http://192.168.137.85/report?bpm=78.0&temp=36.6&fall=0&pulsevalid=1&bpmmode=SIMULATED&impact=0&g=1.02&status=NORMAL
```

The robot parses the values, updates its dashboard, and decides whether to send
any help. If the wearable reports a fall, a low or high heart rate, or a
temperature outside 35-38 C, the robot sounds the buzzer, drives the line to the
room, opens the correct medicine compartment, waits 30 seconds for a handover and
then turns around and returns to its base.

Scheduled medicine visits work on their own clock. The robot keeps the last date
each slot ran in flash, so a dose only triggers once per day.

## Robot states

| State | Meaning |
| --- | --- |
| `AT BASE` | Idle, waiting for a schedule time or a health alert |
| `GOING TO ROOM` | Following the floor line outbound |
| `AT ROOM - WAIT` | Stopped at the room marker, handing over |
| `TURNING AROUND` | Pivoting for the return trip |
| `RETURNING HOME` | Following the line back to the base marker |
| `OBSTACLE STOP` | Something is closer than 20 cm, motors cut |

## Hardware

### Robot base

| Part | Purpose |
| --- | --- |
| ESP32 DevKit | Main controller, runs the web server |
| L298N motor driver | Drives the two DC gear motors |
| 2 x IR line sensor module | Follows the black tape and spots the cross markers |
| HC-SR04 ultrasonic | Obstacle detection |
| SG90 / MG996R servo | Opens the medicine compartment |
| 16x2 I2C LCD (0x27) | Local status display |
| Buzzer and LED | Alert outputs |
| Foam board chassis + 2 DC motors | Body |

### Wearable

| Part | Purpose |
| --- | --- |
| ESP32 DevKit | Reads the sensors and posts the health report |
| MAX30102 (MAX30105 library) | Pulse oximeter, used here for the raw IR reading |
| MPU6050 | Accelerometer used for fall detection |
| DS18B20 | Body temperature, needs a 4.7k pull-up on the data line |
| Li-ion cell + boost module | Portable power |

## Wiring

### Robot base

| Signal | ESP32 GPIO |
| --- | --- |
| L298N IN1 / IN2 (left motor) | 26 / 27 |
| L298N IN3 / IN4 (right motor) | 18 / 19 |
| Left IR sensor | 34 |
| Right IR sensor | 35 |
| HC-SR04 TRIG | 16 |
| HC-SR04 ECHO | 17 (through a 1k/2k divider, HC-SR04 echoes 5 V) |
| Buzzer | 25 |
| Status LED | 23 |
| Medicine servo | 13 |
| LCD SDA / SCL | 21 / 22 |

Keep the L298N ENA and ENB jumpers fitted. The IR sensors feed GPIO 34 and 35,
which are input-only pins, so they have no internal pull-ups to configure.

The line-following logic assumes the IR modules output LOW on black tape. If your
modules do the opposite, set `BLACK_IS_LOW` to `0` at the top of `robot/robot.ino`.

### Wearable

| Signal | ESP32 GPIO |
| --- | --- |
| MAX30102 SDA / SCL | 21 / 22 |
| MPU6050 SDA / SCL | 21 / 22 |
| DS18B20 data | 4 |
| DS18B20 VCC | 3V3 |
| DS18B20 GND | GND |

MAX30102 and MPU6050 share the I2C bus, so they are wired in parallel. Both sit
on 3.3 V and pull-ups are already on the breakout boards.

## Libraries

Install these through the Arduino Library Manager:

| Library | Author |
| --- | --- |
| LiquidCrystal I2C | Frank de Brabander |
| ESP32Servo | Kevin Harrington, John K. Bennett |
| DallasTemperature | Miles Burton |
| OneWire | Paul Stoffregen |
| SparkFun MAX3010x Pulse and Proximity Sensor Library | SparkFun |

`Wire`, `WiFi`, `WebServer`, `Preferences` and `time.h` ship with the ESP32 board
package, so all you need first is the board support:

```
Boards Manager URL: https://espressif.github.io/arduino-esp32/package_esp32_index.json
Board: ESP32 Dev Module
```

## Getting it running

1. Set your Wi-Fi name and password at the top of both sketches, in `WIFI_SSID` /
   `WIFI_PASSWORD` on the robot and `WIFI_NAME` / `WIFI_PASS` on the wearable.
   The committed placeholders are `YOUR_WIFI_SSID` and `YOUR_WIFI_PASSWORD`.
2. Flash `robot/robot.ino` first. Open the serial monitor at 115200 baud and note
   the address it prints, for example `Robot website IP: http://192.168.137.85`.
3. Put that address into `ROBOT_IP` in `wearable/wearable.ino` and flash the
   wearable.
4. Open the robot's IP in a browser on the same network to see the dashboard.
5. Tape a black line between the base and the room, with a wider cross marker at
   each end, and lay the robot on the start marker.

The robot also needs internet access for NTP, because the medicine schedule reads
the current time. It is set to IST (UTC+5:30) inside `setup()`.

## Dashboard

The robot serves a small page at `http://<robot-ip>/`. It shows the latest heart
rate and temperature that came in from the wearable, the current robot state, the
trip reason, the last event and the clock. The **Test medicine visit** button fires
a manual trip so you can demo without waiting for a schedule time.

See [docs/report-protocol.md](docs/report-protocol.md) for the full endpoint list.

## Tuning

| Value | File | What it does |
| --- | --- | --- |
| `TURN_180_MS` | `robot.ino` | Time the pivot takes. Raise it if the robot does not come back on the line |
| `MARKER_CONFIRM_MS` | `robot.ino` | How long both sensors must stay black before it counts as the room marker |
| `IGNORE_MARKER_MS` | `robot.ino` | Stops the base marker being read as the room marker right after a trip starts |
| `OBSTACLE_DISTANCE_CM` | `robot.ino` | Stop distance for the ultrasonic |
| `ROOM_WAIT_MS` | `robot.ino` | Handover window before it returns |
| `MED_HOURS` / `MED_MINUTES` | `robot.ino` | The three daily dose times |
| `MED_SERVO_OPEN_ANGLES` | `robot.ino` | Compartment angles for morning, noon and night |
| `LOW_BPM_LIMIT` / `HIGH_BPM_LIMIT` | `robot.ino` | Alert limits, mirrored on the wearable |

## Things worth knowing

- **The heart rate is simulated.** The MAX30102 is detected and its raw IR value
  is printed for debugging, but the displayed BPM is generated randomly in
  `updateDemoBPM()`. It was done that way for the exhibition because reliable
  pulse readings need a well-seated finger and a lot of filtering. Set
  `DEMO_RANDOM_BPM` to `false` if you want to ignore it, though nothing else
  calculates a real BPM yet.
- Fall detection is real and runs off the MPU6050: free fall under 0.6 G, then an
  impact above 2.5 G, then 2 seconds of stillness between 0.7 G and 1.3 G.
- Put the robot and wearable on the same network. A phone hotspot works fine.
- The robot only listens for reports; it never sends anything back except a short
  `REPORT RECEIVED` or `HEALTH ALERT RECEIVED` string.

## Repo layout

```
robot/robot.ino          Robot base station and web dashboard
wearable/wearable.ino    Wearable sensor band
docs/report-protocol.md  Endpoints and query parameters
```

## License

MIT, see [LICENSE](LICENSE).
