"""Operator-controlled bench session. Open/close this adapter only with motor power off."""
import argparse
import time
import signal
import zlib
import serial
from device import PORT, target

PINS = (38, 39, 40, 41)  # Wire profile PROBE/8, D0 through D3.
HELLO = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41;'
UPTIME_TOLERANCE_MS = 500


def wait_line(port, prefix, seconds, check_interrupt=None):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check_interrupt is not None:
            check_interrupt()
        line = port.readline(256).decode('ascii', 'replace').strip()
        if check_interrupt is not None:
            check_interrupt()
        if line.startswith(('ERROR', 'REFUSED')):
            raise RuntimeError(line)
        if line.startswith(prefix):
            print(line, flush=True)
            return line
        if line.startswith(('ROUTES ', 'EDGES ')) and not prefix.startswith('AYNI MOTOR PROBE/'):
            print(line, flush=True)
    raise RuntimeError(f'No {prefix} receipt; do not retry a pulse whose outcome is unknown')


def send(port, line):
    raw = line.encode('ascii')
    body = raw+b'\n' if line == 'STOP' else raw+f' *{zlib.crc32(raw):08x}\n'.encode('ascii')
    if port.write(body) != len(body):
        raise RuntimeError('Partial serial write; pulse outcome unknown, no retry')


def stop(port):
    send(port, 'STOP')
    wait_line(port, 'STOPPED ', 2)


def validate_request(request):
    if request.channel not in (1, 2) or request.direction not in ('positive', 'negative'):
        raise ValueError('channel must be 1/2 and direction positive/negative')
    if not 1 <= request.percent <= 50 or not 50 <= request.duration_ms <= 5000 or request.duration_ms % 5:
        raise ValueError('percent 1..50, duration-ms 50..5000 in steps of 5; no clamping')


def run_pulse(port, request, check_interrupt=None):
    validate_request(request)
    # Strictly increasing within this Mac lifetime; firmware rejects old/consumed arms.
    nonce = time.monotonic_ns()
    if check_interrupt is not None:
        check_interrupt()
    send(port, f'ARM {nonce:x}')
    wait_line(port, 'ARMED ', 2, check_interrupt)
    direction = 0 if request.direction == 'positive' else 1
    if check_interrupt is not None:
        check_interrupt()
    send(port, f'PULSE {nonce:x} {request.channel} {direction} {request.percent} {request.duration_ms}')
    accepted = wait_line(port, 'ACCEPTED ', 2, check_interrupt)
    expected = (f'ACCEPTED nonce={nonce:x} channel={request.channel} direction={direction} '
                f'duty={request.percent} duration_ms={request.duration_ms}')
    if accepted != expected:
        raise RuntimeError('Accepted parameters do not match request; stopping, no retry')
    edges = wait_line(port, 'EDGES ', request.duration_ms / 1000 + 2, check_interrupt)
    counts = []
    fields = edges.split()[1:]
    for pin, field in zip(PINS, fields):
        name, separator, count = field.partition('=')
        if name != str(pin) or separator != '=' or not count.isascii() or not count.isdigit():
            raise RuntimeError('Invalid GPIO edge receipt; stopping, no retry')
        counts.append(int(count))
    selected = (request.channel - 1) * 2 + direction
    if len(fields) != len(PINS) or any(
            count != request.duration_ms if index == selected else count != 0
            for index, count in enumerate(counts)):
        raise RuntimeError('GPIO edge receipt does not match selected pin; stopping, no retry')
    wait_line(port, 'DONE ', 3, check_interrupt)
    print('Finite waveform completed. Shaft rotation/stop still require human observation.')

def uptime(line):
    # A diagnostic continuity check, not a physical permit or trusted boot identity.
    for field in line.split(';'):
        name, separator, value = field.strip().partition('=')
        if name == 'uptime_ms' and separator and value.isascii() and value.isdigit():
            return int(value)
    raise RuntimeError('Missing device uptime; persistent bench unavailable')


def report(message):
    try:
        print(message, flush=True)
    except Exception:
        pass  # A broken output stream cannot release a possibly powered serial adapter.


def request_status(port, check_interrupt=None):
    nonce = time.monotonic_ns()
    send(port, f'PING {nonce:x}')
    line = wait_line(port, f'PONG nonce={nonce:x};', 3, check_interrupt)
    return uptime(line), time.monotonic()


