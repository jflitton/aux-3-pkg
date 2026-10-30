/* pkg: installs software on A/UX 3, from package files or from a server */
#include "pkg.h"

const char *root = "";
int verbose;

static const char usage[] =
"usage: pkg [-R dir] [-s server] [-u] command [argument ...]\n"
"\n"
"  update                    get the list of packages from the server\n"
"  list                      installed packages\n"
"  list -a                   all packages on the server\n"
"  search word               packages whose name or summary has word in it\n"
"  info package|file         about a package\n"
"  install package|file ...  install from the server, or from package files\n"
"  upgrade [package ...]     install newer versions from the server\n"
"  remove package ...        remove installed packages\n"
"  files package             the files a package installed\n"
"  verify [package ...]      check installed files haven't changed\n"
"  owner path                which package installed a file\n"
"  version                   pkg's own version\n"
"\n"
"  -R dir     act on the A/UX installed under dir instead of /\n"
"  -s server  get the package list from server (an http:// address) this time\n"
"  -u         accept the package list unsigned\n";

static int
isfile(const char *arg)
{
	struct stat st;
	size_t n = strlen(arg);

	return strchr(arg, '/') || (n > 4 && !strcmp(arg + n - 4, ".pkg") &&
		stat(arg, &st) == 0);
}

static void
columns(const char *name, const char *version, const char *summary, const char *note)
{
	say("%-20s %-12s %s%s\n", name, version, summary, note);
}

static int
cmd_list(int all)
{
	char **names;
	int i, n;

	if (all) {
		struct index ix;

		if (loadindex(&ix) < 0)
			return 1;
		for (i = 0; i < ix.n; i++) {
			struct avail *a = &ix.a[i];
			struct record r;
			char *note = NULL;

			if (findavail(&ix, a->m.name) != a)
				continue;	/* an older version */
			if (readrecord(a->m.name, &r) == 0) {
				note = !strcmp(a->m.version, r.m.version) ?
					xstrdup(" (installed)") :
					xsprintf(" (%s installed)", r.m.version);
				freerecord(&r);
			}
			columns(a->m.name, a->m.version, a->m.summary, note ? note : "");
			free(note);
		}
		if (!ix.signed_)
			say("(This list wasn't signed: its contents weren't checked.)\n");
		return 0;
	}
	n = listinstalled(&names);
	for (i = 0; i < n; i++) {
		struct record r;

		if (readrecord(names[i], &r) == 0) {
			columns(r.m.name, r.m.version, r.m.summary, "");
			freerecord(&r);
		}
		free(names[i]);
	}
	free(names);
	if (n == 0)
		say("No packages are installed.\n");
	return 0;
}

static int
contains(const char *s, const char *word)
{
	size_t n = strlen(word);

	for (; *s; s++) {
		size_t i;

		for (i = 0; i < n; i++) {
			int a = s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i];
			int b = word[i] >= 'A' && word[i] <= 'Z' ? word[i] + 32 : word[i];

			if (a != b)
				break;
		}
		if (i == n)
			return 1;
	}
	return 0;
}

static int
cmd_search(const char *word)
{
	struct index ix;
	int i, found = 0;

	if (loadindex(&ix) < 0)
		return 1;
	for (i = 0; i < ix.n; i++) {
		struct avail *a = &ix.a[i];

		if (findavail(&ix, a->m.name) == a &&
		    (contains(a->m.name, word) || contains(a->m.summary, word))) {
			columns(a->m.name, a->m.version, a->m.summary, "");
			found = 1;
		}
	}
	if (!found)
		say("No package matches \"%s\".\n", word);
	return 0;
}

static void
date(long t, char *out)
{
	long days = t / 86400, y = 1970;
	static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int m = 0;

	for (;;) {
		int leap = (y % 4 == 0 && y % 100) || y % 400 == 0;

		if (days < 365 + leap)
			break;
		days -= 365 + leap;
		y++;
	}
	for (;; m++) {
		int n = mdays[m] + (m == 1 && ((y % 4 == 0 && y % 100) || y % 400 == 0));

		if (days < n)
			break;
		days -= n;
	}
	snprintf(out, 11, "%04ld-%02d-%02ld", y, m + 1, days + 1);
}

