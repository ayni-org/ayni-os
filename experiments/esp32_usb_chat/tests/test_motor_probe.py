import sys
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
        port.readline.side_effect = [line.encode() for line in lines]
        port.write.side_effect = lambda data: len(data)
        with patch.object(motor_probe, 'target'), \
             patch.object(motor_probe.serial, 'Serial', return_value=port), \
             patch.object(motor_probe.time, 'monotonic_ns', return_value=1000), \
             patch.object(sys, 'argv', ['motor_probe.py']+arguments):
            try:
                motor_probe.main()
            except RuntimeError:
                return port, True
        return port, False

    def test_one_explicit_pulse_then_stop_without_reset(self):
        port, failed = self.run_probe([
            'AYNI MOTOR PROBE/4;', 'CHECKED all four routes isolated', 'ARMED one pulse only',
            'ACCEPTED channel=1', 'EDGES 38=300 39=0 40=0 41=0', 'DONE finite waveform ended; rotation unverified'],
            ['--channel', '1', '--arm-isolated-bench'])
        self.assertFalse(failed)
        self.assertEqual(self.commands(port), [
            b'CHECK\n', b'ARM 3e8\n', b'PULSE 3e8 1 0 10 300\n', b'STOP\n'])
        self.assertFalse(port.dtr)
        self.assertFalse(port.rts)
        port.close.assert_called_once()

    def test_unknown_pulse_result_only_stops_never_rearms_or_retries(self):
        port, failed = self.run_probe([
            'AYNI MOTOR PROBE/4;', 'CHECKED all four routes isolated', 'ARMED one pulse only',
            'ERROR transmission completion unknown'],
            ['--channel', '2', '--direction', 'negative', '--arm-isolated-bench'])
        self.assertTrue(failed)
        self.assertEqual(self.commands(port), [
            b'CHECK\n', b'ARM 3e8\n', b'PULSE 3e8 2 1 10 300\n', b'STOP\n'])
        port.close.assert_called_once()

    def test_unarmed_observation_never_sends_arm_or_pulse(self):
        port, failed = self.run_probe(['AYNI MOTOR PROBE/4;', 'CHECKED all four routes isolated'], [])
        self.assertFalse(failed)
        self.assertEqual(self.commands(port), [b'CHECK\n', b'STOP\n'])

    def test_failed_route_check_never_arms_or_pulses(self):
        port, failed = self.run_probe(['AYNI MOTOR PROBE/4;', 'ERROR pin routing check failed'],
                                      ['--channel', '2', '--arm-isolated-bench'])
        self.assertTrue(failed)
        self.assertEqual(self.commands(port), [b'CHECK\n', b'STOP\n'])


if __name__ == '__main__':
    unittest.main()
