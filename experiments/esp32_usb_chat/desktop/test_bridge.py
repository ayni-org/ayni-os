import json
import struct
import unittest
from unittest.mock import Mock
import bridge
from protocol import *


class ProtocolTests(unittest.TestCase):
    def test_chunked_utf8_frame_and_boot_log(self):
        frame = encode(USER, 123, '中文，屏幕！'.encode())
        decoder = Decoder()
        result = []
        for byte in b'boot log\r\n' + frame:
            result += decoder.feed(bytes([byte]))
        self.assertEqual(result, [(USER, 123, '中文，屏幕！'.encode())])

    def test_screen_frame_size_and_crc(self):
        body = bytes(MAX_PAYLOAD)
        self.assertEqual(Decoder().feed(encode(DISPLAY, 17, body)), [(DISPLAY, 17, body)])
        broken = bytearray(encode(USER, 1, b'hello'))
        broken[-1] ^= 1
        self.assertEqual(Decoder().feed(broken), [])
        with self.assertRaises(ValueError):
            encode(USER, 1, bytes(MAX_PAYLOAD + 1))

    def test_lossless_tool_number_lexeme(self):
        source = '{"binding_version":1,"arguments":{"text":"你好","wide":9007199254740993}}'
        packet = bridge.tool_packet({'id': 'call1', 'function': {'name': 'ayni_call_capability', 'arguments': source}})
        self.assertIn(b'9007199254740993', packet)
        self.assertIn('你好'.encode(), packet)
        self.assertEqual(struct.unpack_from('<HHHH',packet)[2], 5)

    def test_duplicate_tool_keys_are_refused(self):
        with self.assertRaises(ValueError):
            bridge.tool_packet({'id': 'c', 'function': {'name': 'f', 'arguments': '{"a":1,"a":2}'}})

    def test_answer_does_not_trigger_screen(self):
        app = bridge.Bridge({}, Mock())
        app.turn, app.pending_user = 1, 'hello'
        app.screen = Mock()
        app.handle(ANSWER, 1, b'answer')
        app.screen.assert_not_called()
        self.assertEqual(app.history[-1]['content'], 'answer')

    def test_only_screen_provider_request_renders(self):
        app = bridge.Bridge({}, Mock())
        app.screen = Mock()
        app.handle(RENDER_TEXT, 95, '工具请求正文'.encode())
        app.screen.assert_called_once_with('工具请求正文')
        self.assertEqual(app.document_call, 95)

    def test_late_answer_does_not_replace_current_turn(self):
        app = bridge.Bridge({}, Mock())
        app.turn, app.pending_user = 2, 'current'
        app.handle(ANSWER, 1, b'old')
        self.assertEqual(app.pending_user, 'current')
        self.assertFalse(app.history)

    def test_public_message_frame_uses_utf8_byte_lengths(self):
        data = bridge.frame_messages([{'role':'user','content':'中文'}])
        self.assertEqual(data, b'\x1eAYNI-MESSAGES/1\nuser 6\n'+'中文'.encode())

    def test_cancel_waits_for_old_http_then_discards_result_before_next_turn(self):
        events = Mock()
        app = bridge.Bridge({}, events)
        app.ready, app.current, app.pending_user = True, 17, 'old'
        app.port = Mock(in_waiting=0)
        app.port.read.return_value = b''
        app.send = Mock()
        app.handle(STATUS, 1, b'turn failed/cancelled; no automatic retry')
        events.assert_any_call('busy', True)
        with self.assertRaises(ValueError):
            app.user('next')
        app.completed.put((17, MODEL_TEXT, b'late', None))
        app.poll()
        app.send.assert_not_called()
        self.assertIsNone(app.current)
        events.assert_any_call('busy', False)
        app.user('next')
        self.assertEqual(app.send.call_args.args[0], USER)

    def test_new_document_replaces_old_page_wait_and_ignores_old_ack(self):
        app = bridge.Bridge({}, Mock())
        app.document_call, app.display_waiting = 10, 10
        app.pages = [b'new bitmap']
        app.send = Mock()
        app.screen = Mock(side_effect=lambda text: app.show_page(0))
        app.handle(RENDER_TEXT, 95, '新文档'.encode())
        app.send.assert_called_once_with(DISPLAY, 95, b'new bitmap')
        app.handle(DISPLAY_ACK, 10, b'\x01')
        self.assertEqual(app.display_waiting, 95)
        self.assertFalse(app.turn_screen_confirmed)
        app.handle(DISPLAY_ACK, 95, b'\x01')
        self.assertIsNone(app.display_waiting)
        self.assertTrue(app.turn_screen_confirmed)


if __name__ == '__main__':
    unittest.main()
