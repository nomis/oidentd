/*
** linux.c - Linux user lookup facility.
** Copyright (c) 1998-2006 Ryan McCabe <ryan@numb.org>
** Copyright (c) 2018      Janik Rabe  <oidentd@janikrabe.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License, version 2,
** as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#define _GNU_SOURCE

#include <config.h>

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/netlink.h>

#include "oidentd.h"
#include "util.h"
#include "user_db.h"
#include "inet_util.h"
#include "missing.h"
#include "masq.h"
#include "options.h"
#include "netlink.h"

#ifdef HAVE_LIBCAP_NG
#	include <cap-ng.h>
#else
#	undef LIBNFCT_SUPPORT
#endif

#ifndef MASQ_SUPPORT
#	undef LIBNFCT_SUPPORT
#endif

#ifdef LIBNFCT_SUPPORT
#	include <libnetfilter_conntrack/libnetfilter_conntrack.h>
#endif

#ifdef HAVE_LIBUDB
#	include <udb.h>
#endif

#define CFILE		"/proc/net/tcp"
#define CFILE6		"/proc/net/tcp6"
#define MASQFILE	"/proc/net/ip_masquerade"
#define IPCONNTRACK	"/proc/net/ip_conntrack"
#define NFCONNTRACK	"/proc/net/nf_conntrack"

static int netlink_sock;
extern struct sockaddr_storage proxy;
extern char *ret_os;

extern uid_t uid;
extern gid_t gid;

#ifdef LIBNFCT_SUPPORT
struct ct_masq_query {
	int sock;
	in_port_t lport;
	in_port_t fport;
	struct sockaddr_storage *laddr;
	struct sockaddr_storage *faddr;
	int status;
};
#endif

#ifdef MASQ_SUPPORT
static int masq_ct_line(char *line,
			int sock,
			in_port_t lport,
			in_port_t fport,
			struct sockaddr_storage *laddr,
			struct sockaddr_storage *faddr);
#endif

#ifdef LIBNFCT_SUPPORT
bool drop_privs_libnfct(uid_t uid, gid_t gid);
static bool dispatch_libnfct_query(struct ct_masq_query *queryp);
static int callback_nfct(enum nf_conntrack_msg_type type,
			struct nf_conntrack *ct,
			void *data);
#endif

static uid_t lookup_tcp_diag(	struct sockaddr_storage *src_addr,
							struct sockaddr_storage *dst_addr,
							in_port_t src_port,
							in_port_t dst_port);

#ifdef MASQ_SUPPORT
enum {
	CT_UNKNOWN,
	CT_MASQFILE,
	CT_IPCONNTRACK,
	CT_NFCONNTRACK,
#	ifdef LIBNFCT_SUPPORT
	CT_LIBNFCT,
#	endif
};
FILE *masq_fp;
static int conntrack = CT_UNKNOWN;
#endif

#ifdef LIBNFCT_SUPPORT
bool drop_privs_libnfct(uid_t uid, gid_t gid) {
	if (conntrack != CT_LIBNFCT)
		return true;

	/* drop privileges, keeping only CAP_NET_ADMIN for libnfct queries */

	int ret;
	capng_clear(CAPNG_SELECT_BOTH);
	ret = capng_update(CAPNG_ADD, CAPNG_EFFECTIVE | CAPNG_PERMITTED,
			CAP_NET_ADMIN);
	if (ret) {
		debug("capng_update: error %d", ret);
		return false;
	}

	ret = capng_change_id(
			opt_enabled(CHANGE_UID) ? uid : MISSING_UID,
			opt_enabled(CHANGE_GID) ? gid : MISSING_GID,
			CAPNG_CLEAR_BOUNDING | CAPNG_DROP_SUPP_GRP);
	if (ret) {
		debug("capng_change_id: error %d", ret);
		return false;
	}

	/* don't try to drop privileges again */
	disable_opt(CHANGE_UID);
	disable_opt(CHANGE_GID);

	return true;
}

static bool dispatch_libnfct_query(struct ct_masq_query *queryp) {
	struct nfct_handle *nfcthp = nfct_open(CONNTRACK, 0);

	if (!nfcthp) {
		debug("nfct_open: %s", strerror(errno));
		return false;
	}

	if (nfct_callback_register(nfcthp, NFCT_T_ALL,
			callback_nfct, (void *) queryp)) {
		debug("nfct_callback_register: %s", strerror(errno));
		return false;
	}

	if (nfct_query(nfcthp, NFCT_Q_DUMP, &queryp->faddr->ss_family)) {
		debug("nfct_query: %s", strerror(errno));
		return false;
	}

	if (nfct_close(nfcthp)) {
		debug("nfct_close: %s", strerror(errno));
		return false;
	}

	return (!queryp->status);
}

