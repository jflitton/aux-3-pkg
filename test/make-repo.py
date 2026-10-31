#!/usr/bin/env python3
"""Make the packages, signed package lists and pkg.conf that test/aux-test.sh uses inside
A/UX, as one flat directory (tools/aux-run.py puts each file in /usr/local/bin):

  make-repo.py OUTDIR

r1.index, r1.index.sig    server 1 (main): hello 1.0, macapp 1.0
r2.index, r2.index.sig    server 2, with a key of its own: hello 1.1, macapp 1.0
r3.index, r3.index.sig    main's next list, signed as server 1's: hello 1.1, pkg 2.0,
                          macapp 1.0
*.pkg                     the packages; shelly-1.0.pkg, whose program is a shell; and
                          pkg-1.0.pkg, to install from the file
pkg.conf                  trusting server 1's key for main, and naming server 2 "two"
"""
import os, shutil, subprocess, sys, tempfile

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools')


def tool(*args):
    return subprocess.run([sys.executable, '-I'] + list(args), check=True,
                          capture_output=True, text=True).stdout


def package(work, outdir, name, version, files, extra=''):
    top = os.path.join(work, 'src', '%s-%s' % (name, version))
    for path, (data, mode) in files.items():
        full = os.path.join(top, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, 'wb') as f:
            f.write(data)
        os.chmod(full, mode)
    with open(os.path.join(top, '+PKG'), 'w') as f:
        f.write('format %d\nname %s\nversion %s\nsummary A test package\n%s' % (
            2 if extra else 1, name, version, extra))
    return tool(os.path.join(TOOLS, 'pkg-build.py'), top, outdir).strip()


def main():
    out = sys.argv[1]
    work = tempfile.mkdtemp()
    os.makedirs(out, exist_ok=True)
    keys, pubs = {}, {}
    for name in ('r1', 'r2'):
        keys[name] = os.path.join(work, name + '.key')
        pubs[name] = tool(os.path.join(TOOLS, 'pkg-repo.py'), 'keygen', keys[name]).strip()
    servers = {'r1': [], 'r2': [], 'r3': []}
    signer = {'r1': 'r1', 'r2': 'r2', 'r3': 'r1'}
    # no #! line: A/UX's sh runs a script the kernel won't
    servers['r1'].append(package(work, out, 'hello', '1.0', {
        'usr/local/bin/hello': (b'echo hello from a package\n', 0o755)}))
    hello11 = package(work, out, 'hello', '1.1', {
        'usr/local/bin/hello': (b'echo hello again\n', 0o755)})
    servers['r2'].append(hello11)
    servers['r3'].append(hello11)
    # pkg's own package, whose pkg runs the one being tested: with #!, as pkg runs the
    # new one itself, and the new one says so
    package(work, out, 'pkg', '1.0', {
        'usr/local/bin/pkg': (b'#!/bin/sh\nexec /tmp/pkg.real "$@"\n', 0o755)})
    servers['r3'].append(package(work, out, 'pkg', '2.0', {
        'usr/local/bin/pkg': (b'#!/bin/sh\necho "pkg 2.0: $*"\nexec /tmp/pkg.real "$@"\n',
                              0o755)}))
    mac = package(work, out, 'macapp', '1.0', {
        'mac/bin/Fancy Term': (b'\0\1not really an app', 0o755),
        'mac/bin/%Fancy Term': (b'\0\5\26\7 not really AppleDouble', 0o644)})
    package(work, out, 'shelly', '1.0', {
        'usr/local/bin/shelly': (b'echo shelly here\n', 0o755)}, 'shell /usr/local/bin/shelly\n')
    for name, pkgs in servers.items():
        d = os.path.join(work, name)
        os.makedirs(d)
        for p in pkgs + [mac]:
            shutil.copy(p, d)
        tool(os.path.join(TOOLS, 'pkg-repo.py'), 'index', d, keys[signer[name]])
        for f in ('index', 'index.sig'):
            shutil.copy(os.path.join(d, f), os.path.join(out, '%s.%s' % (name, f)))
    with open(os.path.join(out, 'pkg.conf'), 'w') as f:
        f.write('key %s\nrepo two http://localhost:8080/chunked/r2/ %s\n' % (pubs['r1'],
                                                                           pubs['r2']))
    shutil.rmtree(work)


if __name__ == '__main__':
    main()
