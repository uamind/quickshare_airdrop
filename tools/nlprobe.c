/*
 * nlprobe - minimal nl80211 capability probe (no libnl dependency)
 *
 * Prints, for every wiphy: index, name, advertised interface types
 * (NL80211_ATTR_SUPPORTED_IFTYPES) and current interfaces with their types.
 * With "create" as argv[1] it additionally tries to create a monitor
 * interface (and deletes it again) to see whether the driver really allows it.
 *
 * Build:  zig cc -target aarch64-linux-musl -Os -static nlprobe.c -o nlprobe
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#define NETLINK_GENERIC 16

#define GENL_ID_CTRL 16
#define CTRL_CMD_GETFAMILY 3
#define CTRL_ATTR_FAMILY_ID 1
#define CTRL_ATTR_FAMILY_NAME 2

#define NLMSG_ALIGNTO 4
#define NLMSG_ALIGN(l) (((l) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLA_ALIGN(l) NLMSG_ALIGN(l)

#define NLMSG_NOOP 1
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define NLMSG_OVERRUN 4

#define NLM_F_REQUEST 0x01
#define NLM_F_ACK 0x04
#define NLM_F_DUMP 0x300
#define NLM_F_CREATE 0x400
#define NLM_F_EXCL 0x200

#define NL80211_CMD_GET_WIPHY 1
#define NL80211_CMD_GET_INTERFACE 5
#define NL80211_CMD_NEW_INTERFACE 7
#define NL80211_CMD_DEL_INTERFACE 8

#define NL80211_ATTR_WIPHY 1
#define NL80211_ATTR_WIPHY_NAME 2
#define NL80211_ATTR_IFINDEX 3
#define NL80211_ATTR_IFNAME 4
#define NL80211_ATTR_IFTYPE 5
#define NL80211_ATTR_SUPPORTED_IFTYPES 28

struct nlmsghdr {
	uint32_t nlmsg_len;
	uint16_t nlmsg_type;
	uint16_t nlmsg_flags;
	uint32_t nlmsg_seq;
	uint32_t nlmsg_pid;
};

struct nlattr {
	uint16_t nla_len;
	uint16_t nla_type;
};

struct genlmsghdr {
	uint8_t cmd;
	uint8_t version;
	uint16_t reserved;
};

struct sockaddr_nl_ {
	uint16_t nl_family;
	uint16_t nl_pad;
	uint32_t nl_pid;
	uint32_t nl_groups;
};

static const char *iftype_name(int t)
{
	static char buf[32];
	switch (t) {
	case 0: return "UNSPECIFIED";
	case 1: return "ADHOC";
	case 2: return "STATION";
	case 3: return "AP";
	case 4: return "AP_VLAN";
	case 5: return "WDS";
	case 6: return "MONITOR";
	case 7: return "MESH_POINT";
	case 8: return "P2P_CLIENT";
	case 9: return "P2P_GO";
	case 10: return "P2P_DEVICE";
	case 11: return "OCB";
	case 12: return "NAN";
	default:
		snprintf(buf, sizeof(buf), "IFTYPE_%d", t);
		return buf;
	}
}

#define MAXMSG 8192
static uint8_t msgbuf[MAXMSG];
static uint8_t rcvbuf[65536];
static int fd__nl;

static int nl_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0) {
		printf("socket(AF_NETLINK) failed: %s\n", strerror(errno));
		return -1;
	}
	struct sockaddr_nl_ sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = (uint32_t)getpid();
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("bind failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}

static char *put_attr(char *p, uint16_t type, const void *data, uint16_t len)
{
	struct nlattr *a = (struct nlattr *)p;
	a->nla_type = type;
	a->nla_len = (uint16_t)(sizeof(*a) + len);
	memcpy(p + sizeof(*a), data, len);
	int total = (int)sizeof(*a) + len;
	int aligned = NLA_ALIGN(total);
	memset(p + total, 0, aligned - total);
	return p + aligned;
}

static char *put_attr_str(char *p, uint16_t type, const char *s)
{
	return put_attr(p, type, s, (uint16_t)(strlen(s) + 1));
}

static char *put_attr_u32(char *p, uint16_t type, uint32_t v)
{
	return put_attr(p, type, &v, 4);
}

static int nl_send(char *attrs_end, uint16_t type, uint16_t flags, uint8_t cmd,
		   uint8_t version, uint32_t seq)
{
	char *p = (char *)msgbuf;
	struct nlmsghdr *nlh = (struct nlmsghdr *)p;
	struct genlmsghdr *gh = (struct genlmsghdr *)(p + sizeof(*nlh));
	gh->cmd = cmd;
	gh->version = version;
	gh->reserved = 0;
	char *end = attrs_end ? attrs_end : p + sizeof(*nlh) + sizeof(*gh);
	nlh->nlmsg_len = (uint32_t)(end - p);
	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = flags;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = (uint32_t)getpid();

	struct sockaddr_nl_ sa;
	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_pid = 0; /* kernel */

	if (sendto(fd__nl, msgbuf, nlh->nlmsg_len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("sendto(type=%u cmd=%u) failed: %s\n", type, cmd, strerror(errno));
		return -1;
	}
	return 0;
}

