"""One USB connection and one immutable model config. Keys remain in process memory."""
import argparse
import hashlib
import json
from pathlib import Path
import queue
import ssl
import subprocess
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
from protocol import *
from audio import AudioTransfer, synthesize
from motor import MotorReview

DEFAULT_URL = 'https://api.deepseek.com'
DEFAULT_MODEL = 'deepseek-flash'
ANSWER_LIMIT = 4096
PROMPT_LIMIT = 16384
HTTP_LIMIT = 131072
TOOL_COUNT_LIMIT = 8
TOOL_WIRE_LIMIT = 8192
SYSTEM = ('你是运行在 ESP32-S3 上的 Ayni 助手。你必须调用系统的屏幕能力来显示给用户的完整回复。'
          '先 ayni_list_capabilities，再 ayni_inspect_capability 查看精确绑定与用法，'
          '最后用 ayni_call_capability 调用屏幕能力，arguments 中包含完整回复 text。'
          '每轮最多提议 8 个工具调用，系统按提议顺序执行并返回真实工具结果；成功后用简短文字确认。'
          '只有用户要求声音时，才发现并调用 speaker.play_tone 提示音或 speaker.speak_text 中文朗读。'
          '扬声器默认音量 80%，可按用户要求指定 volume_percent；朗读内容应简短、最多 30 秒。'
          '电机能力 motor.status 查询、motor.plan 仅计划；用户要求转动时发现并调用 motor.run。'
          'motor.run 的两个百分比为 PWM 占空比 -90..90，零表示对应电机不动，时长 50..5000ms 且为5的倍数；'
          '每次实际调用都必须等待 Mac 人工批准；结果只证明控制器输出结束，不证明轮速或距离。不要把 plan 当作已转动。'
          '只能人工架空台架实验；不支持无人运动，停止链尚未完成硬件验证。'
          '只有用户明确要求才可转动；速度与时间不得超出用户要求。两只分别测试应逐次调用，另一只设零。'
          '拒绝、超时、取消或结果未知的动作不得自动重试。不要宣称达到实测转速、距离或角度。'
          '如需停止可调用 motor.stop；该工具成功只代表停止请求已提交。'
          '没有 shell，不要调用。屏幕中文正文 UTF-8 总长最多 4096 字节，不超过 1200 个汉字。')

class Number(str):
    """An original JSON numeric lexeme, never a float intermediate."""


def tool_packet(call):
    nodes = []
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('模型工具参数包含重复字段')
            result[key] = value
        return result
    value = json.loads(call['function']['arguments'], parse_int=Number, parse_float=Number,
                       object_pairs_hook=pairs)
    def visit(value, name='', depth=0):
        if depth > 8 or len(nodes) >= 32:
            raise ValueError('模型工具参数深度或节点数超限')
        index = len(nodes)
        node = [0, 0, name.encode(), b'']
        nodes.append(node)
        if isinstance(value, dict):
            node[0] = 1
            for key, child in value.items():
                visit(child, key, depth+1)
        elif isinstance(value, list):
            node[0] = 2
            for child in value:
                visit(child, '', depth+1)
        elif isinstance(value, bool):
            node[0], node[3] = 3, b'1' if value else b'0'
        elif isinstance(value, Number):
            node[0], node[3] = 4, value.encode()
        elif isinstance(value, str):
            node[0], node[3] = 5, value.encode()
        elif value is not None:
            raise ValueError('不支持的模型工具参数')
        node[1] = len(nodes)
    visit(value)
    identity, name = call['id'].encode(), call['function']['name'].encode()
    if not 0 < len(identity) <= 128 or not 0 < len(name) <= 32:
        raise ValueError('模型工具身份超限')
    result = struct.pack('<HHHH', len(identity), len(name), len(nodes), 0) + identity + name
    for kind, end, name, raw in nodes:
        result += struct.pack('<IIHH', kind, end, len(name), len(raw)) + name + raw
    if len(result) > 8192:
        raise ValueError('模型工具提议超过 8192 字节')
    return result


def frame_messages(messages):
    result = bytearray(b'\x1eAYNI-MESSAGES/1\n')
    for message in messages:
        raw = message['content'].encode()
        result.extend(f"{message['role']} {len(raw)}\n".encode())
        result.extend(raw)
    return bytes(result)