class InteractiveBench:
    """Owns live-port lifetime; only POWER_OFF_QUIT permits the caller to close it."""
    def __init__(self, port):
        self.port = port
        self.baseline = request_status(port)
        self.state = 'waiting_for_power_on'
        self.interrupted = False

    def on_interrupt(self, signum, frame):
        # Defer Ctrl-C to a serial wait/input boundary; STOP itself is not interrupted.
        self.interrupted = True

    def check_interrupt(self):
        if self.interrupted:
            self.interrupted = False
            raise InterruptedError('Operator requested STOP')

    def latch_fault(self, error):
        self.state = 'faulted'
        try:
            stop(self.port)
        except BaseException as stop_error:
            report(f'ERROR supplementary stop: {stop_error}')
        report(f'ERROR {error}; no retry. Port held open. Cut motor power; '
               'only STOP/POWER_OFF_QUIT allowed.')

    def command(self, line):
        if line == 'POWER_OFF_QUIT':
            self.state = 'power_off_confirmed'
            return
        self.check_interrupt()
        if line == 'STOP':
            stop(self.port)
            return
        if line == 'READY' and self.state == 'waiting_for_power_on':
            self.state = 'ready'
            report('READY: waiting for one explicit pulse command.')
            return
        fields = line.split()
        if self.state != 'ready' or len(fields) != 5 or fields[0] != 'PULSE':
            report('REFUSED command or session state; no pulse sent.')
            return
        try:
            request = argparse.Namespace(channel=int(fields[1]), direction=fields[2],
                                         percent=int(fields[3]), duration_ms=int(fields[4]))
            validate_request(request)
        except ValueError as error:
            report(f'REFUSED {error}; no pulse sent.')
            return
        current, received = request_status(self.port, self.check_interrupt)
        expected_uptime = self.baseline[0] + (received-self.baseline[1])*1000
        if abs(current-expected_uptime) > UPTIME_TOLERANCE_MS:
            raise RuntimeError(f'Device uptime mismatch: expected about {expected_uptime:.0f} ms, '
                               f'received {current} ms; refusing to arm')
        run_pulse(self.port, request, self.check_interrupt)
        stop(self.port)
        report('SESSION_IDLE: same open port; observe motor before another command.')

    def run(self):
        report('SESSION_OPEN: port stays open. Commands: READY, '
               'PULSE channel positive|negative percent duration_ms, STOP, POWER_OFF_QUIT.')
        while self.state != 'power_off_confirmed':
            try:
                self.command(input('bench> ').strip())
            except BaseException as error:
                self.latch_fault(error)
                if isinstance(error, (EOFError, OSError)):
                    time.sleep(1)  # Retain the handle on lost input, without a busy loop.


def managed_interactive(port):
    try:
        session = InteractiveBench(port)
        previous_handler = signal.signal(signal.SIGINT, session.on_interrupt)
    except BaseException:
        port.close()  # No READY has been accepted; the opening power-off declaration still applies.
        raise
    # This is the outer close owner: unexpected nested errors cannot authorize close.
    while session.state != 'power_off_confirmed':
        try:
            session.run()
        except BaseException as error:
            session.state = 'faulted'
            try:
                session.latch_fault(error)
            except BaseException:
                session.state = 'faulted'  # Even a failed fault report cannot authorize close.
    try:
        report('CLOSING_AFTER_OPERATOR_POWER_OFF')
        stop(port)
    finally:
        try:
            port.close()
        finally:
            signal.signal(signal.SIGINT, previous_handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--channel', type=int, choices=[1, 2])
    parser.add_argument('--direction', choices=['positive', 'negative'], default='positive')
    parser.add_argument('--percent', type=int, default=10)
    parser.add_argument('--duration-ms', type=int, default=5000)
    parser.add_argument('--check-routes-unpowered', action='store_true')
    parser.add_argument('--driver-power-off', action='store_true',
                        help='Operator declares motor power off before opening/resetting adapter')
    parser.add_argument('--interactive-bench', action='store_true',
                        help='Keep one serial connection open through operator-controlled trials')
    parser.add_argument('--arm-isolated-bench', action='store_true')
    args = parser.parse_args()
    if not (args.driver_power_off or args.check_routes_unpowered):
        parser.error('This adapter resets ESP32 on open. Declare motor power off before opening.')
    if (args.channel is not None or args.interactive_bench) and not args.arm_isolated_bench:
        parser.error('Bench pulses require --arm-isolated-bench')
    if args.interactive_bench and (args.channel is not None or args.check_routes_unpowered):
        parser.error('Interactive mode cannot combine with one-shot or all-pin checks')
    if args.channel is not None:
        if args.check_routes_unpowered:
            parser.error('Routing checks and a pulse must be separate invocations')
        try:
            validate_request(args)
        except ValueError as error:
            parser.error(str(error))
    target()
    port = serial.Serial(port=None, baudrate=921600, timeout=0.05, write_timeout=2, exclusive=True)
    port.dtr = port.rts = False
    port.port = PORT
    port.open()
    # No operator READY has been accepted while obtaining the initial greeting.
    try:
        wait_line(port, HELLO, 5)
    except BaseException:
        port.close()
        raise
    if args.interactive_bench:
        managed_interactive(port)
        return
    try:
        if args.check_routes_unpowered:
            send(port, 'CHECK_UNPOWERED')
            wait_line(port, 'CHECKED ', 5)
        elif args.channel is not None:
            # One-shot opening/closing is confined to the explicitly unpowered check.
            run_pulse(port, args)
    finally:
        try:
            stop(port)
        finally:
            port.close()


if __name__ == '__main__':
    main()
