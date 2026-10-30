#!/usr/bin/env python3
"""Make a pkg server's package list, and the key that signs it.

  pkg-repo.py keygen KEYFILE       make a signing key: KEYFILE gets the secret (keep it
                                   safe and private), the public key is printed
  pkg-repo.py pubkey KEYFILE       print KEYFILE's public key
  pkg-repo.py index DIR [KEYFILE]  write DIR/index listing every *.pkg under DIR, and
                                   DIR/index.sig signing it with KEYFILE

KEYFILE may also be a .env file holding the key's line as PKG_KEY=ed25519-secret ...
(this repository's .env is ignored by git).

Then copy DIR to the server as it is (docs/publishing.md). The public key is what pkg
must trust: built in for the default server (src/defaults.h), or a "key" line in
/etc/pkg.conf.
"""
import gzip, hashlib, os, sys, tarfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ed25519  # noqa: E402

PREFIX = 'ed25519-secret '


def die(msg):
    sys.exit('pkg-repo: %s' % msg)


def read_key(path):
    try:
        text = open(path).read().strip()
    except OSError as e:
        die('can\'t read %s: %s' % (path, e.strerror))
    for line in text.splitlines():   # (or a .env with the key as PKG_KEY)
        name, eq, value = line.partition('=')
        if eq and name.strip() == 'PKG_KEY':
            text = value.strip().strip('"\'')
    if not text.startswith(PREFIX) or len(text) != len(PREFIX) + 64:
        die('%s isn\'t a key made by pkg-repo.py keygen' % path)
    return bytes.fromhex(text[len(PREFIX):])


def keygen(path):
    seed = os.urandom(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(PREFIX + seed.hex() + '\n')
    print(ed25519.public_key(seed).hex())


def package_meta(path):
    with gzip.open(path) as z, tarfile.open(fileobj=z, mode='r|') as tar:
        first = tar.next()
        if first is None or first.name != '+PKG' or not first.isfile():
            die('%s doesn\'t start with +PKG: not a package' % path)
        text = tar.extractfile(first).read().decode('ascii')
    meta = dict(line.partition(' ')[::2] for line in text.splitlines()
                if line and not line.startswith('#'))
    for key in ('name', 'version', 'summary'):
        if key not in meta:
            die('%s: +PKG has no %s' % (path, key))
    return meta


def index(top, keyfile):
    seed = read_key(keyfile) if keyfile else None
    lines = ['aux-pkg-index 1']
    for dirpath, dirs, files in os.walk(top):
        dirs.sort()
        for name in sorted(files):
            if not name.endswith('.pkg'):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, top)
            data = open(full, 'rb').read()
            m = package_meta(full)
            if '\t' in rel or '\t' in m['summary']:
                die('%s: no tabs allowed' % rel)
            lines.append('\t'.join((m['name'], m['version'], str(len(data)),
                                    hashlib.sha256(data).hexdigest(), rel, m['summary'])))
    text = ('\n'.join(lines) + '\n').encode('ascii')
    with open(os.path.join(top, 'index'), 'wb') as f:
        f.write(text)
    sig_path = os.path.join(top, 'index.sig')
    if seed:
        sig = ed25519.sign(seed, text)
        with open(sig_path, 'w') as f:
            f.write('ed25519\t%s\t%s\n' % (ed25519.public_key(seed).hex(), sig.hex()))
    elif os.path.exists(sig_path):
        os.remove(sig_path)
    print('%s: %d packages%s' % (os.path.join(top, 'index'), len(lines) - 1,
                                 ', signed' if seed else ', unsigned'))


def main():
    a = sys.argv[1:]
    if a[:1] == ['keygen'] and len(a) == 2:
        keygen(a[1])
    elif a[:1] == ['pubkey'] and len(a) == 2:
        print(ed25519.public_key(read_key(a[1])).hex())
    elif a[:1] == ['index'] and len(a) in (2, 3):
        index(a[1], a[2] if len(a) == 3 else None)
    else:
        sys.exit(__doc__.strip().split('\n\n')[1])


if __name__ == '__main__':
    main()
