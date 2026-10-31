# Runs inside A/UX, started by tools/aux-run.py (make test-aux): the runtime's and the
# unit tests, then pkg end to end against test/httpd on the loopback interface, which
# also answers DNS. The files are in /usr/local/bin; test/make-repo.py made the packages
# and package lists. Prints a line per check; exits 1 if any failed.

rm -f /tmp/fails
rt-test arg || echo rt-test >> /tmp/fails
unit || echo unit >> /tmp/fails

# t NAME TEXT COMMAND...: COMMAND's output should have TEXT (a grep pattern) in it
t() {
	name=$1
	want=$2
	shift
	shift
	"$@" > /tmp/t.out 2>&1
	if grep "$want" /tmp/t.out > /dev/null; then
		echo "ok   $name"
	else
		echo "FAIL $name"
		sed 's/^/     | /' /tmp/t.out
		echo "$name" >> /tmp/fails
	fi
}

# e NAME STATUS TEXT COMMAND...: as t, and COMMAND should exit with STATUS
e() {
	name=$1
	status=$2
	want=$3
	shift
	shift
	shift
	"$@" > /tmp/t.out 2>&1
	got=$?
	if test $got -eq $status && grep "$want" /tmp/t.out > /dev/null; then
		echo "ok   $name"
	else
		echo "FAIL $name (exit $got)"
		sed 's/^/     | /' /tmp/t.out
		echo "$name" >> /tmp/fails
	fi
}

mkdir /tmp/www /tmp/www/r1 /tmp/www/r2 /tmp/www/r3
cd /usr/local/bin
for r in r1 r2 r3; do
	mv $r.index /tmp/www/$r/index
	mv $r.index.sig /tmp/www/$r/index.sig
done
cp hello-1.0.pkg macapp-1.0.pkg /tmp/www/r1
cp hello-1.1.pkg macapp-1.0.pkg /tmp/www/r3
mv hello-1.1.pkg macapp-1.0.pkg /tmp/www/r2
mv pkg-2.0.pkg /tmp/www/r3
mv hello-1.0.pkg pkg-1.0.pkg /tmp
mv pkg.conf /etc/pkg.conf
echo "nameserver 127.0.0.1" > /etc/resolv.conf
cd /tmp
httpd 8080 /tmp/www &
httpd=$!
sleep 2

pkg update -s http://pkg.test:8080/r1/ > /tmp/update.out 2>&1
t 'update, the server found by DNS' 'main: 2 packages at http://pkg.test:8080/r1/' \
	cat /tmp/update.out
t '...and server two, chunked, localhost from /etc/hosts' \
	'two: 2 packages at http://localhost:8080/chunked/r2/' cat /tmp/update.out
t 'list -a' 'hello  *1.0  *A test package' pkg list -a
t "list -a, server two's" 'two/hello  *1.1  *A test package' pkg list -a
t 'install' 'Installed hello 1.0' pkg install hello
t 'the installed program runs' 'hello from a package' hello
t "it's root's, mode 755" '^-rwxr-xr-x  *1 root ' ls -l /usr/local/bin/hello
t 'verify' 'No changes found' pkg verify
t 'Mac file names' 'Installed macapp 1.0' pkg install macapp
t '...with the AppleDouble file' '%Fancy Term' ls /mac/bin
t "upgrade doesn't take server two's" 'up to date' pkg upgrade
t 'install from server two' 'Upgraded hello from 1.0 to 1.1' pkg install two/hello
t 'the new version runs' 'hello again' hello
t "main doesn't trust server two's key" "isn't signed by a key pkg trusts" \
	pkg -s http://pkg.test:8080/r2/ update

# the command line (patterns never start with -, which grep would take as an option)
e 'pkg alone' 2 'usage: pkg' pkg
e 'pkg help, on stdout' 0 'owner /path' sh -c 'pkg help 2> /dev/null'
e 'pkg -h' 0 'owner /path' pkg -h
e 'not a command' 2 "instal isn't a command" pkg instal hello
e 'not an option' 2 "x isn't an option" pkg -x list
e 'too few arguments' 2 '^usage: pkg search word$' pkg search
e 'an option with the wrong command' 2 's goes with update' pkg list -s http://pkg.test/
e '...and another' 2 'unsigned goes with update' pkg install --unsigned hello
e 'a server name for -s' 2 'http:// address, not two' pkg update -s two
e 'a mistake anywhere does nothing' 2 "isn't a directory" pkg remove hello -r /no/such
t '...hello is still there' 'hello again' hello
e '-- ends the options' 1 "x isn't installed" pkg remove -- -x
e 'search without a match' 1 'No package matches' pkg search zzz
t 'list shows where a package came from' '^two/hello  *1.1 ' pkg list
e 'files SERVER/NAME' 0 '^/usr/local/bin/hello$' pkg files two/hello
e 'SERVER/NAME must be where it came from' 1 'hello came from two, not main' \
	pkg remove main/hello
