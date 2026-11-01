"""One held USB session for an operator-authorized raised-wheel test, 50% / 5s per motor.
Open/close only with the driver OFF. RUN needs POWER_ON_READY; OBSERVED is a real
operator statement, never generated from GPIO counts. No motion retries or reconnects.
"""
import argparse
from enum import Enum, auto
import json
from pathlib import Path
import queue
import signal
import sys
import threading
import time
ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT/'desktop'))
from bridge import Bridge


class State(Enum):
    STARTING = auto()
    UNPOWERED = auto()
    IDLE = auto()
    CALL = auto()
    OBSERVE = auto()
    HOLD = auto()
    CLOSING = auto()


class LiveCheck:
    def __init__(self):
        self.state = State.STARTING
        self.events = []
        self.case = 0
        self.completed = set()
        self.approved = None
        self.observation = None
        self.interrupted = False
        self.app = Bridge({'port':'/dev/cu.usbmodem5D013686941','url':'https://api.deepseek.com',
            'model':'deepseek-flash','key':'','driver_power_off':True}, self.emit,
            offline=True, motor_check='output1')

    def emit(self, kind, value):
        event = {'type':kind, 'value':value}
        self.events.append(event)
        print(json.dumps(event, ensure_ascii=False), flush=True)
        if kind == 'answer' and self.state == State.CALL:
            self.completed.add(self.case)
            self.state = State.IDLE
            print(f'CASE_COMPLETE motor={self.case}; same USB port held', flush=True)
        if kind == 'status' and str(value).startswith(('turn failed/cancelled',
                'initialize failed', 'kernel stopped/held')) and self.state != State.CLOSING:
            self.state = State.HOLD
            print('FAULT_HOLD: no further RUN. Cut driver power before POWER_OFF_QUIT.', flush=True)

    def fail(self, message):
        print(f'FAULT_HOLD {message}; cut driver power; port stays open', flush=True)
        self.state = State.HOLD
        try:
            self.app.stop_motor()
        except Exception:
            print('STOP transport unavailable; physical stop unconfirmed', flush=True)

    def command(self, line):
        if line == 'POWER_OFF_QUIT':
            # This command is an explicit operator statement that independent VIN is OFF.
            self.state = State.CLOSING
            self.app.close(timeout=10, driver_power_off=True)
            path = ROOT/'artifacts/motor-live-events.json'
            path.write_text(json.dumps({'completed':sorted(self.completed),
                'normal_stop':self.app.closed, 'events':self.events}, ensure_ascii=False, indent=2))
            return False
        if line == 'STOP':
            self.fail('operator stop')
        elif line == 'POWER_ON_READY' and self.state == State.UNPOWERED:
            self.state = State.IDLE
            print('POWER_ON_READY accepted; no motion yet. RUN 1 or RUN 2.', flush=True)
        elif line in ('RUN 1', 'RUN 2') and self.state == State.IDLE:
            motor = int(line[-1])
            if motor in self.completed:
                raise ValueError('This motor was already tested; no automatic repeat')
            self.case = motor
            self.approved = self.observation = None
            self.app.motor_check = f'output{motor}'
            self.state = State.CALL
            self.app.user(f'人工已授权电机{motor}单次50% PWM/5000ms；另一台0。')
        elif line == 'OBSERVED' and self.state == State.OBSERVE:
            phase, key, _ = self.app.motor.pending
            if phase != 2:
                raise ValueError('No live visual-observation ticket')
            self.app.motor_answer({'permit':str(key[0]), 'ticket':key[1].hex(), 'approved':True}, 2)
            self.state = State.CALL
        else:
            raise ValueError(f'Command unavailable in {self.state.name}: {line}')
        return True

    def tick(self):
        if self.interrupted:
            self.interrupted = False
            self.fail('interrupt; no USB close')
        self.app.poll()
        if self.state == State.STARTING and self.app.ready:
            self.state = State.UNPOWERED
            print('SESSION_READY_UNPOWERED: port held; wait for operator power-on statement.', flush=True)
        pending = self.app.motor.pending
        if not pending or self.state not in (State.CALL, State.OBSERVE):
            return
        phase, key, _ = pending
        permit, ticket, first, second, duration, _ = key
        expected = (50, 0, 5000) if self.case == 1 else (0, 50, 5000)
        if (first, second, duration) != expected:
            raise ValueError('Proposal differs from the authorized 50% / 5s case')
        if phase == 1 and self.approved is None:
            self.app.motor_answer({'permit':str(permit), 'ticket':ticket.hex(), 'approved':True}, 1)
            self.approved = permit
            print(f'APPROVED motor={self.case} percent=50 duration_ms=5000; no retry', flush=True)
        elif phase == 2 and self.observation is None:
            self.observation = permit
            self.state = State.OBSERVE
            print(f'OBSERVATION_REQUIRED motor={self.case}: await actual stopped-wheel report; port held', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver-power-off', required=True, action='store_true')
    parser.add_argument('--operator-authorized-live', required=True, action='store_true')
    parser.parse_args()
    commands = queue.Queue()
    def reader():
        for line in sys.stdin:
            commands.put(line.strip())
        commands.put('EOF')
    session = LiveCheck()
    signal.signal(signal.SIGINT, lambda *_: setattr(session, 'interrupted', True))
    session.app.connect() # Caller has just declared independent motor VIN OFF.
    threading.Thread(target=reader, daemon=True).start()
    print('SESSION_OPEN: same port retained until explicit POWER_OFF_QUIT', flush=True)
    while True:
        try:
            try:
                command = commands.get_nowait()
            except queue.Empty:
                command = None
            if command == 'EOF':
                session.fail('input EOF')
            elif command and not session.command(command):
                break
            session.tick()
        except Exception as error:
            session.fail(str(error))
            time.sleep(0.1)


if __name__ == '__main__':
    main()
