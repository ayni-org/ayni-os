"""Narrow device operations: known board, app/table only, never NVS/bootloader writes."""
import argparse
from pathlib import Path
import subprocess
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
PORT = '/dev/cu.usbmodem5D013686941'
OLD = Path('/Users/danny/Documents/Codex/2026-10-23/task/artifacts/galactic-fullscreen-pair3-v1/app.bin')
BACKUP = ROOT / 'artifacts/recovery'
APP_OFFSET, APP_SIZE = 0x410000, 0x200000
TABLE_OFFSET, TABLE_SIZE = 0x8000, 0x1000
VOLUME_OFFSET, VOLUME_SIZE = 0x800000, 0x800000
WIFI_NVS_OFFSET, WIFI_NVS_SIZE = 0x406000, 0xa000
WIFI_APP_SIZE = VOLUME_OFFSET - APP_OFFSET


def target():
    from serial.tools.list_ports import comports
    matches = [p for p in comports() if p.device == PORT]
    if len(matches) != 1 or (matches[0].vid, matches[0].pid, matches[0].serial_number) != (0x1a86, 0x55d3, '5D01368694'):
        raise RuntimeError('Verified target identity is absent/changed')


def esptool(*args):
    target()
    subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', PORT,
        '--baud', '460800', '--before', 'default-reset', '--after', 'hard-reset', *map(str,args)], check=True)


def preflight():
    BACKUP.mkdir(parents=True, exist_ok=True)
    for offset, size, name in [(0, TABLE_OFFSET, 'bootloader-original.bin'),
                              (TABLE_OFFSET, TABLE_SIZE, 'partition-original.bin'),
                              (APP_OFFSET, APP_SIZE, 'application-original.bin')]:
        saved = BACKUP/name
        if saved.exists():
            if saved.stat().st_size != size:
                raise RuntimeError('Partial recovery file exists; preserve it and inspect manually')
        else:
            esptool('read-flash', offset, size, saved)
    actual = (BACKUP/'application-original.bin').read_bytes()
    if not actual.startswith(OLD.read_bytes()):
        raise RuntimeError('Current app differs from the supplied old firmware; preserved backup, deployment refused')
    with tempfile.TemporaryDirectory(prefix='ayni-blank-check-') as temporary:
        probe = Path(temporary)/'high-region.bin'
        chunk = 512 * 1024
        for offset in range(VOLUME_OFFSET, VOLUME_OFFSET+VOLUME_SIZE, chunk):
            # Bounded read-only retries recover an observed USB corruption; no writes retry here.
            for attempt in range(3):
                try:
                    esptool('read-flash', offset, chunk, probe)
                    break
                except subprocess.CalledProcessError:
                    if attempt == 2:
                        raise
            if any(value != 255 for value in probe.read_bytes()):
                raise RuntimeError('Proposed volume is not completely erased; no formatting or deployment authorized')
            print(f'Blank chunk verified: {offset:#x}..{offset+chunk:#x}', flush=True)
    (BACKUP/'preflight-ok.txt').write_text(
        'Known USB target; current app matches supplied old firmware.\n'
        'Original app, bootloader and partition sector preserved; private NVS was not read.\n'
        '0x800000..0x1000000 was entirely FF before first deployment.\n')
    print('Preflight passed. No flash contents were changed.')


def deploy(variant='wifi'):
    if variant != 'wifi':
        raise RuntimeError('Only the Wi-Fi voice firmware is supported')
    if not (BACKUP/'preflight-ok.txt').is_file():
        raise RuntimeError('A complete successful preflight is required')
    directory, binary = 'wifi-firmware', 'ayni_wifi_chat.bin'
    build = ROOT/'build'/directory
    app = build/binary
    table = build/'partition_table/partition-table.bin'
    capacity = WIFI_APP_SIZE if variant == 'wifi' else APP_SIZE
    if not 0 < app.stat().st_size <= capacity or table.stat().st_size > TABLE_SIZE:
        raise RuntimeError('Image exceeds the preserved app/table region')
    if variant == 'wifi':
        wifi_preflight(table)
    # No generated idf.py flash list: it would also include a bootloader write.
    esptool('write-flash', TABLE_OFFSET, table, APP_OFFSET, app)
    if variant == 'wifi':
        (BACKUP/'wifi-layout-validated.txt').write_text(
            'Wi-Fi app/table written after preserving prior app/table and checking newly used regions.\n')


