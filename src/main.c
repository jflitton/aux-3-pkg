/* pkg: installs software on A/UX 3, from package files or from a server */
#include "pkg.h"

const char *root = "";

static const struct command {
	const char *name, *args, *what;
	int min, max;		/* how many arguments; max -1 for any number */
	int locks;		/* changes things */
} commands[] = {
	{ "update", "[-s url] [--unsigned]", "get the package lists from the servers",
		0, 0, 1 },
	{ "list", "[-a]", "installed packages; -a, all on the servers", 0, 0, 0 },
	{ "search", "word", "packages whose name or summary has word in it", 1, 1, 0 },
	{ "info", "package|file", "about a package", 1, 1, 0 },
	{ "install", "package|file ...", "install from a server, or from package files",
		1, -1, 1 },
	{ "upgrade", "[package ...]", "install newer versions from their own servers",
		0, -1, 1 },
	{ "remove", "package ...", "remove installed packages", 1, -1, 1 },
	{ "files", "package", "the files a package installed", 1, 1, 0 },
	{ "verify", "[package ...]", "check installed files haven't changed", 0, -1, 0 },
	{ "owner", "/path", "which package installed a file", 1, 1, 0 },
	{ "version", "", "pkg's own version", 0, 0, 0 },
	{ "help", "", "this", 0, 0, 0 },
};
#define NCOMMANDS	(int)(sizeof(commands) / sizeof(commands[0]))

static const char options[] =
"\n"
"  -r dir        act on the A/UX installed under dir instead of /\n"
"  -s url        take main's list from url instead, until the next update\n"
"  --unsigned    accept main's list even if it isn't signed\n"
"\n"
"A package is NAME, or SERVER/NAME for a particular server's, as pkg list shows\n"
"them. A package file is a path with a / in it, or a name ending in .pkg.\n"
"Options can go before or after the command.\n";

static void
usage(int fd)
{
	int i;

	dprintf(fd, "usage: pkg [-r dir] command [argument ...]\n\n");
	for (i = 0; i < NCOMMANDS; i++) {
		char *s = xsprintf("%s %s", commands[i].name, commands[i].args);

		dprintf(fd, "  %-29s %s\n", s, commands[i].what);
		free(s);
	}
	dprintf(fd, "%s", options);
}

/* A mistake on the command line: says what, and returns pkg's exit status for one */
static int
mistake(const char *fmt, ...)
{
	char buf[300];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	warn("%s", buf);
	return 2;
}

/* If arg is SERVER/NAME, SERVER (a new string), with *name set to NAME; otherwise NULL */
static char *
splitname(const char *arg, const char **name)
{
	const char *slash = strchr(arg, '/');
	char *server;

	if (!slash)
		return NULL;
	server = xstrdup(arg);
	server[slash - arg] = 0;
	if (!validname(server) || !validname(slash + 1)) {
		free(server);
		return NULL;
	}
	*name = slash + 1;
	return server;
}

/* A package file, rather than a package on a server. That's decided by how arg is
   written, not by what's on disk: a name ending in .pkg (which no package's can), or a
   path with a / in it, but SERVER/NAME is a package. */
static int
isfile(const char *arg)
{
	size_t n = strlen(arg);
	const char *name;
	char *server;

	if (n > 4 && !strcmp(arg + n - 4, ".pkg"))
		return 1;
	if (!strchr(arg, '/'))
		return 0;
	if (!(server = splitname(arg, &name)))
		return 1;
	free(server);
	return 0;
}

static void
columns(const char *name, const char *version, const char *summary, const char *note)
{
	say("%-20s %-12s %s%s\n", name, version, summary, note);
}

/* a package on a server, as it's named on the command line */
static char *
fullname(struct avail *a)
{
	return strcmp(a->s->name, MAINSERVER) ? xsprintf("%s/%s", a->s->name, a->m.name) :
		xstrdup(a->m.name);
}

static void
showavail(struct avail *a, const char *note)
{
	char *name = fullname(a);

	columns(name, a->m.version, a->m.summary, note);
	free(name);
}

