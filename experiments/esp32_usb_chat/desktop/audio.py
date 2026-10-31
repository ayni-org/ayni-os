"""Local speech backend for speaker.speak_text. No network and no Mac playback."""
from pathlib import Path
import struct
import subprocess
import tempfile
import time

PCM_RATE = 16000
MAX_PCM_BYTES = PCM_RATE * 30 * 2
MAX_TEXT_BYTES = 1024
VOICE = 'Tingting (中文（中国大陆）)'


def read_pcm_caf(raw):
    if len(raw) > MAX_PCM_BYTES + 16384 or raw[:8] != b'caff\x00\x01\x00\x00':
        raise ValueError('本地语音文件头或大小不支持')
    at, description, samples = 8, None, None
    while at < len(raw):
        if len(raw)-at < 12:
            raise ValueError('本地语音文件 chunk 不完整')
        tag, size = struct.unpack_from('>4sq', raw, at)
        at += 12
        if size < 0 or size > len(raw)-at:
            raise ValueError('本地语音文件 chunk 长度无效')
        body = raw[at:at+size]
        at += size
        if tag == b'desc':
            if description is not None or size != 32:
                raise ValueError('本地语音格式描述无效')
            description = struct.unpack('>d4sIIIII', body)
        elif tag == b'data':
            if samples is not None or size < 4:
                raise ValueError('本地语音数据无效')
            samples = body[4:]
    # CAF uses its own flags: bit 1 is LittleEndian, bit 0 is Float.
    # This is not the AudioStreamBasicDescription flag layout (CAFFile.h).
    if description != (16000.0, b'lpcm', 2, 2, 1, 1, 16):
        raise ValueError('本地语音须为 16 kHz、16 位、单声道 PCM')
    if not samples or len(samples) % 2 or len(samples) > MAX_PCM_BYTES:
        raise ValueError('合成语音为空或超过 30 秒；不会静默截断')
    return samples


def synthesize(text, cancelled=None):
    if not isinstance(text, str) or not text or len(text.encode()) > MAX_TEXT_BYTES or '\x00' in text:
        raise ValueError('朗读文本须为 1..1024 UTF-8 字节且不含空字符')
    # Text never appears in process arguments; the private temporary directory is removed.
    with tempfile.TemporaryDirectory(prefix='ayni-local-speech-') as temporary:
        root = Path(temporary)
        source, output = root/'text.txt', root/'speech.caf'
        source.write_text(text, encoding='utf-8')
        process = None
        try:
            process = subprocess.Popen(['/usr/bin/say', '-v', VOICE, '-r', '180', '-o', str(output),
                '--data-format=LEI16@16000', '--channels=1', '-f', str(source)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 35
            while process.poll() is None:
                if cancelled is not None and cancelled.is_set():
                    raise ValueError('本地语音合成已取消')
                if time.monotonic() >= deadline:
                    raise ValueError('Mac 本地中文语音合成超时')
                try:
                    process.wait(timeout=0.1)
                except subprocess.TimeoutExpired:
                    pass
        except OSError:
            raise ValueError('Mac 本地中文语音合成不可用或超时') from None
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if process.returncode or not output.is_file():
            raise ValueError('Mac 本地中文语音合成失败；请核对已安装的 Tingting 语音')
        if output.stat().st_size > MAX_PCM_BYTES + 16384:
            raise ValueError('合成语音超过 30 秒；不会静默截断')
        return read_pcm_caf(output.read_bytes())


class AudioTransfer:
    """One accepted Call, explicit byte-offset ACK for every PCM chunk; no blind retries."""
    CHUNK_BYTES = 8192

    def __init__(self, identity, pcm):
        if not pcm or len(pcm) % 2 or len(pcm) > MAX_PCM_BYTES:
            raise ValueError('PCM 长度无效')
        self.identity, self.pcm = identity, pcm
        self.sent = 0
        self.done = False

    def begin(self):
        return struct.pack('<II', PCM_RATE, len(self.pcm))

    def acknowledge(self, payload):
        if len(payload) != 4 or struct.unpack('<I', payload)[0] != self.sent or self.done:
            raise ValueError('音频分块回执不匹配；不自动重发')
        if self.sent == len(self.pcm):
            self.done = True
            return None
        end = min(len(self.pcm), self.sent + self.CHUNK_BYTES)
        chunk = struct.pack('<I', self.sent) + self.pcm[self.sent:end]
        self.sent = end
        return chunk