/*
** Callback for libnetfilter_conntrack queries
*/

static int callback_nfct(enum nf_conntrack_msg_type type,
			struct nf_conntrack *ct,
			void *data) {
	char buf[1024];
	nfct_snprintf(buf, sizeof(buf), ct, NFCT_T_UNKNOWN,
			NFCT_O_DEFAULT, NFCT_OF_SHOW_LAYER3);

	struct ct_masq_query *query = (struct ct_masq_query *) data;
	int ret = masq_ct_line(buf, query->sock,
			query->lport, query->fport,
			query->laddr, query->faddr);

	if (ret == 1)
		return NFCT_CB_CONTINUE;

	query->status = ret;
	return NFCT_CB_STOP;
}

#endif

/*
** System-dependent initialization; called only once.
** Called before privileges are dropped.
** Returns false on failure.
*/

bool core_init(void) {
#ifdef MASQ_SUPPORT
	if (!opt_enabled(MASQ)) {
		masq_fp = NULL;
		return true;
	}

	masq_fp = fopen(MASQFILE, "r");
	if (masq_fp == NULL) {
		if (errno != ENOENT) {
			o_log(LOG_CRIT, "fopen: %s: %s", MASQFILE, strerror(errno));
			return false;
		}

		masq_fp = fopen(NFCONNTRACK, "r");
		if (masq_fp == NULL) {
			if (errno != ENOENT) {
				o_log(LOG_CRIT, "fopen: %s: %s", NFCONNTRACK, strerror(errno));
				return false;
			}

			masq_fp = fopen(IPCONNTRACK, "r");
			if (masq_fp == NULL) {
				if (errno != ENOENT) {
					o_log(LOG_CRIT, "fopen: %s: %s", IPCONNTRACK, strerror(errno));
					return false;
				}

#	ifdef LIBNFCT_SUPPORT
				conntrack = CT_LIBNFCT;
				return true;
#	else
				o_log(LOG_CRIT, "NAT/IP masquerading support is unavailable");
				disable_opt(MASQ);
#	endif
			} else {
				conntrack = CT_IPCONNTRACK;
			}
		} else {
			conntrack = CT_NFCONNTRACK;
		}
	} else {
		conntrack = CT_MASQFILE;
	}
#endif
	return true;
}


#ifdef WANT_IPV6

/*
** Returns the UID of the owner of an IPv6 connection,
** or MISSING_UID on failure.
*/

uid_t get_user6(	in_port_t lport,
				in_port_t fport,
				struct sockaddr_storage *laddr,
				struct sockaddr_storage *faddr)
{
	FILE *fp;
	char buf[1024];

	if (netlink_sock != -1) {
		uid_t uid = lookup_tcp_diag(laddr, faddr, lport, fport);

		if (uid != MISSING_UID)
			return (uid);
	}

	lport = ntohs(lport);
	fport = ntohs(fport);

	fp = fopen(CFILE6, "r");
	if (fp == NULL) {
		debug("fopen: %s: %s", CFILE6, strerror(errno));
		return MISSING_UID;
	}

	/* Eat the header line. */
	fgets(buf, sizeof(buf), fp);

	while (fgets(buf, sizeof(buf), fp)) {
		struct in6_addr remote6;
		struct in6_addr local6;
		u_int32_t portl_temp;
		u_int32_t portf_temp;
		in_port_t portl;
		in_port_t portf;
		uid_t uid;
		int ret;
		u_long inode;

		ret = sscanf(buf,
			"%*d: %8x%8x%8x%8x:%x %8x%8x%8x%8x:%x %*x %*X:%*X %*x:%*X %*x %u %*d %lu",
			&local6.s6_addr32[0], &local6.s6_addr32[1], &local6.s6_addr32[2],
			&local6.s6_addr32[3], &portl_temp,
			&remote6.s6_addr32[0], &remote6.s6_addr32[1], &remote6.s6_addr32[2],
			&remote6.s6_addr32[3], &portf_temp,
			&uid, &inode);

		if (ret != 12)
			continue;

		portl = (in_port_t) portl_temp;
		portf = (in_port_t) portf_temp;

		if (!memcmp(&local6, sin_addr(laddr), sizeof(local6)) &&
			!memcmp(&remote6, sin_addr(faddr), sizeof(remote6)) &&
			portl == lport &&
			portf == fport)
		{
			fclose(fp);

			if (inode == 0 && uid == 0)
				return MISSING_UID;

			return (uid);
		}
	}

