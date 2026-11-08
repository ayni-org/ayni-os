"""ADR-0097: normal deployment preserves existing regions and bounds all writes."""
from pathlib import Path
import importlib.util
import struct
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / 'device.py'
spec = importlib.util.spec_from_file_location('wifi_device', SOURCE)
device = importlib.util.module_from_spec(spec)
spec.loader.exec_module(device)


def table(app_size):
    entries = [(b'audio', 1, 0x40, 0x100000, 0x300000),
               (b'demo_cfg', 1, 2, 0x400000, 0x6000),
               (b'factory', 0, 0, device.APP_OFFSET, app_size),
               (b'ayni', 1, 0x83, device.VOLUME_OFFSET, device.VOLUME_SIZE)]
    raw = b''.join(struct.pack('<HBBII16sI', 0x50aa, kind, sub, offset, size, name, 0)
                   for name, kind, sub, offset, size in entries)
    return raw.ljust(device.TABLE_SIZE, b'\xff')


class WifiDeployment(unittest.TestCase):
    def test_first_deployment_only_reads_and_preserves_previous_app(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / 'new-table.bin'
            target.write_bytes(table(device.WIFI_APP_SIZE))
            backup = root / 'backup'
            backup.mkdir()
            operations = []

            def read(action, offset, size, path):
                operations.append((action, offset, size))
                self.assertEqual(action, 'read-flash')
                Path(path).write_bytes(table(device.APP_SIZE) if offset == device.TABLE_OFFSET
                                       else b'\xff' * size)

            with patch.object(device, 'BACKUP', backup), patch.object(device, 'esptool', read):
                device.wifi_preflight(target)
            self.assertEqual((backup / 'pre-wifi-application.bin').stat().st_size, device.APP_SIZE)
            self.assertEqual((backup / 'pre-wifi-partition.bin').read_bytes(), table(device.APP_SIZE))
            self.assertTrue(all(offset < device.VOLUME_OFFSET for _, offset, _ in operations))
            new_reads = [size for _, offset, size in operations
                         if offset >= device.APP_OFFSET + device.APP_SIZE]
            self.assertLessEqual(max(new_reads), 512 * 1024)

    def test_occupied_new_region_is_refused_without_any_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / 'new-table.bin'
            target.write_bytes(table(device.WIFI_APP_SIZE))
            backup = root / 'backup'
            backup.mkdir()

            def read(action, offset, size, path):
                self.assertEqual(action, 'read-flash')
                raw = table(device.APP_SIZE) if offset == device.TABLE_OFFSET else b'\xff' * size
                if offset == device.WIFI_NVS_OFFSET:
                    raw = b'occupied' + raw[8:]
                Path(path).write_bytes(raw)

            with patch.object(device, 'BACKUP', backup), patch.object(device, 'esptool', read):
                with self.assertRaisesRegex(RuntimeError, 'not blank'):
                    device.wifi_preflight(target)

    def test_wifi_deploy_writes_only_table_and_application(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            backup = root / 'backup'
            backup.mkdir()
            (backup / 'preflight-ok.txt').write_text('fixture')
            build = root / 'build/wifi-firmware'
            (build / 'partition_table').mkdir(parents=True)
            app = build / 'ayni_wifi_chat.bin'
            app.write_bytes(b'candidate')
            target = build / 'partition_table/partition-table.bin'
            target.write_bytes(table(device.WIFI_APP_SIZE))
            with patch.object(device, 'ROOT', root), patch.object(device, 'BACKUP', backup), \
                    patch.object(device, 'wifi_preflight') as preflight, \
                    patch.object(device, 'esptool') as flash:
                device.deploy('wifi')
                preflight.assert_called_once_with(target)
                flash.assert_called_once_with('write-flash', device.TABLE_OFFSET, target,
                                              device.APP_OFFSET, app)


if __name__ == '__main__':
    unittest.main()
