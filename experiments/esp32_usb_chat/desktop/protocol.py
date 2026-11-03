"""Experiment wire v4: bounded length, per-payload CRC32, unique 64-bit correlation."""
import struct
import time
import zlib

HEADER = struct.Struct('<4sBBHQII')
MAGIC = b'AYN1'
VERSION = 4
MAX_PAYLOAD = 240 * 320 * 2
HELLO, USER, MODEL_REQUEST, MODEL_TEXT, MODEL_ERROR, ANSWER, STATUS, DISPLAY, DISPLAY_ACK, CANCEL, SHUTDOWN, CONFIGURE, MODEL_TOOLS, RENDER_TEXT, RENDER_INFO = range(1, 16)
SPEECH_REQUEST, AUDIO_BEGIN, AUDIO_CHUNK, AUDIO_END, AUDIO_ACK, AUDIO_ERROR, AUDIO_CANCEL = range(16, 23)

MOTOR_REVIEW, MOTOR_DECISION, MOTOR_OBSERVE, MOTOR_OBSERVED, MOTOR_SETTINGS, MOTOR_STOP, MOTOR_POWER_OFF, MOTOR_HEARTBEAT, MOTOR_STATE = range(23, 32)
# Mac -> device: erase the device-held conversation record ("clear context").
FORGET = 32


def encode(kind, identity, payload=b''):
    if not isinstance(payload, bytes) or len(payload) > MAX_PAYLOAD:
        raise ValueError('payload capacity exceeded')
    return HEADER.pack(MAGIC, VERSION, kind, 0, identity, len(payload), zlib.crc32(payload)) + payload


class Decoder:
    def __init__(self):
        self.buffer = bytearray()
        self.last_byte = 0.0

    def feed(self, data):
        now = time.monotonic()
        if self.last_byte and now - self.last_byte > 10:
            self.buffer.clear()
        self.last_byte = now
        self.buffer.extend(data)
        packets = []
        while len(self.buffer) >= len(MAGIC):
            start = self.buffer.find(MAGIC)
            if start < 0:
                del self.buffer[:-3]
                break
            del self.buffer[:start]
            if len(self.buffer) < HEADER.size:
                break
            _, version, kind, flags, identity, size, crc = HEADER.unpack_from(self.buffer)
            if version != VERSION or flags or size > MAX_PAYLOAD:
                del self.buffer[0]
                continue
            end = HEADER.size + size
            if len(self.buffer) < end:
                break
            payload = bytes(self.buffer[HEADER.size:end])
            del self.buffer[:end]
            if zlib.crc32(payload) == crc:
                packets.append((kind, identity, payload))
        return packets
