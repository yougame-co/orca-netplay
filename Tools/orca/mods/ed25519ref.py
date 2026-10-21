"""Ed25519 (RFC 8032, section 5.1), standard library only: what sign-bundle.py signs flat bundles
with. The algorithm as the RFC's reference code gives it: slow (pure Python, tens of milliseconds a
signature) and not constant-time, which is fine for signing a few bundles on a trusted machine and
never fine for a server. Orca verifies with Dolphin's vendored Externals/ed25519.

  keypair(seed)            -> public key (32 bytes) for a 32-byte seed (the private key)
  sign(seed, message)      -> 64-byte signature
  verify(public, message, signature) -> bool
"""

import hashlib

P = 2**255 - 19
L = 2**252 + 27742317777372353535851937790883648493
D = -121665 * pow(121666, P - 2, P) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _sha512(data):
    return hashlib.sha512(data).digest()


def _sha512_mod_l(data):
    return int.from_bytes(_sha512(data), "little") % L


# Points in extended coordinates (X, Y, Z, T), x = X/Z, y = Y/Z, x*y = T/Z.
def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * D % P
    d = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, p):
    q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            q = _add(q, p)
        p = _add(p, p)
        s >>= 1
    return q


def _equal(p, q):
    return (p[0] * q[2] - q[0] * p[2]) % P == 0 and (p[1] * q[2] - q[1] * p[2]) % P == 0


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P != 0:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P != 0:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * pow(5, P - 2, P) % P
_GX = _recover_x(_GY, 0)
G = (_GX, _GY, 1, _GX * _GY % P)


def _compress(p):
    zinv = pow(p[2], P - 2, P)
    x, y = p[0] * zinv % P, p[1] * zinv % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % P)


def _expand(seed):
    if len(seed) != 32:
        raise ValueError("an Ed25519 private key (seed) is 32 bytes")
    h = _sha512(seed)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def keypair(seed):
    a, _ = _expand(seed)
    return _compress(_mul(a, G))


def sign(seed, message):
    a, prefix = _expand(seed)
    public = _compress(_mul(a, G))
    r = _sha512_mod_l(prefix + message)
    rs = _compress(_mul(r, G))
    h = _sha512_mod_l(rs + public + message)
    return rs + int.to_bytes((r + h * a) % L, 32, "little")


def verify(public, message, signature):
    if len(public) != 32 or len(signature) != 64:
        return False
    a = _decompress(public)
    r = _decompress(signature[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= L:
        return False
    h = _sha512_mod_l(signature[:32] + public + message)
    return _equal(_mul(s, G), _add(r, _mul(h, a)))


if __name__ == "__main__":
    # RFC 8032 section 7.1, TEST 1 (empty message) and TEST 2 (one byte).
    vectors = [
        ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
        ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ]
    for seed, public, message, signature in vectors:
        seed, public, message, signature = (bytes.fromhex(x) for x in (seed, public, message, signature))
        assert keypair(seed) == public, "public key"
        assert sign(seed, message) == signature, "signature"
        assert verify(public, message, signature), "verify"
        assert not verify(public, message + b"x", signature), "verify refuses another message"
    print("ed25519ref: RFC 8032 vectors pass")
