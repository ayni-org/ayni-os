"""One-time USB OTA bootstrap for the verified board; never writes NVS or Ayni data.

prepare only reads and saves complete old regions. install validates those backups,
rechecks the live old table/app header, and writes once without a retry. Later upgrades
use /update and application .bin only, never this bootstrap again.
"""
import argparse
import binascii
from pathlib import Path
import struct

import device

ROOT = Path(__file__).resolve().parent
BACKUP = ROOT / 'artifacts/recovery/pre-ota-20261111'
BUILD = ROOT / 'build/wifi-firmware'
SECTOR = 0x1000
TABLE = 0x8000
OTA_DATA = 0x9000
FIRST = 0x100000
FIRST_SIZE = 0x300000
SECOND = 0x410000
SECOND_SIZE = 0x3f0000
CHUNK = 0x100000
PRESERVED = {'demo_cfg': (1, 2, 0x400000, 0x6000, 0),
             'nvs': (1, 2, 0x406000, 0xa000, 0),
             'ayni': (1, 0x83, 0x800000, 0x800000, 0)}
OLD_LAYOUT = dict(PRESERVED, audio=(1, 0x40, FIRST, FIRST_SIZE, 0),
                  factory=(0, 0, SECOND, SECOND_SIZE, 0))
NEW_LAYOUT = dict(PRESERVED, otadata=(1, 0, OTA_DATA, 2 * SECTOR, 0),
                  ota_0=(0, 0x10, FIRST, FIRST_SIZE, 0),
                  ota_1=(0, 0x11, SECOND, SECOND_SIZE, 0))
REGIONS = [(0, TABLE, 'bootloader.bin'), (TABLE, SECTOR, 'partition.bin'),
           (OTA_DATA, 2 * SECTOR, 'gap.bin'), (FIRST, FIRST_SIZE, 'audio.bin'),
           (SECOND, SECOND_SIZE, 'application.bin')]


def partitions(raw):
    """Only the frozen bootstrap layouts are supported, including flags and all entries."""
    result = {}
    entry = struct.Struct('<HBBII16sI')
    for at in range(0, len(raw) - entry.size + 1, entry.size):
        magic, kind, sub, offset, size, name, flags = entry.unpack_from(raw, at)
        if magic != 0x50aa:
            break
        label = name.rstrip(b'\x00').decode('ascii')
        if label in result:
            raise RuntimeError('Duplicate partition label')
        result[label] = (kind, sub, offset, size, flags)
    return result


def selection():
    """IDF 6.0.3 esp_ota_select_entry_t: CRC covers sequence only (otatool.py).

    seq 3 selects new ota_0, NEW=0; seq 2 keeps old ota_1, VALID=2.
    No eFuse or secure-version changes. Normal first boot must confirm the new app.
    """
    def entry(seq, state):
        crc = binascii.crc32(struct.pack('<I', seq), 0xffffffff) & 0xffffffff
        return struct.pack('<I20sII', seq, b'\xff' * 20, state, crc).ljust(SECTOR, b'\xff')
    return entry(3, 0) + entry(2, 2)


def save_region(offset, size, name):
    saved = BACKUP / name
    if saved.exists():
        if saved.stat().st_size != size:
            raise RuntimeError('Partial backup exists; preserve and inspect it')
        return
    chunks = []
    for start in range(offset, offset + size, CHUNK):
        count = min(CHUNK, offset + size - start)
        chunk = BACKUP / f'{name}.{start:08x}.chunk'
        if not chunk.exists():
            temporary = chunk.with_suffix('.partial')
            # Only read operations may be repeated after interruption; no write retry exists.
            device.esptool('read-flash', start, count, temporary)
            if temporary.stat().st_size != count:
                raise RuntimeError('Short flash backup')
            temporary.replace(chunk)
        if chunk.stat().st_size != count:
            raise RuntimeError('Short completed backup chunk')
        chunks.append(chunk)
        print(f'Backup verified: {name} {start:#x}+{count:#x}', flush=True)
    with saved.with_suffix('.partial').open('wb') as output:
        for chunk in chunks:
            output.write(chunk.read_bytes())
    saved.with_suffix('.partial').replace(saved)


