# Report protocol

Everything the two boards say to each other goes over plain HTTP on the local
network. There is no MQTT, no cloud service and no pairing step - just a web
server on the robot that the wearable polls.

## Robot endpoints

| Method | Path | Who calls it | Purpose |
| --- | --- | --- | --- |
| GET | `/` | Browser | Dashboard page |
| GET | `/data` | Dashboard JS | Robot status as JSON, polled every 1.5 s |
| GET / POST | `/report` | Wearable | Health reading, sent every 2 s |
| GET / POST | `/medicine` | Dashboard button | Kicks off a manual medicine trip |

## GET /report

The only query parameters the robot actually reads are `bpm`, `temp` and `fall`.
Everything else the wearable appends is ignored by `handleReport()` and is only
there so the serial log shows the full picture and so other endpoints can pick
them up later.

| Parameter | Type | Accepted range | Notes |
| --- | --- | --- | --- |
| `bpm` | float | 0 - 250 | Outside the range it is dropped and the old value stays |
| `temp` | float | -55 - 125 | Keeps the DS18B20 `-127` disconnect value from being stored |
| `fall` | text | `1`, `true`, `yes` | Anything else counts as no fall |
| `pulsevalid` | 1 / 0 | - | Sent, not used |
| `bpmmode` | text | `SIMULATED` | Sent, not used |
| `impact` | 1 / 0 | - | Sent, not used |
| `g` | float | 0 - 16 | Sent, not used |
| `status` | text | - | Sent, not used |

Response is plain text:

```
REPORT RECEIVED
```

or

```
HEALTH ALERT RECEIVED
```

The wearable treats any 2xx reply as a successful send and only then clears its
pending impact flag.

## Alert conditions

A report raises an alert, and therefore starts a trip if the robot is idle, when
any of these is true:

```
fall == 1
bpm > 0  and bpm < 50
bpm > 120
temp >= 38
temp > -90 and temp < 35
```

The robot only reacts the moment the condition first appears. It keeps
`healthAlertActive` so a wearable that stays in an alert state does not retrigger
a trip on every poll.

Health status text shown on the dashboard, in priority order:

| Status | Trigger |
| --- | --- |
| `FALL DETECTED` | Fall flag set |
| `HIGH TEMPERATURE WARNING` | 38 C or above |
| `LOW TEMPERATURE WARNING` | Below 35 C, ignoring the -99 "no reading yet" value |
| `LOW BPM WARNING` | Under 50 |
| `HIGH BPM WARNING` | Over 120 |
| `NORMAL` | None of the above |

## GET /data

```json
{
  "bpm": 78.0,
  "temp": 36.6,
  "fall": false,
  "health": "NORMAL",
  "robot": "AT BASE",
  "reason": "",
  "event": "Health report received",
  "time": "27-10-2026 20:14:02"
}
```

`reason` is the trip reason and is empty while the robot is idle. `time` comes
from NTP and reads `Time unavailable` until the first sync lands, which is
usually a few seconds after boot.

## POST /medicine

Starts a trip to the room with the morning compartment. Returns `409` with
`Robot is already busy.` if the robot is not idle.