/*
 * Where an installed package's new versions come from: the server it came from. One
 * installed from a file, or before records named servers, follows the first server that
 * has it. NULL if there's none, with *gone set if that's because pkg no longer has a
 * list from its server.
 */
static struct avail *
follows(struct index *ix, struct record *r, int *gone)
{
	struct server *s = NULL;

	*gone = 0;
	if (r->server && !(s = findserver(ix, r->server))) {
		*gone = 1;
		return NULL;
	}
	return findavail(ix, r->m.name, s);
}

static void
gonewarn(struct record *r)
{
	warn("%s came from %s, which pkg has no package list from; pkg install SERVER/%s "
		"gets it from another server", r->m.name, r->server, r->m.name);
}

/*
 * The package that arg means: SERVER/NAME is NAME from that server. Otherwise an
 * installed package comes from the server it came from, and anything else from the
 * first server that has it. NULL, having said why, if there's none.
 */
static struct avail *
choose(struct index *ix, const char *arg)
{
	const char *name = arg;
	char *sname = splitname(arg, &name);
	struct server *s = NULL;
	struct avail *a = NULL;
	struct record r;
	int gone;

	if (sname) {
		if (!(s = findserver(ix, sname)))
			warn("there's no server called %s: add it to %s, then pkg update", sname,
				CONFFILE);
		else if (!(a = findavail(ix, name, s)))
			warn("there's no package called %s at %s (try pkg search)", name, sname);
		free(sname);
		return a;
	}
	if (readrecord(name, &r) == 0) {
		a = follows(ix, &r, &gone);
		if (gone)
			gonewarn(&r);
		else if (!a && r.server)
			warn("there's no package called %s at %s (try pkg search)", name,
				r.server);
		else if (!a)
			warn("there's no package called %s (try pkg search)", name);
		freerecord(&r);
		return a;
	}
	if (!(a = findavail(ix, name, NULL)))
		warn("there's no package called %s (try pkg search)", name);
	return a;
}

/*
 * The name of the installed package arg names: NAME, or SERVER/NAME as pkg list shows it,
 * which must be the server it came from. NULL, having said why, if there's none.
 */
static const char *
installedname(const char *arg)
{
	const char *name = arg;
	char *sname = splitname(arg, &name);
	struct record r;

	if (!validname(name) || readrecord(name, &r) < 0) {
		warn("%s isn't installed", name);
		name = NULL;
	} else {
		if (sname && !r.server)
			warn("%s isn't recorded as coming from %s: give its name alone", name,
				sname);
		else if (sname && strcmp(r.server, sname))
			warn("%s came from %s, not %s", name, r.server, sname);
		if (sname && (!r.server || strcmp(r.server, sname)))
			name = NULL;
		freerecord(&r);
	}
	free(sname);
	return name;
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
			int gone;

			if (findavail(&ix, a->m.name, a->s) != a)
				continue;	/* an older version */
			if (readrecord(a->m.name, &r) == 0) {
				if (follows(&ix, &r, &gone) == a)
					note = !strcmp(a->m.version, r.m.version) ?
						xstrdup(" (installed)") :
						xsprintf(" (%s installed)", r.m.version);
				freerecord(&r);
			}
			showavail(a, note ? note : "");
			free(note);
		}
		for (i = 0; i < ix.ns; i++)
			if (!ix.s[i].signed_)
				say("(The list from %s wasn't signed: its contents weren't "
					"checked.)\n", ix.s[i].name);
		return 0;
	}
	n = listinstalled(&names);
	for (i = 0; i < n; i++) {
		struct record r;

		if (readrecord(names[i], &r) == 0) {
			char *name = r.server && strcmp(r.server, MAINSERVER) ?
				xsprintf("%s/%s", r.server, r.m.name) : xstrdup(r.m.name);

			columns(name, r.m.version, r.m.summary, "");
			free(name);
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

		if (findavail(&ix, a->m.name, a->s) == a &&
		    (contains(a->m.name, word) || contains(a->m.summary, word))) {
			showavail(a, "");
			found = 1;
		}
	}
	if (!found)
		say("No package matches \"%s\".\n", word);
	return !found;
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
	const char *name = arg;
	char *sname = NULL;
	int have = 0, found = 0, i;

	if (isfile(arg)) {
		struct meta m;

		if (package_meta(arg, &m) < 0)
			return 1;
		say("%s %s\n%s\n%s", m.name, m.version, m.summary, m.extra ? m.extra : "");
		freemeta(&m);
		return 0;
	}
	sname = splitname(arg, &name);
	if (readrecord(name, &r) == 0) {
		char d[11];
		int files = 0;

		for (i = 0; i < r.n; i++)
			files += r.e[i].type != 'd';
		date(r.installed, d);
		say("%s %s\n%s\n%sinstalled %s from %s%s%s (%d file%s)\n", r.m.name,
			r.m.version, r.m.summary, r.m.extra ? r.m.extra : "", d,
			r.server ? r.server : "", r.server ? ", " : "", r.from, files,
			files == 1 ? "" : "s");
		freerecord(&r);
		have = 1;
	}
	if (loadindex(&ix) == 0)
		for (i = 0; i < ix.ns; i++) {
			struct server *s = &ix.s[i];
			struct avail *a = findavail(&ix, name, s);

			if (!a || (sname && strcmp(sname, s->name)))
				continue;
			if (!have && !found)
				say("%s %s\n%s\n", a->m.name, a->m.version, a->m.summary);
			say("on %s: %s, %s%s (%ld KB)\n", s->name, a->m.version, s->url, a->file,
				(a->size + 1023) / 1024);
			found = 1;
		}
	if (!have && !found)
		warn("there's no package called %s", arg);
	free(sname);
	return !have && !found;
}

