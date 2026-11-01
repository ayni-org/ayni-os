"""Operator-authorized unpowered GPIO check. Never supplies a visual wheel observation."""
import argparse
import json
from pathlib import Path
import re
import sys
import time
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/'desktop'))
from bridge import Bridge


def check(motor):
    events = []
    def emit(kind, value):
        events.append({'type':kind, 'value':value})
        print(json.dumps(events[-1], ensure_ascii=False), flush=True)
    app = Bridge({'port':'/dev/cu.usbmodem5D013686941','url':'https://api.deepseek.com',
        'model':'deepseek-flash','key':'','driver_power_off':True}, emit,
        offline=True, motor_check=f'output{motor}')
    submitted = approved = answered = False
    app.connect()
    try:
        deadline = time.monotonic()+100
        while time.monotonic() < deadline:
            app.poll()
            if app.ready and not submitted:
                app.user('驱动断电，仅检查有限GPIO输出，不测试车轮。')
                submitted = True
            if app.motor.pending:
                phase, key, _ = app.motor.pending
                if phase == 1 and not approved:
                    # This command is permitted only by this invocation's explicit operator flag.
                    app.motor_answer({'permit':str(key[0]), 'ticket':key[1].hex(), 'approved':True}, 1)
                    approved = True
            if any(e['type']=='answer' for e in events):
                answered = True
                break
            if any(e['type']=='status' and str(e['value']).startswith(
                    ('turn failed/cancelled', 'initialize failed', 'kernel stopped/held')) for e in events):
                break
    finally:
        app.close(timeout=10, driver_power_off=True)
    expected = (5000, 0, 0, 0) if motor == 1 else (0, 0, 5000, 0)
    completed = False
    for event in events:
        if event['type'] != 'status':
            continue
        message = event['value']
        match = re.search(r'edges38/39/40/41=(\d+)/(\d+)/(\d+)/(\d+) counted=1', message)
        if message.startswith('motor stage=execution-end status=0 ') and match:
            completed = tuple(map(int, match.groups())) == expected
    passed = approved and answered and completed and app.closed
    (ROOT/f'artifacts/motor-unpowered-output{motor}.json').write_text(json.dumps(
        {'passed':passed, 'driver_power':'operator-declared-off', 'visual_observation_sent':False,
         'expected_counts':expected, 'events':events}, ensure_ascii=False, indent=2))
    if not passed:
        raise SystemExit(f'Motor {motor} GPIO path did not complete; inspect stage diagnostics. No retry.')
    print(f'Motor {motor}: finite GPIO counts and output-stop notification passed, no powered-wheel claim.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver-power-off', action='store_true', required=True)
    parser.add_argument('--operator-approved-unpowered', action='store_true', required=True,
                        help='Explicit operator exception for this invocation only; 50 percent, 5s per channel')
    parser.add_argument('--motor', type=int, choices=[1, 2], required=True)
    args = parser.parse_args()
    check(args.motor)


if __name__ == '__main__':
    main()
