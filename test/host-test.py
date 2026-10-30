#!/usr/bin/env python3
"""End-to-end tests of pkg on this machine. Builds packages and a signed server with
tools/, serves it over HTTP, and runs pkg against a scratch root (-R).

  host-test.py PKG

The server answers under a few prefixes, to test the ways a server or network can
behave: /plain/ (as it is), /chunked/ (chunked replies), /redirect/ (a 302 to /plain/),
/https/ (a redirect to HTTPS), /tampered/ (index changed after signing), /unsigned/ (no
index.sig), /damaged/ (packages changed after indexing).
"""
import gzip, http.server, io, os, re, shutil, subprocess, sys, tarfile, tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', 'tools')
PKG = os.path.abspath(sys.argv[1])
WORK = tempfile.mkdtemp(prefix='pkg-test.')
ROOT = os.path.join(WORK, 'root')
REPO = os.path.join(WORK, 'repo')
KEY = os.path.join(WORK, 'key')
passed = failed = 0


def check(ok, what, detail=''):
    global passed, failed
    if ok:
        passed += 1
        print('ok   %s' % what)
    else:
        failed += 1
        print('FAIL %s\n%s' % (what, ''.join('     | %s\n' % l for l in str(detail)
                                                  .splitlines())))


def pkg(*args):
    p = subprocess.run([PKG, '-R', ROOT] + list(args), capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def tool(*args):
    return subprocess.run([sys.executable, '-I'] + list(args), check=True,
                          capture_output=True, text=True).stdout


def make_tree(top, meta, files):
    """files: path -> bytes (a file, mode 644), (bytes, mode), or ('link', target)"""
    os.makedirs(top)
    with open(os.path.join(top, '+PKG'), 'w') as f:
        f.write(''.join('%s %s\n' % kv for kv in meta))
    for path, what in files.items():
        full = os.path.join(top, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        if isinstance(what, tuple) and what[0] == 'link':
            os.symlink(what[1], full)
            continue
        data, mode = what if isinstance(what, tuple) else (what, 0o644)
        with open(full, 'wb') as f:
            f.write(data)
        os.chmod(full, mode)


def build(name, version, files, summary='A test package', extra=()):
    top = os.path.join(WORK, 'src', '%s-%s' % (name, version))
    make_tree(top, [('format', 1), ('name', name), ('version', version),
                    ('summary', summary)] + list(extra), files)
    os.makedirs(os.path.join(REPO, 'packages'), exist_ok=True)
    return tool(os.path.join(TOOLS, 'pkg-build.py'), top,
                os.path.join(REPO, 'packages')).strip()


def raw_package(path, members):
    """A package made by hand, for the bad ones: members are (name, bytes or None)."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for name, data in members:
            info = tarfile.TarInfo(name)
            if data is None:
                info.type = tarfile.DIRTYPE
                tar.addfile(info)
            else:
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
    with open(path, 'wb') as f:
        f.write(gzip.compress(buf.getvalue()))


def index():
    tool(os.path.join(TOOLS, 'pkg-repo.py'), 'index', REPO, KEY)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def send(self, code, body=b'', headers=()):
        self.send_response(code)
        for k, v in headers:
            self.send_header(k, v)
        if ('Transfer-Encoding', 'chunked') not in headers:
            self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        if ('Transfer-Encoding', 'chunked') in headers:
            for i in range(0, len(body), 1000):
                part = body[i:i + 1000]
                self.wfile.write(b'%x\r\n%s\r\n' % (len(part), part))
            self.wfile.write(b'0\r\nX-Trailer: yes\r\n\r\n')
        else:
            self.wfile.write(body)

    def do_GET(self):
        m = re.match(r'/(\w+)/(.*)$', self.path)
        if not m:
            return self.send(404)
        mode, rel = m.groups()
        if mode == 'redirect':
            return self.send(302, headers=[('Location', '/plain/' + rel)])
        if mode == 'https':
            return self.send(301, headers=[('Location', 'https://example.com/' + rel)])
        full = os.path.join(REPO, rel)
        if '..' in rel or not os.path.isfile(full):
            return self.send(404, b'not here')
        if mode == 'unsigned' and rel == 'index.sig':
            return self.send(404)
        body = open(full, 'rb').read()
        if mode == 'tampered' and rel == 'index':
            body = body.replace(b'A test package', b'A test pAckage')
        if mode == 'damaged' and rel.endswith('.pkg'):
            body = body[:-20] + bytes([body[-20] ^ 1]) + body[-19:]
        headers = [('Transfer-Encoding', 'chunked')] if mode == 'chunked' else []
        self.send(200, body, headers)


def files_under(top):
    out = []
    for dirpath, dirs, files in os.walk(top):
        out += [os.path.relpath(os.path.join(dirpath, f), top) for f in files + dirs]
    return sorted(out)


def temporaries():
    return [f for f in files_under(ROOT) if '.pkg-' in f or f.endswith('.part')]


def main():
    os.makedirs(os.path.join(ROOT, 'etc'))
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = 'http://localhost:%d' % server.server_address[1]
    url = base + '/plain/'

    pub = tool(os.path.join(TOOLS, 'pkg-repo.py'), 'keygen', KEY).strip()
    with open(os.path.join(ROOT, 'etc', 'pkg.conf'), 'w') as f:
        f.write('# test\nkey %s\n' % pub)

    hello10 = build('hello', '1.0', {
        'usr/local/bin/hello': (b'#!/bin/sh\necho hello\n', 0o755),
        'usr/local/bin/hi': ('link', 'hello'),
        'usr/local/share/hello/README': b'Says hello.\n',
    }, summary='Prints a greeting', extra=[('url', 'http://example.com/hello')])
    build('macapp', '1.0', {
        'mac/bin/Fancy Term': (b'\0\1COFF', 0o755),
        'mac/bin/%Fancy Term': b'\0\5\26\7 AppleDouble',
    })
    build('clash', '1.0', {'usr/local/bin/hello': b'mine\n'})
    bad = os.path.join(WORK, 'bad')
    os.makedirs(bad)
    meta = b'format 1\nname evil\nversion 1\nsummary x\n'
    raw_package(os.path.join(bad, 'nopkg.pkg'), [('usr/x', b'x')])
    raw_package(os.path.join(bad, 'dotdot.pkg'), [('+PKG', meta), ('../etc/x', b'x')])
    raw_package(os.path.join(bad, 'future.pkg'), [('+PKG', meta.replace(b'format 1',
                                                                            b'format 2'))])
    raw_package(os.path.join(bad, 'own.pkg'), [('+PKG', meta),
                                               ('usr/local/lib/pkg/installed/x', b'x')])
    raw_package(os.path.join(bad, 'twice.pkg'), [('+PKG', meta), ('usr/y', b'1'),
                                                 ('usr/y', b'2')])
    with open(os.path.join(bad, 'text.pkg'), 'w') as f:
        f.write('not a package\n')

    # the package list
    code, out = pkg('update')
    check(code and 'no server' in out, 'update without a server says so', out)
    index()
    code, out = pkg('-s', url, 'update')
    check(code == 0 and '3 packages' in out, 'update', out)
    code, out = pkg('-s', base + '/tampered/', 'update')
    check(code and "isn't signed by a key pkg trusts" in out, 'tampered list refused', out)
    code, out = pkg('list', '-a')
    check(code == 0 and 'hello' in out and 'unsigned' not in out,
          'refused list leaves the old one', out)
    code, out = pkg('-s', base + '/unsigned/', 'update')
    check(code and "isn't signed" in out, 'unsigned list refused', out)
    code, out = pkg('-u', '-s', base + '/unsigned/', 'update')
    check(code == 0 and 'unsigned' in out, 'unsigned list with -u', out)
    code, out = pkg('list', '-a')
    check("wasn't signed" in out, 'unsigned list marked', out)
    pkg('-s', url, 'update')

    code, out = pkg('list', '-a')
    check(code == 0 and re.search(r'hello\s+1\.0\s+Prints a greeting', out), 'list -a', out)
    code, out = pkg('search', 'GREET')
    check(code == 0 and 'hello' in out and 'macapp' not in out, 'search', out)
    code, out = pkg('search', 'zzz')
    check('No package matches' in out, 'search without a match', out)
    code, out = pkg('info', 'hello')
    check(code == 0 and 'Prints a greeting' in out and 'on the server' in out, 'info',
          out)
    code, out = pkg('info', hello10)
    check(code == 0 and 'url http://example.com/hello' in out, 'info of a file', out)
    code, out = pkg('list')
    check('No packages are installed' in out, 'list with nothing installed', out)

    # install, inspect
    code, out = pkg('install', 'hello')
    check(code == 0 and 'Installed hello 1.0' in out, 'install from the server', out)
    hello = os.path.join(ROOT, 'usr/local/bin/hello')
    check(open(hello).read() == '#!/bin/sh\necho hello\n' and
          os.stat(hello).st_mode & 0o777 == 0o755, 'installed file and mode')
    check(os.readlink(os.path.join(ROOT, 'usr/local/bin/hi')) == 'hello', 'installed link')
    code, out = pkg('list')
    check(re.search(r'hello\s+1\.0', out), 'list', out)
    code, out = pkg('files', 'hello')
    check('/usr/local/bin/hi -> hello' in out and '/usr/local/share/hello/README' in out,
          'files', out)
    code, out = pkg('owner', '/usr/local/bin/hello')
    check(code == 0 and out.strip() == 'hello', 'owner', out)
    code, out = pkg('owner', '/etc/pkg.conf')
    check(code == 1 and 'No package' in out, 'owner of an unowned file', out)
    code, out = pkg('verify')
    check(code == 0, 'verify', out)
    readme = os.path.join(ROOT, 'usr/local/share/hello/README')
    with open(readme, 'w') as f:
        f.write('Says hullo.\n')
    code, out = pkg('verify', 'hello')
    check(code == 1 and 'README: changed' in out, 'verify finds a change', out)
    with open(readme, 'w') as f:
        f.write('Says hello.\n')
    code, out = pkg('install', 'hello')
    check(code == 0 and 'already installed' in out, 'install again', out)
    code, out = pkg('info', 'hello')
    check('installed' in out and '(3 files)' in out, 'info of an installed package', out)

    # files in the way
    before = files_under(ROOT)
    code, out = pkg('install', 'clash')
    check(code == 1 and 'already there, from package hello' in out,
          "won't overwrite another package's file", out)
    check(files_under(ROOT) == before and not os.path.exists(os.path.join(
        ROOT, 'usr/local/lib/pkg/installed/clash')), '...and changes nothing',
        set(files_under(ROOT)) ^ set(before))
    os.makedirs(os.path.join(ROOT, 'mac/bin'))
    with open(os.path.join(ROOT, 'mac/bin/Fancy Term'), 'w') as f:
        f.write('mine')
    before = files_under(ROOT)
    code, out = pkg('install', 'macapp')
    check(code == 1 and 'no package installed it' in out,
          "won't overwrite a file no package installed", out)
    check(files_under(ROOT) == before, '...and changes nothing',
          set(files_under(ROOT)) ^ set(before))
    os.remove(os.path.join(ROOT, 'mac/bin/Fancy Term'))
    code, out = pkg('install', 'macapp')
    check(code == 0 and os.path.exists(os.path.join(ROOT, 'mac/bin/%Fancy Term')),
          'names with spaces and %', out)

    # upgrade
    pkg('upgrade')
    build('hello', '1.1', {
        'usr/local/bin/hello': (b'#!/bin/sh\necho hello again\n', 0o755),
        'usr/local/bin/hi': ('link', 'hello'),
        'usr/local/share/hello/NEWS': b'Now says it again.\n',
    }, summary='Prints a greeting')
    index()
    pkg('-s', url, 'update')
    code, out = pkg('list', '-a')
    check(re.search(r'hello\s+1\.1\s+Prints a greeting \(1\.0 installed\)', out),
          'list -a shows upgrades', out)
    code, out = pkg('upgrade')
    check(code == 0 and 'Upgraded hello from 1.0 to 1.1' in out, 'upgrade', out)
    check('again' in open(hello).read() and not os.path.exists(readme) and
          os.path.exists(os.path.join(ROOT, 'usr/local/share/hello/NEWS')),
          'upgrade replaces, removes and adds files')
    code, out = pkg('upgrade')
    check(code == 0 and 'up to date' in out, 'upgrade with nothing to do', out)
    code, out = pkg('verify')
    check(code == 0, 'verify after upgrade', out)

    # remove
    code, out = pkg('remove', 'hello')
    check(code == 0 and 'Removed hello 1.1' in out, 'remove', out)
    check(not os.path.exists(hello) and not os.path.exists(os.path.join(
        ROOT, 'usr/local/share')), 'remove takes files and the directories it made',
        files_under(ROOT))
    code, out = pkg('remove', 'hello')
    check(code == 1 and "isn't installed" in out, 'remove what is not installed', out)

    # from a file, and bad files
    code, out = pkg('install', hello10)
    check(code == 0 and 'Installed hello 1.0' in out, 'install from a file', out)
    code, out = pkg('info', 'hello')
    check(hello10 in out, '...recording where it came from', out)
    pkg('remove', 'hello')
    for name, expect in (('nopkg', "doesn't start with +PKG"),
                         ('dotdot', "isn't allowed"),
                         ('future', 'newer pkg'),
                         ('own', "pkg's own directory"),
                         ('twice', 'in it twice'),
                         ('text', 'not a gzip file')):
        before = files_under(ROOT)
        code, out = pkg('install', os.path.join(bad, name + '.pkg'))
        check(code == 1 and expect in out and files_under(ROOT) == before,
              'bad package refused: %s' % name, out)

    # the network misbehaving
    pkg('-s', base + '/damaged/', 'update')
    code, out = pkg('install', 'hello')
    check(code == 1 and "doesn't match the package list" in out and not temporaries(),
          'damaged download refused', out)
    code, out = pkg('-s', base + '/redirect/', 'update')
    check(code == 0, 'update through a redirect', out)
    code, out = pkg('install', 'hello')
    check(code == 0, 'install through a redirect', out)
    pkg('remove', 'hello')
    code, out = pkg('-s', base + '/https/', 'update')
    check(code == 1 and "HTTPS, which pkg can't use" in out, 'redirect to HTTPS refused', out)
    code, out = pkg('-s', base + '/chunked/', 'update')
    check(code == 0, 'update with chunked replies', out)
    code, out = pkg('install', 'hello')
    check(code == 0, 'install with chunked replies', out)
    code, out = pkg('-s', 'http://0x7F.0x00.0x00.0x01:%d/plain/' % server.server_address[1],
                    'update')
    check(code == 0, 'address in hex, as A/UX\'s /etc/hosts writes them', out)
    code, out = pkg('-s', 'http://localhost:1/', 'update')
    check(code == 1 and "can't connect" in out, 'server not there', out)
    code, out = pkg('-s', 'http://no-such-host.invalid/', 'update')
    check(code == 1 and ('no host' in out or "can't look up" in out), 'no such host', out)
    code, out = pkg('-s', 'https://example.com/', 'update')
    check(code == 1 and "can't use HTTPS" in out, 'https:// refused', out)

    # the lock
    lock = os.path.join(ROOT, 'usr/local/lib/pkg/lock')
    with open(lock, 'w') as f:
        f.write('%d\n' % os.getpid())
    code, out = pkg('remove', 'hello')
    check(code == 1 and 'another pkg is running' in out, 'lock held', out)
    with open(lock, 'w') as f:
        f.write('999999\n')
    code, out = pkg('remove', 'hello')
    check(code == 0 and not os.path.exists(lock), 'stale lock', out)

    check(not temporaries(), 'no temporaries left', temporaries())
    code, out = pkg('version')
    check(code == 0 and out.startswith('pkg '), 'version', out)
    server.shutdown()
    print('host-test: %d passed, %d failed' % (passed, failed))
    if not failed:
        shutil.rmtree(WORK)
    else:
        print('host-test: left %s' % WORK)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
