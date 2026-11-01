"""Normal Core motor admission/decline check. Driver power must stay OFF; never approves PWM."""
import argparse
import json
from pathlib import Path
import sys
import time
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/'desktop'))
from bridge import Bridge


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver-power-off', required=True, action='store_true')
    parser.parse_args()
    events = []
    def emit(kind, value):
        event = {'type': kind, 'value': value}
        events.append(event)
        print(json.dumps(event, ensure_ascii=False), flush=True)
    app = Bridge({'port':'/dev/cu.usbmodem5D013686941', 'url':'https://api.deepseek.com',
        'model':'deepseek-flash', 'key':'', 'driver_power_off':True}, emit,
        offline=True, motor_check=True)
    started = declined = False
    passed = False
    app.connect()
    try:
        deadline = time.monotonic()+100
        while time.monotonic() < deadline:
            app.poll()
            if app.ready and not started:
                app.user('离线检查电机提议和拒批，不执行运动。')
                started = True
            if app.motor.pending and not declined:
                _, key, _ = app.motor.pending
                app.motor_answer({'permit':str(key[0]), 'ticket':key[1].hex(), 'approved':False}, 1)
                declined = True
            messages = [e['value'] for e in events if e['type'] == 'status']
            if any(m.startswith('turn failed/cancelled') for m in messages):
                passed = declined and any('binding=motor.run' in m and 'outcome=3 status=0' in m
                                         and 'effect=1' in m for m in messages)
                break
            if any(str(m).startswith(('initialize failed', 'kernel stopped/held')) for m in messages):
                break
    finally:
        app.close(timeout=10, driver_power_off=True)
    (ROOT/'artifacts/motor-decline-events.json').write_text(json.dumps(
        {'passed':passed, 'declined':declined, 'normal_stop':app.closed, 'events':events},
        ensure_ascii=False, indent=2))
    if not passed or not app.closed:
        raise SystemExit('Motor admission/decline not verified; no PWM approval was sent.')
    print('Core motor admission, approval request, no-effect decline and joined shutdown passed.')


if __name__ == '__main__':
    main()