	fclose(fp);
	return MISSING_UID;
}

#endif

/*
** Returns the UID of the owner of an IPv4 connection,
** or MISSING_UID on failure.
*/

uid_t get_user4(	in_port_t lport,
				in_port_t fport,
				struct sockaddr_storage *laddr,
				struct sockaddr_storage *faddr)
{
	uid_t uid;
	FILE *fp;
	char buf[1024];
	u_int32_t inode;
	in_addr_t laddr4;
	in_addr_t faddr4;

	if (netlink_sock != -1) {
		uid = lookup_tcp_diag(laddr, faddr, lport, fport);

		if (uid != MISSING_UID)
			return (uid);
	}

	laddr4 = SIN4(laddr)->sin_addr.s_addr;
	faddr4 = SIN4(faddr)->sin_addr.s_addr;

	lport = ntohs(lport);
	fport = ntohs(fport);

	fp = fopen(CFILE, "r");
	if (fp == NULL) {
		debug("fopen: %s: %s", CFILE, strerror(errno));
		return MISSING_UID;
	}

	/* Eat the header line. */
	fgets(buf, sizeof(buf), fp);

	/*
	** The line should never be longer than 1024 chars, so fgets should be OK.
	*/

	while (fgets(buf, sizeof(buf), fp)) {
		int ret;
		u_int32_t portl_temp;
		u_int32_t portf_temp;
		in_port_t portl;
		in_port_t portf;
		in_addr_t local;
		in_addr_t remote;

		ret = sscanf(buf,
			"%*d: %x:%x %x:%x %*x %*x:%*x %*x:%*x %*x %u %*d %u",
			&local, &portl_temp, &remote, &portf_temp, &uid, &inode);

		if (ret != 6)
			continue;

		portl = (in_port_t) portl_temp;
		portf = (in_port_t) portf_temp;

		if (opt_enabled(PROXY)) {
			if (faddr4 == SIN4(&proxy)->sin_addr.s_addr &&
				remote != SIN4(&proxy)->sin_addr.s_addr &&
				lport == portl &&
				fport == portf)
			{
				goto out_success;
			}
		}

		if (local == laddr4 &&
			remote == faddr4 &&
			portl == lport &&
			portf == fport)
		{
			goto out_success;
		}
	}

	fclose(fp);
	return MISSING_UID;

out_success:
	fclose(fp);

	/*
	** If the inode is zero, the socket is dead, and its owner
	** has probably been set to root.  It would be incorrect
	** to return a successful response here.
	*/

	if (inode == 0 && uid == 0)
		return MISSING_UID;

	return (uid);
}

#ifdef MASQ_SUPPORT

/*
** Handle a request to a host that's IP masquerading through us.
** Returns true on success, false on failure.
*/

bool masq(	int sock,
			in_port_t lport,
			in_port_t fport,
			struct sockaddr_storage *laddr,
			struct sockaddr_storage *faddr)
{
	char buf[1024];

	/*
	** There's no masq support for IPv6 yet.
	*/

	if (faddr->ss_family != AF_INET)
		return false;

	lport = ntohs(lport);
	fport = ntohs(fport);

#ifdef LIBNFCT_SUPPORT
	if (conntrack == CT_LIBNFCT) {
		struct ct_masq_query query = { sock,
				lport, fport,
				laddr, faddr, 1 };
		return dispatch_libnfct_query(&query);
	}
#endif

	/* rewind fp to read new contents */
	rewind(masq_fp);

	if (conntrack == CT_MASQFILE) {
		/* eat the header line */
		fgets(buf, sizeof(buf), masq_fp);
	}

	while (fgets(buf, sizeof(buf), masq_fp)) {
		int ret = masq_ct_line(buf, sock, lport, fport, laddr, faddr);
		if (ret == 1)
			continue;
		return !ret;
	}

	return false;
}

/*
** Process a connection tracking file entry.
** The lport and fport arguments are in host byte order.
** Returns -1 if an error occurs.
** Returns  0 if the entry matches and the request has been handled.
** Returns  1 if the entry does not match the query.
**/

