import struct
import unittest
from unittest.mock import Mock
from motor import MotorReview, REVIEW
import bridge
from protocol import *


class MotorTests(unittest.TestCase):
    def setUp(self):
        self.events = Mock()
        self.now = 10
        self.review = MotorReview(self.events, lambda: self.now)
        self.permit = 9007199254740993

    def packet(self, phase=1, nonce=1, first=50, second=0, duration=5000, limit=90):
        return REVIEW.pack(nonce, self.permit+1, self.permit+2, 18, 19,
                           first, second, duration, limit, phase, 0)

    def answer(self, approved=True):
        return {'permit': str(self.permit), 'ticket': self.packet()[:40].hex(), 'approved': approved}

    def test_approve_and_observe_exact_attempt_with_large_ids(self):
        self.review.receive(self.permit, self.packet(), 1)
        identity, packet = self.review.answer(self.answer(), 1)
        self.assertEqual(identity, self.permit)
        self.assertEqual(packet, self.packet()[:40] + struct.pack('<II', 1, 0))
        self.assertIsNotNone(self.review.active)
        self.review.receive(self.permit, self.packet(2), 2)
        self.review.answer(self.answer(), 2)
        self.assertIsNone(self.review.active)
        self.assertFalse(self.review.latched)

    def test_decline_never_produces_approval_and_cannot_reuse(self):
        self.review.receive(self.permit, self.packet(), 1)
        _, packet = self.review.answer(self.answer(False), 1)
        self.assertEqual(packet[-8:], bytes(8))
        with self.assertRaises(ValueError):
            self.review.answer(self.answer(), 1)
        with self.assertRaises(ValueError):
            self.review.receive(self.permit, self.packet(), 1)

    def test_duplicate_review_does_not_open_second_dialog(self):
        self.review.receive(self.permit, self.packet(), 1)
        self.review.receive(self.permit, self.packet(), 1)
        self.events.assert_called_once()

    def test_changed_identity_or_phase_and_expired_approval_rejected(self):
        self.review.receive(self.permit, self.packet(), 1)
        for command in [dict(self.answer(), permit='2'), dict(self.answer(), ticket='00'),
                        dict(self.answer(), approved=1)]:
            with self.subTest(command=command), self.assertRaises(ValueError):
                self.review.answer(command, 1)
        with self.assertRaises(ValueError):
            self.review.answer(self.answer(), 2)
        self.now = 40
        with self.assertRaises(ValueError):
            self.review.answer(self.answer(), 1)
        self.review.expire()
        self.assertIsNone(self.review.active)

    def test_observation_cannot_replace_command_or_claim_different_attempt(self):
        self.review.receive(self.permit, self.packet(), 1)
        self.review.answer(self.answer(), 1)
        for packet in [self.packet(2, second=50), self.packet(2, nonce=2)]:
            with self.assertRaises(ValueError):
                self.review.receive(self.permit, packet, 2)

    def test_bad_observation_and_stop_latch_future_actions(self):
        self.review.receive(self.permit, self.packet(), 1)
        self.review.answer(self.answer(), 1)
        self.review.receive(self.permit, self.packet(2), 2)
        self.review.answer(self.answer(False), 2)
        self.assertTrue(self.review.latched)
        with self.assertRaises(ValueError):
            self.review.receive(self.permit, self.packet(nonce=2), 1)
        self.review.stop()
        self.assertIsNone(self.review.pending)

    def test_duty_limit_and_duration_contract(self):
        for first, second, duration, limit in [(91, 0, 5000, 90), (50, 0, 5000, 49),
                (0, 0, 5000, 90), (50, 0, 5001, 90), (50, 0, 49, 90)]:
            with self.subTest(first=first, duration=duration), self.assertRaises(ValueError):
                self.review.receive(self.permit, self.packet(first=first, second=second,
                    duration=duration, limit=limit), 1)
        self.review.receive(self.permit, self.packet(first=-90, second=90), 1)

    def test_device_terminal_dismisses_only_matching_action(self):
        self.review.receive(self.permit, self.packet(), 1)
        self.review.finished(2)
        self.assertIsNotNone(self.review.pending)
        self.review.finished(self.permit)
        self.assertIsNone(self.review.pending)
        with self.assertRaises(ValueError):
            self.review.answer(self.answer(), 1)

    def test_bridge_close_refuses_to_close_usb_without_power_off(self):
        app = bridge.Bridge({}, Mock())
        app.port = Mock()
        app.send = Mock()
        with self.assertRaises(ValueError):
            app.close()
        app.port.close.assert_not_called()
        app.send.assert_called_once_with(MOTOR_STOP, 0)

    def test_bridge_close_after_explicit_power_off_requests_joined_shutdown(self):
        app = bridge.Bridge({}, Mock())
        app.port = Mock()
        app.send = Mock()
        app.closed = True
        app.close(driver_power_off=True)
        self.assertEqual(app.send.call_args_list[0].args, (MOTOR_POWER_OFF, 0))
        app.port.close.assert_called_once()

    def test_bridge_requires_power_off_before_any_serial_open(self):
        app = bridge.Bridge({}, Mock())
        with self.assertRaises(ValueError):
            app.connect()
        self.assertIsNone(app.port)

    def test_motor_run_is_not_invented_by_final_text_or_plan(self):
        app = bridge.Bridge({}, Mock())
        app.send = Mock()
        app.turn, app.pending_user = 1, 'hello'
        app.handle(ANSWER, 1, b'motor.run 90%')
        app.send.assert_not_called()
        self.assertIsNone(app.motor.pending)


if __name__ == '__main__':
    unittest.main()
