"""One explicitly armed, isolated bench pulse. No model, retry, flash or automatic reset."""
import argparse
import time
import zlib
import serial
from device import PORT, target


def wait_line(port, prefix, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = port.readline(256).decode('ascii', 'replace').strip()
        if line.startswith(('ERROR', 'REFUSED')):
            raise RuntimeError(line)
        if line.startswith(('ROUTES ', 'EDGES ')) and not prefix.startswith('AYNI MOTOR PROBE/'):
            print(line, flush=True)
        if line.startswith(prefix):
            print(line, flush=True)
            return
    raise RuntimeError(f'No {prefix} receipt; do not retry a pulse whose outcome is unknown')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--channel', type=int, choices=[1, 2])
    parser.add_argument('--direction', choices=['positive', 'negative'], default='positive')
    parser.add_argument('--percent', type=int, default=10)
    parser.add_argument('--duration-ms', type=int, default=300)
    parser.add_argument('--arm-isolated-bench', action='store_true',
                        help='Explicit one-pulse bench authorization after power/rating/stop checks')
    args = parser.parse_args()
    if args.channel is not None and not args.arm_isolated_bench:
        parser.error('A pulse requires --arm-isolated-bench; without channel only observes handshake')
    if not 1 <= args.percent <= 20 or not 50 <= args.duration_ms <= 300:
        parser.error('Bench bounds: percent 1..20, duration-ms 50..300; no clamping')
    target()
    port = serial.Serial(port=None, baudrate=921600, timeout=0.05, write_timeout=2, exclusive=True)
    port.dtr = port.rts = False
    port.port = PORT
    port.open()
    def send(line):
        raw = line.encode('ascii')
        body = raw+b'\n' if line == 'STOP' else raw+f' *{zlib.crc32(raw):08x}\n'.encode('ascii')
        if port.write(body) != len(body):
            raise RuntimeError('Partial serial write; pulse outcome unknown, no retry')
    try:
        wait_line(port, 'AYNI MOTOR PROBE/4;', 5)
        send('CHECK')
        wait_line(port, 'CHECKED ', 5)
        if args.channel is None:
            return
        # Strictly increasing within this Mac lifetime; firmware rejects old/consumed arms.
        nonce = time.monotonic_ns()
        send(f'ARM {nonce:x}')
        wait_line(port, 'ARMED ', 2)
        direction = 0 if args.direction == 'positive' else 1
        send(f'PULSE {nonce:x} {args.channel} {direction} {args.percent} {args.duration_ms}')
        wait_line(port, 'ACCEPTED ', 2)
        wait_line(port, 'DONE ', 3)
        print('Finite waveform completed. Shaft rotation/stop still require human observation.')
    finally:
        try:
            send('STOP')
        finally:
            port.close()


if __name__ == '__main__':
    main()
