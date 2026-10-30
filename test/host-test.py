#!/usr/bin/env python3
"""End-to-end tests of pkg on this machine. Builds packages and a signed server with
tools/, serves it over HTTP, and runs pkg against a scratch root (-r).

  host-test.py PKG

The server answers under a few prefixes, to test the ways a server or network can
behave: /plain/ (as it is), /chunked/ (chunked replies), /redirect/ (a 302 to /plain/),
/https/ (a redirect to HTTPS), /tampered/ (index changed after signing), /unsigned/ (no
index.sig), /damaged/ (packages changed after indexing). /second/ is another server, with
its own key.
"""
import gzip, http.server, io, os, re, shutil, subprocess, sys, tarfile, tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', 'tools')
PKG = os.path.abspath(sys.argv[1])
WORK = tempfile.mkdtemp(prefix='pkg-test.')
ROOT = os.path.join(WORK, 'root')
REPO = os.path.join(WORK, 'repo')
REPO2 = os.path.join(WORK, 'repo2')
KEY = os.path.join(WORK, 'key')
KEY2 = os.path.join(WORK, 'key2')
CONF = os.path.join(ROOT, 'etc', 'pkg.conf')
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


def pkg(*args, root=True, cwd=None, split=False):
    """pkg's exit status and output; split gives stdout and stderr apart"""
    p = subprocess.run([PKG] + (['-r', ROOT] if root else []) + list(args), cwd=cwd,
                       capture_output=True, text=True)
    return (p.returncode, p.stdout, p.stderr) if split else (p.returncode,
                                                             p.stdout + p.stderr)


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


def build(name, version, files, summary='A test package', extra=(), repo=REPO):
    top = os.path.join(WORK, 'src', os.path.basename(repo), '%s-%s' % (name, version))
    make_tree(top, [('format', 1), ('name', name), ('version', version),
                    ('summary', summary)] + list(extra), files)
    os.makedirs(os.path.join(repo, 'packages'), exist_ok=True)
    return tool(os.path.join(TOOLS, 'pkg-build.py'), top,
                os.path.join(repo, 'packages')).strip()


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


def index(repo=REPO, key=KEY):
    tool(os.path.join(TOOLS, 'pkg-repo.py'), 'index', repo, key)