def tool_packets(calls):
    if not isinstance(calls, list) or not 0 < len(calls) <= TOOL_COUNT_LIMIT:
        raise ValueError('模型工具提议数量必须为 1..8；不会截断或执行部分提议')
    identities = [call['id'] for call in calls]
    if len(set(identities)) != len(identities):
        raise ValueError('同一模型响应的工具 ID 重复；整组拒绝')
    packets = [tool_packet(call) for call in calls]
    if len(packets) == 1:
        return packets[0]
    # An impossible single-call ID length (zero) selects the additive batch-v1 form.
    result = struct.pack('<HHHH', 0, 0, len(packets), 1)
    for packet in packets:
        result += struct.pack('<I', len(packet)) + packet
    if len(result) > TOOL_WIRE_LIMIT:
        raise ValueError('模型工具组超过 8192 字节；不会截断或执行部分提议')
    return result



class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def request_model(config, context):
    parsed = urllib.parse.urlsplit(config['url'])
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError('API 地址必须是无凭据、无查询参数的 HTTPS 根地址')
    endpoint = config['url'].rstrip('/') + '/chat/completions'
    body = dict(context)
    body.update(model=config['model'], stream=False, max_tokens=1800, parallel_tool_calls=False)
    if parsed.hostname == 'api.deepseek.com':
        body['thinking'] = {'type': 'disabled'}
    request = urllib.request.Request(endpoint, json.dumps(body, ensure_ascii=False).encode(),
        {'Content-Type': 'application/json', 'Authorization': 'Bearer ' + config['key']}, method='POST')
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
        urllib.request.HTTPSHandler(context=ssl.create_default_context()), NoRedirect())
    try:
        with opener.open(request, timeout=70) as response:
            raw = response.read(HTTP_LIMIT + 1)
    except urllib.error.HTTPError as error:
        # Never echo vendor error bodies: they can repeat credentials or prompt data.
        raise ValueError(f'模型服务返回 HTTP {error.code}；请求不会自动重试') from None
    except (urllib.error.URLError, TimeoutError, OSError):
        raise ValueError('模型网络/TLS 请求失败，远端是否已受理未知；不会自动重试') from None
    if len(raw) > HTTP_LIMIT:
        raise ValueError('模型响应 JSON 超过 128 KiB')
    value = json.loads(raw)
    choice = value['choices'][0]
    if choice.get('finish_reason') not in ('stop', 'tool_calls'):
        raise ValueError('模型未完整结束回复（可能达到输出上限）；不会把截断内容当成成功')
    message = choice['message']
    if message.get('tool_calls'):
        return MODEL_TOOLS, tool_packets(message['tool_calls'])
    answer = message.get('content')
    if not isinstance(answer, str) or not answer or len(answer.encode()) > ANSWER_LIMIT:
        raise ValueError('模型回复为空或超过 4096 字节；不会静默截断')
    return MODEL_TEXT, answer.encode()