static int parse_attrs(char *base, int len, void (*cb)(uint16_t, const void *, int, void *), void *ctx)
{
	int off = 0;
	while (off + (int)sizeof(struct nlattr) <= len) {
		struct nlattr *a = (struct nlattr *)(base + off);
		if (a->nla_len < sizeof(*a) || off + a->nla_len > len)
			break;
		cb(a->nla_type, base + off + sizeof(*a), a->nla_len - (int)sizeof(*a), ctx);
		off += NLA_ALIGN(a->nla_len);
	}
	return off;
}

/* ---- genl family resolution ---- */
static uint16_t g_family_id;
static uint8_t g_family_ver = 1;

struct famctx { uint16_t id; uint8_t ver; };

static void fam_cb(uint16_t type, const void *data, int len, void *ctx)
{
	struct famctx *f = ctx;
	if (type == CTRL_ATTR_FAMILY_ID && len >= 2) {
		uint16_t v;
		memcpy(&v, data, 2);
		f->id = v;
	}
}

static int resolve_nl80211(int fd)
{
	fd__nl = fd;
	char *p = (char *)msgbuf;
	char *end = p + sizeof(struct nlmsghdr) + sizeof(struct genlmsghdr);
	end = put_attr_str(end, CTRL_ATTR_FAMILY_NAME, "nl80211");
	if (nl_send(end, GENL_ID_CTRL, NLM_F_REQUEST, CTRL_CMD_GETFAMILY, 1, 100) < 0)
		return -1;

	ssize_t n = recv(fd, rcvbuf, sizeof(rcvbuf), 0);
	if (n <= 0) {
		printf("recv(GETFAMILY) failed: %s\n", n ? strerror(errno) : "eof");
		return -1;
	}
	for (struct nlmsghdr *h = (struct nlmsghdr *)rcvbuf; (int)n >= (int)sizeof(*h) && h->nlmsg_len >= sizeof(*h);
	     n -= NLMSG_ALIGN(h->nlmsg_len),
	     h = (struct nlmsghdr *)((char *)h + NLMSG_ALIGN(h->nlmsg_len))) {
		if (h->nlmsg_type == NLMSG_ERROR) {
			int err = *(int *)((char *)h + sizeof(*h));
			printf("GETFAMILY error: %s (%d)\n", strerror(-err), err);
			return -1;
		}
		char *payload = (char *)h + sizeof(*h) + sizeof(struct genlmsghdr);
		int plen = h->nlmsg_len - (int)sizeof(*h) - (int)sizeof(struct genlmsghdr);
		struct famctx f = { 0, 1 };
		parse_attrs(payload, plen, fam_cb, &f);
		if (f.id) {
			g_family_id = f.id;
			return 0;
		}
	}
	printf("nl80211 family not found\n");
	return -1;
}

/* ---- wiphy dump ---- */
struct wiphy_info {
	int idx;
	char name[32];
	unsigned bits[16];
	int ntypes;
	int printed;
};

