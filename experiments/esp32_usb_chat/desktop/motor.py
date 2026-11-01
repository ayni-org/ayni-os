"""Volatile approval correlation for actual Core attempts; never originates a motor command."""
import struct
import time

REVIEW = struct.Struct('<5Q2i4I')
DECISION = struct.Struct('<5Q2I')


class MotorReview:
    def __init__(self, emit, clock=time.monotonic):
        self.emit = emit
        self.clock = clock
        self.pending = None
        self.active = None
        self.last_nonce = 0
        self.latched = False

    def receive(self, permit, payload, phase):
        if len(payload) != REVIEW.size:
            raise ValueError('电机确认帧长度错误')
        nonce, call, operation, boot, request, first, second, duration, limit, wire_phase, reserved = REVIEW.unpack(payload)
        if (not all((permit, nonce, call, operation, boot, request)) or reserved or wire_phase != phase
                or phase not in (1, 2) or not 1 <= limit <= 90 or not 50 <= duration <= 5000
                or duration % 5 or not (first or second) or max(abs(first), abs(second)) > limit):
            raise ValueError('电机确认参数无效，已要求停止')
        key = (permit, payload[:40], first, second, duration, limit)
        if self.pending and self.pending[0] == phase and self.pending[1] == key:
            return # Exact retransmission cannot open a second approval window.
        if phase == 1:
            if self.latched or self.active is not None or nonce <= self.last_nonce:
                raise ValueError('重复、过期或并发电机提议；已要求停止')
            self.last_nonce = nonce
            self.active = key
        elif self.active != key:
            raise ValueError('电机停止观察与已批准动作不匹配')
        self.pending = (phase, key, self.clock() + 30)
        self.emit('motor_review' if phase == 1 else 'motor_observe', {
            'permit': str(permit), 'ticket': payload[:40].hex(),
            'motor1_percent': first, 'motor2_percent': second,
            'duration_ms': duration, 'max_percent': limit})

    def answer(self, command, phase):
        if not self.pending:
            raise ValueError('电机确认已失效')
        expected_phase, key, deadline = self.pending
        permit, ticket, *_ = key
        if (phase != expected_phase or command.get('permit') != str(permit)
                or command.get('ticket') != ticket.hex() or type(command.get('approved')) is not bool
                or self.clock() >= deadline):
            raise ValueError('电机确认身份或期限不匹配；不会执行')
        approved = command['approved']
        self.pending = None
        if phase == 2 or not approved:
            self.active = None
        if phase == 2 and not approved:
            self.latched = True
        return permit, ticket + struct.pack('<II', approved, 0)

    def expire(self):
        if self.pending and self.clock() >= self.pending[2]:
            phase, key, _ = self.pending
            self.pending = None
            self.active = None
            self.latched = self.latched or phase == 2
            self.emit('motor_dismiss', '确认超时；本次不再接受批准。请核对电机状态。')

    def finished(self, permit):
        if self.active is not None and self.active[0] == permit:
            self.pending = None
            self.active = None
            self.emit('motor_dismiss', '本次电机调用已结束；旧确认失效。')

    def stop(self):
        self.latched = True
        self.pending = None
        self.emit('motor_dismiss', '已请求锁定停止；重新连接前必须关闭驱动电源。')