static int
cmd_info(const char *arg)
{
	struct record r;
	struct index ix;
	struct avail *a = NULL;
	int have = 0;

	if (isfile(arg)) {
		struct meta m;

		if (package_meta(arg, &m) < 0)
			return 1;
		say("%s %s\n%s\n%s", m.name, m.version, m.summary, m.extra ? m.extra : "");
		freemeta(&m);
		return 0;
	}
	if (readrecord(arg, &r) == 0) {
		char d[11];
		int i, files = 0;

		for (i = 0; i < r.n; i++)
			files += r.e[i].type != 'd';
		date(r.installed, d);
		say("%s %s\n%s\n%sinstalled %s from %s (%d file%s)\n", r.m.name,
			r.m.version, r.m.summary, r.m.extra ? r.m.extra : "", d, r.from, files,
			files == 1 ? "" : "s");
		have = 1;
	}
	if (loadindex(&ix) == 0 && (a = findavail(&ix, arg))) {
		if (!have)
			say("%s %s\n%s\n", a->m.name, a->m.version, a->m.summary);
		else if (vercmp(a->m.version, r.m.version) > 0)
			say("version %s is on the server\n", a->m.version);
		say("on the server: %s%s (%ld KB)\n", ix.url, a->file, (a->size + 1023) / 1024);
	}
	if (have)
		freerecord(&r);
	if (!have && !a) {
		warn("there's no package called %s", arg);
		return 1;
	}
	return 0;
}

static int
install_avail(struct index *ix, struct avail *a)
{
	char *path, *url;
	int r;

	if (fetch_package(ix, a, &path) < 0)
		return -1;
	url = xsprintf("%s%s", ix->url, a->file);
	r = install_file(path, url, a->m.name);
	unlink(path);
	free(path);
	free(url);
	return r;
}

static int
cmd_install(char **args, int n)
{
	struct index ix;
	int i, have_ix = 0, bad = 0;

	for (i = 0; i < n; i++) {
		struct avail *a;
		struct record r;

		if (isfile(args[i])) {
			bad |= install_file(args[i], args[i], NULL) < 0;
			continue;
		}
		if (!have_ix && loadindex(&ix) < 0)
			return 1;
		have_ix = 1;
		if (!(a = findavail(&ix, args[i]))) {
			warn("there's no package called %s at %s (try pkg search)", args[i], ix.url);
			bad = 1;
			continue;
		}
		if (readrecord(a->m.name, &r) == 0) {
			int same = !strcmp(r.m.version, a->m.version);

			freerecord(&r);
			if (same) {
				say("%s %s is already installed\n", a->m.name, a->m.version);
				continue;
			}
		}
		if (!ix.signed_)
			warn("the package list wasn't signed: installing %s unchecked", args[i]);
		bad |= install_avail(&ix, a) < 0;
	}
	return bad;
}

static int
cmd_upgrade(char **args, int n)
{
	struct index ix;
	char **names = args;
	int i, bad = 0, did = 0, all = n == 0;

	if (loadindex(&ix) < 0)
		return 1;
	if (all)
		n = listinstalled(&names);
	for (i = 0; i < n; i++) {
		struct record r;
		struct avail *a = findavail(&ix, names[i]);

		if (readrecord(names[i], &r) < 0) {
			if (!all) {
				warn("%s isn't installed", names[i]);
				bad = 1;
			}
			continue;
		}
		if (a && vercmp(a->m.version, r.m.version) > 0) {
			bad |= install_avail(&ix, a) < 0;
			did = 1;
		}
		freerecord(&r);
	}
	if (!did && !bad)
		say("Everything is up to date.\n");
	if (all) {
		for (i = 0; i < n; i++)
			free(names[i]);
		free(names);
	}
	return bad;
}

