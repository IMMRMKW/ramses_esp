"""Build release images and catalogues using only the Python standard library."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

MANUFACTURER = 0xFFFF
IMAGE_TYPE = 0xC601


def file_version(version: str) -> int:
    match = re.fullmatch(r"v(\d+)\.(\d+)(?:\.(\d+))?", version)
    if not match:
        raise ValueError("Release version must be vMAJOR.MINOR or vMAJOR.MINOR.PATCH")
    parts = [int(part or 0) for part in match.groups()]
    if any(part > 255 for part in parts):
        raise ValueError("Version components must be <= 255")
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8)


def package(app: Path, output: Path, version: str) -> None:
    numeric = file_version(version)
    data = app.read_bytes()
    # ESP-IDF app descriptor: fixed magic and project/version fields after image headers.
    if len(data) < 176 or data[0] != 0xE9 or data[32:36] != struct.pack("<I", 0xABCD5432):
        raise ValueError("Expected an ESP application binary, not a merged flash image")
    embedded_version = data[48:80].split(b"\0", 1)[0].decode()
    project = data[80:112].split(b"\0", 1)[0].decode()
    if embedded_version != version or project != "ramses_esp_c6":
        raise ValueError(f"Application descriptor mismatch: {project}, {embedded_version}")
    if struct.unpack_from("<H", data, 12)[0] != 13:
        raise ValueError("Application must target ESP32-C6")
    if len(data) > 0x1E0000:
        raise ValueError("Application exceeds OTA partition")
    output.mkdir(parents=True, exist_ok=True)
    header_length = 60  # Standard header plus min/max hardware version.
    payload = struct.pack("<HI", 0, len(data)) + data
    header = struct.pack(
        "<IHHHHHIH32sIHH", 0x0BEEF11E, 0x0100, header_length, 4,
        MANUFACTURER, IMAGE_TYPE, numeric, 2, b"ELECRAM RAMSES ESP32-C6", header_length + len(payload), 1, 1,
    )
    ota = header + payload
    filename = "ramses_esp_c6.ota"
    (output / filename).write_bytes(ota)
    release = f"https://github.com/IMMRMKW/ramses_esp/releases/download/{version}"
    release_url = f"https://github.com/IMMRMKW/ramses_esp/releases/tag/{version}"
    manifest = {
        "version": version, "file_version": numeric, "project": project, "target": "esp32c6",
        "url": f"{release}/ramses_esp_c6.bin", "sha256": hashlib.sha256(data).hexdigest(),
    }
    zha = {"firmwares": [{
        "file_version": numeric, "manufacturer_id": MANUFACTURER, "image_type": IMAGE_TYPE,
        "manufacturer_names": ["ELECRAM"], "model_names": ["Ramses_esp32c6"],
        "checksum": "sha3-256:" + hashlib.sha3_256(ota).hexdigest(), "file_size": len(ota),
        "binary_url": f"{release}/{filename}", "min_hardware_version": 1, "max_hardware_version": 1,
        "release_url": release_url,
    }]}
    z2m = [{
        "fileVersion": numeric, "manufacturerCode": MANUFACTURER, "imageType": IMAGE_TYPE,
        "fileSize": len(ota), "url": f"{release}/{filename}", "sha512": hashlib.sha512(ota).hexdigest(),
        "manufacturerName": ["ELECRAM"], "modelId": "Ramses_esp32c6",
        "hardwareVersionMin": 1, "hardwareVersionMax": 1,
    }]
    for name, value in (("update.json", manifest), ("zha-ota.json", zha), ("z2m-ota.json", z2m)):
        (output / name).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--app", type=Path, default=Path("build/ramses_esp_c6.bin"))
    parser.add_argument("--output", type=Path, default=Path("release"))
    args = parser.parse_args()
    package(args.app, args.output, args.version)
