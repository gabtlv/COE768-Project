/* COE768 P2P Project - Peer (content client + content server)
 *
 * Usage: peer index_host [index_port]       (default port 3000)
 *
 * Commands (typed at the prompt):
 *   R <content>   register a local file as content (opens a TCP socket for it)
 *   D <content>   search the index server and download the content,
 *                 then automatically register as a server of it
 *   O             list content registered at the index server
 *   T <content>   de-register one of your contents
 *   L             list your own registered contents
 *   Q             de-register everything and quit
 *
 * Content names are file names in the current directory (max 10 chars).
 * Run each peer in its own directory so downloads don't overwrite originals.
 *
 * The peer uses select() to wait on stdin and on every TCP listening socket
 * at the same time. A download request is served by a forked child (as in
 * Lab 3) so the user prompt stays responsive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "p2p.h"

#define MAXLOCAL	20	/* max contents this peer can serve */
#define LINESIZ		128
#define UDP_TIMEOUT	3	/* seconds to wait for the index server */

struct local {
	char	peer[NAMESIZ + 1];	/* peer name used to register it */
	char	content[NAMESIZ + 1];
	int	sd;			/* TCP listening socket for this content */
	int	port;
};

static struct local locals[MAXLOCAL];
static int nlocal = 0;

static int udp;				/* UDP socket to the index server */
static char myname[NAMESIZ + 1];
static char myip[IPSIZ];
static fd_set afds;			/* sockets select() watches */

/* ---------- helpers ---------- */

/* Read one line from stdin with read() (no stdio buffering, so it plays
 * nicely with select() on descriptor 0). Returns -1 on EOF. */
static int read_line(char *buf, int size)
{
	int n = 0;
	char c;
	while (n < size - 1) {
		int r = read(0, &c, 1);
		if (r <= 0) {
			if (n == 0) return -1;
			break;
		}
		if (c == '\n') break;
		if (c != '\r') buf[n++] = c;
	}
	buf[n] = '\0';
	/* discard the rest of an over-long line */
	if (n == size - 1) {
		while (read(0, &c, 1) == 1 && c != '\n')
			;
	}
	return n;
}

static void prompt(void)
{
	printf("\n[%s] Command (R/D/O/T/L/Q): ", myname);
	fflush(stdout);
}

/* Ask the user for a peer name of 1..10 characters */
static int ask_name(const char *msg, char *name)
{
	char line[LINESIZ];
	while (1) {
		printf("%s", msg);
		fflush(stdout);
		if (read_line(line, sizeof(line)) < 0)
			return -1;
		if (line[0] == '\0')
			return -1;
		if (strlen(line) > NAMESIZ) {
			printf("Name must be at most %d characters.\n", NAMESIZ);
			continue;
		}
		strcpy(name, line);
		return 0;
	}
}

/* Send a PDU to the index server and wait for one reply.
 * Returns number of Data bytes in the reply, or -1 on timeout/error. */
static int udp_transact(struct pdu *req, int dlen, struct pdu *rep)
{
	int n;
	if (send(udp, req, dlen + 1, 0) < 0) {
		perror("send to index server");
		return -1;
	}
	n = recv(udp, rep, sizeof(*rep), 0);
	if (n < 1) {
		printf("No reply from index server (%s)\n",
		       n < 0 ? strerror(errno) : "empty");
		return -1;
	}
	return n - 1;
}

static void print_error(struct pdu *rep, int dlen)
{
	char msg[DATASIZ + 1];
	if (dlen < 0) dlen = 0;
	memcpy(msg, rep->data, dlen);
	msg[dlen] = '\0';
	printf("Index server error: %s\n", msg);
}

static int find_local(const char *content)
{
	int i;
	for (i = 0; i < nlocal; i++)
		if (strcmp(locals[i].content, content) == 0)
			return i;
	return -1;
}

