/*
 * A test server for running pkg inside A/UX, where QEMU gives no network card: serves
 * files over HTTP and answers every DNS question with 127.0.0.1, both on the loopback
 * interface.
 *
 *   httpd PORT DIR      HTTP on PORT from DIR, DNS on port 53
 *
 * GET /chunked/PATH sends DIR/PATH in chunks; GET /PATH sends it with a length.
 */
#include "aux.h"

static void
serve(int c, const char *dir)
{
	char req[2048], path[512], head[256];
	int n = 0, r, fd, chunked = 0;
	char *p, *e;

	while (n < (int)sizeof(req) - 1 && (r = recv(c, req + n, sizeof(req) - 1 - n, 0)) > 0) {
		n += r;
		req[n] = 0;
		if (strstr(req, "\r\n\r\n"))
			break;
	}
	req[n] = 0;
	if (strncmp(req, "GET /", 5) || !(e = strchr(req + 4, ' ')))
		return;
	*e = 0;
	p = req + 4;
	if (!strncmp(p, "/chunked/", 9)) {
		chunked = 1;
		p += 8;
	}
	snprintf(path, sizeof(path), "%s%s", dir, p);
	if (strstr(p, "..") || (fd = open(path, O_RDONLY)) < 0) {
		static const char nf[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";

		send(c, nf, sizeof(nf) - 1, 0);
		return;
	}
	if (chunked)
		snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
			"Connection: close\r\n\r\n");
	else {
		struct stat st;

		fstat(fd, &st);
		snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n"
			"Connection: close\r\n\r\n", st.st_size);
	}
	send(c, head, strlen(head), 0);
	for (;;) {
		char buf[1500];

		n = read(fd, buf, chunked ? 700 : sizeof(buf));
		if (n <= 0)
			break;
		if (chunked) {
			snprintf(head, sizeof(head), "%x\r\n", n);
			send(c, head, strlen(head), 0);
		}
		send(c, buf, n, 0);
		if (chunked)
			send(c, "\r\n", 2, 0);
	}
	if (chunked)
		send(c, "0\r\n\r\n", 5, 0);
	close(fd);
}

static void
dns(int u)
{
	unsigned char m[512];
	struct sockaddr_in from;
	int len = sizeof(from), n;
	static const unsigned char answer[] = {
		0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 127, 0, 0, 1,
	};

	n = recvfrom(u, m, sizeof(m) - sizeof(answer), 0, (struct sockaddr *)&from, &len);
	if (n < 12)
		return;
	m[2] = 0x81, m[3] = 0x80;	/* a reply, recursion available */
	m[6] = 0, m[7] = 1;		/* one answer */
	m[8] = m[9] = m[10] = m[11] = 0;
	memcpy(m + n, answer, sizeof(answer));
	sendto(u, m, n + sizeof(answer), 0, (struct sockaddr *)&from, len);
}

static int
listen_on(int type, int port)
{
	struct sockaddr_in sin;
	int fd = socket(AF_INET, type, 0), on = 1;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(0x7f000001);
	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0 ||
	    (type == SOCK_STREAM && listen(fd, 5) < 0)) {
		dprintf(2, "httpd: can't listen on port %d: %s\n", port, strerror(errno));
		exit(1);
	}
	return fd;
}

int
main(int argc, char **argv)
{
	int s, u;

	if (argc != 3) {
		dprintf(2, "usage: httpd PORT DIR\n");
		return 2;
	}
	s = listen_on(SOCK_STREAM, strtol(argv[1], NULL, 10));
	u = listen_on(SOCK_DGRAM, 53);
	signal(SIGPIPE, SIG_IGN);
	for (;;) {
		fd_set r;

		FD_ZERO(&r);
		FD_SET(s, &r);
		FD_SET(u, &r);
		if (select((s > u ? s : u) + 1, &r, NULL, NULL, NULL) <= 0)
			continue;
		if (FD_ISSET(u, &r))
			dns(u);
		if (FD_ISSET(s, &r)) {
			int c = accept(s, NULL, NULL);

			if (c >= 0) {
				serve(c, argv[2]);
				close(c);
			}
		}
	}
}
