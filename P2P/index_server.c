/* COE768 P2P Project - Index Server (UDP)
 *
 * Usage: index_server [port]          (default port 3000)
 *
 * Keeps a table of (peer name, content name, address) registrations and
 * serves four request types from peers:
 *   R  register content          -> A (ok) or E (peer name conflict)
 *   S  search for content        -> S (address of least-used server) or E
 *   T  de-register content       -> A or E
 *   O  list registered content   -> one or more O PDUs, ended by an empty O
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "p2p.h"

#define MAXENTRIES	200

struct entry {
	int	used;			/* 1 = slot holds a registration */
	char	peer[NAMESIZ + 1];
	char	content[NAMESIZ + 1];
	char	ip[IPSIZ + 1];
	char	port[PORTSIZ + 1];
	int	uses;			/* times this server was handed out */
	long	seq;			/* registration order (tie-breaker) */
};

static struct entry table[MAXENTRIES];
static long next_seq = 0;

static int s;					/* UDP socket */
static struct sockaddr_in cli;			/* address of current peer */
static socklen_t cli_len;

/* Send a PDU of type 'type' with 'len' bytes of data back to the peer */
static void reply(char type, const char *data, int len)
{
	struct pdu p;
	p.type = type;
	if (len > 0)
		memcpy(p.data, data, len);
	sendto(s, &p, len + 1, 0, (struct sockaddr *)&cli, cli_len);
}

static void reply_error(const char *msg)
{
	char buf[DATASIZ];
	int len = snprintf(buf, DATASIZ, "%s", msg);
	if (len >= DATASIZ) len = DATASIZ - 1;
	reply('E', buf, len);
	printf("   -> E \"%s\"\n", buf);
}

static void reply_ack(const char *peer)
{
	char buf[NAMESIZ];
	put_field(buf, peer, NAMESIZ);
	reply('A', buf, NAMESIZ);
	printf("   -> A %s\n", peer);
}

/* ---------- R: content registration ---------- */
static void do_register(struct pdu *req, int dlen)
{
	char peer[NAMESIZ + 1], content[NAMESIZ + 1], ip[IPSIZ + 1], port[PORTSIZ + 1];
	int i, free_slot = -1;

	if (dlen < LEN_ADDR) {
		reply_error("Malformed R PDU");
		return;
	}
	get_field(peer, req->data + OFF_PEER, NAMESIZ);
	get_field(content, req->data + OFF_CONTENT, NAMESIZ);
	get_field(ip, req->data + OFF_IP, IPSIZ);
	get_field(port, req->data + OFF_PORT, PORTSIZ);
	printf("R  peer=%s content=%s addr=%s:%s\n", peer, content, ip, port);

	if (peer[0] == '\0' || content[0] == '\0' || atoi(port) <= 0) {
		reply_error("Missing peer name, content name or port");
		return;
	}
	/* The peer may not know its own IP (getsockname gives 0.0.0.0);
	 * in that case use the source address of this datagram. */
	if (ip[0] == '\0' || strcmp(ip, "0.0.0.0") == 0)
		snprintf(ip, sizeof(ip), "%s", inet_ntoa(cli.sin_addr));

	for (i = 0; i < MAXENTRIES; i++) {
		if (!table[i].used) {
			if (free_slot < 0) free_slot = i;
			continue;
		}
		/* Same peer name already registered this content -> conflict */
		if (strcmp(table[i].peer, peer) == 0 &&
		    strcmp(table[i].content, content) == 0) {
			char msg[DATASIZ];
			snprintf(msg, sizeof(msg),
				 "Peer name '%s' already registered '%s'. Choose another peer name.",
				 peer, content);
			reply_error(msg);
			return;
		}
	}
	if (free_slot < 0) {
		reply_error("Index server table is full");
		return;
	}

	table[free_slot].used = 1;
	strcpy(table[free_slot].peer, peer);
	strcpy(table[free_slot].content, content);
	strcpy(table[free_slot].ip, ip);
	strcpy(table[free_slot].port, port);
	table[free_slot].uses = 0;
	table[free_slot].seq = next_seq++;
	reply_ack(peer);
}