mkdir /tmp/two
cp /tmp/hello-1.0.pkg /tmp/two/hello
e 'SERVER/NAME is a package, even with a file by that name' 0 'on two: 1.1, ' \
	pkg info two/hello
e '...and ./ makes it a file' 0 '^hello 1.0$' pkg info ./two/hello
rm -r /tmp/two
e 'NAME.pkg is a file' 1 "can't open nothere.pkg" pkg install nothere.pkg
e 'owner, // and a trailing /' 0 '^hello$' pkg owner //usr/local//bin/hello/
e 'owner of a relative path' 2 'starting with /' pkg owner usr/local/bin/hello
e 'owner with ..' 2 'without . or ..' pkg owner /usr/local/bin/../bin/hello
t 'remove' 'Removed hello 1.1' pkg remove hello
t "it's gone" 'not found' hello
t 'remove the Mac files' 'Removed macapp 1.0' pkg remove macapp
t 'install from a file' 'Installed hello 1.0' pkg install /tmp/hello-1.0.pkg
t 'owner' '^hello$' pkg owner /usr/local/bin/hello
t 'remove it' 'Removed hello 1.0' pkg remove hello
t 'nothing left installed' 'No packages are installed' pkg list
t 'a server that is not there' "can't connect" pkg -s http://127.0.0.1:1/ update

# shells: Guest's chsh takes only what /etc/shells lists
perms() { ls -l /etc/shells | awk '{ print substr($1, 2), $3, $4 }'; }
before=`perms`
echo 'echo not listed' > /tmp/notlisted
chmod 755 /tmp/notlisted
t "Guest's chsh refuses a shell that isn't listed" ':/bin/csh$' sh -c \
	'su Guest -c "chsh Guest /tmp/notlisted"; grep "^Guest:" /etc/passwd'
t 'a package with a shell' 'Added /usr/local/bin/shelly to /etc/shells' \
	pkg install /usr/local/bin/shelly-1.0.pkg
t "...keeping /etc/shells' owner and mode" "$before" perms
t "...so a user's chsh takes it" 'shelly' sh -c \
	'su Guest -c "chsh Guest /usr/local/bin/shelly"; grep "^Guest:" /etc/passwd'
e "remove refused while it's someone's shell" 1 'login shell of Guest' pkg remove shelly
t '...and it stays in /etc/shells' '^/usr/local/bin/shelly$' cat /etc/shells
chsh Guest /bin/csh > /dev/null
t 'remove' 'Took /usr/local/bin/shelly out of /etc/shells' pkg remove shelly
e "...and it's gone" 1 '^0$' grep -c shelly /etc/shells

# pkg upgrades itself first, then runs the new pkg for the rest. Its package's pkg is a
# script that runs the one being tested; 2.0's says it's running first.
mv /usr/local/bin/pkg /tmp/pkg.real
t 'pkg as a package' 'Installed pkg 1.0' /tmp/pkg.real install /tmp/pkg-1.0.pkg
pkg update -s http://pkg.test:8080/r1/ > /dev/null 2>&1
t '...and hello' 'Installed hello 1.0' pkg install hello
pkg update -s http://pkg.test:8080/r3/ > /dev/null 2>&1
pkg upgrade > /tmp/upgrade.out 2>&1
echo "exit $?" >> /tmp/upgrade.out
t 'upgrade does pkg first' '^Upgraded pkg from 1.0 to 2.0$' sed -n 1p /tmp/upgrade.out
t '...then runs the new pkg' '^pkg 2.0: upgrade$' sed -n 2p /tmp/upgrade.out
t '...which does the rest' '^Upgraded hello from 1.0 to 1.1$' sed -n 3p /tmp/upgrade.out
t '...and succeeds' '^exit 0$' sed -n 4p /tmp/upgrade.out
t '...and lets go of the lock' 'no lock' sh -c \
	'test -f /usr/local/lib/pkg/lock || echo no lock'
t '...leaving everything up to date' 'up to date' pkg upgrade

kill $httpd
test -s /tmp/fails && exit 1
exit 0
