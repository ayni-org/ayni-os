import sys
import signal
import unittest
import zlib
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import motor_probe


class ProbeTests(unittest.TestCase):
    def commands(self, port):
        commands = []
        for call in port.write.call_args_list:
            raw = call.args[0]
            if raw == b'STOP\n':
                commands.append(raw)
                continue
            payload, checksum = raw[:-1].rsplit(b' *', 1)
            self.assertEqual(int(checksum, 16), zlib.crc32(payload))
            commands.append(payload+b'\n')
        return commands
    def run_probe(self, lines, arguments):
        port = Mock()
        port.readline.side_effect = [line.encode() for line in lines + ['STOPPED outputs commanded low']]
        port.write.side_effect = lambda data: len(data)
        with patch.object(motor_probe, 'target'), \
             patch.object(motor_probe.serial, 'Serial', return_value=port), \
             patch.object(motor_probe.time, 'monotonic_ns', return_value=1000), \
             patch.object(sys, 'argv', ['motor_probe.py', '--driver-power-off']+arguments):
            try:
                motor_probe.main()
            except RuntimeError:
                return port, True
        return port, False

    def test_unpowered_explicit_pulse_then_stop_without_reopen(self):
        port, failed = self.run_probe([
            'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41;', 'ARMED one pulse only',
            'ACCEPTED nonce=3e8 channel=1 direction=0 duty=50 duration_ms=5000', 'EDGES 38=5000 39=0 40=0 41=0', 'DONE finite waveform ended; rotation unverified'],
            ['--channel', '1', '--percent', '50', '--arm-isolated-bench'])
        self.assertFalse(failed)
        self.assertEqual(self.commands(port), [
            b'ARM 3e8\n', b'PULSE 3e8 1 0 50 5000\n', b'STOP\n'])
        self.assertFalse(port.dtr)
        self.assertFalse(port.rts)
        port.close.assert_called_once()

    def test_unknown_pulse_result_only_stops_never_rearms_or_retries(self):
        port, failed = self.run_probe([
            'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41;', 'ARMED one pulse only',
            'ERROR transmission completion unknown'],
            ['--channel', '2', '--direction', 'negative', '--arm-isolated-bench'])
        self.assertTrue(failed)
        self.assertEqual(self.commands(port), [
            b'ARM 3e8\n', b'PULSE 3e8 2 1 10 5000\n', b'STOP\n'])
        port.close.assert_called_once()

    def test_unarmed_observation_never_sends_arm_or_pulse(self):
        port, failed = self.run_probe(['AYNI MOTOR PROBE/8; D0..D3=38,39,40,41;'], [])
        self.assertFalse(failed)
        self.assertEqual(self.commands(port), [b'STOP\n'])

    def test_failed_route_check_never_arms_or_pulses(self):
        port, failed = self.run_probe(['AYNI MOTOR PROBE/8; D0..D3=38,39,40,41;', 'ERROR pin routing check failed'],
                                      ['--check-routes-unpowered'])
        self.assertTrue(failed)
        self.assertEqual(self.commands(port), [b'CHECK_UNPOWERED\n', b'STOP\n'])

    def test_mismatched_accepted_channel_stops_without_retry(self):
        port, failed = self.run_probe([
            'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41;', 'ARMED one pulse only',
            'ACCEPTED nonce=3e8 channel=1 direction=0 duty=10 duration_ms=5000'],
            ['--channel', '2', '--arm-isolated-bench'])
        self.assertTrue(failed)
        self.assertEqual(self.commands(port), [
            b'ARM 3e8\n', b'PULSE 3e8 2 0 10 5000\n', b'STOP\n'])
        port.close.assert_called_once()

    def test_edge_receipt_for_another_pin_stops_without_retry(self):
        port, failed = self.run_probe([
            'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41;', 'ARMED one pulse only',
            'ACCEPTED nonce=3e8 channel=2 direction=0 duty=10 duration_ms=5000',
            'EDGES 38=5000 39=0 40=0 41=0'],
            ['--channel', '2', '--arm-isolated-bench'])
        self.assertTrue(failed)
        self.assertEqual(self.commands(port), [
            b'ARM 3e8\n', b'PULSE 3e8 2 0 10 5000\n', b'STOP\n'])
        port.close.assert_called_once()

    def test_unpowered_route_check_is_explicit_and_never_pulses(self):
        port, failed = self.run_probe([
            'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41;',
            'CHECKED all four routes isolated; setup/release edges=0'],
            ['--check-routes-unpowered'])
        self.assertFalse(failed)
        self.assertEqual(self.commands(port), [b'CHECK_UNPOWERED\n', b'STOP\n'])
        port.close.assert_called_once()

    def fake_status(self, initial):
        # Lifecycle tests isolate the status transport; its nonce codec is tested separately.
        first = True
        def status(port, check_interrupt=None):
            nonlocal first
            if first:
                first = False
                line = initial
            else:
                line = motor_probe.wait_line(port, motor_probe.HELLO, 3, check_interrupt)
            return motor_probe.uptime(line), motor_probe.time.monotonic()
        return status

    def interactive(self, lines, commands, nonces):
        port = Mock()
        port.readline.side_effect = [line.encode() for line in lines]
        port.write.side_effect = lambda data: len(data)
        with patch.object(motor_probe, 'target'), \
             patch.object(motor_probe, 'request_status', side_effect=self.fake_status(lines[0])), \
             patch.object(motor_probe.serial, 'Serial', return_value=port), \
             patch.object(motor_probe.time, 'monotonic', return_value=10.0), \
             patch.object(motor_probe.time, 'monotonic_ns', side_effect=nonces), \
             patch('builtins.input', side_effect=commands), \
             patch.object(sys, 'argv', ['motor_probe.py', '--driver-power-off',
                                        '--interactive-bench', '--arm-isolated-bench']):
            motor_probe.main()
        return port

    def test_two_operator_pulses_share_one_open_serial_connection(self):
        hello = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=1000;'
        port = self.interactive([
            hello, hello, 'ARMED one pulse only',
            'ACCEPTED nonce=3e8 channel=1 direction=0 duty=10 duration_ms=5000',
            'EDGES 38=5000 39=0 40=0 41=0', 'DONE finite waveform ended', 'STOPPED low',
            hello, 'ARMED one pulse only',
            'ACCEPTED nonce=3e9 channel=2 direction=0 duty=20 duration_ms=5000',
            'EDGES 38=0 39=0 40=5000 41=0', 'DONE finite waveform ended', 'STOPPED low',
            'STOPPED low'],
            ['READY', 'PULSE 1 positive 10 5000', 'PULSE 2 positive 20 5000', 'POWER_OFF_QUIT'],
            [1000, 1001])
        self.assertEqual(self.commands(port), [
            b'ARM 3e8\n', b'PULSE 3e8 1 0 10 5000\n', b'STOP\n',
            b'ARM 3e9\n', b'PULSE 3e9 2 0 20 5000\n', b'STOP\n', b'STOP\n'])
        port.open.assert_called_once()
        port.close.assert_called_once()
        port.reset_input_buffer.assert_not_called()

    def test_changed_uptime_refuses_to_arm_and_waits_for_power_off(self):
        hello = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=10000;'
        reset_hello = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=1000;'
        port = self.interactive([hello, reset_hello, 'STOPPED low', 'STOPPED low'],
            ['READY', 'PULSE 2 positive 20 5000', 'PULSE 2 positive 20 5000', 'POWER_OFF_QUIT'], [])
        self.assertEqual(self.commands(port), [b'STOP\n', b'STOP\n'])
        port.open.assert_called_once()
        port.close.assert_called_once()

    def test_session_never_pulses_before_operator_ready(self):
        hello = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=1000;'
        port = self.interactive([hello, 'STOPPED low'],
            ['PULSE 2 positive 20 5000', 'POWER_OFF_QUIT'], [])
        self.assertEqual(self.commands(port), [b'STOP\n'])

    def test_open_requires_operator_power_off_declaration(self):
        with patch.object(motor_probe.serial, 'Serial') as factory, \
             patch.object(sys, 'argv', ['motor_probe.py']), \
             patch('sys.stderr'):
            with self.assertRaises(SystemExit) as result:
                motor_probe.main()
        self.assertEqual(result.exception.code, 2)
        factory.assert_not_called()

    def test_repeated_interrupts_during_stop_cannot_close_before_power_off(self):
        hello = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=1000;'
        port = Mock()
        port.readline.return_value = hello.encode()
        port.write.side_effect = lambda data: len(data)
        commands = iter(['READY', 'STOP', 'PULSE 2 positive 20 5000', 'POWER_OFF_QUIT'])
        def read_command(prompt):
            port.close.assert_not_called()
            return next(commands)
        with patch.object(motor_probe, 'target'), \
             patch.object(motor_probe, 'request_status', return_value=(1000, motor_probe.time.monotonic())), \
             patch.object(motor_probe.serial, 'Serial', return_value=port), \
             patch.object(motor_probe, 'stop', side_effect=[KeyboardInterrupt(), KeyboardInterrupt(), None]), \
             patch('builtins.input', side_effect=read_command), \
             patch.object(sys, 'argv', ['motor_probe.py', '--driver-power-off',
                                        '--interactive-bench', '--arm-isolated-bench']):
            motor_probe.main()
        self.assertEqual(self.commands(port), [])
        port.open.assert_called_once()
        port.close.assert_called_once()

    def test_input_error_holds_port_and_latches_fault_until_power_off(self):
        hello = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=1000;'
        port = self.interactive([hello, 'STOPPED low', 'STOPPED low'],
            [OSError('input unavailable'), 'PULSE 2 positive 20 5000', 'POWER_OFF_QUIT'], [])
        self.assertEqual(self.commands(port), [b'STOP\n', b'STOP\n'])
        port.close.assert_called_once()

    def test_pending_interrupt_on_armed_receipt_never_sends_pulse(self):
        hello = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=1000;'
        lines = iter([hello, hello, 'ARMED one pulse only', 'STOPPED low', 'STOPPED low'])
        port = Mock()
        def read_receipt(size):
            line = next(lines)
            if line.startswith('ARMED'):
                signal.getsignal(signal.SIGINT)(signal.SIGINT, None)
            return line.encode()
        port.readline.side_effect = read_receipt
        port.write.side_effect = lambda data: len(data)
        with patch.object(motor_probe, 'target'), \
             patch.object(motor_probe, 'request_status', side_effect=self.fake_status(hello)), \
             patch.object(motor_probe.serial, 'Serial', return_value=port), \
             patch.object(motor_probe.time, 'monotonic', return_value=10.0), \
             patch.object(motor_probe.time, 'monotonic_ns', return_value=1000), \
             patch('builtins.input', side_effect=['READY', 'PULSE 2 positive 20 5000', 'POWER_OFF_QUIT']), \
             patch.object(sys, 'argv', ['motor_probe.py', '--driver-power-off',
                                        '--interactive-bench', '--arm-isolated-bench']):
            motor_probe.main()
        self.assertEqual(self.commands(port), [b'ARM 3e8\n', b'STOP\n', b'STOP\n'])
        port.close.assert_called_once()

    def test_fault_output_error_does_not_release_powered_port(self):
        class BrokenFaultOutput:
            def write(self, text):
                if text.startswith('ERROR'):
                    raise ValueError('output closed')
                return len(text)
            def flush(self):
                pass
        hello = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=10000;'
        reset = 'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=1000;'
        with patch('sys.stdout', BrokenFaultOutput()):
            port = self.interactive([hello, reset, 'STOPPED low', 'STOPPED low'],
                ['READY', 'PULSE 2 positive 20 5000', 'POWER_OFF_QUIT'], [])
        self.assertEqual(self.commands(port), [b'STOP\n', b'STOP\n'])
        port.close.assert_called_once()

    def test_status_uses_matching_nonce_instead_of_queued_greetings(self):
        port = Mock()
        port.write.side_effect = lambda data: len(data)
        port.readline.side_effect = [
            b'AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; uptime_ms=1000;',
            b'PONG nonce=3e7; uptime_ms=2000;',
            b'PONG nonce=3e8; uptime_ms=20500;']
        with patch.object(motor_probe.time, 'monotonic_ns', return_value=1000), \
             patch.object(motor_probe.time, 'monotonic', return_value=10.0):
            self.assertEqual(motor_probe.request_status(port), (20500, 10.0))
        self.assertEqual(self.commands(port), [b'PING 3e8\n'])


if __name__ == '__main__':
    unittest.main()
