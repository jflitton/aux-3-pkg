# Formats

The files `pkg` reads and writes. Each has a format number; `pkg` refuses a number higher
than it knows (see [decisions.md](decisions.md), "No dependencies yet"). All text is
ASCII lines ending in a newline; fields are separated by one tab, so names and paths may
contain spaces (Mac files often do) but not tabs or newlines.

## Names, versions and paths

- A package **name** is 1 to 64 of `a-z 0-9 + - . _`, starting with a letter or digit.
- A **version** is 1 to 64 of `A-Z a-z 0-9 + - . _ ~`. Versions compare as Debian's do:
  runs of digits numerically, everything else character by character, and `~` sorts
  before anything, even the end (`1.0~beta1` < `1.0` < `1.0-1` < `1.0.1` < `1.10`).
- A **path** in a package is relative to `/`, without a leading `/` or `./`: for example
  `usr/local/bin/hello`. No empty, `.` or `..` components, no control characters, at most
  255 bytes. Nothing may go under `usr/local/lib/pkg`, which is `pkg`'s own.

## Package file (`NAME-VERSION.pkg`)

A gzip-compressed ustar archive. The first member is `+PKG`, a regular file of `key
value` lines (the key, one space, the rest of the line):

    format 1
    name hello
    version 1.0-1
    summary Prints a greeting

`format`, `name`, `version` and `summary` are required; other keys are kept and shown by
`pkg info` (for example `url`, `license`, `maintainer`). Every other member is the
package's content: regular files, directories and symbolic links, with their modes,
owners and modification times. Other member types, and other names starting with `+`,
make the package invalid. A Mac file's resource fork and Finder information are its
AppleDouble `%NAME` file beside it, an ordinary file to `pkg`.

`tools/pkg-build.py` makes these.

## Server

A server is a URL ending in `/`, under which are:

| Path | What |
|---|---|
| `index` | the packages on offer |
| `index.sig` | signatures of `index` |
| the packages | wherever `index` says, relative to the server's URL |

`index`:

    aux-pkg-index 1
    hello	1.0-1	12345	SHA256	hello-1.0-1.pkg	Prints a greeting

The first line names the format. Then one line per package: name, version, size in bytes,
the SHA-256 of the package file (64 lowercase hex digits), its path relative to the
server, and its summary. Lines starting with `#` are ignored. A name may appear more than
once, at different versions; `pkg` takes the highest.

`index.sig`: one or more lines of

    ed25519	PUBLIC-KEY	SIGNATURE

the key (64 hex digits) and an Ed25519 signature of the whole of `index` (128 hex
digits). `pkg` accepts `index` if a line's key is one it trusts and its signature is good.

`tools/pkg-repo.py` makes both from a directory of packages.

## On the Mac

| Path | What |
|---|---|
| `/etc/pkg.conf` | settings (below) |
| `/usr/local/lib/pkg/installed/NAME` | what's installed (below) |
| `/usr/local/lib/pkg/index`, `index.sig` | the last package list fetched |
| `/usr/local/lib/pkg/index.url` | the server it came from, and whether it was signed |
| `/usr/local/lib/pkg/download/` | downloads in progress |
| `/usr/local/lib/pkg/lock` | the process ID of the `pkg` changing things, while it runs |

`/etc/pkg.conf`, optional, `#` starting a comment:

    server http://example.com/aux/     # instead of the built-in server
    key 0123...cdef                    # also trust this signing key
    unsigned                           # accept the server's package list unsigned

The record of an installed package: `key value` lines as in `+PKG` (plus `installed`, the
time as seconds since 1970, and `from`, the URL or file it came from), then a line per
thing it installed:

    f	PATH	MODE	UID	GID	SIZE	SHA256	a regular file (MODE in octal)
    l	PATH	TARGET					a symbolic link
    d	PATH						a directory it created

Only directories the package created are listed; removing the package removes them if
they are empty.