static void iftypes_cb(uint16_t type, const void *data, int len, void *ctx)
{
	struct wiphy_info *w = ctx;
	(void)data; (void)len;
	if (type < 16 * 32) {
		w->bits[type / 32] |= 1u << (type % 32);
		w->ntypes++;
	}
}

static void wiphy_cb(uint16_t type, const void *data, int len, void *ctx)
{
	struct wiphy_info *w = ctx;
	if (type == NL80211_ATTR_WIPHY && len >= 4)
		memcpy(&w->idx, data, 4);
	else if (type == NL80211_ATTR_WIPHY_NAME && len > 0) {
		int cl = len < (int)sizeof(w->name) - 1 ? len : (int)sizeof(w->name) - 1;
		memcpy(w->name, data, cl);
		w->name[cl] = 0;
	} else if (type == NL80211_ATTR_SUPPORTED_IFTYPES)
		parse_attrs((char *)data, len, iftypes_cb, ctx);
}

/* ---- interface dump ---- */
struct iface_info {
	uint32_t index;
	uint32_t wiphy;
	uint32_t type;
	char name[32];
};

static void iface_cb(uint16_t type, const void *data, int len, void *ctx)
{
	struct iface_info *i = ctx;
	if (type == NL80211_ATTR_IFINDEX && len >= 4)
		memcpy(&i->index, data, 4);
	else if (type == NL80211_ATTR_WIPHY && len >= 4)
		memcpy(&i->wiphy, data, 4);
	else if (type == NL80211_ATTR_IFTYPE && len >= 4)
		memcpy(&i->type, data, 4);
	else if (type == NL80211_ATTR_IFNAME && len > 0) {
		int cl = len < (int)sizeof(i->name) - 1 ? len : (int)sizeof(i->name) - 1;
		memcpy(i->name, data, cl);
		i->name[cl] = 0;
	}
}

static void print_dump(char *payload, int plen, void (*cb)(uint16_t, const void *, int, void *), void *ctx)
{
	parse_attrs(payload, plen, cb, ctx);
}

static int dump(int fd, uint8_t cmd, void (*cb)(uint16_t, const void *, int, void *), void *ctx, size_t ctxsz,
		void (*show)(void *))
{
	fd__nl = fd;
	if (nl_send(NULL, g_family_id, NLM_F_REQUEST | NLM_F_DUMP, cmd, g_family_ver, 200) < 0)
		return -1;
	for (;;) {
		ssize_t n = recv(fd, rcvbuf, sizeof(rcvbuf), 0);
		if (n <= 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				printf("recv timeout\n");
				return -1;
			}
			printf("recv failed: %s\n", n ? strerror(errno) : "eof");
			return -1;
		}
		ssize_t left = n;
		struct nlmsghdr *h = (struct nlmsghdr *)rcvbuf;
		int done = 0;
		while (left >= (int)sizeof(*h) && h->nlmsg_len >= sizeof(*h) && (int)h->nlmsg_len <= left) {
			if (h->nlmsg_type == NLMSG_DONE) {
				done = 1;
				break;
			}
			if (h->nlmsg_type == NLMSG_ERROR) {
				int err = *(int *)((char *)h + sizeof(*h));
				printf("cmd %u error: %s (%d)\n", cmd, strerror(-err), err);
				return -1;
			}
			char *payload = (char *)h + sizeof(*h) + sizeof(struct genlmsghdr);
			int plen = h->nlmsg_len - (int)sizeof(*h) - (int)sizeof(struct genlmsghdr);
			memset(ctx, 0, ctxsz);
			print_dump(payload, plen, cb, ctx);
			if (show)
				show(ctx);
			left -= NLMSG_ALIGN(h->nlmsg_len);
			h = (struct nlmsghdr *)((char *)h + NLMSG_ALIGN(h->nlmsg_len));
		}
		if (done)
			return 0;
	}
}