/* ---------- R: content registration ---------- */
static void register_content(const char *content)
{
	struct sockaddr_in reg_addr;
	socklen_t alen;
	struct pdu req, rep;
	char name[NAMESIZ + 1], portstr[PORTSIZ];
	int sd, n;

	if (strlen(content) == 0 || strlen(content) > NAMESIZ) {
		printf("Content name must be 1-%d characters.\n", NAMESIZ);
		return;
	}
	if (find_local(content) >= 0) {
		printf("You already serve '%s'.\n", content);
		return;
	}
	if (nlocal == MAXLOCAL) {
		printf("Too many registered contents.\n");
		return;
	}
	if (access(content, R_OK) != 0) {
		printf("No readable file named '%s' in this directory.\n", content);
		return;
	}

	/* 1. Open a TCP passive socket for this content; let the OS pick the port */
	if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		return;
	}
	memset(&reg_addr, 0, sizeof(reg_addr));
	reg_addr.sin_family = AF_INET;
	reg_addr.sin_port = htons(0);
	reg_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sd, (struct sockaddr *)&reg_addr, sizeof(reg_addr)) < 0) {
		perror("bind");
		close(sd);
		return;
	}
	listen(sd, 5);
	alen = sizeof(struct sockaddr_in);
	getsockname(sd, (struct sockaddr *)&reg_addr, &alen);
	snprintf(portstr, sizeof(portstr), "%d", ntohs(reg_addr.sin_port));

	/* 2. Send R PDUs until the index server accepts our peer name */
	strcpy(name, myname);
	while (1) {
		req.type = 'R';
		memset(req.data, 0, DATASIZ);
		put_field(req.data + OFF_PEER, name, NAMESIZ);
		put_field(req.data + OFF_CONTENT, content, NAMESIZ);
		put_field(req.data + OFF_IP, myip, IPSIZ);
		put_field(req.data + OFF_PORT, portstr, PORTSIZ);

		n = udp_transact(&req, LEN_ADDR, &rep);
		if (n < 0)
			break;
		if (rep.type == 'A') {
			strcpy(locals[nlocal].peer, name);
			strcpy(locals[nlocal].content, content);
			locals[nlocal].sd = sd;
			locals[nlocal].port = ntohs(reg_addr.sin_port);
			nlocal++;
			FD_SET(sd, &afds);
			strcpy(myname, name);	/* keep the accepted name */
			printf("Registered '%s' as peer '%s' at %s:%s\n",
			       content, name, myip, portstr);
			return;
		}
		if (rep.type == 'E') {
			print_error(&rep, n);
			if (ask_name("Enter another peer name (blank to cancel): ", name) < 0)
				break;
			continue;
		}
		printf("Unexpected reply type '%c'\n", rep.type);
		break;
	}
	printf("Registration of '%s' cancelled.\n", content);
	close(sd);
}