static int
install_avail(struct avail *a)
{
	char *path, *url;
	int r;

	if (fetch_package(a, &path) < 0)
		return -1;
	url = xsprintf("%s%s", a->s->url, a->file);
	r = install_file(path, url, a->s->name, a->m.name);
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
			bad |= install_file(args[i], args[i], NULL, NULL) < 0;
			continue;
		}
		if (!have_ix && loadindex(&ix) < 0)
			return 1;
		have_ix = 1;
		if (!(a = choose(&ix, args[i]))) {
			bad = 1;
			continue;
		}
		if (readrecord(a->m.name, &r) == 0) {
			int same = !strcmp(r.m.version, a->m.version) && r.server &&
				!strcmp(r.server, a->s->name);

			freerecord(&r);
			if (same) {
				say("%s %s is already installed\n", a->m.name, a->m.version);
				continue;
			}
		}
		if (!a->s->signed_)
			warn("the list from %s wasn't signed: installing %s unchecked", a->s->name,
				a->m.name);
		bad |= install_avail(a) < 0;
	}
	return bad;
}

/* The newer version of installed package name, from the server it follows; NULL if
   there's none. Says nothing. */
static struct avail *
newer(struct index *ix, const char *name)
{
	struct avail *a = NULL;
	struct record r;
	int gone;

	if (validname(name) && readrecord(name, &r) == 0) {
		a = follows(ix, &r, &gone);
		if (a && vercmp(a->m.version, r.m.version) <= 0)
			a = NULL;
		freerecord(&r);
	}
	return a;
}

/* Whether arg names pkg's own package, as installedname() would take it. Says nothing. */
static int
isself(const char *arg)
{
	const char *name = arg;
	char *sname = splitname(arg, &name);
	struct record r;
	int yes = 0;

	if (!strcmp(name, SELF) && readrecord(SELF, &r) == 0) {
		yes = !sname || (r.server && !strcmp(r.server, sname));
		freerecord(&r);
	}
	free(sname);
	return yes;
}

/*
 * pkg upgrades itself before anything else, as the others' new versions may need the new
 * pkg, and then, if there's more to do, runs the new pkg to do it: pkg upgrade again,
 * with the same packages less pkg. Not with -r, where the pkg running isn't the one it
 * upgraded. args are upgrade's, none for all that's installed. 1 if it upgraded pkg or
 * tried to, setting *bad if that failed; 0 if pkg isn't to be upgraded.
 */