def conf(*lines):
    with open(CONF, 'w') as f:
        f.write(''.join(l + '\n' for l in lines))


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
        full = os.path.join(REPO2 if mode == 'second' else REPO, rel)
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
    conf('# test', 'key %s' % pub)

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
    raw_package(os.path.join(bad, 'pkgkey.pkg'), [('+PKG', meta + b'server main\n')])
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
    code, out = pkg('--unsigned', '-s', base + '/unsigned/', 'update')
    check(code == 0 and 'unsigned' in out, 'unsigned list with --unsigned', out)
    code, out = pkg('list', '-a')
    check("wasn't signed" in out, 'unsigned list marked', out)
    pkg('-s', url, 'update')

    code, out = pkg('list', '-a')
    check(code == 0 and re.search(r'hello\s+1\.0\s+Prints a greeting', out), 'list -a', out)
    code, out = pkg('search', 'GREET')
    check(code == 0 and 'hello' in out and 'macapp' not in out, 'search', out)
    code, out = pkg('search', 'zzz')
    check(code == 1 and 'No package matches' in out, 'search without a match', out)
    code, out = pkg('info', 'hello')
    check(code == 0 and 'Prints a greeting' in out and 'on main: 1.0, ' in out, 'info',
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
                         ('pkgkey', "pkg's own"),
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
    check(code == 2 and "can't use HTTPS" in out, 'https:// refused', out)

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

    # several servers
    pub2 = tool(os.path.join(TOOLS, 'pkg-repo.py'), 'keygen', KEY2).strip()
    build('hello', '2.0', {'usr/local/bin/hello': (b'#!/bin/sh\necho hello from extra\n',
                                                   0o755)}, repo=REPO2)
    build('only', '1.0', {'usr/local/bin/only': b'only\n'}, repo=REPO2)
    index(REPO2, KEY2)
    second = base + '/second/'
    conf('key %s' % pub, 'repo extra %s %s' % (second, pub2))
    code, out = pkg('-s', url, 'update')
    check(code == 0 and re.search(r'^main: \d+ packages at %s$' % url, out, re.M) and
          'extra: 2 packages at %s\n' % second in out, 'update gets every server', out)
    code, out = pkg('list', '-a')
    check(re.search(r'^hello\s+1\.1\s', out, re.M) and
          re.search(r'^extra/hello\s+2\.0\s', out, re.M) and
          re.search(r'^extra/only\s+1\.0\s', out, re.M), "list -a shows each server's",
          out)
    code, out = pkg('install', 'hello')
    check(code == 0 and 'Installed hello 1.1' in out,
          'the first server with a package has it, not the highest version', out)
    code, out = pkg('list', '-a')
    check(re.search(r'^hello\s+1\.1\s.*\(installed\)$', out, re.M) and
          re.search(r'^extra/hello\s+2\.0\s+A test package$', out, re.M),
          'list -a marks the one installed', out)
    code, out = pkg('upgrade')
    check(code == 0 and 'up to date' in out, "upgrade doesn't take another server's", out)
    code, out = pkg('install', 'only')
    check(code == 0 and 'Installed only 1.0' in out, 'install from the server with it', out)
    code, out = pkg('install', 'extra/hello')
    check(code == 0 and 'Upgraded hello from 1.1 to 2.0' in out and
          'from extra' in open(hello).read(), 'install server/package', out)
    code, out = pkg('info', 'hello')
    check('from extra, ' + second in out and 'on main: 1.1, ' in out and
          'on extra: 2.0, ' in out, 'info names the servers', out)
    code, out = pkg('info', 'extra/only')
    check(code == 0 and 'on extra: 1.0' in out, 'info server/package', out)
    build('hello', '3.0', {'usr/local/bin/hello': (b'#!/bin/sh\necho hello 3\n', 0o755)})
    index()
    pkg('-s', url, 'update')
    code, out = pkg('upgrade')
    check(code == 0 and 'up to date' in out,
          'upgrade stays with the server a package came from', out)
    code, out = pkg('install', 'main/hello')
    check(code == 0 and 'Upgraded hello from 2.0 to 3.0' in out, '...until told', out)
    code, out = pkg('install', 'extra/hello')
    check(code == 0 and 'Downgraded hello from 3.0 to 2.0' in out, 'downgrade', out)
    pkg('remove', 'hello')
    pkg('install', hello10)
    code, out = pkg('upgrade')
    check(code == 0 and 'Upgraded hello from 1.0 to 3.0' in out,
          'one installed from a file follows the first server that has it', out)
    pkg('remove', 'hello')
    code, out = pkg('install', 'nowhere/hello')
    check(code == 1 and 'no server called nowhere' in out, 'server/package, no server',
          out)
    code, out = pkg('install', 'extra/macapp')
    check(code == 1 and 'no package called macapp at extra' in out,
          "server/package that isn't there", out)

    # whose keys sign what
    code, out = pkg('-s', second, 'update')
    check(code == 1 and "isn't signed by a key pkg trusts" in out and
          'kept the package list pkg had from main' in out and 'extra: 2 packages' in out,
          "main doesn't trust another server's key", out)
    code, out = pkg('list', '-a')
    check(re.search(r'^hello\s+3\.0\s', out, re.M), '...and keeps its old list', out)
    conf('key %s' % pub, 'repo extra %s %s' % (url, pub2))
    code, out = pkg('-s', url, 'update')
    check(code == 1 and "isn't signed by a key pkg trusts for extra" in out,
          "a repo doesn't trust main's key", out)
    conf('repo extra %s %s' % (second, pub2))
    code, out = pkg('-s', url, 'update')
    check(code == 1 and "isn't signed by a key pkg trusts.\n" in out,
          "a repo's key doesn't sign main's list", out)
    conf('key %s' % pub, 'repo loose %s' % (base + '/unsigned/'))
    code, out = pkg('--unsigned', '-s', url, 'update')
    check(code == 1 and "trusts for loose" in out, "--unsigned is for main's list only",
          out)
    conf('key %s' % pub, 'repo loose %s unsigned' % (base + '/unsigned/'))
    code, out = pkg('-s', url, 'update')
    check(code == 0 and re.search(r'^loose: .*\(unsigned: not checked\)$', out, re.M),
          'a repo accepted unsigned', out)
    code, out = pkg('list', '-a')
    check('list from loose wasn\'t signed' in out and 'main wasn\'t' not in out,
          '...is marked', out)

    # servers coming and going
    conf('key %s' % pub, 'repo extra %s %s' % (second, pub2))
    pkg('-s', url, 'update')
    pkg('install', 'extra/hello')
    lists = os.path.join(ROOT, 'usr/local/lib/pkg/lists')
    check(sorted(os.listdir(lists)) == ['extra', 'main'], 'a list per server',
          os.listdir(lists))
    conf('key %s' % pub, 'repo extra http://localhost:1/ %s' % pub2)
    code, out = pkg('-s', url, 'update')
    check(code == 1 and "can't connect" in out and
          'kept the package list pkg had from extra' in out, 'a server down', out)
    code, out = pkg('info', 'extra/hello')
    check(code == 0 and 'on extra: 2.0, ' + second in out, '...keeps its list and address',
          out)
    conf('key %s' % pub)
    code, out = pkg('-s', url, 'update')
    check(code == 0 and 'extra' not in out and os.listdir(lists) == ['main'],
          'a server taken out of pkg.conf is dropped', out)
    code, out = pkg('upgrade')
    check('hello came from extra, which pkg has no package list from' in out and
          'up to date' not in out, "...and what came from it isn't upgraded from elsewhere",
          out)
    code, out = pkg('upgrade', 'hello')
    check(code == 1 and 'came from extra' in out, '...even when named', out)
    pkg('remove', 'hello', 'only')

    # pkg.conf mistakes
    conf('key %s' % pub, 'repo main %s %s' % (second, pub2),
         'repo extra %s %s' % (second, pub2), 'repo extra %s %s' % (url, pub),
         'repo bad %s 1234' % second)
    code, out = pkg('-s', url, 'update')
    check('"repo main": that name isn\'t allowed' in out and
          'two repo lines for extra' in out and '"1234" on the repo line for bad' in out
          and 'extra: 2 packages' in out, 'repo lines that are no good', out)
    conf('key %s' % pub)
    pkg('-s', url, 'update')

    # the command line
    conf('key %s' % pub, 'repo extra %s %s' % (second, pub2))
    pkg('update', '-s', url)
    code, out = pkg('install', 'extra/hello')
    check(code == 0 and 'Installed hello 2.0' in out, 'options after the command', out)
    code, out = pkg('list', '-r', ROOT, root=False)
    check(code == 0 and re.search(r'^extra/hello\s+2\.0\s', out, re.M),
          "-r after the command, and list shows a package's server", out)
    code, out = pkg('remove', 'hello', '-r', '/no/such/dir')
    check(code == 2 and "isn't a directory" in out and os.path.exists(hello),
          'a mistake anywhere does nothing', out)
    code, out = pkg('-s', 'extra', 'update')
    check(code == 2 and "http:// address, not extra" in out and 'main:' not in out,
          '-s takes an address, checked first', out)
    for args, expect in ((['instal', 'hello'], "instal isn't a command"),
                         (['-x', 'list'], "-x isn't an option"),
                         (['search'], 'usage: pkg search word\n'),
                         (['owner', 'a', 'b'], 'usage: pkg owner /path\n'),
                         (['-s', url, 'list'], '-s goes with update'),
                         (['--unsigned', 'install', 'hello'], '--unsigned goes with update'),
                         (['search', '-a', 'x'], '-a goes with list'),
                         (['update', '-s'], '-s needs a URL'),
                         (['owner', 'usr/local/bin/hello'], 'starting with /'),
                         (['owner', '/usr/local/bin/../bin/hello'], 'without . or ..')):
        code, out = pkg(*args)
        check(code == 2 and expect in out and len(out.splitlines()) == 1,
              'mistake: pkg %s' % ' '.join(args), out)
    code, out, err = pkg(split=True)
    check(code == 2 and out == '' and err.startswith('usage: pkg'), 'pkg alone', err)
    for args in (['help'], ['-h'], ['--help'], ['install', '--help']):
        code, out, err = pkg(*args, split=True)
        check(code == 0 and out.startswith('usage: pkg') and 'owner /path' in out and
              err == '', 'pkg %s' % ' '.join(args), out + err)
    code, out = pkg('remove', '--', '-x')
    check(code == 1 and "-x isn't installed" in out, '-- ends the options', out)

    # files and packages, by how they're written
    os.makedirs(os.path.join(WORK, 'extra'))
    shutil.copy(hello10, os.path.join(WORK, 'extra', 'hello'))
    code, out = pkg('info', 'extra/hello', cwd=WORK)
    check(code == 0 and 'on extra: 2.0' in out, 'SERVER/NAME is a package, even when a '
          'file has that name', out)
    code, out = pkg('info', './extra/hello', cwd=WORK)
    check(code == 0 and 'url http://example.com/hello' in out, '...and ./ makes it a file',
          out)
    code, out = pkg('install', 'nothere.pkg', cwd=WORK)
    check(code == 1 and "can't open nothere.pkg" in out, 'NAME.pkg is a file', out)

    # SERVER/NAME for what's installed
    for cmd in ('files', 'verify', 'upgrade', 'info'):
        code, out = pkg(cmd, 'extra/hello')
        check(code == 0, '%s SERVER/NAME' % cmd, out)
    code, out = pkg('remove', 'main/hello')
    check(code == 1 and 'hello came from extra, not main' in out and os.path.exists(hello),
          "SERVER/NAME has to be where it came from", out)
    code, out = pkg('remove', 'extra/hello')
    check(code == 0 and 'Removed hello 2.0' in out, 'remove SERVER/NAME', out)
    pkg('install', hello10)
    code, out = pkg('files', 'main/hello')
    check(code == 1 and "isn't recorded as coming from main" in out,
          '...and one from a file has none', out)

    # owner
    code, out = pkg('owner', '/usr//local/bin/hello/')
    check(code == 0 and out == 'hello\n', 'owner, // and a trailing /', out)
    code, out = pkg('owner', '/usr/local/share/hello')
    check(code == 0 and out == 'hello\n', 'owner of a directory a package made', out)
    code, out = pkg('owner', '/usr/local/bin')
    check(code == 1 and 'No package installed /usr/local/bin.' in out,
          "owner of one it didn't", out)
    pkg('remove', 'hello')
    conf('key %s' % pub)
    pkg('update', '-s', url)

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