static void show_wiphy(void *ctx)
{
	struct wiphy_info *w = ctx;
	printf("wiphy %d %-8s iftypes:", w->idx, w->name);
	int n = 0;
	int has_monitor = 0, has_nan = 0, has_p2pdev = 0, has_ap = 0;
	for (int t = 0; t < 16 * 32; t++) {
		if (w->bits[t / 32] & (1u << (t % 32))) {
			printf(" %s", iftype_name(t));
			n++;
			if (t == 6) has_monitor = 1;
			if (t == 12) has_nan = 1;
			if (t == 10) has_p2pdev = 1;
			if (t == 3) has_ap = 1;
		}
	}
	if (!n)
		printf(" <none reported>");
	printf("\n");
	printf("  ---> MONITOR=%s NAN=%s P2P_DEVICE=%s AP=%s\n",
	       has_monitor ? "YES" : "no", has_nan ? "YES" : "no",
	       has_p2pdev ? "YES" : "no", has_ap ? "YES" : "no");
}

static void show_iface(void *ctx)
{
	struct iface_info *i = ctx;
	printf("iface idx=%-3u wiphy=%u name=%-12s type=%s\n", i->index, i->wiphy, i->name,
	       iftype_name((int)i->type));
}

/* ---- explicit monitor interface creation test ---- */
static int try_create_monitor(int fd, int wiphy)
{
	char *p = (char *)msgbuf;
	char *end = p + sizeof(struct nlmsghdr) + sizeof(struct genlmsghdr);
	end = put_attr_u32(end, NL80211_ATTR_WIPHY, (uint32_t)wiphy);
	end = put_attr_str(end, NL80211_ATTR_IFNAME, "qsamon0");
	end = put_attr_u32(end, NL80211_ATTR_IFTYPE, 6);
	fd__nl = fd;
	if (nl_send(end, g_family_id, NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL,
		    NL80211_CMD_NEW_INTERFACE, g_family_ver, 300) < 0)
		return -1;
	ssize_t n = recv(fd, rcvbuf, sizeof(rcvbuf), 0);
	if (n <= 0) {
		printf("NEW_INTERFACE recv failed: %s\n", n ? strerror(errno) : "eof");
		return -1;
	}
	struct nlmsghdr *h = (struct nlmsghdr *)rcvbuf;
	if ((int)h->nlmsg_len < (int)sizeof(*h) || h->nlmsg_type != NLMSG_ERROR) {
		printf("NEW_INTERFACE(type=monitor) -> unexpected reply type %u\n", h->nlmsg_type);
		return -1;
	}
	int err = *(int *)((char *)h + sizeof(*h));
	if (err == 0) {
		printf("NEW_INTERFACE(type=monitor) -> OK (driver accepted a monitor interface)\n");
		char *q = (char *)msgbuf;
		char *de = q + sizeof(struct nlmsghdr) + sizeof(struct genlmsghdr);
		de = put_attr_str(de, NL80211_ATTR_IFNAME, "qsamon0");
		if (nl_send(de, g_family_id, NLM_F_REQUEST | NLM_F_ACK, NL80211_CMD_DEL_INTERFACE,
			    g_family_ver, 301) == 0) {
			recv(fd, rcvbuf, sizeof(rcvbuf), 0);
			printf("  (cleanup DEL_INTERFACE sent)\n");
		}
		return 0;
	}
	printf("NEW_INTERFACE(type=monitor) -> refused: %s (%d)\n", strerror(-err), err);
	return -1;
}

int main(int argc, char **argv)
{
	int do_create = (argc > 1 && strcmp(argv[1], "create") == 0);
	int fd = nl_open();
	if (fd < 0)
		return 1;
	if (resolve_nl80211(fd) < 0)
		return 1;
	printf("nl80211 family id=%u\n", g_family_id);

	struct wiphy_info w;
	if (dump(fd, NL80211_CMD_GET_WIPHY, wiphy_cb, &w, sizeof(w), show_wiphy) < 0)
		return 1;

	struct iface_info i;
	if (dump(fd, NL80211_CMD_GET_INTERFACE, iface_cb, &i, sizeof(i), show_iface) < 0)
		return 1;

	if (do_create) {
		printf("--- monitor interface creation test (wiphy 0) ---\n");
		try_create_monitor(fd, 0);
	}
	close(fd);
	return 0;
}