def wifi_preflight(new_table):
    """No format/erase fallback. Check only newly used areas, not private NVS."""
    with tempfile.TemporaryDirectory(prefix='ayni-wifi-preflight-') as temporary:
        directory = Path(temporary)
        current_path = directory/'partition.bin'
        esptool('read-flash', TABLE_OFFSET, TABLE_SIZE, current_path)
        current = current_path.read_bytes()
        proposed = new_table.read_bytes()
        if current.startswith(proposed):
            return
        entries = {}
        layout = struct.Struct('<HBBII16sI')
        for at in range(0, len(current)-layout.size+1, layout.size):
            magic, kind, subtype, offset, size, name, flags = layout.unpack_from(current, at)
            if magic != 0x50aa:
                break
            entries[name.rstrip(b'\x00').decode('ascii')] = (kind, subtype, offset, size, flags)
        if entries.get('factory') != (0, 0, APP_OFFSET, APP_SIZE, 0) or entries.get('ayni', ())[2:4] != (VOLUME_OFFSET, VOLUME_SIZE):
            raise RuntimeError('Current partition layout is not the known USB profile; no writes made')
        saved_table = BACKUP/'pre-wifi-partition.bin'
        saved_app = BACKUP/'pre-wifi-application.bin'
        if not saved_table.exists() and not saved_app.exists():
            # Preserve this validated Ayni USB build, in addition to the older original app.
            esptool('read-flash', APP_OFFSET, APP_SIZE, saved_app)
            saved_table.write_bytes(current)
        elif not (saved_table.is_file() and saved_app.is_file() and saved_table.stat().st_size == TABLE_SIZE and saved_app.stat().st_size == APP_SIZE):
            raise RuntimeError('Partial pre-Wi-Fi backup exists; preserve and inspect it')
        if (BACKUP/'wifi-layout-validated.txt').exists():
            return  # A prior successful Wi-Fi deployment owns the new regions; preserve them.
        probe = directory/'unused.bin'
        for offset, size in ((WIFI_NVS_OFFSET, WIFI_NVS_SIZE),
                             (APP_OFFSET+APP_SIZE, WIFI_APP_SIZE-APP_SIZE)):
            chunk_bytes = 512 * 1024
            for start in range(offset, offset + size, chunk_bytes):
                count = min(chunk_bytes, offset + size - start)
                # Same bounded read-only retry used by the original preflight.
                # No write, erase, playback or other action is retried here.
                for attempt in range(3):
                    try:
                        esptool('read-flash', start, count, probe)
                        break
                    except subprocess.CalledProcessError:
                        if attempt == 2:
                            raise
                if any(value != 255 for value in probe.read_bytes()):
                    raise RuntimeError('New Wi-Fi region is not blank; no erase or deployment authorized')
        print('Wi-Fi layout checked; original app, audio, demo_cfg and Ayni data preserved')


def restore():
    app, table = BACKUP/'application-original.bin', BACKUP/'partition-original.bin'
    if app.stat().st_size != APP_SIZE or table.stat().st_size != TABLE_SIZE:
        raise RuntimeError('Recovery artifacts are incomplete')
    esptool('write-flash', TABLE_OFFSET, table, APP_OFFSET, app)
    print('Original app/table restored. Ayni volume preserved; private NVS/bootloader untouched.')


def erase_volume():
    # Core schema 9 does not migrate an older log; this discards the whole Ayni volume only.
    esptool('erase-region', VOLUME_OFFSET, VOLUME_SIZE)
    print('Ayni volume erased; app, table, audio, demo_cfg and bootloader untouched.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['preflight','deploy','restore','reset','erase-volume'])
    parser.add_argument('--variant', choices=['wifi'], default='wifi',
                        help='The active Wi-Fi voice firmware')
    args = parser.parse_args()
    {'preflight': preflight, 'deploy': lambda: deploy(args.variant), 'restore': restore,
     'reset': lambda: esptool('chip-id'), 'erase-volume': erase_volume}[args.action]()
