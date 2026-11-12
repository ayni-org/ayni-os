"""ADR-0099: bootstrap preserves fallback/config/data and seeds IDF OTA selection."""
import binascii
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota_install as ota


def table(layout):
    return b''.join(struct.pack('<HBBII16sI', 0x50aa, kind, sub, start, size,
                                name.encode(), flags)
                    for name, (kind, sub, start, size, flags) in layout.items()).ljust(ota.SECTOR, b'\xff')


def image(size):
    raw = bytearray(size)
    raw[0], raw[23] = 0xe9, 1
    raw[80:112] = b'ayni_wifi_chat'.ljust(32, b'\x00')
    return bytes(raw)


class OtaBootstrap(unittest.TestCase):
    def test_selection_preserves_valid_old_slot_and_requires_new_confirmation(self):
        raw = ota.selection()
        self.assertEqual(len(raw), 2 * ota.SECTOR)
        for at, seq, state, slot in [(0, 3, 0, 0), (ota.SECTOR, 2, 2, 1)]:
            actual, label, actual_state, crc = struct.unpack_from('<I20sII', raw, at)
            self.assertEqual((actual, actual_state, (actual - 1) % 2), (seq, state, slot))
            self.assertEqual(crc, binascii.crc32(struct.pack('<I', seq), 0xffffffff) & 0xffffffff)
            self.assertEqual(raw[at + 32:at + ota.SECTOR], b'\xff' * (ota.SECTOR - 32))

    def test_prepare_and_install_only_write_authorized_bootstrap_regions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build, backup = root / 'build', root / 'backup'
            (build / 'partition_table').mkdir(parents=True)
            (build / 'bootloader').mkdir()
            (build / 'partition_table/partition-table.bin').write_bytes(table(ota.NEW_LAYOUT))
            (build / 'bootloader/bootloader.bin').write_bytes(b'boot')
            (build / 'ayni_wifi_chat.bin').write_bytes(image(1024))
            (build / 'sdkconfig').write_text('CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y\n')
            operations = []

            def flash(action, *args):
                operations.append((action, args))
                if action == 'read-flash':
                    start, size, path = args
                    self.assertLessEqual(size, ota.CHUNK)
                    if start == ota.TABLE:
                        raw = table(ota.OLD_LAYOUT)
                    elif start == ota.SECOND:
                        raw = image(size)
                    else:
                        raw = b'\xff' * size
                    Path(path).write_bytes(raw)

            with patch.object(ota, 'BUILD', build), patch.object(ota, 'BACKUP', backup), \
                    patch.object(ota.device, 'esptool', flash):
                ota.prepare()
                self.assertTrue(all(action == 'read-flash' for action, _ in operations))
                self.assertEqual((backup / 'audio.bin').stat().st_size, ota.FIRST_SIZE)
                self.assertEqual((backup / 'application.bin').stat().st_size, ota.SECOND_SIZE)
                ota.install()
            writes = [args for action, args in operations if action == 'write-flash']
            self.assertEqual(len(writes), 1)
            self.assertEqual(writes[0][::2], (0, ota.TABLE, ota.OTA_DATA, ota.FIRST))
            self.assertNotIn(ota.SECOND, writes[0][::2])

    def test_candidate_does_not_accept_changed_preserved_region(self):
        changed = dict(ota.NEW_LAYOUT)
        changed['ayni'] = (1, 0x83, 0x900000, 0x700000, 0)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'partition_table').mkdir()
            (root / 'partition_table/partition-table.bin').write_bytes(table(changed))
            with patch.object(ota, 'BUILD', root):
                with self.assertRaisesRegex(RuntimeError, 'layout mismatch'):
                    ota.candidate()


if __name__ == '__main__':
    unittest.main()