/* ---------- S: search, pick the least-used content server ---------- */
static void do_search(struct pdu *req, int dlen)
{
	char peer[NAMESIZ + 1], content[NAMESIZ + 1], buf[DATASIZ];
	int i, best = -1;

	if (dlen < LEN_NAMES) {
		reply_error("Malformed S PDU");
		return;
	}
	get_field(peer, req->data + OFF_PEER, NAMESIZ);
	get_field(content, req->data + OFF_CONTENT, NAMESIZ);
	printf("S  peer=%s content=%s\n", peer, content);

	for (i = 0; i < MAXENTRIES; i++) {
		if (!table[i].used || strcmp(table[i].content, content) != 0)
			continue;
		/* least used wins; on a tie, the earliest registration wins */
		if (best < 0 || table[i].uses < table[best].uses ||
		    (table[i].uses == table[best].uses && table[i].seq < table[best].seq))
			best = i;
	}
	if (best < 0) {
		reply_error("Content not found");
		return;
	}

	table[best].uses++;
	memset(buf, 0, sizeof(buf));
	put_field(buf + OFF_PEER, table[best].peer, NAMESIZ);
	put_field(buf + OFF_CONTENT, table[best].content, NAMESIZ);
	put_field(buf + OFF_IP, table[best].ip, IPSIZ);
	put_field(buf + OFF_PORT, table[best].port, PORTSIZ);
	reply('S', buf, LEN_ADDR);
	printf("   -> S server=%s addr=%s:%s (now used %d time%s)\n",
	       table[best].peer, table[best].ip, table[best].port,
	       table[best].uses, table[best].uses == 1 ? "" : "s");
}

/* ---------- T: content de-registration ---------- */
static void do_deregister(struct pdu *req, int dlen)
{
	char peer[NAMESIZ + 1], content[NAMESIZ + 1];
	int i;

	if (dlen < LEN_NAMES) {
		reply_error("Malformed T PDU");
		return;
	}
	get_field(peer, req->data + OFF_PEER, NAMESIZ);
	get_field(content, req->data + OFF_CONTENT, NAMESIZ);
	printf("T  peer=%s content=%s\n", peer, content);

	for (i = 0; i < MAXENTRIES; i++) {
		if (table[i].used && strcmp(table[i].peer, peer) == 0 &&
		    strcmp(table[i].content, content) == 0) {
			table[i].used = 0;
			reply_ack(peer);
			return;
		}
	}
	reply_error("No such registration");
}

/* ---------- O: list registered content ---------- */
static void do_list(void)
{
	char buf[DATASIZ];
	int i, n = 0, total = 0;

	printf("O  list request\n");
	for (i = 0; i < MAXENTRIES; i++) {
		if (!table[i].used)
			continue;
		put_field(buf + n * LEN_NAMES + OFF_PEER, table[i].peer, NAMESIZ);
		put_field(buf + n * LEN_NAMES + OFF_CONTENT, table[i].content, NAMESIZ);
		n++;
		total++;
		if (n == LIST_PER_PDU) {	/* PDU full: send it */
			reply('O', buf, n * LEN_NAMES);
			n = 0;
		}
	}
	if (n > 0)
		reply('O', buf, n * LEN_NAMES);
	reply('O', NULL, 0);			/* empty O = end of list */
	printf("   -> O (%d registration%s)\n", total, total == 1 ? "" : "s");
}

int main(int argc, char **argv)
{
	struct sockaddr_in sin;
	struct pdu req;
	int port = DEFAULT_PORT, n;

	switch (argc) {
	case 1:
		break;
	case 2:
		port = atoi(argv[1]);
		break;
	default:
		fprintf(stderr, "Usage: %s [port]\n", argv[0]);
		exit(1);
	}

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		fprintf(stderr, "Can't create socket\n");
		exit(1);
	}
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(port);
	if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
		fprintf(stderr, "Can't bind to port %d\n", port);
		exit(1);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("Index server listening on UDP port %d\n", port);

	while (1) {
		cli_len = sizeof(cli);
		n = recvfrom(s, &req, sizeof(req), 0, (struct sockaddr *)&cli, &cli_len);
		if (n < 1)
			continue;
		switch (req.type) {
		case 'R': do_register(&req, n - 1);	break;
		case 'S': do_search(&req, n - 1);	break;
		case 'T': do_deregister(&req, n - 1);	break;
		case 'O': do_list();			break;
		default:
			printf("?  unknown PDU type '%c'\n", req.type);
			reply_error("Unknown PDU type");
		}
	}
}
