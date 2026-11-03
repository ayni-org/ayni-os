import struct
import unittest
from audio import AudioTransfer, MAX_PCM_BYTES, read_pcm_caf


def caf(pcm=b'\x00\x80\xff\x7f', flags=2, channels=1):
    description = struct.pack('>d4sIIIII', 16000.0, b'lpcm', flags, channels*2, 1, channels, 16)
    def chunk(tag, data):
        return struct.pack('>4sq', tag, len(data)) + data
    return b'caff\x00\x01\x00\x00' + chunk(b'desc', description) + chunk(b'free', bytes(8)) + chunk(b'data', bytes(4)+pcm)


class AudioTests(unittest.TestCase):
    def test_caf_preserves_little_endian_signed_pcm(self):
        pcm = read_pcm_caf(caf())
        self.assertEqual(struct.unpack('<hh', pcm), (-32768, 32767))

    def test_caf_rejects_other_endian_float_and_stereo(self):
        for raw in (caf(flags=0), caf(flags=3), caf(channels=2)):
            with self.subTest(raw=raw[:20]), self.assertRaises(ValueError):
                read_pcm_caf(raw)

    def test_caf_rejects_truncated_odd_and_over_duration(self):
        for raw in (caf()[:-1], caf(b'\x00'), caf(bytes(MAX_PCM_BYTES+2))):
            with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                read_pcm_caf(raw)

    def test_pcm_chunks_require_matching_offsets_and_end_exactly(self):
        pcm = bytes(20000)
        transfer = AudioTransfer(17, pcm)
        self.assertEqual(struct.unpack('<II', transfer.begin()), (16000, len(pcm)))
        chunks=[]
        while True:
            packet = transfer.acknowledge(struct.pack('<I', transfer.sent))
            if packet is None:
                break
            chunks.append(packet)
        self.assertEqual([struct.unpack_from('<I', p)[0] for p in chunks], [0, 8192, 16384])
        self.assertEqual(b''.join(p[4:] for p in chunks), pcm)
        with self.assertRaises(ValueError):
            transfer.acknowledge(struct.pack('<I', len(pcm)))

    def test_wrong_ack_and_unsupported_pcm_length_are_refused(self):
        transfer=AudioTransfer(8,bytes(20))
        with self.assertRaises(ValueError):
            transfer.acknowledge(struct.pack('<I', 2))
        for pcm in (b'', b'\x00', bytes(MAX_PCM_BYTES+2)):
            with self.assertRaises(ValueError):
                AudioTransfer(8,pcm)


if __name__=='__main__':
    unittest.main()
