#!/usr/bin/env python3
"""Ed25519 (RFC 8032) in pure Python, for signing package indexes and testing pkg's
verifier. Slow, and not constant-time: fine for signing on the publisher's own machine.

  ed25519.py selftest     check against RFC 8032's first test vector
"""
import hashlib, sys

P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _inv(x):
    return pow(x, P - 2, P)


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * _inv(D * y * y + 1) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * _inv(5) % P
_G = (_recover_x(_GY, 0), _GY, 1, _recover_x(_GY, 0) * _GY % P)


def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * D % P
    d = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, p):
    q = (0, 1, 1, 0)
    while s:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _encode(p):
    zi = _inv(p[2])
    x, y = p[0] * zi % P, p[1] * zi % P
    return (y | (x & 1) << 255).to_bytes(32, 'little')


def _decode(s):
    y = int.from_bytes(s, 'little')
    sign, y = y >> 255, y & ((1 << 255) - 1)
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _sha512_int(*parts):
    return int.from_bytes(hashlib.sha512(b''.join(parts)).digest(), 'little')


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], 'little')
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def public_key(seed):
    """The 32-byte public key of a 32-byte secret seed."""
    return _encode(_mul(_expand(seed)[0], _G))


def sign(seed, msg):
    a, prefix = _expand(seed)
    pub = _encode(_mul(a, _G))
    r = _sha512_int(prefix, msg) % L
    big_r = _encode(_mul(r, _G))
    s = (r + _sha512_int(big_r, pub, msg) % L * a) % L
    return big_r + s.to_bytes(32, 'little')


def verify(pub, msg, sig):
    a = _decode(pub)
    r = _decode(sig[:32])
    s = int.from_bytes(sig[32:], 'little')
    if a is None or r is None or s >= L:
        return False
    h = _sha512_int(sig[:32], pub, msg) % L
    return _encode(_mul(s, _G)) == _encode(_add(r, _mul(h, a)))


def selftest():
    seed = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
    pub = bytes.fromhex('d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a')
    sig = bytes.fromhex('e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb'
                        '8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b')
    assert public_key(seed) == pub, 'public key'
    assert sign(seed, b'') == sig, 'signature'
    assert verify(pub, b'', sig), 'verify'
    assert not verify(pub, b'x', sig), 'verify wrong message'


if __name__ == '__main__':
    if sys.argv[1:] != ['selftest']:
        sys.exit(__doc__.strip().split('\n\n')[1])
    selftest()
    print('ed25519.py: RFC 8032 test 1 passes')
