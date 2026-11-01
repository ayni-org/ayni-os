import sys
from pathlib import Path
import unittest
from unittest.mock import Mock, patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import motor_live_check as live


class LiveSessionTests(unittest.TestCase):
    def setUp(self):
        self.bridge = Mock()
        self.bridge.ready = False
        self.bridge.closed = True
        self.bridge.motor.pending = None
        self.factory = patch.object(live, 'Bridge', return_value=self.bridge)
        self.factory.start()
        self.addCleanup(self.factory.stop)
        self.session = live.LiveCheck()

    def prepare_case(self):
        self.session.state = live.State.UNPOWERED
        self.session.command('POWER_ON_READY')
        self.session.command('RUN 1')
        return (15, bytes(40), 50, 0, 5000, 90)

    def test_power_off_open_does_not_allow_motion_before_operator_ready(self):
        self.bridge.ready = True
        self.session.tick()
        self.assertEqual(self.session.state, live.State.UNPOWERED)
        with self.assertRaises(ValueError):
            self.session.command('RUN 1')
        self.bridge.user.assert_not_called()
        self.bridge.close.assert_not_called()

    def test_only_exact_authorized_case_is_approved_once(self):
        key = self.prepare_case()
        self.bridge.motor.pending = (1, key, 100)
        self.session.tick()
        self.session.tick()
        self.bridge.motor_answer.assert_called_once()
        self.assertTrue(self.bridge.motor_answer.call_args.args[0]['approved'])
        self.assertEqual(self.bridge.motor_answer.call_args.args[1], 1)

    def test_changed_model_proposal_never_gets_approval(self):
        self.prepare_case()
        self.bridge.motor.pending = (1, (15, bytes(40), 90, 0, 5000, 90), 100)
        with self.assertRaises(ValueError):
            self.session.tick()
        self.bridge.motor_answer.assert_not_called()

    def test_gpio_stop_does_not_generate_human_observation(self):
        key = self.prepare_case()
        self.bridge.motor.pending = (2, key, 100)
        self.session.tick()
        self.assertEqual(self.session.state, live.State.OBSERVE)
        self.bridge.motor_answer.assert_not_called()
        self.session.command('OBSERVED')
        self.assertEqual(self.bridge.motor_answer.call_args.args[1], 2)

    def test_interrupt_requests_stop_and_keeps_port_open(self):
        self.prepare_case()
        self.session.interrupted = True
        self.session.tick()
        self.assertEqual(self.session.state, live.State.HOLD)
        self.bridge.stop_motor.assert_called_once()
        self.bridge.close.assert_not_called()
        with self.assertRaises(ValueError):
            self.session.command('RUN 2')

    def test_only_explicit_power_off_statement_closes_usb(self):
        self.session.state = live.State.HOLD
        with patch.object(Path, 'write_text'):
            self.assertFalse(self.session.command('POWER_OFF_QUIT'))
        self.bridge.close.assert_called_once_with(timeout=10, driver_power_off=True)


if __name__ == '__main__':
    unittest.main()
