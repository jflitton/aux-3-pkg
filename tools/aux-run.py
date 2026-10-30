#!/usr/bin/env python3
"""Boot a throwaway copy of an A/UX 3.1 disk in QEMU and run a shell script there.

  aux-run.py [-t SECONDS] SCRIPT [FILE...]

Each FILE goes into /usr/local/bin under its own name, mode 755. SCRIPT runs as root once
A/UX has started (an inittab "once" line, after /etc/rc), in /tmp, with /usr/local/bin
first in PATH. Its output is printed here, and this exits with its status: 2 if it
didn't finish within SECONDS (default 300), with a screenshot in build/run/screen.png.

Environment (the Makefile sets defaults):
  AUX_DISK     a stock A/UX 3.1 disk image; only read, the run boots a clone
  AUX_ROM      a Quadra 800 ROM, for QEMU's q800
  AUX_PATCHES  an aux-3-patches checkout, for its tools/ufs.py and tools/ufsput.py

The clone, QEMU's serial log and any screenshot are in build/run/.
"""
import os, re, shutil, socket, struct, subprocess, sys, time, zlib

RUN = 'build/run'
LOG = '/aux-run.log'
# init runs "once" lines with no stdin, so a script it starts directly is read from fd 0:
# the inittab line gives it fds 0-2, and the script never redirects its own stdin.
INITTAB = 'ar:2:once:/bin/sh /etc/aux-run </dev/null >/dev/null 2>&1\n'
WRAPPER = '''exec >%s 2>&1
PATH=/usr/local/bin:/bin:/usr/bin:/etc:/usr/etc:/usr/ucb
export PATH
cd /tmp
/bin/sh /etc/aux-run.sh
echo "aux-run: exit $?"
sync
sync
''' % LOG


def env(name):
    v = os.environ.get(name)
    if not v:
        sys.exit('aux-run: set %s, in local.mk or on make\'s command line' % name)
    return v


class Disk:
    def __init__(self, path, patches):
        self.path = path
        self.tools = os.path.join(patches, 'tools')
        out = self.tool('ufs.py', 'find', 'Root&Usr')
        self.part = out.split()[0]

    def tool(self, name, *args, text=True):
        return subprocess.run([sys.executable, '-I', os.path.join(self.tools, name),
                               self.path] + list(args), check=True, capture_output=True,
                              text=text).stdout

    def cat(self, path):
        return self.tool('ufs.py', 'cat', self.part, path, text=False)

    def put(self, src, path, mode='755'):
        self.tool('ufsput.py', self.part, 'put', src, path, mode, '0', '0', '--apply')


def ppm_to_png(ppm, png):
    data = open(ppm, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', data)
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():]
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return (struct.pack('>I', len(body)) + kind + body +
                struct.pack('>I', zlib.crc32(kind + body)))
    with open(png, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2,
                                                                 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def main():
    args = sys.argv[1:]
    limit = 300
    if args[:1] == ['-t']:
        limit = int(args[1])
        args = args[2:]
    if not args:
        sys.exit(__doc__.strip().split('\n\n')[1])
    script, files = args[0], args[1:]
    disk_src, rom, patches = env('AUX_DISK'), env('AUX_ROM'), env('AUX_PATCHES')

    os.makedirs(RUN, exist_ok=True)
    disk_path = os.path.join(RUN, 'disk.dsk')
    for f in ('disk.dsk', 'serial.log', 'screen.ppm', 'screen.png'):
        if os.path.exists(os.path.join(RUN, f)):
            os.remove(os.path.join(RUN, f))
    if subprocess.run(['cp', '-c', disk_src, disk_path], capture_output=True).returncode:
        shutil.copyfile(disk_src, disk_path)
    os.chmod(disk_path, 0o644)

    disk = Disk(disk_path, patches)
    for f in files:
        disk.put(f, '/usr/local/bin/' + os.path.basename(f))
    wrapper = os.path.join(RUN, 'aux-run')
    with open(wrapper, 'w') as f:
        f.write(WRAPPER)
    disk.put(wrapper, '/etc/aux-run')
    disk.put(script, '/etc/aux-run.sh', '644')
    inittab = disk.cat('/etc/inittab')
    inittab_new = os.path.join(RUN, 'inittab')
    with open(inittab_new, 'wb') as f:
        f.write(inittab + INITTAB.encode())
    disk.put(inittab_new, '/etc/inittab', '644')

    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    qemu = subprocess.Popen([
        'qemu-system-m68k', '-M', 'q800', '-m', '64', '-bios', rom, '-display', 'none',
        '-serial', 'file:%s/serial.log' % RUN,
        '-device', 'scsi-hd,scsi-id=0,drive=hd0',
        '-drive', 'file=%s,format=raw,if=none,id=hd0' % disk_path,
        '-monitor', 'tcp:127.0.0.1:%d,server,nowait' % port])
    start = time.time()
    log = None
    try:
        while time.time() - start < limit:
            time.sleep(3)
            if qemu.poll() is not None:
                sys.exit('aux-run: QEMU exited (%d)' % qemu.returncode)
            try:
                text = disk.cat(LOG).decode('mac_roman')
            except subprocess.CalledProcessError:
                continue
            if re.search(r'^aux-run: exit \d+$', text, re.M):
                log = text
                break
        mon = socket.create_connection(('127.0.0.1', port))
        if log is None:
            mon.sendall(b'screendump %s/screen.ppm\n' % RUN.encode())
            time.sleep(2)
        mon.sendall(b'quit\n')
        qemu.wait(15)
    finally:
        if qemu.poll() is None:
            qemu.kill()
    if log is None:
        shot = os.path.join(RUN, 'screen.ppm')
        if os.path.exists(shot):
            ppm_to_png(shot, os.path.join(RUN, 'screen.png'))
        sys.exit('aux-run: no result after %d s; screenshot in %s/screen.png' %
                 (limit, RUN) if os.path.exists(shot) else 'aux-run: no result')
    body, status = re.match(r'(.*)^aux-run: exit (\d+)$', log, re.M | re.S).groups()
    sys.stdout.write(body)
    print('aux-run: finished in %d s, exit %s' % (time.time() - start, status))
    sys.exit(int(status))


if __name__ == '__main__':
    main()
