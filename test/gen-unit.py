#!/usr/bin/env python3
"""Write the test vectors test/unit.c checks: hashes from hashlib, signatures from
tools/ed25519.py, gzip files from zlib.

  gen-unit.py OUT.h
"""
import hashlib, os, random, sys, zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import ed25519  # noqa: E402


def carray(name, data):
    lines = ['static const unsigned char %s[%d] = {' % (name, max(len(data), 1))]
    for i in range(0, len(data), 16):
        lines.append('\t' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',')
    if not data:
        lines.append('\t0')
    lines.append('};')
    return '\n'.join(lines)


def gzip(data, level, strategy=zlib.Z_DEFAULT_STRATEGY, name=None):
    z = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
    body = z.compress(data) + z.flush()
    flags = 8 if name else 0
    head = bytes([0x1f, 0x8b, 8, flags, 0, 0, 0, 0, 0, 3]) + (name + b'\0' if name else b'')
    tail = zlib.crc32(data).to_bytes(4, 'little') + (len(data) & 0xffffffff).to_bytes(4,
                                                                                      'little')
    return head + body + tail


def main():
    rng = random.Random(1)
    out = ['/* written by test/gen-unit.py */']

    msgs = [bytes(rng.randrange(256) for _ in range(n))
            for n in (0, 3, 55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 1000)]
    for i, m in enumerate(msgs):
        out.append(carray('shamsg%d' % i, m))
    out.append('static const unsigned char *const shamsg[] = { %s };' % ', '.join(
        'shamsg%d' % i for i in range(len(msgs))))
    out.append('static const struct { int len; const char *sha256, *sha512; } shas[] = {')
    for m in msgs:
        out.append('\t{ %d, "%s", "%s" },' % (len(m), hashlib.sha256(m).hexdigest(),
                                                hashlib.sha512(m).hexdigest()))
    out.append('};')

    # signatures: good ones, then each spoiled one way
    sigs = []
    for i, n in enumerate((0, 1, 77, 300)):
        seed = bytes(rng.randrange(256) for _ in range(32))
        msg = bytes(rng.randrange(256) for _ in range(n))
        pub = ed25519.public_key(seed)
        sig = ed25519.sign(seed, msg)
        sigs.append((pub, msg, sig, 1))
    pub, msg, sig, _ = sigs[2]
    bad_r = bytes([sig[0] ^ 1]) + sig[1:]
    bad_s = sig[:32] + bytes([sig[32] ^ 1]) + sig[33:]
    big_s = sig[:32] + (int.from_bytes(sig[32:], 'little') + ed25519.L).to_bytes(32, 'little')
    bad_msg = bytes([msg[0] ^ 0x80]) + msg[1:]
    other_pub = sigs[1][0]
    sigs += [(pub, msg, bad_r, 0), (pub, msg, bad_s, 0), (pub, msg, big_s, 0),
             (pub, bad_msg, sig, 0), (other_pub, msg, sig, 0)]
    assert [ed25519.verify(p, m, s) for p, m, s, _ in sigs] == [bool(g) for *_, g in sigs]
    for i, (p, m, s, good) in enumerate(sigs):
        out.append(carray('sig%d_pk' % i, p))
        out.append(carray('sig%d_msg' % i, m))
        out.append(carray('sig%d_sig' % i, s))
    out.append('static const struct { const unsigned char *pk, *msg, *sig; int n, good; } '
               'sigs[] = {')
    for i, (p, m, s, good) in enumerate(sigs):
        out.append('\t{ sig%d_pk, sig%d_msg, sig%d_sig, %d, %d },' % (i, i, i, len(m), good))
    out.append('};')

    # gzip files: text-like data, incompressible data, long runs, and more than one
    # window (32KB) so back references cross the ring's wrap
    words = [bytes(rng.choice(b'abcdefghijklmnopqrstuvwxyz') for _ in range(rng.randrange(
        2, 10))) for _ in range(300)]
    text = b' '.join(rng.choice(words) for _ in range(40000))[:200000]
    noise = bytes(rng.randrange(256) for _ in range(40000))
    runs = b''.join(bytes([rng.randrange(4)]) * rng.randrange(1, 600) for _ in range(500))
    cases = [(b'', 6, zlib.Z_DEFAULT_STRATEGY, None), (b'hello, world\n', 9,
             zlib.Z_DEFAULT_STRATEGY, b'hello.txt'), (text, 9, zlib.Z_DEFAULT_STRATEGY, None),
             (text, 1, zlib.Z_DEFAULT_STRATEGY, None), (text, 6, zlib.Z_FIXED, None),
             (text[:70000], 0, zlib.Z_DEFAULT_STRATEGY, None),
             (noise, 9, zlib.Z_DEFAULT_STRATEGY, None), (runs, 9, zlib.Z_RLE, None),
             (runs, 9, zlib.Z_HUFFMAN_ONLY, None)]
    damaged = bytearray(gzip(text[:5000], 9))
    damaged[-6] ^= 1
    truncated = gzip(text[:5000], 9)[:-100]
    entries = []
    for i, (data, level, strategy, name) in enumerate(cases):
        gz = gzip(data, level, strategy, name)
        out.append(carray('gz%d' % i, gz))
        entries.append('\t{ gz%d, %d, "%s", 1 },' % (i, len(gz),
                                                     hashlib.sha256(data).hexdigest()))
    for i, gz in enumerate((bytes(damaged), truncated), len(cases)):
        out.append(carray('gz%d' % i, gz))
        entries.append('\t{ gz%d, %d, "", 0 },' % (i, len(gz)))
    out.append('static const struct { const unsigned char *gz; int len; const char *sha256; '
               'int good; } gzs[] = {')
    out += entries
    out.append('};')

    with open(sys.argv[1], 'w') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    main()
