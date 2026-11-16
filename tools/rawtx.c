/*
 * rawtx - (1) set a monitor interface's channel via nl80211, (2) inject a raw
 *         802.11 vendor "action" frame (4-address Apple AWDL flavour) through an
 *         AF_PACKET socket on that monitor interface.
 *
 * Usage: rawtx <ifname> <freq> [count]
 * Build: zig cc -target aarch64-linux-musl -Os -static rawtx.c -o rawtx
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define NETLINK_GENERIC 16
#define GENL_ID_CTRL 16
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2

#define NLMSG_ALIGN(l) (((l) + 3) & ~3u)
#define NLA_ALIGN(l) NLMSG_ALIGN(l)
#define NLMSG_ERROR 2
#define NLM_F_REQUEST 0x01
#define NLM_F_ACK 0x04

#define NL80211_CMD_SET_WIPHY 2
#define NL80211_ATTR_WIPHY 1
#define NL80211_ATTR_IFINDEX 3
#define NL80211_ATTR_IFNAME 4
#define NL80211_ATTR_WIPHY_FREQ 38

struct nlmsghdr {
	uint32_t nlmsg_len;
	uint16_t nlmsg_type;
	uint16_t nlmsg_flags;
	uint32_t nlmsg_seq;
	uint32_t nlmsg_pid;
};
struct nlattr { uint16_t nla_len; uint16_t nla_type; };
struct genlmsghdr { uint8_t cmd; uint8_t version; uint16_t reserved; };
struct sockaddr_nl_ { uint16_t nl_family; uint16_t nl_pad; uint32_t nl_pid; uint32_t nl_groups; };

static uint8_t sbuf[4096];
static uint8_t rbuf[16384];
static int nlfd = -1;
static uint16_t fam;
static uint8_t famver = 1;

static char *put_attr(char *p, uint16_t t, const void *d, uint16_t l)
{
	struct nlattr *a = (struct nlattr *)p;
	a->nla_type = t;
	a->nla_len = (uint16_t)(sizeof(*a) + l);
	if (l) memcpy(p + sizeof(*a), d, l);
	int tot = (int)sizeof(*a) + l, al = NLA_ALIGN(tot);
	memset(p + tot, 0, al - tot);
	return p + al;
}
static char *put_u32(char *p, uint16_t t, uint32_t v) { return put_attr(p, t, &v, 4); }
static char *put_str(char *p, uint16_t t, const char *s) { return put_attr(p, t, s, (uint16_t)(strlen(s) + 1)); }

static int nl_sendraw(char *end, uint16_t type, uint16_t flags, uint8_t cmd, uint32_t seq)
{
	char *p = (char *)sbuf;
	struct nlmsghdr *h = (struct nlmsghdr *)p;
	struct genlmsghdr *g = (struct genlmsghdr *)(p + sizeof(*h));
	g->cmd = cmd; g->version = famver; g->reserved = 0;
	char *e = end ? end : p + sizeof(*h) + sizeof(*g);
	h->nlmsg_len = (uint32_t)(e - p);
	h->nlmsg_type = type; h->nlmsg_flags = flags; h->nlmsg_seq = seq;
	h->nlmsg_pid = (uint32_t)getpid();
	struct sockaddr_nl_ sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	return (int)sendto(nlfd, sbuf, h->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static int nl_expect_ack(void)
{
	ssize_t n = recv(nlfd, rbuf, sizeof(rbuf), 0);
	if (n <= 0) return -errno;
	struct nlmsghdr *h = (struct nlmsghdr *)rbuf;
	if (h->nlmsg_type != NLMSG_ERROR) return -999;
	return *(int *)((char *)h + sizeof(*h));
}

static void nl_open(void)
{
	nlfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	struct sockaddr_nl_ sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = (uint32_t)getpid();
	bind(nlfd, (struct sockaddr *)&sa, sizeof(sa));
	struct timeval tv = { .tv_sec = 2 };
	setsockopt(nlfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	char *p = (char *)sbuf;
	char *e = p + sizeof(struct nlmsghdr) + sizeof(struct genlmsghdr);
	e = put_str(e, CTRL_ATTR_FAMILY_NAME, "nl80211");
	nl_sendraw(e, GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY, 10);
	ssize_t n = recv(nlfd, rbuf, sizeof(rbuf), 0);
	if (n > 0) {
		struct nlmsghdr *h = (struct nlmsghdr *)rbuf;
		int off = (int)sizeof(*h) + (int)sizeof(struct genlmsghdr);
		while (off + 4 <= (int)h->nlmsg_len) {
			struct nlattr *a = (struct nlattr *)(rbuf + off);
			if (a->nla_len < 4) break;
			if (a->nla_type == CTRL_ATTR_FAMILY_ID && a->nla_len >= 6)
				memcpy(&fam, rbuf + off + 4, 2);
			off += NLA_ALIGN(a->nla_len);
		}
	}
}

static int set_channel(unsigned ifindex, unsigned freq)
{
	char *p = (char *)sbuf;
	char *e = p + sizeof(struct nlmsghdr) + sizeof(struct genlmsghdr);
	e = put_u32(e, NL80211_ATTR_IFINDEX, ifindex);
	e = put_u32(e, NL80211_ATTR_WIPHY_FREQ, freq);
	if (nl_sendraw(e, fam, NLM_F_REQUEST | NLM_F_ACK, NL80211_CMD_SET_WIPHY, 20) < 0)
		return -errno;
	return nl_expect_ack();
}

int main(int argc, char **argv)
{
	if (argc < 3) { printf("usage: %s <ifname> <freq> [count]\n", argv[0]); return 2; }
	const char *ifn = argv[1];
	unsigned freq = (unsigned)strtoul(argv[2], NULL, 10);
	int count = argc > 3 ? atoi(argv[3]) : 3;

	unsigned ifi = if_nametoindex(ifn);
	if (!ifi) { printf("if_nametoindex(%s) failed: %s\n", ifn, strerror(errno)); return 1; }
	printf("iface %s index=%u\n", ifn, ifi);

	nl_open();
	printf("nl80211 family=%u\n", fam);
	int rc = set_channel(ifi, freq);
	if (rc == 0) printf("SET_WIPHY freq=%u -> OK\n", freq);
	else printf("SET_WIPHY freq=%u -> %s (%d)\n", freq, strerror(-rc), rc);

	/* AF_PACKET raw socket bound to the monitor interface */
	int ps = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (ps < 0) { printf("socket(AF_PACKET) failed: %s\n", strerror(errno)); return 1; }
	struct sockaddr_ll sll;
	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = (int)ifi;
	if (bind(ps, (struct sockaddr *)&sll, sizeof(sll)) < 0)
		printf("bind(AF_PACKET) failed: %s\n", strerror(errno));

	/* radiotap (8 bytes, no fields) + 802.11 vendor action frame */
	uint8_t f[128];
	int n = 0;
	f[n++] = 0; f[n++] = 0; f[n++] = 8; f[n++] = 0;      /* radiotap hdr */
	memset(&f[n], 0, 4); n += 4;
	f[n++] = 0xd0; f[n++] = 0x00;                        /* mgmt, action */
	f[n++] = 0x00; f[n++] = 0x00;                        /* duration */
	for (int i = 0; i < 6; i++) f[n++] = 0xff;            /* DA broadcast */
	uint8_t sa[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	memcpy(&f[n], sa, 6); n += 6;                         /* SA */
	for (int i = 0; i < 6; i++) f[n++] = 0xff;            /* BSSID */
	f[n++] = 0x00; f[n++] = 0x00;                        /* seq */
	f[n++] = 0x7f;                                        /* category: vendor */
	f[n++] = 0x00; f[n++] = 0x25; f[n++] = 0x00;          /* Apple OUI */
	f[n++] = 0x12;                                        /* OUI type: AWDL */
	for (int i = 0; i < 24; i++) f[n++] = (uint8_t)(0xa0 + i); /* plausible payload */

	int sent = 0;
	for (int k = 0; k < count; k++) {
		ssize_t r = sendto(ps, f, (size_t)n, 0, (struct sockaddr *)&sll, sizeof(sll));
		if (r < 0) printf("send #%d failed: %s\n", k, strerror(errno));
		else { sent++; printf("send #%d ok (%zd bytes)\n", k, r); }
		usleep(150000);
	}
	printf("injected %d/%d frames of %d bytes on %s (freq %u)\n", sent, count, n, ifn, freq);
	close(ps);
	close(nlfd);
	return sent == count ? 0 : 1;
}