static int
upgrade_self(struct index *ix, char **args, int n, int *bad)
{
	struct avail *a;
	char **argv;
	int i, j, self = n == 0, more = 0;

	if (n == 0) {
		char **names;
		int k = listinstalled(&names);

		for (i = 0; i < k; i++) {
			more |= strcmp(names[i], SELF) && newer(ix, names[i]);
			free(names[i]);
		}
		free(names);
	}
	for (i = 0; i < n; i++)
		if (isself(args[i]))
			self = 1;
		else {
			const char *name = args[i];

			free(splitname(args[i], &name));
			more |= newer(ix, name) != NULL;
		}
	if (!self || !(a = newer(ix, SELF)))
		return 0;
	if (install_avail(a) < 0) {
		*bad = 1;
		return 1;
	}
	if (!more || *root)
		return 1;
	argv = xmalloc((n + 3) * sizeof(char *));
	argv[0] = SELF;
	argv[1] = "upgrade";
	for (i = 0, j = 2; i < n; i++)
		if (!isself(args[i]))
			argv[j++] = args[i];
	argv[j] = NULL;
	execve(SELFPROG, argv, environ);
	warn("can't run the new pkg, %s: %s; carrying on with this one", SELFPROG,
		strerror(errno));
	free(argv);
	return 1;
}

static int
cmd_upgrade(char **args, int n)
{
	struct index ix;
	char **names = args;
	int i, bad = 0, did, stuck = 0, all = n == 0;

	if (loadindex(&ix) < 0)
		return 1;
	did = upgrade_self(&ix, args, n, &bad);
	if (all)
		n = listinstalled(&names);
	for (i = 0; i < n; i++) {
		const char *name = all ? names[i] : installedname(names[i]);
		struct record r;
		struct avail *a;
		int gone;

		if (name && did && !strcmp(name, SELF))
			continue;	/* done first */
		if (!name || readrecord(name, &r) < 0) {
			bad |= !all;
			continue;
		}
		if (!(a = follows(&ix, &r, &gone)) && gone) {
			gonewarn(&r);
			bad |= !all;
			stuck = 1;
		} else if (a && vercmp(a->m.version, r.m.version) > 0) {
			bad |= install_avail(a) < 0;
			did = 1;
		}
		freerecord(&r);
	}
	if (!did && !bad && !stuck)
		say("Everything is up to date.\n");
	if (all) {
		for (i = 0; i < n; i++)
			free(names[i]);
		free(names);
	}
	return bad;
}

static int
cmd_files(const char *arg)
{
	const char *name = installedname(arg);
	struct record r;
	int i;

	if (!name || readrecord(name, &r) < 0)
		return 1;
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
	for (i = 0; i < n; i++) {
		const char *name = all ? names[i] : installedname(names[i]);

		bad |= !name || verify_package(name) < 0;
	}
	if (!bad)
		say("No changes found.\n");
	if (all) {
		for (i = 0; i < n; i++)
			free(names[i]);
		free(names);
	}
	return bad;
}

/* A path is taken as written, so it has to be whole: from /, without . or .. (which
   pkg can't follow without knowing which directories are symbolic links) */
static int
cmd_owner(const char *arg)
{
	char *path, *p, *who;
	const char *s, *e;
	int r = 0;

	if (*arg != '/')
		return mistake("owner takes a whole path, starting with /");
	path = p = xmalloc(strlen(arg) + 1);
	for (s = arg; *s; s++)		/* without the first /, and any // */
		if (*s != '/' || (p > path && p[-1] != '/'))
			*p++ = *s;
	if (p > path && p[-1] == '/')
		p--;
	*p = 0;
	for (s = path; *s; s = *e ? e + 1 : e) {
		if (!(e = strchr(s, '/')))
			e = s + strlen(s);
		if ((e - s == 1 && s[0] == '.') || (e - s == 2 && s[0] == '.' && s[1] == '.')) {
			free(path);
			return mistake("owner takes a path without . or .. in it");
		}
	}
	if ((who = owner(path, 1))) {
		say("%s\n", who);
		free(who);
	} else {
		say("No package installed /%s.\n", path);
		r = 1;
	}
	free(path);
	return r;
}

