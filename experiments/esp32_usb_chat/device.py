"""Narrow device operations: known board, app/table only, never NVS/bootloader writes."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
PORT = '/dev/cu.usbmodem5D013686941'
OLD = Path('/Users/danny/Documents/Codex/2026-10-23/task/artifacts/galactic-fullscreen-pair3-v1/app.bin')
BACKUP = ROOT / 'artifacts/recovery'
APP_OFFSET, APP_SIZE = 0x410000, 0x200000
TABLE_OFFSET, TABLE_SIZE = 0x8000, 0x1000
VOLUME_OFFSET, VOLUME_SIZE = 0x800000, 0x800000


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


def deploy(variant='stable'):
    if not (BACKUP/'preflight-ok.txt').is_file():
        raise RuntimeError('A complete successful preflight is required')
    directory, binary = {'stable': ('firmware', 'ayni_usb_chat.bin'),
                         'motor': ('motor-firmware', 'ayni_usb_chat.bin'),
                         'probe': ('motor-probe', 'ayni_motor_probe.bin')}[variant]
    build = ROOT/'build'/directory
    app = build/binary
    table = build/'partition_table/partition-table.bin'
    if not 0 < app.stat().st_size <= APP_SIZE or table.stat().st_size > TABLE_SIZE:
        raise RuntimeError('Image exceeds the preserved app/table region')
    # No generated idf.py flash list: it would also include a bootloader write.
    esptool('write-flash', TABLE_OFFSET, table, APP_OFFSET, app)


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
    parser.add_argument('--variant', choices=['stable', 'motor', 'probe'], default='stable',
                        help='Explicit build selection for deploy; stable is the validated screen/speaker build')
    args = parser.parse_args()
    if args.action != 'deploy' and args.variant != 'stable':
        parser.error('--variant applies only to deploy')
    {'preflight': preflight, 'deploy': lambda: deploy(args.variant), 'restore': restore,
     'reset': lambda: esptool('chip-id'), 'erase-volume': erase_volume}[args.action]()