static int masq_ct_line(char *line,
			int sock,
			in_port_t lport,
			in_port_t fport,
			struct sockaddr_storage *laddr,
			struct sockaddr_storage *faddr) {
	char os[24];
	char family[16];
	char proto[16];
	in_port_t mport;
	in_port_t nport;
	in_port_t masq_lport;
	in_port_t masq_fport;
	char user[MAX_ULEN];
	in_addr_t remoten;
	in_addr_t localm;
	in_addr_t remotem;
	in_addr_t localn;
	struct sockaddr_storage ss;
	int ret;

	if (conntrack == CT_MASQFILE) {
		u_int32_t mport_temp;
		u_int32_t nport_temp;
		u_int32_t masq_lport_temp;
		u_int32_t masq_fport_temp;

		ret = sscanf(line, "%15s %X:%X %X:%X %X %X %*d %*d %*u",
				proto, &localm, &masq_lport_temp,
				&remotem, &masq_fport_temp, &mport_temp, &nport_temp);

		if (ret != 7)
			return 1;

		mport = (in_port_t) mport_temp;
		nport = (in_port_t) nport_temp;
		masq_lport = (in_port_t) masq_lport_temp;
		masq_fport = (in_port_t) masq_fport_temp;
	} else if (conntrack == CT_IPCONNTRACK) {
		int l1, l2, l3, l4, r1, r2, r3, r4;
		int nl1, nl2, nl3, nl4, nr1, nr2, nr3, nr4;
		u_int32_t nport_temp;
		u_int32_t mport_temp;
		u_int32_t masq_lport_temp;
		u_int32_t masq_fport_temp;

		ret = sscanf(line,
			"%15s %*d %*d ESTABLISHED src=%d.%d.%d.%d dst=%d.%d.%d.%d sport=%d dport=%d src=%d.%d.%d.%d dst=%d.%d.%d.%d sport=%d dport=%d",
			proto, &l1, &l2, &l3, &l4, &r1, &r2, &r3, &r4,
			&masq_lport_temp, &masq_fport_temp,
			&nl1, &nl2, &nl3, &nl4, &nr1, &nr2, &nr3, &nr4,
			&nport_temp, &mport_temp);

		if (ret != 21) {
			ret = sscanf(line,
				"%15s %*d %*d ESTABLISHED src=%d.%d.%d.%d dst=%d.%d.%d.%d sport=%d dport=%d packets=%*d bytes=%*d src=%d.%d.%d.%d dst=%d.%d.%d.%d sport=%d dport=%d",
			proto, &l1, &l2, &l3, &l4, &r1, &r2, &r3, &r4,
			&masq_lport_temp, &masq_fport_temp,
			&nl1, &nl2, &nl3, &nl4, &nr1, &nr2, &nr3, &nr4,
			&nport_temp, &mport_temp);
		}

		if (ret != 21)
			return 1;

		masq_lport = (in_port_t) masq_lport_temp;
		masq_fport = (in_port_t) masq_fport_temp;

		nport = (in_port_t) nport_temp;
		mport = (in_port_t) mport_temp;

		localm = l1 << 24 | l2 << 16 | l3 << 8 | l4;
		remotem = r1 << 24 | r2 << 16 | r3 << 8 | r4;

		localn = nl1 << 24 | nl2 << 16 | nl3 << 8 | nl4;
		remoten = nr1 << 24 | nr2 << 16 | nr3 << 8 | nr4;
#ifdef LIBNFCT_SUPPORT
	} else if (conntrack == CT_NFCONNTRACK || conntrack == CT_LIBNFCT) {
#else
	} else if (conntrack == CT_NFCONNTRACK) {
#endif
		int l1, l2, l3, l4, r1, r2, r3, r4;
		int nl1, nl2, nl3, nl4, nr1, nr2, nr3, nr4;
		u_int32_t nport_temp;
		u_int32_t mport_temp;
		u_int32_t masq_lport_temp;
		u_int32_t masq_fport_temp;

		ret = sscanf(line,
			"%15s %*d %15s %*d %*d ESTABLISHED src=%d.%d.%d.%d dst=%d.%d.%d.%d sport=%d dport=%d packets=%*d bytes=%*d src=%d.%d.%d.%d dst=%d.%d.%d.%d sport=%d dport=%d",
			family, proto, &l1, &l2, &l3, &l4, &r1, &r2, &r3, &r4,
			&masq_lport_temp, &masq_fport_temp,
			&nl1, &nl2, &nl3, &nl4, &nr1, &nr2, &nr3, &nr4,
			&nport_temp, &mport_temp);

		/* Added to handle /proc/sys/net/netfilter/nf_conntrack_acct = 0 */
		if (ret != 22) {
			ret = sscanf(line,
				"%15s %*d %15s %*d %*d ESTABLISHED src=%d.%d.%d.%d dst=%d.%d.%d.%d sport=%d dport=%d src=%d.%d.%d.%d dst=%d.%d.%d.%d sport=%d dport=%d",
				family, proto, &l1, &l2, &l3, &l4, &r1, &r2, &r3, &r4,
				&masq_lport_temp, &masq_fport_temp,
				&nl1, &nl2, &nl3, &nl4, &nr1, &nr2, &nr3, &nr4,
				&nport_temp, &mport_temp);
		}

		if (ret != 22)
			return 1;

		if (strcasecmp(family, "ipv4"))
			return 1;

		masq_lport = (in_port_t) masq_lport_temp;
		masq_fport = (in_port_t) masq_fport_temp;

		nport = (in_port_t) nport_temp;
		mport = (in_port_t) mport_temp;

		localm = l1 << 24 | l2 << 16 | l3 << 8 | l4;
		remotem = r1 << 24 | r2 << 16 | r3 << 8 | r4;

		localn = nl1 << 24 | nl2 << 16 | nl3 << 8 | nl4;
		remoten = nr1 << 24 | nr2 << 16 | nr3 << 8 | nr4;
	} else
		return -1;

	if (strcasecmp(proto, "tcp"))
		return 1;

	if (mport != lport)
		return 1;

	if (nport != fport)
		return 1;

	/* Local NAT, don't forward or do masquerade entry lookup. */
	if (localm == remoten) {
		uid_t con_uid = MISSING_UID;
		struct passwd *pw;
		char suser[MAX_ULEN];
		char ipbuf[MAX_IPLEN];

		sin_setv4(htonl(remotem), &ss);
		get_ip(faddr, ipbuf, sizeof(ipbuf));

		if (con_uid == MISSING_UID && faddr->ss_family == AF_INET)
			con_uid = get_user4(htons(masq_lport), htons(masq_fport), laddr, &ss);

		/* Add call to get_user6 when IPv6 NAT is supported. */

		if (con_uid == MISSING_UID)
			return -1;

		pw = getpwuid(con_uid);
		if (pw == NULL) {
			sockprintf(sock, "%d,%d:ERROR:%s\r\n",
				lport, fport, ERROR("NO-USER"));

			debug("getpwuid(%u): %s", con_uid, strerror(errno));
			return 0;
		}

		ret = get_ident(pw, masq_lport, masq_fport, laddr, &ss, suser, sizeof(suser));
		if (ret == -1) {
			sockprintf(sock, "%d,%d:ERROR:%s\r\n",
				lport, fport, ERROR("HIDDEN-USER"));

			o_log(NORMAL, "[%s] %d (%d) , %d (%d) : HIDDEN-USER (%s)",
				ipbuf, lport, masq_lport, fport, masq_fport, pw->pw_name);

			return 0;
		}

		sockprintf(sock, "%d,%d:USERID:%s:%s\r\n",
			lport, fport, ret_os, suser);

		o_log(NORMAL, "[%s] Successful lookup: %d (%d) , %d (%d) : %s (%s)",
			ipbuf, lport, masq_lport, fport, masq_fport, pw->pw_name, suser);

		return 0;
	}

	if (localn != ntohl(SIN4(faddr)->sin_addr.s_addr)) {
		if (!opt_enabled(PROXY))
			return 1;

		if (SIN4(faddr)->sin_addr.s_addr != SIN4(&proxy)->sin_addr.s_addr)
			return 1;

		if (localn == SIN4(&proxy)->sin_addr.s_addr)
			return 1;
	}

	sin_setv4(htonl(localm), &ss);

	ret = find_masq_entry(&ss, user, sizeof(user), os, sizeof(os));

	if (opt_enabled(FORWARD) && (ret != 0 || !opt_enabled(MASQ_OVERRIDE))) {
		char ipbuf[MAX_IPLEN];

		if (fwd_request(sock, lport, masq_lport, fport, masq_fport, &ss) == 0)
			return 0;

		get_ip(&ss, ipbuf, sizeof(ipbuf));

		debug("Forward to %s (%d %d) failed", ipbuf, masq_lport, fport);
	}

	if (ret == 0) {
		char ipbuf[MAX_IPLEN];

		sockprintf(sock, "%d,%d:USERID:%s:%s\r\n",
			lport, fport, os, user);

		get_ip(faddr, ipbuf, sizeof(ipbuf));

		o_log(NORMAL,
			"[%s] (Masqueraded) Successful lookup: %d , %d : %s",
			ipbuf, lport, fport, user);

		return 0;
	}
}

