"""Send canonical ECU telemetry CSV from standard input to the serial firmware."""

import argparse
import csv
import math
import sys
import time

CHANNELS = {
    "rpm": (0, 16400),
    "boost_psi": (-35, 300),
    "iat_c": (-40, 215),
    "coolant_c": (-40, 215),
}


def encode_row(row: dict[str, str]) -> bytes:
    values = []
    for key, (low, high) in CHANNELS.items():
        if row[key] is None:
            raise ValueError(f"Missing {key} field")
        value = row[key].strip()
        if value:
            number = float(value)
            if not math.isfinite(number) or not low <= number <= high:
                raise ValueError(f"{key} is out of range")
            value = f"{number:.4f}".rstrip("0").rstrip(".")
        values.append(value)
    return ("H1," + ",".join(values) + "\n").encode("ascii")


def main() -> None:
    import serial

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="ESP32 USB serial device, e.g. COM7 or /dev/ttyUSB0")
    args = parser.parse_args()
    rows = csv.DictReader(sys.stdin)
    if rows.fieldnames is None or any(rows.fieldnames.count(key) != 1 for key in CHANNELS):
        parser.error("CSV header must include rpm,boost_psi,iat_c,coolant_c exactly once")
    try:
        with serial.Serial(args.port, 115200, write_timeout=1) as port:
            time.sleep(2)  # Opening USB UART may reset the board.
            for row in rows:
                try:
                    frame = encode_row(row)
                except (TypeError, ValueError) as error:
                    print(f"Skipped CSV line {rows.line_num}: {error}", file=sys.stderr)
                    continue
                port.write(frame)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
