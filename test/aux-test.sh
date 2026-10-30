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

mkdir /tmp/www /tmp/www/r1 /tmp/www/r2
cd /usr/local/bin
for r in r1 r2; do
	mv $r.index /tmp/www/$r/index
	mv $r.index.sig /tmp/www/$r/index.sig
done
cp hello-1.0.pkg macapp-1.0.pkg /tmp/www/r1
mv hello-1.1.pkg macapp-1.0.pkg /tmp/www/r2
mv hello-1.0.pkg /tmp
mv pkg.conf /etc/pkg.conf
echo "nameserver 127.0.0.1" > /etc/resolv.conf
cd /tmp
httpd 8080 /tmp/www &
httpd=$!
sleep 2

t 'update, the server found by DNS' '2 packages at http://pkg.test:8080/r1/' \
	pkg -s http://pkg.test:8080/r1/ update
t 'list -a' 'hello  *1.0  *A test package' pkg list -a
t 'install' 'Installed hello 1.0' pkg install hello
t 'the installed program runs' 'hello from a package' hello
t "it's root's, mode 755" '^-rwxr-xr-x  *1 root ' ls -l /usr/local/bin/hello
t 'verify' 'No changes found' pkg verify
t 'Mac file names' 'Installed macapp 1.0' pkg install macapp
t '...with the AppleDouble file' '%Fancy Term' ls /mac/bin
t 'update, chunked, localhost from /etc/hosts' '2 packages' \
	pkg -s http://localhost:8080/chunked/r2/ update
t 'list -a shows the upgrade' '(1.0 installed)' pkg list -a
t 'upgrade' 'Upgraded hello from 1.0 to 1.1' pkg upgrade
t 'the new version runs' 'hello again' hello
t 'remove' 'Removed hello 1.1' pkg remove hello
t "it's gone" 'not found' hello
t 'remove the Mac files' 'Removed macapp 1.0' pkg remove macapp
t 'install from a file' 'Installed hello 1.0' pkg install /tmp/hello-1.0.pkg
t 'owner' '^hello$' pkg owner /usr/local/bin/hello
t 'remove it' 'Removed hello 1.0' pkg remove hello
t 'nothing left installed' 'No packages are installed' pkg list
t 'a server that is not there' "can't connect" pkg -s http://127.0.0.1:1/ update

kill $httpd
test -s /tmp/fails && exit 1
exit 0
