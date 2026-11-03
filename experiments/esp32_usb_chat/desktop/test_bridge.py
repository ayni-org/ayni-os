import json
import struct
import unittest
from unittest.mock import Mock, patch
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

    def test_model_response_keeps_both_inspections_and_original_ids(self):
        calls = [{'id': identity, 'type': 'function', 'function': {
            'name': 'ayni_inspect_capability', 'arguments': json.dumps({
                'binding_id': binding, 'binding_version': 1})}}
            for identity, binding in [('screen-id', 'screen.show_text'),
                                      ('speech-id', 'speaker.speak_text')]]
        response = Mock()
        response.read.return_value = json.dumps({'choices': [{
            'finish_reason': 'tool_calls', 'message': {'tool_calls': calls}}]}).encode()
        opener = Mock()
        opener.open.return_value.__enter__ = Mock(return_value=response)
        opener.open.return_value.__exit__ = Mock(return_value=False)
        with patch.object(bridge.urllib.request, 'build_opener', return_value=opener):
            kind, packet = bridge.request_model({
                'url': bridge.DEFAULT_URL, 'model': bridge.DEFAULT_MODEL, 'key': 'test-only'},
                {'max_tokens': 1800, 'messages': [], 'tools': []})
        self.assertEqual(kind, MODEL_TOOLS)
        self.assertEqual(struct.unpack_from('<HHHH', packet), (0, 0, 2, 1))
        offset = 8
        for call in calls:
            size, = struct.unpack_from('<I', packet, offset)
            offset += 4
            self.assertEqual(packet[offset:offset+size], bridge.tool_packet(call))
            offset += size
        self.assertEqual(offset, len(packet))

    def test_request_keeps_device_budget_and_tools_only_option(self):
        response = Mock()
        response.read.return_value = json.dumps({'choices': [{
            'finish_reason': 'stop', 'message': {'content': '摘要'}}]}).encode()
        opener = Mock()
        opener.open.return_value.__enter__ = Mock(return_value=response)
        opener.open.return_value.__exit__ = Mock(return_value=False)
        config = {'url': bridge.DEFAULT_URL, 'model': bridge.DEFAULT_MODEL, 'key': 'test-only'}
        with patch.object(bridge.urllib.request, 'build_opener', return_value=opener):
            self.assertEqual(bridge.request_model(config, {'max_tokens': 900, 'messages': []}),
                             (MODEL_TEXT, '摘要'.encode()))
        sent = json.loads(opener.open.call_args.args[0].data)
        self.assertEqual(sent['max_tokens'], 900)
        self.assertNotIn('parallel_tool_calls', sent) # A text-only summary has no tools.
        long = {'choices': [{'finish_reason': 'stop', 'message': {'content': '字' * 2000}}]}
        response.read.return_value = json.dumps(long).encode()
        with patch.object(bridge.urllib.request, 'build_opener', return_value=opener):
            # 6000 bytes: a summary may be that long, a tool-calling turn's answer may not.
            self.assertEqual(bridge.request_model(config, {'max_tokens': 900, 'messages': []})[0],
                             MODEL_TEXT)
            with self.assertRaises(ValueError):
                bridge.request_model(config, {'max_tokens': 900, 'messages': [], 'tools': []})
        for limit in (None, 0, 8193, '1800', True):
            with self.subTest(limit=limit), self.assertRaises(ValueError):
                bridge.request_model(config, {'max_tokens': limit, 'messages': []})

    def test_tool_batch_rejects_duplicate_ids_and_capacity_without_truncation(self):
        call = {'id': 'same', 'function': {'name': 'ayni_list_capabilities', 'arguments': '{}'}}
        for calls in ([], [call, call], [call]*9):
            with self.subTest(count=len(calls)), self.assertRaises(ValueError):
                bridge.tool_packets(calls)
        large = [{'id': str(i), 'function': {'name': 'ayni_call_capability',
                   'arguments': json.dumps({'text': 'x'*4200})}} for i in range(2)]
        with self.assertRaises(ValueError):
            bridge.tool_packets(large)
        self.assertEqual(bridge.tool_packets([call]), bridge.tool_packet(call))

    def test_confirmed_shutdown_is_status_without_error(self):
        events = Mock()
        app = bridge.Bridge({}, events)
        app.ready, app.pending_user = True, 'closing'
        app.handle(STATUS, 0, b'stopped; reset board before reconnecting')
        self.assertTrue(app.closed)
        self.assertFalse(app.ready)
        self.assertIsNone(app.pending_user)
        self.assertFalse(any(call.args[0] == 'error' for call in events.call_args_list))

    def test_answer_does_not_trigger_screen(self):
        app = bridge.Bridge({}, Mock())
        app.turn, app.pending_user = 1, 'hello'
        app.screen = Mock()
        app.handle(ANSWER, 1, b'answer')
        app.screen.assert_not_called()
        self.assertIsNone(app.pending_user)

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

    def test_user_text_goes_to_device_unframed(self):
        app = bridge.Bridge({}, Mock())
        app.ready = True
        app.send = Mock()
        app.user('中文')
        app.send.assert_called_once_with(USER, 1, '中文'.encode())
        with self.assertRaises(ValueError):
            app.user('x') # One turn at a time.
        app.pending_user = None
        with self.assertRaises(ValueError):
            app.user('字' * 1366) # 4098 bytes.

    def test_clear_asks_device_and_reports_its_answer(self):
        events = Mock()
        app = bridge.Bridge({}, events)
        app.ready = True
        app.send = Mock()
        app.forget()
        app.send.assert_called_once_with(FORGET, 0)
        app.handle(STATUS, 0, b'context cleared on device')
        events.assert_any_call('status', 'context cleared on device')
        app.handle(STATUS, 0, b'context clear refused: device busy')
        events.assert_any_call('error', 'context clear refused: device busy')
        app.pending_user = 'running'
        with self.assertRaises(ValueError):
            app.forget()

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

    def test_offline_fixture_counts_only_this_turns_results(self):
        app = bridge.Bridge({}, Mock(), offline=True)
        earlier = [{'role': 'user', 'content': '上一轮'}] + [
            {'role': 'tool', 'tool_call_id': str(at), 'content': '{}'} for at in range(3)] + [
            {'role': 'assistant', 'content': '完成'}]
        request = {'max_tokens': 1800, 'tools': [],
                   'messages': earlier + [{'role': 'user', 'content': '这一轮'}]}
        app.handle(MODEL_REQUEST, 41, json.dumps(request).encode())
        identity, kind, packet, error = app.completed.get(timeout=5)
        self.assertEqual((identity, kind, error), (41, MODEL_TOOLS, None))
        self.assertIn(b'ayni_list_capabilities', packet) # The plan starts again.
        app.current = None
        summary = {'max_tokens': 1800, 'messages': earlier} # No tools: text only.
        app.handle(MODEL_REQUEST, 42, json.dumps(summary).encode())
        self.assertEqual(app.completed.get(timeout=5)[:2], (42, MODEL_TEXT))

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
        app.handle(DISPLAY_ACK, 95, b'\x01')
        self.assertIsNone(app.display_waiting)


if __name__ == '__main__':
    unittest.main()
