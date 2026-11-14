# heirowECU ESP32 Wi-Fi bridge

Stream RPM, boost, intake air temperature, and coolant temperature to an Android-powered radio or a Linux display. The ESP32 hosts a private Wi-Fi network with a browser dashboard and a read-only JSON endpoint. The dashboard and telemetry API both work locally without an internet connection.

## Hardware

| PlatformIO target | Board | UART RX | UART TX |
| --- | --- | --- | --- |
| `esp32` | ESP32 Dev Module, ESP32-WROOM | GPIO 16 | GPIO 17 |
| `esp32s3` | ESP32-S3-DevKitC-1-N8 | GPIO 18 | GPIO 17 |

Use a wired ELM327-compatible OBD-II adapter with an accessible UART and the vehicle protocol your ECU supports. The firmware uses ELM automatic protocol selection for standards-compliant CAN, ISO 9141-2, and ISO 14230 KWP2000 OBD diagnostics. The adapter handles the vehicle connection. Connect adapter UART TX to ESP32 RX and adapter UART RX to ESP32 TX, with the signal reference and 3.3 V logic levels specified by the adapter. A TTL level shifter or an RS-232 interface may be required. Keep vehicle battery, K-line and CAN voltages off ESP32 pins. Use a properly regulated supply. A USB K+DCAN cable requires a USB host and its own protocol support, which this UART firmware does not provide.

Only OBD Mode 01 live-data requests are sent. The controller must expose the requested generic PIDs. Some ME7.5 software exposes more channels through a dedicated logger than through generic OBD. In that case, the USB serial feed below can supply values from a logger with the matching ECU definitions. Real vehicle compatibility and performance vary by ECU, adapter, and bus. The shipped firmware is an auxiliary display.

## Flash and connect

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html), then run these from the repository root. Replace `esp32` with `esp32s3` for the corresponding board.

```sh
pio run -e esp32
pio run -e esp32 -t upload
pio device monitor --baud 115200
```

Reset the board while the serial monitor is open to read the SSID and unique WPA2 password. The bridge stores the generated password in local ESP32 flash. The ELM UART defaults to 38,400 baud. Set `ELM_BAUD` in your local build flags if the adapter requires a different baud rate.

- **Android radio or phone**. Join the bridge Wi-Fi and choose the ESP32 source in [heirowECU Live](https://socketjack.com/heirowECU/download/android). Use the address `192.168.4.1`. You can also open the dashboard URL in a browser on the radio.
- **Linux cluster**. Join the same Wi-Fi and open [the dashboard](http://192.168.4.1/) in a browser. On Raspberry Pi OS or another Linux installation with Chromium, `chromium --kiosk http://192.168.4.1/` opens it full-screen.
- **Bench demo**. Build and flash `esp32-demo` or `esp32s3-demo`. The firmware uses an SSID beginning with `heirowECU-DEMO-`, labels its simulated telemetry, and runs without an OBD adapter.

## Live data contract

`GET http://192.168.4.1/v1/telemetry` returns HTTP 204 when no usable readings are fresh. Otherwise, HTTP 200 returns an example like this:

```json
{
  "version": 1,
  "sequence": 42,
  "values": {
    "rpm": 2850,
    "boost_psi": 9.4,
    "iat_c": 34,
    "coolant_c": 91
  }
}
```

Both responses include `Cache-Control: no-store`. Unavailable channels are omitted. The device advances the sequence on a fresh sample or change in available channels. Reconnecting or rebooting can reset the sequence. Demo firmware adds `"demo": true`.

| Field | Source |
| --- | --- |
| `rpm` | PID `0C`, engine speed |
| `boost_psi` | PID `0B` absolute manifold pressure minus PID `33` barometric pressure, converted from kPa to gauge psi |
| `iat_c` | PID `0F`, intake air temperature in °C |
| `coolant_c` | PID `05`, engine coolant temperature in °C |

Each reading expires after 2.5 seconds without an update. Barometric pressure expires after 10 seconds. Missing or expired MAP or barometric pressure means unavailable boost. Negative boost is manifold vacuum. The application does not substitute a sea-level pressure or a boost target for measured data.

## Feed a dedicated logger over USB serial

Flash `esp32-serial` or `esp32s3-serial` instead of the OBD build to use a dedicated external logger as the source. Send line-delimited ASCII at 115,200 baud to the board’s USB serial port:

```text
H1,2850,9.4,34,91
```

Fields are RPM, **gauge** psi, IAT °C, and coolant °C, in order. Leave a field empty when its value is unknown. Each accepted line replaces the current set of readings. Send another line at each live sample. Readings expire after 2.5 seconds without data. The serial build reads values from USB. Close the serial monitor before starting your logger.

For loggers that produce a live CSV stream, install `pyserial==3.5` and pipe canonical columns `rpm,boost_psi,iat_c,coolant_c` into the included bridge.

```sh
python -m pip install pyserial==3.5
python tools/serial-feed.py --port COM7
```

Paste the CSV header and test rows on standard input for a bench test. A live pipeline should emit the header once and flush every sample. Convert logger fields into the documented canonical units upstream.

## Developer checks

Compile the portable decoder independently of Arduino:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -I include firmware/Telemetry.cpp tests/telemetry_checks.cpp -o /tmp/heirowecu-telemetry-test
/tmp/heirowecu-telemetry-test
```

The dashboard browser check uses Playwright and a local telemetry fixture. From a development checkout, run `npm install --no-save playwright`, `npx playwright install chromium`, and `node tests/dashboard.cjs`.
