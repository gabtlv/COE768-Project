/* COE768 P2P Project - shared definitions for index_server.c and peer.c
 *
 * PDU format:   | Type (1 byte) | Data (<= 100 bytes) |
 *
 * Data field layouts used in this implementation (fixed-size fields,
 * unused bytes are zero-filled; a name may use all 10 bytes with no '\0'):
 *
 *   R  (peer -> index)        peer[10] content[10] ip[16] port[6]
 *   S  (peer -> index)        peer[10] content[10]
 *   S  (index -> peer)        peer[10] content[10] ip[16] port[6]
 *                             (peer = name of the chosen content server)
 *   T  (peer -> index)        peer[10] content[10]
 *   D  (client -> server)     peer[10] content[10]
 *   O  (peer -> index)        (empty)
 *   O  (index -> peer)        up to 5 x { peer[10] content[10] };
 *                             an O PDU with an empty Data field ends the list
 *   A  (index -> peer)        peer[10]
 *   E  (any)                  error message text
 *   C  (server -> client)     content data, streamed over TCP: the first
 *                             packet starts with 'C', the remaining bytes and
 *                             all later packets are raw data. The server
 *                             closes the connection when the content is done
 *                             (same mechanism as Lab 3).
 *
 * ip and port are carried as ASCII strings ("10.1.1.4", "40123") so they are
 * easy to read in a Wireshark trace.
 */
#ifndef P2P_H
#define P2P_H

#include <string.h>

#define DEFAULT_PORT	3000	/* UDP port of the index server */
#define DATASIZ		100	/* max Data field size (all PDUs except C) */
#define NAMESIZ		10	/* peer name and content name field size */
#define IPSIZ		16	/* "255.255.255.255" + '\0' */
#define PORTSIZ		6	/* "65535" + '\0' */
#define CBUFSIZ		1400	/* bytes per TCP write for C-type data */

/* Offsets inside the Data field */
#define OFF_PEER	0
#define OFF_CONTENT	(OFF_PEER + NAMESIZ)		/* 10 */
#define OFF_IP		(OFF_CONTENT + NAMESIZ)		/* 20 */
#define OFF_PORT	(OFF_IP + IPSIZ)		/* 36 */
#define LEN_NAMES	(2 * NAMESIZ)			/* 20 */
#define LEN_ADDR	(OFF_PORT + PORTSIZ)		/* 42 */
#define LIST_PER_PDU	(DATASIZ / LEN_NAMES)		/* 5 */

struct pdu {
	char type;
	char data[DATASIZ];
};

/* Copy a C string into a fixed-size field, zero-filling the rest */
static inline void put_field(char *dst, const char *src, int size)
{
	memset(dst, 0, size);
	strncpy(dst, src, size);
}

/* Copy a fixed-size field out into a C string (dst must hold size + 1) */
static inline void get_field(char *dst, const char *src, int size)
{
	memcpy(dst, src, size);
	dst[size] = '\0';
}

#endif
