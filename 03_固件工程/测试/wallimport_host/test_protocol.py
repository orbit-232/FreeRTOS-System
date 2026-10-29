"""Host-only protocol simulation for the ASCII lock-wallpaper import.

This does not open COM or touch the TF card.  It validates the framing,
sequence/duplicate handling, chunk CRC, final CRC, timeout and existing-target
rules using the exact prepared asset bytes.
"""
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.abspath(__file__))
ASSET = os.path.join(ROOT, '..', '..', 'assets', 'lockscreen',
                     'lock_wallpaper.rgb565')
ASSET = os.path.normpath(ASSET)
CHUNK = 1024


class Receiver:
    def __init__(self, target_exists=False):
        self.target_exists = target_exists
        self.expected = 0
        self.last = -1
        self.data = bytearray()
        self.final_crc = None
        self.acked = []
        self.nacked = []

    def begin(self, crc):
        if self.target_exists:
            return 'ERR exists'
        self.final_crc = crc
        return 'ACK BEGIN'

    def chunk(self, seq, crc, payload):
        if seq == self.last:
            self.acked.append(seq)
            return 'ACK'
        if seq != self.expected:
            self.nacked.append(seq)
            return 'NACK'
        if (zlib.crc32(payload) & 0xffffffff) != crc:
            self.nacked.append(seq)
            return 'NACK'
        self.data.extend(payload)
        self.last = seq
        self.expected += 1
        self.acked.append(seq)
        return 'ACK'

    def timeout(self, idle_ms, limit_ms=30000):
        return idle_ms > limit_ms

    def done(self):
        return len(self.data) == 2048000 and (zlib.crc32(self.data) & 0xffffffff) == self.final_crc


def chunks(raw):
    payload = raw[24:]
    return [payload[i:i + CHUNK] for i in range(0, len(payload), CHUNK)]


def main():
    raw = open(ASSET, 'rb').read()
    magic, w, h, fmt, _, nbytes, crc, _ = struct.unpack('<4sHHHHIII', raw[:24])
    assert magic == b'PWL1' and w == 1280 and h == 800 and fmt == 1 and nbytes == 2048000
    cs = chunks(raw)
    assert len(cs) == 2000

    r = Receiver()
    assert r.begin(crc) == 'ACK BEGIN'
    for i, c in enumerate(cs):
        assert r.chunk(i, zlib.crc32(c) & 0xffffffff, c) == 'ACK'
    assert r.done()

    r = Receiver()
    r.begin(crc)
    for i, c in enumerate(cs):
        assert r.chunk(i, zlib.crc32(c) & 0xffffffff, c) == 'ACK'
        if i == 7:
            assert r.chunk(i, zlib.crc32(c) & 0xffffffff, c) == 'ACK'  # lost ACK retry
    assert r.done() and r.acked.count(7) == 2

    r = Receiver()
    r.begin(crc)
    bad = zlib.crc32(cs[0]) ^ 1
    assert r.chunk(0, bad & 0xffffffff, cs[0]) == 'NACK'
    assert r.chunk(0, zlib.crc32(cs[0]) & 0xffffffff, cs[0]) == 'ACK'
    assert len(r.data) == CHUNK

    r = Receiver(target_exists=True)
    assert r.begin(crc) == 'ERR exists' and len(r.data) == 0
    assert Receiver().timeout(30001) and not Receiver().timeout(29999)
    print('WALLIMPORT_PROTOCOL_TESTS_OK chunks=%d bytes=%d' % (len(cs), nbytes))


if __name__ == '__main__':
    main()
