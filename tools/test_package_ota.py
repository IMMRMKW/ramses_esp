import json
from pathlib import Path
import struct
import tempfile
import unittest

from package_ota import file_version, package


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / "app.bin"
        self.data = bytearray(512)
        self.data[0] = 0xE9
        struct.pack_into("<H", self.data, 12, 13)
        struct.pack_into("<I", self.data, 32, 0xABCD5432)
        self.data[48:54] = b"v0.5.0"
        self.data[80:93] = b"ramses_esp_c6"
        self.app.write_bytes(self.data)

    def test_header_payload_and_catalogues(self):
        package(self.app, self.root, "v0.5.0")
        ota = (self.root / "ramses_esp_c6.ota").read_bytes()
        self.assertEqual(struct.unpack_from("<I", ota)[0], 0x0BEEF11E)
        self.assertEqual(struct.unpack_from("<H", ota, 6)[0], 60)
        self.assertEqual(struct.unpack_from("<I", ota, 52)[0], len(ota))
        self.assertEqual(ota[66:], self.data)
        zha = json.loads((self.root / "zha-ota.json").read_text())["firmwares"][0]
        z2m = json.loads((self.root / "z2m-ota.json").read_text())[0]
        self.assertEqual(zha["file_version"], z2m["fileVersion"])
        self.assertEqual(zha["file_size"], len(ota))
        self.assertEqual(file_version("v0.5"), file_version("v0.5.0"))
        self.assertGreater(file_version("v1.0"), file_version("v0.255.255"))

    def test_reject_version_mismatch(self):
        with self.assertRaises(ValueError):
            package(self.app, self.root, "v0.6")

    def test_reject_merged_or_wrong_target(self):
        for offset in (0, 12, 32):
            data = self.data.copy()
            data[offset] = 0
            self.app.write_bytes(data)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                package(self.app, self.root, "v0.5.0")

    def test_reject_nonrelease_versions(self):
        for version in ("main", "v1.2.3-dirty", "v256.0", "v1.2/other"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                file_version(version)


if __name__ == "__main__":
    unittest.main()