class Bridge:
    def __init__(self, config, emit, renderer=None, offline=False, speaker_check=False, batch_check=False, motor_check=False):
        self.config = dict(config)
        self.emit = emit
        self.renderer = renderer
        self.offline = offline
        self.speaker_check = speaker_check
        self.batch_check = batch_check
        self.motor_check = motor_check
        if motor_check and not offline:
            raise ValueError("motor_check requires an offline model fixture")
        self.port = None
        self.decoder = Decoder()
        self.write_lock = threading.Lock()
        self.completed = queue.Queue()
        self.current = None
        self.history = []
        self.pending_user = None
        self.turn = 0
        self.ready = False
        self.closed = False
        self.configured = False
        self.motor = MotorReview(emit)
        self.motor_heartbeat_at = 0.0
        self.motor_settings_sent = False
        self.driver_power_off_confirmed = False
        self.shutting_down = False
        self.page = 0
        self.pages = []
        self.display_id = 0
        self.document_call = 0
        self.display_waiting = None
        self.turn_screen_text = None
        self.turn_screen_confirmed = False
        self.speech_results = queue.Queue()
        self.speech_synthesizing = None
        self.speech_cancelled = None
        self.speech_cancel = threading.Event()
        self.speech_thread = None
        self.audio_transfer = None
        self.audio_deadline = 0.0

    def connect(self):
        if self.config.get("driver_power_off") is not True:
            raise ValueError("串口打开可能复位 ESP32；请先确认驱动 10V 电源关闭、两轮静止")
        import serial
        from serial.tools.list_ports import comports
        parsed = urllib.parse.urlsplit(self.config['url'])
        if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError('API 地址必须是无凭据、无查询参数的 HTTPS 根地址')
        if not self.offline and not self.config.get('key', '').strip():
            raise ValueError('请在本地窗口填写 API Key')
        key = self.config.get('key', '')
        if not self.offline and (len(key) > 512 or not key.isascii() or any(ord(c) <= 32 or ord(c) >= 127 for c in key)):
            raise ValueError('API Key 必须是不含空格、换行或控制字符的 ASCII 字符串')
        if not self.config.get('model', '').strip():
            raise ValueError('请填写模型名')
        matches = [p for p in comports() if p.device == self.config['port']]
        if not matches or (matches[0].vid, matches[0].pid, matches[0].serial_number) != (0x1a86, 0x55d3, '5D01368694'):
            raise ValueError('串口身份与已验证设备不一致')
        port = serial.Serial(port=None, baudrate=921600, timeout=0.03, write_timeout=5, exclusive=True)
        port.dtr = False
        port.rts = False
        port.port = self.config['port']
        port.open()
        self.port = port
        self.driver_power_off_confirmed = False
        self.emit('status', '已打开 USB；等待设备握手')
        self.send(CONFIGURE, 0, self.route())

    def route(self):
        # Includes only the public route/model; it is not a credential fingerprint.
        identity = self.config['url'].rstrip('/') + '\n' + self.config['model']
        if self.offline:
            identity += '\noffline-device-check'
        return hashlib.sha256(identity.encode()).hexdigest()[:16].encode()

    def send(self, kind, identity, payload=b''):
        with self.write_lock:
            frame = encode(kind, identity, payload)
            written = self.port.write(frame)
            if written != len(frame):
                raise ValueError('串口写入不完整；不会自动重发模型请求')

    def stop_motor(self):
        self.motor.stop()
        self.send(MOTOR_STOP, 0)

    def motor_settings(self, limit):
        if type(limit) is not int or not 1 <= limit <= 90:
            raise ValueError('PWM 上限必须为 1..90 的整数')
        if self.motor.active is not None or self.motor.latched:
            raise ValueError('电机忙或停止已锁定；不能更改上限')
        self.config['motor_max_percent'] = limit
        self.send(MOTOR_SETTINGS, 0, struct.pack('<I', limit))

    def motor_answer(self, command, phase):
        identity, packet = self.motor.answer(command, phase)
        self.send(MOTOR_DECISION if phase == 1 else MOTOR_OBSERVED, identity, packet)

    def close(self, timeout=5, driver_power_off=False):
        if driver_power_off is not True:
            self.stop_motor()
            raise ValueError('请先关闭驱动 10V 电源并确认两轮静止，再关闭 USB 会话')
        self.driver_power_off_confirmed = True
        self.shutting_down = True
        self.motor.stop()
        self.speech_cancel.set()
        try:
            self.send(MOTOR_POWER_OFF, 0)
            self.send(SHUTDOWN, 0)
            self.port.flush()
            deadline = time.monotonic() + timeout
            while not self.closed and time.monotonic() < deadline:
                self.poll()
            if not self.closed:
                self.emit('error', '驱动已由人工断电；未收到 Ayni 完整关闭回执，保留日志，不自动重放')
        finally:
            # Only the explicit physical-power-off statement authorizes a reset-prone close.
            self.port.close()
            if self.speech_thread is not None:
                self.speech_thread.join()
            self.config['key'] = ''
            self.emit('closed', True)

    def user(self, text):
        if not self.ready or self.pending_user is not None or self.current is not None:
            raise ValueError('设备尚未就绪或上一轮尚未结束')
        if not text.strip() or len(text.encode()) > 4096:
            raise ValueError('输入不能为空且必须不超过 4096 字节')
        messages = [{'role': 'system', 'content': SYSTEM}] + self.history + [{'role': 'user', 'content': text}]
        body = frame_messages(messages)
        if len(body) > PROMPT_LIMIT:
            raise ValueError('会话已达到 16 KiB；请清空电脑端会话后继续')
        self.turn += 1
        self.pending_user = text
        self.turn_screen_text = None
        self.turn_screen_confirmed = False
        self.send(USER, self.turn, body)
        self.emit('busy', True)

    def handle(self, kind, identity, payload):
        if kind == HELLO:
            if self.configured and self.ready:
                self.ready = False
                self.stop_motor()
                self.emit('ready', False)
                self.emit('error', '会话中检测到 ESP32 重新握手；已锁定电机，不重放动作。请关闭驱动电源后重新连接。')
            elif not self.configured:
                self.send(CONFIGURE, 0, self.route())
                self.configured = True
        elif kind == STATUS:
            status = payload.decode('utf-8', 'strict')
            self.emit('status', status)
            if status.startswith('stopped;'):
                self.closed = True
                self.ready = False
                self.emit('ready', False)
                self.pending_user = None
                self.emit('busy', self.current is not None)
                return # A confirmed normal shutdown is not a turn/provider error.
            if status == 'ready':
                self.ready = True
                self.emit('ready', True)
                if not self.motor_settings_sent:
                    self.motor_settings(self.config.get('motor_max_percent', 90))
                    self.motor_settings_sent = True

            elif status.startswith('task held '):
                # Held is still the same live Goal; the owner may cancel it.
                self.emit('busy', True)
                self.emit('error', status)
            elif any(status.startswith(s) for s in ('turn failed/cancelled', 'input refused:', 'input error=', 'initialize failed=', 'kernel stopped/held=', 'stopped;')):
                self.pending_user = None
                self.emit('busy', self.current is not None)
                if status.startswith(('initialize failed=', 'kernel stopped/held=')):
                    self.ready = False
                    self.emit('ready', False)
                if self.current is not None:
                    self.emit('status', '设备任务已结束；等待旧模型网络请求结束后才能发送下一条，不会自动重试')
                self.emit('error', status)
            elif any(s in status for s in ('failed', 'error=')):
                # A Provider diagnostic precedes the Core Task's terminal result.
                self.emit('error', status)
        elif kind in (MOTOR_REVIEW, MOTOR_OBSERVE):
            try:
                self.motor.receive(identity, payload, 1 if kind == MOTOR_REVIEW else 2)
            except ValueError:
                self.stop_motor()
                raise
        elif kind == MOTOR_STATE:
            state = json.loads(payload)
            if state.get('busy') is False:
                self.motor.finished(identity)
            self.emit('motor_state', state)
        elif kind == MODEL_REQUEST:
            if self.current is not None:
                self.send(MODEL_ERROR, identity, b'busy')
                return
            value = json.loads(payload)
            messages = value['messages']
            if not isinstance(messages, list) or any(not isinstance(m, dict) or m.get('role') not in ('user', 'assistant', 'system', 'tool') for m in messages):
                self.send(MODEL_ERROR, identity, b'invalid messages')
                return
            self.current = identity
            self.emit('status', 'ESP32 的 Ayni 已派发模型调用')
            def work():
                try:
                    if self.offline:
                        count = sum(m['role'] == 'tool' for m in messages)
                        screen_text = '屏幕能力检查通过。' + chr(10)*2 + chr(10).join(f'第{i:02d}段：屏幕是 Ayni 能力，模型提议、Core 校验、设备执行。' for i in range(1,36))
                        plan = [('ayni_list_capabilities', {}),
                            ('ayni_inspect_capability', {'binding_id':'screen.show_text','binding_version':1}),
                            ('ayni_call_capability', {'binding_id':'screen.show_text','binding_version':1,'arguments':{'text':screen_text}})]
                        if self.speaker_check:
                            verified = []
                            if self.batch_check and count == 7:
                                verified = [m['content'] for m in messages if m['role'] == 'tool'][-3:-1]
                            elif not self.batch_check and count in (3, 5):
                                verified = [[m for m in messages if m['role'] == 'tool'][-1]['content']]
                            for content in verified:
                                facts = json.loads(content)
                                if not (facts.get('played') is True and facts.get('volume_percent') == 80
                                        and facts.get('confirmed') == 'i2s-drained' and facts.get('samples', 0) > 0):
                                    raise ValueError('扬声器正常检查未收到默认 80% 的实际 I²S 完成结果')
                                self.emit('speaker_verified', facts)
                            plan = [('ayni_list_capabilities', {}),
                                ('ayni_inspect_capability', {'binding_id':'speaker.play_tone','binding_version':1}),
                                ('ayni_call_capability', {'binding_id':'speaker.play_tone','binding_version':1,'arguments':{'frequency_hz':660,'duration_ms':300}}),
                                ('ayni_inspect_capability', {'binding_id':'speaker.speak_text','binding_version':1}),
                                ('ayni_call_capability', {'binding_id':'speaker.speak_text','binding_version':1,'arguments':{'text':'你好，我是运行在 ESP32 上的 Ayni 助手。'}}),
                                ('ayni_inspect_capability', {'binding_id':'screen.show_text','binding_version':1}),
                                ('ayni_call_capability', {'binding_id':'screen.show_text','binding_version':1,'arguments':{'text':'扬声器能力检查：提示音与中文朗读完成，软件音量默认为百分之八十。'}})]
                        if self.motor_check:
                            first = 0 if self.motor_check == 'output2' else 50
                            second = 50 if self.motor_check == 'output2' else 0
                            duration = 5000 if self.motor_check in ('output1', 'output2') else 50
                            plan = [('ayni_list_capabilities', {}),
                                ('ayni_inspect_capability', {'binding_id':'motor.run','binding_version':1}),
                                ('ayni_call_capability', {'binding_id':'motor.run','binding_version':1,
                                    'arguments':{'motor1_percent':first,'motor2_percent':second,'duration_ms':duration}})]
                        groups = [[step] for step in plan]
                        if self.batch_check and self.speaker_check:
                            groups = [[plan[0]], [plan[1], plan[3], plan[5]], [plan[2], plan[4], plan[6]]]
                        completed, next_group = 0, None
                        for group in groups:
                            if completed == count:
                                next_group = group
                                break
                            completed += len(group)
                        if next_group is not None:
                            calls = [{'id': f'check_{identity}_{count}_{at}', 'function': {
                                'name': name, 'arguments': json.dumps(arguments, ensure_ascii=False)}}
                                for at, (name, arguments) in enumerate(next_group)]
                            answer = tool_packets(calls)
                            kind = MODEL_TOOLS
                            self.emit('offline_tool_group', {'count': len(calls), 'ids': [c['id'] for c in calls]})
                        else:
                            if count != len(plan):
                                raise ValueError('离线检查的工具组/结果数量不一致')
                            kind, answer = MODEL_TEXT, '设备能力已返回实际输出完成回执。'.encode()
                    else:
                        kind, answer = request_model(self.config, value)
                    self.completed.put((identity, kind, answer, None))
                except Exception as error:
                    safe = str(error) if isinstance(error, ValueError) else '模型响应格式错误；不会自动重试'
                    self.completed.put((identity, MODEL_ERROR, None, safe))
            threading.Thread(target=work, daemon=True).start()
        elif kind == ANSWER:
            if identity != self.turn or self.pending_user is None:
                return
            answer = payload.decode('utf-8', 'strict')
            remembered = self.turn_screen_text if self.turn_screen_confirmed else answer
            self.history.extend([{'role': 'user', 'content': self.pending_user}, {'role': 'assistant', 'content': remembered}])
            self.pending_user = None
            self.emit('answer', answer)
            self.emit('busy', False)

        elif kind == RENDER_TEXT:
            # A new authorized document supersedes any old page refresh. Old
            # acknowledgements cannot complete the new Call because IDs differ.
            self.display_waiting = None
            self.document_call = identity
            self.turn_screen_text = payload.decode('utf-8', 'strict')
            self.turn_screen_confirmed = False
            self.emit('screen_text', payload.decode('utf-8', 'strict'))
            self.screen(payload.decode('utf-8', 'strict'))
        elif kind == SPEECH_REQUEST:
            if self.speech_synthesizing is not None or self.audio_transfer is not None:
                self.send(AUDIO_ERROR, identity, b'local speech backend busy')
                return
            text = payload.decode('utf-8', 'strict')
            self.speech_synthesizing = identity
            cancelled = threading.Event()
            self.speech_cancel = cancelled
            self.emit('status', '扬声器能力请求 Mac 本地合成中文语音')
            def speech_work():
                try:
                    self.speech_results.put((identity, synthesize(text, cancelled), None))
                except Exception as error:
                    self.speech_results.put((identity, None, str(error) if isinstance(error, ValueError) else '本地语音合成失败'))
            self.speech_thread = threading.Thread(target=speech_work)
            self.speech_thread.start()
        elif kind == AUDIO_ACK:
            transfer = self.audio_transfer
            if transfer is None or transfer.identity != identity:
                return
            try:
                chunk = transfer.acknowledge(payload)
                if chunk is None:
                    self.send(AUDIO_END, identity)
                    self.audio_transfer = None
                    self.emit('status', 'PCM 已完整交付 ESP32，等待 I²S 实际播放结果')
                else:
                    self.send(AUDIO_CHUNK, identity, chunk)
                    self.audio_deadline = time.monotonic() + 10
            except ValueError as error:
                self.audio_transfer = None
                self.send(AUDIO_ERROR, identity, b'PCM ACK mismatch')
                self.emit('error', str(error))
        elif kind == AUDIO_CANCEL:
            if self.speech_synthesizing == identity:
                self.speech_cancelled = identity
                self.speech_cancel.set()
            if self.audio_transfer is not None and self.audio_transfer.identity == identity:
                self.audio_transfer = None
            self.emit('status', '设备取消了当前扬声器调用；不会自动重放')
        elif kind == DISPLAY_ACK:
            if identity == self.display_waiting:
                self.display_waiting = None
                self.turn_screen_confirmed = payload == b'\x01'
                self.emit('display', {'page': self.page + 1, 'count': len(self.pages), 'ok': payload == b'\x01'})

    def poll(self):
        self.motor.expire()
        if self.ready and self.motor_settings_sent and not self.shutting_down and time.monotonic() >= self.motor_heartbeat_at:
            self.send(MOTOR_HEARTBEAT, 0)
            self.motor_heartbeat_at = time.monotonic() + 0.5
        data = self.port.read(min(self.port.in_waiting or 1, 8192))
        for packet in self.decoder.feed(data):
            self.handle(*packet)
        while True:
            try:
                identity, pcm, error = self.speech_results.get_nowait()
            except queue.Empty:
                break
            if self.speech_synthesizing != identity:
                continue
            self.speech_synthesizing = None
            if self.speech_cancelled == identity:
                self.speech_cancelled = None
                continue
            if error:
                self.send(AUDIO_ERROR, identity, b'local speech synthesis failed')
                self.emit('error', error)
            else:
                self.audio_transfer = AudioTransfer(identity, pcm)
                self.send(AUDIO_BEGIN, identity, self.audio_transfer.begin())
                self.audio_deadline = time.monotonic() + 10
        if self.audio_transfer is not None and time.monotonic() >= self.audio_deadline:
            identity = self.audio_transfer.identity
            self.audio_transfer = None
            self.send(AUDIO_ERROR, identity, b'PCM transfer timeout')
            self.emit('error', '音频分块传输超时；不会自动重发')
        while True:
            try:
                identity, kind, answer, error = self.completed.get_nowait()
            except queue.Empty:
                break
            if self.current != identity:
                continue
            self.current = None
            if self.pending_user is None:
                self.emit('status', '旧模型请求已结束，其晚到结果已丢弃；可以发送下一条')
                self.emit('busy', False)
                continue
            if error:
                self.emit('error', error)
                self.send(MODEL_ERROR, identity, b'network/result failure')
            else:
                self.send(kind, identity, answer)

    def screen(self, text):
        if not self.renderer:
            return
        # Native CoreText supports arbitrary Chinese and punctuation without shipping fonts.
        result = subprocess.run([self.renderer, '--render'], input=json.dumps({'text': text}).encode(),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=15)
        import base64
        self.pages = [base64.b64decode(page, validate=True) for page in json.loads(result.stdout)['pages']]
        self.page = 0
        self.send(RENDER_INFO, self.document_call, struct.pack("<I", len(self.pages)))
        self.show_page(0)

    def show_page(self, page):
        if self.display_waiting is not None:
            raise ValueError('屏幕仍在刷新，请稍后翻页')
        if not 0 <= page < len(self.pages):
            return
        self.page = page
        self.display_id += 1
        self.display_waiting = self.document_call
        self.send(DISPLAY, self.document_call, self.pages[page])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--renderer')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--speaker-check', action='store_true')
    parser.add_argument('--motor-check', choices=['decline', 'output1', 'output2'])
    args = parser.parse_args()
    if args.motor_check and not args.offline:
        parser.error('--motor-check requires --offline and driver power OFF')
    if args.speaker_check and not args.offline:
        parser.error('--speaker-check requires --offline')
    output_lock = threading.Lock()
    def emit(kind, value):
        with output_lock:
            try:
                print(json.dumps({'type': kind, 'value': value}, ensure_ascii=False), flush=True)
            except BrokenPipeError:
                pass # Keep the port open after a UI crash until physical power is cut.
    commands = queue.Queue()
    def read_commands():
        for line in sys.stdin:
            try:
                commands.put(json.loads(line))
            except ValueError:
                emit('error', '本地命令格式错误')
        commands.put({'action': 'stdin_closed'})
    threading.Thread(target=read_commands, daemon=True).start()
    bridge = None
    while True:
        try:
            try:
                command = commands.get(timeout=0.01 if bridge is None else 0)
            except queue.Empty:
                command = None
            if command:
                action = command['action']
                if action == 'connect':
                    if bridge:
                        raise ValueError('本次连接的模型配置已经冻结；更改需断开并重启设备')
                    candidate = Bridge(command, emit, args.renderer, args.offline, args.speaker_check, motor_check=args.motor_check)
                    candidate.connect()
                    bridge = candidate
                elif action == 'send':
                    if not bridge:
                        raise ValueError('请先连接设备')
                    bridge.user(command['text'])
                elif action == 'page' and bridge:
                    bridge.show_page(command['index'])
                elif action == 'clear' and bridge:
                    if bridge.pending_user is not None:
                        raise ValueError('请等待当前任务结束')
                    bridge.history.clear()
                    emit('status', '已清空电脑端后续请求的会话上下文；设备持久日志保留')
                elif action == 'cancel' and bridge:
                    bridge.send(CANCEL, bridge.turn)
                elif action == 'motor_settings' and bridge:
                    bridge.motor_settings(command['limit'])
                elif action == 'motor_decision' and bridge:
                    bridge.motor_answer(command, 1)
                elif action == 'motor_observed' and bridge:
                    bridge.motor_answer(command, 2)
                elif action == 'motor_stop' and bridge:
                    bridge.stop_motor()
                elif action == 'stdin_closed':
                    if bridge and not bridge.driver_power_off_confirmed:
                        bridge.stop_motor()
                        emit('error', '窗口输入已断开；串口保留以避免复位，请关闭驱动电源。')
                    else:
                        return
                elif action == 'exit':
                    if bridge:
                        try:
                            bridge.close(driver_power_off=command.get('driver_power_off', False))
                        except Exception as error:
                            if not bridge.driver_power_off_confirmed:
                                raise
                            emit('error', '驱动已由人工断电；USB/关闭回执不可确认，不会自动重放')
                    return
            if bridge:
                bridge.poll()
        except Exception as error:
            if bridge and not isinstance(error, ValueError):
                try:
                    bridge.stop_motor()
                except Exception:
                    pass # The finite waveform may end, but disconnected USB proves no stop.
                bridge.ready = False
                emit('ready', False)
            emit('error', str(error) if isinstance(error, ValueError) else 'USB/显示操作失败；请切断驱动电源，当前停止状态未知')
            time.sleep(0.1)


if __name__ == '__main__':
    main()