#endif

/*
** Much of the code for this function has been borrowed from
** a patch to pidentd written by Alexey Kuznetsov <kuznet@ms2.inr.ac.ru>
** and distributed with the iproute2 package.
**
** Ryan McCabe <ryan@numb.org> has made some cleanups and converted the
** routine to support both IPv4 and IPv6 queries.
*/

static uid_t lookup_tcp_diag(	struct sockaddr_storage *src_addr,
							struct sockaddr_storage *dst_addr,
							in_port_t src_port,
							in_port_t dst_port)
{
	struct sockaddr_nl nladdr;
	struct {
		struct nlmsghdr nlh;
		struct tcpdiagreq r;
	} req;
	size_t addr_len = sin_addr_len(dst_addr);
	struct iovec iov[1];
	struct msghdr msghdr;
	char buf[8192];

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;

	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = TCPDIAG_GETSOCK;
	req.nlh.nlmsg_flags = NLM_F_REQUEST;
	req.nlh.nlmsg_pid = 0;
	req.nlh.nlmsg_seq = 1;

	memset(&req.r, 0, sizeof(req.r));

	req.r.tcpdiag_states = ~0U;
	req.r.tcpdiag_family = dst_addr->ss_family;
	memcpy(&req.r.id.tcpdiag_dst, sin_addr(dst_addr), addr_len);
	memcpy(&req.r.id.tcpdiag_src, sin_addr(src_addr), addr_len);
	req.r.id.tcpdiag_dport = dst_port;
	req.r.id.tcpdiag_sport = src_port;
	req.r.id.tcpdiag_cookie[0] = TCPDIAG_NOCOOKIE;
	req.r.id.tcpdiag_cookie[1] = TCPDIAG_NOCOOKIE;

	iov[0].iov_base = &req;
	iov[0].iov_len = sizeof(req);

	msghdr.msg_name = &nladdr;
	msghdr.msg_namelen = sizeof(nladdr);
	msghdr.msg_iov = iov;
	msghdr.msg_iovlen = 1;
	msghdr.msg_control = NULL;
	msghdr.msg_controllen = 0;
	msghdr.msg_flags = 0;

	if (sendmsg(netlink_sock, &msghdr, 0) < 0) {
		if (errno == ECONNREFUSED) {
			close(netlink_sock);
			netlink_sock = -1;
		}

		return MISSING_UID;
	}

	iov[0].iov_base = buf;
	iov[0].iov_len = sizeof(buf);

	while (1) {
		ssize_t ret;
		size_t uret;
		struct nlmsghdr *h;

		msghdr.msg_name = &nladdr;
		msghdr.msg_namelen = sizeof(nladdr);
		msghdr.msg_iov = iov;
		msghdr.msg_iovlen = 1;
		msghdr.msg_control = NULL;
		msghdr.msg_controllen = 0;
		msghdr.msg_flags = 0;

		ret = recvmsg(netlink_sock, &msghdr, 0);
		if (ret < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;

			return MISSING_UID;
		}

		if (ret == 0)
			return MISSING_UID;

		h = (struct nlmsghdr *) buf;

		uret = (size_t) ret;
		while (NLMSG_OK(h, uret)) {
			struct tcpdiagmsg *r;

			if (h->nlmsg_seq != 1) {
				h = NLMSG_NEXT(h, uret);
				continue;
			}

			if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR)
				return MISSING_UID;

			r = NLMSG_DATA(h);

			if (r->id.tcpdiag_dport == dst_port &&
				r->id.tcpdiag_sport == src_port &&
				!memcmp(r->id.tcpdiag_dst, sin_addr(dst_addr), addr_len) &&
				!memcmp(r->id.tcpdiag_src, sin_addr(src_addr), addr_len))
			{
				if (r->tcpdiag_inode == 0 && r->tcpdiag_uid == 0)
					return MISSING_UID;

				return (r->tcpdiag_uid);
			}

			return MISSING_UID;
		}

		if ((msghdr.msg_flags & MSG_TRUNC) || uret != 0)
			return MISSING_UID;
	}

	return MISSING_UID;
}

/*
** Just open a netlink socket here.
*/

int k_open(void) {
	netlink_sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_TCPDIAG);
	return (0);
}