/* ---------- D: search + download, then register ---------- */
static void download_content(const char *content)
{
	struct pdu req, rep;
	struct sockaddr_in server;
	char sname[NAMESIZ + 1], sip[IPSIZ + 1], sport[PORTSIZ + 1];
	char buf[CBUFSIZ];
	int n, sd, fd, total;

	if (strlen(content) == 0 || strlen(content) > NAMESIZ) {
		printf("Content name must be 1-%d characters.\n", NAMESIZ);
		return;
	}
	if (find_local(content) >= 0) {
		printf("You already have and serve '%s'.\n", content);
		return;
	}

	/* 1. Ask the index server where the content is (S PDU) */
	req.type = 'S';
	memset(req.data, 0, DATASIZ);
	put_field(req.data + OFF_PEER, myname, NAMESIZ);
	put_field(req.data + OFF_CONTENT, content, NAMESIZ);
	n = udp_transact(&req, LEN_NAMES, &rep);
	if (n < 0)
		return;
	if (rep.type == 'E') {
		print_error(&rep, n);
		return;
	}
	if (rep.type != 'S' || n < LEN_ADDR) {
		printf("Unexpected reply type '%c'\n", rep.type);
		return;
	}
	get_field(sname, rep.data + OFF_PEER, NAMESIZ);
	get_field(sip, rep.data + OFF_IP, IPSIZ);
	get_field(sport, rep.data + OFF_PORT, PORTSIZ);
	printf("Content server for '%s': peer '%s' at %s:%s\n", content, sname, sip, sport);

	/* 2. TCP connection to the content server */
	if ((sd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("socket");
		return;
	}
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons(atoi(sport));
	if (inet_aton(sip, &server.sin_addr) == 0) {
		printf("Bad server address %s\n", sip);
		close(sd);
		return;
	}
	if (connect(sd, (struct sockaddr *)&server, sizeof(server)) < 0) {
		perror("connect to content server");
		close(sd);
		return;
	}

	/* 3. Send the D PDU */
	req.type = 'D';
	memset(req.data, 0, DATASIZ);
	put_field(req.data + OFF_PEER, myname, NAMESIZ);
	put_field(req.data + OFF_CONTENT, content, NAMESIZ);
	write(sd, &req, LEN_NAMES + 1);

	/* 4. Receive C (content) or E (error) */
	n = read(sd, buf, sizeof(buf));
	if (n <= 0) {
		printf("Content server closed the connection without data.\n");
		close(sd);
		return;
	}
	if (buf[0] == 'E') {
		printf("Content server error: ");
		fwrite(buf + 1, 1, n - 1, stdout);
		while ((n = read(sd, buf, sizeof(buf))) > 0)
			fwrite(buf, 1, n, stdout);
		printf("\n");
		close(sd);
		return;
	}
	if (buf[0] != 'C') {
		printf("Unexpected PDU type '%c' from content server\n", buf[0]);
		close(sd);
		return;
	}
	if ((fd = open(content, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
		perror("create local file");
		close(sd);
		return;
	}
	write(fd, buf + 1, n - 1);
	total = n - 1;
	while ((n = read(sd, buf, sizeof(buf))) > 0) {	/* until server closes */
		write(fd, buf, n);
		total += n;
	}
	close(fd);
	close(sd);
	printf("Downloaded '%s' (%d bytes) from peer '%s'\n", content, total, sname);

	/* 5. Become a content server for it */
	register_content(content);
}

/* ---------- serve a download request (runs in a child process) ---------- */
static int serve_download(int sd, const char *mycontent)
{
	struct pdu req;
	char content[NAMESIZ + 1], buf[CBUFSIZ];
	int n, got = 0, fd;

	/* read the D PDU (type + peer + content = 21 bytes) */
	while (got < LEN_NAMES + 1) {
		n = read(sd, (char *)&req + got, LEN_NAMES + 1 - got);
		if (n <= 0) break;
		got += n;
	}
	if (got < 1 + LEN_NAMES || req.type != 'D') {
		buf[0] = 'E';
		n = snprintf(buf + 1, DATASIZ, "Expected a D PDU");
		write(sd, buf, n + 1);
		close(sd);
		return 1;
	}
	get_field(content, req.data + OFF_CONTENT, NAMESIZ);

	if (strcmp(content, mycontent) != 0 || (fd = open(content, O_RDONLY)) < 0) {
		buf[0] = 'E';
		n = snprintf(buf + 1, DATASIZ, "Content '%s' not available here", content);
		if (n > DATASIZ - 1) n = DATASIZ - 1;
		write(sd, buf, n + 1);
		close(sd);
		return 1;
	}

	/* C PDU: 'C' + data; split across as many TCP writes as needed */
	buf[0] = 'C';
	n = read(fd, buf + 1, sizeof(buf) - 1);
	if (n < 0) n = 0;
	write(sd, buf, n + 1);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		write(sd, buf, n);

	close(fd);
	close(sd);		/* closing the connection = end of content */
	return 0;
}

static void handle_connection(int i)
{
	struct sockaddr_in client;
	socklen_t alen = sizeof(client);
	int new_sd, j;

	new_sd = accept(locals[i].sd, (struct sockaddr *)&client, &alen);
	if (new_sd < 0)
		return;
	printf("\n>> Upload of '%s' to %s:%d\n", locals[i].content,
	       inet_ntoa(client.sin_addr), ntohs(client.sin_port));
	fflush(stdout);		/* so the child doesn't re-print buffered output */

	switch (fork()) {
	case 0:			/* child: serve the download, then exit */
		for (j = 0; j < nlocal; j++)
			close(locals[j].sd);
		close(udp);
		exit(serve_download(new_sd, locals[i].content));
	case -1:
		perror("fork");
		close(new_sd);
		break;
	default:		/* parent: child owns the connection now */
		close(new_sd);
	}
}

/* ---------- T: content de-registration ---------- */
static void deregister_content(const char *content)
{
	struct pdu req, rep;
	int i, n;

	if ((i = find_local(content)) < 0) {
		printf("You have not registered '%s'.\n", content);
		return;
	}
	req.type = 'T';
	memset(req.data, 0, DATASIZ);
	put_field(req.data + OFF_PEER, locals[i].peer, NAMESIZ);
	put_field(req.data + OFF_CONTENT, content, NAMESIZ);
	n = udp_transact(&req, LEN_NAMES, &rep);
	if (n < 0)
		return;
	if (rep.type == 'A')
		printf("De-registered '%s'.\n", content);
	else if (rep.type == 'E')
		print_error(&rep, n);

	/* stop serving it locally */
	FD_CLR(locals[i].sd, &afds);
	close(locals[i].sd);
	locals[i] = locals[--nlocal];
}

/* ---------- O: list registered content ---------- */
static void list_online(void)
{
	struct pdu req, rep;
	char peer[NAMESIZ + 1], content[NAMESIZ + 1];
	int n, k, count = 0;

	req.type = 'O';
	if (send(udp, &req, 1, 0) < 0) {
		perror("send");
		return;
	}
	printf("Content on-line:\n  %-10s  %-10s\n", "CONTENT", "PEER");
	while (1) {
		n = recv(udp, &rep, sizeof(rep), 0);
		if (n < 1) {
			printf("No reply from index server.\n");
			return;
		}
		if (rep.type != 'O') {
			if (rep.type == 'E')
				print_error(&rep, n - 1);
			return;
		}
		if (n == 1)		/* empty O = end of list */
			break;
		for (k = 0; k + LEN_NAMES <= n - 1; k += LEN_NAMES) {
			get_field(peer, rep.data + k + OFF_PEER, NAMESIZ);
			get_field(content, rep.data + k + OFF_CONTENT, NAMESIZ);
			printf("  %-10s  %-10s\n", content, peer);
			count++;
		}
	}
	if (count == 0)
		printf("  (none)\n");
}

static void list_local(void)
{
	int i;
	if (nlocal == 0) {
		printf("You are not serving any content.\n");
		return;
	}
	printf("Your contents:\n");
	for (i = 0; i < nlocal; i++)
		printf("  %-10s  as peer %-10s  TCP port %d\n",
		       locals[i].content, locals[i].peer, locals[i].port);
}

static void quit(void)
{
	printf("De-registering all content...\n");
	while (nlocal > 0)
		deregister_content(locals[nlocal - 1].content);
	close(udp);
	printf("Bye.\n");
	exit(0);
}

static void reaper(int sig)
{
	int status;
	(void)sig;
	while (waitpid(-1, &status, WNOHANG) > 0)
		;
}

/* ---------- main ---------- */
int main(int argc, char **argv)
{
	struct sockaddr_in sin, local;
	struct hostent *hp;
	struct timeval tv;
	socklen_t alen;
	fd_set rfds;
	char *host, line[LINESIZ], cmd, arg[LINESIZ];
	int port = DEFAULT_PORT, i;

	switch (argc) {
	case 2:
		host = argv[1];
		break;
	case 3:
		host = argv[1];
		port = atoi(argv[2]);
		break;
	default:
		fprintf(stderr, "Usage: %s index_host [index_port]\n", argv[0]);
		exit(1);
	}

	/* UDP socket "connected" to the index server */
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	if ((hp = gethostbyname(host)))
		memcpy(&sin.sin_addr, hp->h_addr, hp->h_length);
	else if (inet_aton(host, &sin.sin_addr) == 0) {
		fprintf(stderr, "Can't get index server address\n");
		exit(1);
	}
	if ((udp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		fprintf(stderr, "Can't create socket\n");
		exit(1);
	}
	if (connect(udp, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		fprintf(stderr, "Can't connect to index server\n");
		exit(1);
	}
	tv.tv_sec = UDP_TIMEOUT;
	tv.tv_usec = 0;
	setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* After connect(), getsockname on the UDP socket gives the IP of the
	 * interface used to reach the index server: that is our address. */
	alen = sizeof(local);
	getsockname(udp, (struct sockaddr *)&local, &alen);
	snprintf(myip, sizeof(myip), "%s", inet_ntoa(local.sin_addr));

	signal(SIGCHLD, reaper);
	signal(SIGPIPE, SIG_IGN);

	if (ask_name("Enter peer name (max 10 chars): ", myname) < 0)
		exit(0);
	printf("Peer '%s' on %s, index server %s:%d\n", myname, myip,
	       inet_ntoa(sin.sin_addr), port);
	printf("  R <content>  register     D <content>  download\n"
	       "  O            list online  T <content>  de-register\n"
	       "  L            list local   Q            quit\n");

	FD_ZERO(&afds);
	FD_SET(0, &afds);			/* stdin */

	prompt();
	while (1) {
		memcpy(&rfds, &afds, sizeof(rfds));
		if (select(FD_SETSIZE, &rfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)	/* interrupted by SIGCHLD */
				continue;
			perror("select");
			quit();
		}

		/* a peer is connecting to download one of our contents */
		for (i = 0; i < nlocal; i++)
			if (FD_ISSET(locals[i].sd, &rfds)) {
				handle_connection(i);
				prompt();
			}

		/* user typed a command */
		if (FD_ISSET(0, &rfds)) {
			if (read_line(line, sizeof(line)) < 0)
				quit();				/* EOF (Ctrl-D) */
			arg[0] = '\0';
			if (sscanf(line, " %c %127s", &cmd, arg) < 1) {
				prompt();
				continue;
			}
			switch (cmd) {
			case 'R': case 'r': case 'D': case 'd': case 'T': case 't':
				if (arg[0] == '\0') {
					printf("Content name: ");
					fflush(stdout);
					if (read_line(arg, sizeof(arg)) < 0)
						quit();
				}
				if (cmd == 'R' || cmd == 'r')
					register_content(arg);
				else if (cmd == 'D' || cmd == 'd')
					download_content(arg);
				else
					deregister_content(arg);
				break;
			case 'O': case 'o': list_online();	break;
			case 'L': case 'l': list_local();	break;
			case 'Q': case 'q': quit();		break;
			default:
				printf("Unknown command '%c'\n", cmd);
			}
			prompt();
		}
	}
}