static int
cmd_files(const char *name)
{
	struct record r;
	int i;

	if (readrecord(name, &r) < 0) {
		warn("%s isn't installed", name);
		return 1;
	}
	for (i = 0; i < r.n; i++)
		if (r.e[i].type == 'l')
			say("/%s -> %s\n", r.e[i].path, r.e[i].target);
		else
			say("/%s%s\n", r.e[i].path, r.e[i].type == 'd' ? "/" : "");
	freerecord(&r);
	return 0;
}

static int
cmd_verify(char **args, int n)
{
	char **names = args;
	int i, bad = 0, all = n == 0;

	if (all)
		n = listinstalled(&names);
	for (i = 0; i < n; i++)
		bad |= verify_package(names[i]) < 0;
	if (!bad)
		say("No changes found.\n");
	if (all) {
		for (i = 0; i < n; i++)
			free(names[i]);
		free(names);
	}
	return bad;
}

static int
cmd_owner(const char *path)
{
	char *who;

	while (*path == '/')
		path++;
	if (!(who = owner(path))) {
		say("No package installed /%s.\n", path);
		return 1;
	}
	say("%s\n", who);
	free(who);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *server = NULL, *cmd;
	int i, allow_unsigned = 0, r = 0, locks;
	char **args;

	signal(SIGPIPE, SIG_IGN);
	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		const char *o = argv[i];

		if (!strcmp(o, "-R") && i + 1 < argc)
			root = argv[++i];
		else if (!strcmp(o, "-s") && i + 1 < argc)
			server = argv[++i];
		else if (!strcmp(o, "-u"))
			allow_unsigned = 1;
		else if (!strcmp(o, "-v"))
			verbose = 1;
		else {
			write(2, usage, sizeof(usage) - 1);
			return 2;
		}
	}
	if (i == argc) {
		write(2, usage, sizeof(usage) - 1);
		return 2;
	}
	if (*root) {
		char *r2 = xstrdup(root);
		struct stat st;

		while (strlen(r2) > 1 && r2[strlen(r2) - 1] == '/')
			r2[strlen(r2) - 1] = 0;
		if (!strcmp(r2, "/"))
			r2[0] = 0;
		if (*r2 && (stat(r2, &st) < 0 || !S_ISDIR(st.st_mode))) {
			warn("%s isn't a directory", r2);
			return 2;
		}
		root = r2;
	}
	cmd = argv[i];
	args = argv + i + 1;
	argc -= i + 1;

	locks = !strcmp(cmd, "update") || !strcmp(cmd, "install") ||
		!strcmp(cmd, "upgrade") || !strcmp(cmd, "remove");
	if (locks && lock() < 0)
		return 1;
	if (!strcmp(cmd, "update") && argc == 0)
		r = update(server, allow_unsigned) < 0;
	else if (server)
		r = (warn("-s goes with update: other commands use the list it got"), 2);
	else if (!strcmp(cmd, "list") && argc == 0)
		r = cmd_list(0);
	else if (!strcmp(cmd, "list") && argc == 1 && !strcmp(args[0], "-a"))
		r = cmd_list(1);
	else if (!strcmp(cmd, "search") && argc == 1)
		r = cmd_search(args[0]);
	else if (!strcmp(cmd, "info") && argc == 1)
		r = cmd_info(args[0]);
	else if (!strcmp(cmd, "install") && argc > 0)
		r = cmd_install(args, argc);
	else if (!strcmp(cmd, "upgrade"))
		r = cmd_upgrade(args, argc);
	else if (!strcmp(cmd, "remove") && argc > 0) {
		for (i = 0; i < argc; i++)
			r |= remove_package(args[i]) < 0;
	} else if (!strcmp(cmd, "files") && argc == 1)
		r = cmd_files(args[0]);
	else if (!strcmp(cmd, "verify"))
		r = cmd_verify(args, argc);
	else if (!strcmp(cmd, "owner") && argc == 1)
		r = cmd_owner(args[0]);
	else if (!strcmp(cmd, "version") && argc == 0)
		say("pkg %s\n", PKG_VERSION);
	else {
		write(2, usage, sizeof(usage) - 1);
		r = 2;
	}
	if (locks)
		unlock();
	return r;
}