def check_backup():
    for _, size, name in REGIONS:
        if not (BACKUP / name).is_file() or (BACKUP / name).stat().st_size != size:
            raise RuntimeError('Complete bootstrap backup is required')
    if partitions((BACKUP / 'partition.bin').read_bytes()) != OLD_LAYOUT:
        raise RuntimeError('Current layout is not the frozen pre-OTA Wi-Fi layout')
    if (BACKUP / 'gap.bin').read_bytes() != b'\xff' * (2 * SECTOR):
        raise RuntimeError('OTA metadata gap is occupied; no erase or write allowed')
    old = (BACKUP / 'application.bin').read_bytes()
    # App descriptor: 24-byte image header + 8-byte segment header + project offset 48.
    if old[0] != 0xe9 or old[80:112].split(b'\x00')[0] != b'ayni_wifi_chat':
        raise RuntimeError('Old rollback application is not this project')


def candidate():
    app = BUILD / 'ayni_wifi_chat.bin'
    table = BUILD / 'partition_table/partition-table.bin'
    boot = BUILD / 'bootloader/bootloader.bin'
    if partitions(table.read_bytes()) != NEW_LAYOUT:
        raise RuntimeError('Candidate partition layout mismatch')
    raw = app.read_bytes()
    if not 112 <= len(raw) <= FIRST_SIZE or raw[0] != 0xe9 or raw[23] != 1 or \
            raw[80:112].split(b'\x00')[0] != b'ayni_wifi_chat':
        raise RuntimeError('Invalid or oversized project image')
    if not 0 < boot.stat().st_size <= TABLE or table.stat().st_size > SECTOR:
        raise RuntimeError('Bootloader/table exceeds its bounded region')
    config = (BUILD / 'sdkconfig').read_text()
    if 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' not in config or \
            'CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y' in config:
        raise RuntimeError('Expected software rollback without eFuse anti-rollback')
    return boot, table, app


def prepare():
    candidate()
    BACKUP.mkdir(parents=True, exist_ok=True)
    # Refuse a changed table before reading the old audio/app regions.
    save_region(TABLE, SECTOR, 'partition.bin')
    if partitions((BACKUP / 'partition.bin').read_bytes()) != OLD_LAYOUT:
        raise RuntimeError('Not the known pre-OTA layout; no writes performed')
    for offset, size, name in REGIONS:
        save_region(offset, size, name)
    check_backup()
    (BACKUP / 'otadata-new.bin').write_bytes(selection())
    print('Complete backup saved. No flash writes performed; NVS and Ayni data were not read.')


def install():
    boot, table, app = candidate()
    check_backup()
    # Recheck current layout and rollback app prefix immediately before the one write.
    live_table, live_app = BACKUP / 'live-table.bin', BACKUP / 'live-app-prefix.bin'
    device.esptool('read-flash', TABLE, SECTOR, live_table)
    device.esptool('read-flash', SECOND, SECTOR, live_app)
    if live_table.read_bytes() != (BACKUP / 'partition.bin').read_bytes() or \
            live_app.read_bytes() != (BACKUP / 'application.bin').read_bytes()[:SECTOR]:
        raise RuntimeError('Live source changed since backup; bootstrap refused')
    metadata = BACKUP / 'otadata-new.bin'
    metadata.write_bytes(selection())
    # Preserve ota_1 at SECOND as the old valid fallback. Never use IDF's generated flash list.
    device.esptool('write-flash', 0, boot, TABLE, table, OTA_DATA, metadata, FIRST, app)
    print('OTA bootstrap written and verified by esptool; old app, NVS and Ayni data preserved.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'install'])
    args = parser.parse_args()
    {'prepare': prepare, 'install': install}[args.action]()