int
main(int argc, char **argv)
{
	const struct command *c = NULL;
	const char *server = NULL, *cmd = NULL;
	char **args = xmalloc(argc * sizeof(char *));
	int i, n = 0, all = 0, unsigned_ok = 0, help = 0, dashdash = 0, r = 0;

	signal(SIGPIPE, SIG_IGN);
	for (i = 1; i < argc; i++) {	/* options anywhere: no package starts with - */
		const char *o = argv[i];

		if (dashdash || o[0] != '-' || !o[1]) {
			if (cmd)
				args[n++] = argv[i];
			else
				cmd = o;
		} else if (!strcmp(o, "--"))
			dashdash = 1;
		else if (!strcmp(o, "-r") && i + 1 < argc)
			root = argv[++i];
		else if (!strcmp(o, "-s") && i + 1 < argc)
			server = argv[++i];
		else if (!strcmp(o, "-r") || !strcmp(o, "-s"))
			return mistake("%s needs %s", o, o[1] == 'r' ? "a directory" : "a URL");
		else if (!strcmp(o, "-a"))
			all = 1;
		else if (!strcmp(o, "--unsigned"))
			unsigned_ok = 1;
		else if (!strcmp(o, "-h") || !strcmp(o, "--help"))
			help = 1;
		else
			return mistake("%s isn't an option (pkg help lists them)", o);
	}
	if (help || (cmd && !strcmp(cmd, "help") && n == 0)) {
		usage(1);
		return 0;
	}
	if (!cmd) {
		usage(2);
		return 2;
	}
	for (i = 0; i < NCOMMANDS && !c; i++)
		if (!strcmp(commands[i].name, cmd))
			c = &commands[i];

	/* every mistake is found before anything's done */
	if (!c)
		return mistake("%s isn't a command (pkg help lists them)", cmd);
	if (n < c->min || (c->max >= 0 && n > c->max)) {
		dprintf(2, "usage: pkg %s%s%s\n", c->name, *c->args ? " " : "", c->args);
		return 2;
	}
	if (server && strcmp(cmd, "update"))
		return mistake("-s goes with update");
	if (unsigned_ok && strcmp(cmd, "update"))
		return mistake("--unsigned goes with update");
	if (all && strcmp(cmd, "list"))
		return mistake("-a goes with list");
	if (server && !strncmp(server, "https://", 8))
		return mistake("-s %s: pkg can't use HTTPS; give the server's http:// address",
			server);
	if (server && strncmp(server, "http://", 7))
		return mistake("-s takes a server's http:// address, not %s", server);
	if (*root) {
		char *r2 = xstrdup(root);
		struct stat st;

		while (strlen(r2) > 1 && r2[strlen(r2) - 1] == '/')
			r2[strlen(r2) - 1] = 0;
		if (!strcmp(r2, "/"))
			r2[0] = 0;
		if (*r2 && (stat(r2, &st) < 0 || !S_ISDIR(st.st_mode)))
			return mistake("-r %s: that isn't a directory", r2);
		root = r2;
	}

	if (c->locks && lock() < 0)
		return 1;
	if (!strcmp(cmd, "update"))
		r = update(server, unsigned_ok) < 0;
	else if (!strcmp(cmd, "list"))
		r = cmd_list(all);
	else if (!strcmp(cmd, "search"))
		r = cmd_search(args[0]);
	else if (!strcmp(cmd, "info"))
		r = cmd_info(args[0]);
	else if (!strcmp(cmd, "install"))
		r = cmd_install(args, n);
	else if (!strcmp(cmd, "upgrade"))
		r = cmd_upgrade(args, n);
	else if (!strcmp(cmd, "remove")) {
		for (i = 0; i < n; i++) {
			const char *name = installedname(args[i]);

			r |= !name || remove_package(name) < 0;
		}
	} else if (!strcmp(cmd, "files"))
		r = cmd_files(args[0]);
	else if (!strcmp(cmd, "verify"))
		r = cmd_verify(args, n);
	else if (!strcmp(cmd, "owner"))
		r = cmd_owner(args[0]);
	else if (!strcmp(cmd, "version"))
		say("pkg %s\n", PKG_VERSION);
	if (c->locks)
		unlock();
	return r;
}
