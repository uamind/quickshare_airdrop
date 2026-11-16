/*
 * delif - delete a virtual wireless interface via NL80211_CMD_DEL_INTERFACE
 *         (cfg80211 interfaces cannot be removed with "ip link del").
 *
 * Usage: delif <ifname>
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>

#define AF_NETLINK_ 16
#define NETLINK_GENERIC 16
#define GENL_ID_CTRL 16
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2
#define NLA_ALIGN(l) (((l) + 3) & ~3u)
#define NLMSG_ERROR 2
#define NLM_F_REQUEST 0x01
#define NLM_F_ACK 0x04
#define NL80211_CMD_DEL_INTERFACE 8
#define NL80211_ATTR_IFINDEX 3
#define NL80211_ATTR_IFNAME 4

struct nlmsghdr { uint32_t nlmsg_len; uint16_t nlmsg_type, nlmsg_flags; uint32_t nlmsg_seq, nlmsg_pid; };
struct nlattr { uint16_t nla_len, nla_type; };
struct genlmsghdr { uint8_t cmd, version; uint16_t reserved; };
struct snl { uint16_t nl_family, nl_pad; uint32_t nl_pid, nl_groups; };

static uint8_t sbuf[2048], rbuf[8192];
static int fd;
static uint16_t fam;
static uint8_t famver = 1;

static char *put_u32(char *p, uint16_t t, uint32_t v)
{
	struct nlattr *a = (struct nlattr *)p;
	a->nla_type = t;
	a->nla_len = (uint16_t)(sizeof(*a) + 4);
	memcpy(p + sizeof(*a), &v, 4);
	return p + (int)sizeof(*a) + 4;
}

static char *put_str(char *p, uint16_t t, const char *s)
{
	uint16_t l = (uint16_t)(strlen(s) + 1);
	struct nlattr *a = (struct nlattr *)p;
	a->nla_type = t;
	a->nla_len = (uint16_t)(sizeof(*a) + l);
	memcpy(p + sizeof(*a), s, l);
	int tot = (int)sizeof(*a) + l, al = NLA_ALIGN(tot);
	memset(p + tot, 0, al - tot);
	return p + al;
}

static int send_msg(char *end, uint16_t type, uint16_t flags, uint8_t cmd, uint32_t seq)
{
	char *p = (char *)sbuf;
	struct nlmsghdr *h = (struct nlmsghdr *)p;
	struct genlmsghdr *g = (struct genlmsghdr *)(p + sizeof(*h));
	g->cmd = cmd; g->version = famver; g->reserved = 0;
	char *e = end ? end : p + sizeof(*h) + sizeof(*g);
	h->nlmsg_len = (uint32_t)(e - p);
	h->nlmsg_type = type; h->nlmsg_flags = flags; h->nlmsg_seq = seq;
	h->nlmsg_pid = (uint32_t)getpid();
	struct snl sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	return (int)sendto(fd, sbuf, h->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa));
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("usage: %s <ifname>\n", argv[0]); return 2; }
	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0) { printf("socket: %s\n", strerror(errno)); return 1; }
	struct snl sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = (uint32_t)getpid();
	bind(fd, (struct sockaddr *)&sa, sizeof(sa));
	struct timeval tv = { .tv_sec = 2 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	char *p = (char *)sbuf;
	char *e = p + sizeof(struct nlmsghdr) + sizeof(struct genlmsghdr);
	e = put_str(e, CTRL_ATTR_FAMILY_NAME, "nl80211");
	send_msg(e, GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY, 1);
	ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
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
	printf("nl80211 family=%u, deleting %s ...\n", fam, argv[1]);

	p = (char *)sbuf;
	e = p + sizeof(struct nlmsghdr) + sizeof(struct genlmsghdr);
	unsigned ii = if_nametoindex(argv[1]);
	if (!ii) { printf("if_nametoindex(%s): %s\n", argv[1], strerror(errno)); return 1; }
	printf("ifindex=%u\n", ii);
	e = put_u32(e, NL80211_ATTR_IFINDEX, ii);
	if (send_msg(e, fam, NLM_F_REQUEST | NLM_F_ACK, NL80211_CMD_DEL_INTERFACE, 2) < 0) {
		printf("sendto: %s\n", strerror(errno));
		return 1;
	}
	n = recv(fd, rbuf, sizeof(rbuf), 0);
	if (n <= 0) { printf("recv: %s\n", n ? strerror(errno) : "eof"); return 1; }
	struct nlmsghdr *h = (struct nlmsghdr *)rbuf;
	if (h->nlmsg_type != NLMSG_ERROR) {
		printf("unexpected reply type %u (treating as success)\n", h->nlmsg_type);
		return 0;
	}
	int err = *(int *)((char *)h + sizeof(*h));
	if (err == 0) { printf("DEL_INTERFACE %s -> OK\n", argv[1]); return 0; }
	printf("DEL_INTERFACE %s -> %s (%d)\n", argv[1], strerror(-err), err);
	return 1;
}
