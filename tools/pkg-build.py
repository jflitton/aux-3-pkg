#!/usr/bin/env python3
"""Make a pkg package from a directory.

  pkg-build.py DIR [OUTDIR]

DIR holds the package's +PKG (docs/formats.md) and its files laid out as they'll be
installed, relative to /: DIR/usr/local/bin/hello becomes /usr/local/bin/hello. Writes
OUTDIR/NAME-VERSION.pkg (OUTDIR defaults to the current directory) and prints its path.

Files keep their modes and modification times and are owned by root. Names are written
in Mac Roman, as A/UX's Finder shows them. Symbolic links stay links; a file with several
names becomes separate copies. macOS's own clutter (.DS_Store, ._ files) is left out. A Mac file's resource fork goes in as its A/UX AppleDouble file,
%NAME, which must already be in DIR. The output is the same each time for the same DIR.
"""
import gzip, io, os, re, stat, sys, tarfile

NAME = re.compile(r'[a-z0-9][a-z0-9+._-]{0,63}$')
VERSION = re.compile(r'[A-Za-z0-9+._~-]{1,64}$')
PKGDIR = 'usr/local/lib/pkg'


def die(msg):
    sys.exit('pkg-build: %s' % msg)


def read_meta(path):
    meta = {}
    with open(path, encoding='ascii') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            key, sep, value = line.partition(' ')
            if not sep:
                die('%s line %d has no value' % (path, n))
            meta[key] = value
    for key in ('format', 'name', 'version', 'summary'):
        if key not in meta:
            die('%s has no %s line' % (path, key))
    if meta['format'] != '1':
        die('%s: this pkg-build makes format 1' % path)
    if not NAME.match(meta['name']):
        die('%s: name %r isn\'t allowed (docs/formats.md)' % (path, meta['name']))
    if not VERSION.match(meta['version']):
        die('%s: version %r isn\'t allowed (docs/formats.md)' % (path, meta['version']))
    return meta


def check_path(rel):
    if any(ord(c) < 0x20 or ord(c) == 0x7f for c in rel):
        die('%r: control characters (tabs, newlines) aren\'t allowed in names' % rel)
    try:
        if len(rel.encode('mac_roman')) > 255:
            die('%s: longer than 255 bytes' % rel)
    except UnicodeEncodeError:
        die('%s: has characters Mac Roman doesn\'t' % rel)
    if rel == PKGDIR or rel.startswith(PKGDIR + '/'):
        die('%s: nothing may go in pkg\'s own directory' % rel)
    if rel.startswith('+'):
        die('%s: names starting with + are reserved' % rel)


def walk(top):
    """(relative path, full path) of everything under top, sorted, +PKG and clutter left
    out."""
    out = []
    for dirpath, dirs, files in os.walk(top):
        dirs.sort()
        rel_dir = os.path.relpath(dirpath, top)
        for name in sorted(dirs + files):
            if name == '.DS_Store' or name.startswith('._'):
                continue
            rel = name if rel_dir == '.' else rel_dir + '/' + name
            if rel == '+PKG':
                continue
            out.append((rel, os.path.join(dirpath, name)))
    return sorted(out)


def build(top, outdir):
    meta = read_meta(os.path.join(top, '+PKG'))
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT,
                      encoding='mac_roman') as tar:
        pkg = open(os.path.join(top, '+PKG'), 'rb').read()
        info = tarfile.TarInfo('+PKG')
        info.size, info.mode, info.mtime = len(pkg), 0o644, 0
        tar.addfile(info, io.BytesIO(pkg))
        for rel, full in walk(top):
            check_path(rel)
            st = os.lstat(full)
            info = tarfile.TarInfo(rel)
            info.mode = stat.S_IMODE(st.st_mode)
            info.mtime = int(st.st_mtime)
            info.uid = info.gid = 0
            info.uname = info.gname = 'root'
            if stat.S_ISLNK(st.st_mode):
                info.type = tarfile.SYMTYPE
                info.linkname = os.readlink(full)
                tar.addfile(info)
            elif stat.S_ISDIR(st.st_mode):
                info.type = tarfile.DIRTYPE
                tar.addfile(info)
            elif stat.S_ISREG(st.st_mode):
                info.size = st.st_size
                with open(full, 'rb') as f:
                    tar.addfile(info, f)
            else:
                die('%s: only files, directories and symbolic links can go in a package'
                    % rel)
    out = os.path.join(outdir, '%s-%s.pkg' % (meta['name'], meta['version']))
    with open(out, 'wb') as f:
        with gzip.GzipFile(filename='', mode='wb', fileobj=f, mtime=0, compresslevel=9) as z:
            z.write(raw.getvalue())
    return out


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.strip().split('\n\n')[1])
    print(build(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else '.'))


if __name__ == '__main__':
    main()
