/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ctype.h>

#include <spa/utils/cleanup.h>
#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

#include <module-rtp/sap.h>
#include <module-rtp/ptp.h>
#include "network-utils.h"

#ifdef __FreeBSD__
#define SO_PASSCRED LOCAL_CREDS_PERSISTENT
#endif

/** \page page_module_rtp_sap SAP Announce and create RTP streams
 *
 * The `rtp-sap` module announces RTP streams that match the rules with the
 * announce-stream action.
 *
 * It will create source RTP streams that are announced with SAP when they
 * match the rule with the create-stream action.
 *
 * If no stream.rules are given, it will announce all streams with
 * sess.sap.announce = true and it will create a receiver for all announced
 * streams.
 *
 * ## Module Name
 *
 * `libpipewire-module-rtp-sap`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `local.ifname = <str>`: interface name to use
 * - `sap.ip = <str>`: IP address of the SAP messages, default "224.0.0.56"
 * - `sap.port = <int>`: port of the SAP messages, default 9875
 * - `sap.cleanup.sec = <int>`: cleanup interval in seconds, default 90 seconds
 * - `source.ip =<str>`: source IP address, default "0.0.0.0"
 * - `net.ttl = <int>`: TTL to use, default 1
 * - `net.loop = <bool>`: loopback multicast, default false
 * - `stream.rules` = <rules>: match rules, use create-stream and announce-stream actions
 * - `sap.max-sessions = <int>`: maximum number of concurrent send/receive sessions to track
 * - `sap.preamble-extra = [strings]`: extra attributes to add to the atomic SDP preamble
 * - `sap.end-extra = [strings]`: extra attributes to add to the end of the SDP message
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_REMOTE_NAME
 *
 * ## Example configuration
 *\code{.unparsed}
 * # ~/.config/pipewire/pipewire.conf.d/my-rtp-sap.conf
 *
 * context.modules = [
 * {   name = libpipewire-module-rtp-sap
 *     args = {
 *         #local.ifname = "eth0"
 *         #sap.ip = "224.0.0.56"
 *         #sap.port = 9875
 *         #sap.cleanup.sec = 5
 *         #source.ip = "0.0.0.0"
 *         #net.ttl = 1
 *         #net.loop = false
 *         stream.rules = [
 *             {   matches = [
 *                     # any of the items in matches needs to match, if one does,
 *                     # actions are emitted.
 *                     {   # all keys must match the value. ! negates. ~ starts regex.
 *                         #rtp.origin = "wim 3883629975 0 IN IP4 0.0.0.0"
 *                         #rtp.payload = "127"
 *                         #rtp.fmt = "L16/48000/2"
 *                         #rtp.session = "PipeWire RTP Stream on fedora"
 *                         #rtp.ts-offset = 0
 *                         #rtp.ts-refclk = "private"
 *                         sess.sap.announce = true
 *                     }
 *                 ]
 *                 actions = {
 *                     announce-stream = {
 *                     }
 *                 }
 *             }
 *             {   matches = [
 *                     {   # all keys must match the value. ! negates. ~ starts regex.
 *                         #rtp.origin = "wim 3883629975 0 IN IP4 0.0.0.0"
 *                         #rtp.payload = "127"
 *                         #rtp.fmt = "L16/48000/2"
 *                         #rtp.session = "PipeWire RTP Stream on fedora"
 *                         #rtp.ts-offset = 0
 *                         #rtp.ts-refclk = "private"
 *                         rtp.session = "~.*"
 *                     }
 *                 ]
 *                 actions = {
 *                     create-stream = {
 *                         #sess.latency.msec = 100
 *                         #sess.ts-direct = false
 *                         #target.object = ""
 *                     }
 *                 }
 *             }
 *         ]
 *     }
 * }
 * ]
 *\endcode
 *
 * \since 0.3.67
 */

#define NAME "rtp-sap"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_MAX_SESSIONS		64

#define DEFAULT_ANNOUNCE_RULES	\
	"[ { matches = [ { sess.sap.announce = true } ] actions = { announce-stream = { } } } ]"
#define DEFAULT_CREATE_RULES	\
        "[ { matches = [ { rtp.session = \"~.*\" } ] actions = { create-stream = { } } } ] "

#define DEFAULT_CLEANUP_SEC	90
#define SAP_INTERVAL_SEC	5
#define SAP_MIME_TYPE		"application/sdp"

#define DEFAULT_SAP_IP		"224.0.0.56"
#define DEFAULT_SAP_PORT	9875

#define DEFAULT_SOURCE_IP	"0.0.0.0"
#define DEFAULT_SOURCE_IP6	"::"
#define DEFAULT_TTL		1
#define DEFAULT_LOOP		false

#define MAX_SDP			2048

#define USAGE	"( local.ifname=<local interface name to use> ) "					\
		"( sap.ip=<SAP IP address to send announce, default:"DEFAULT_SAP_IP"> ) "		\
		"( sap.port=<SAP port to send on, default:"SPA_STRINGIFY(DEFAULT_SAP_PORT)"> ) "	\
 		"( sap.cleanup.sec=<cleanup interval in seconds, default 90> ) "			\
		"( source.ip=<source IP address, default:"DEFAULT_SOURCE_IP"> ) "			\
		"( net.ttl=<desired TTL, default:"SPA_STRINGIFY(DEFAULT_TTL)"> ) "			\
		"( net.loop=<desired loopback, default:"SPA_STRINGIFY(DEFAULT_LOOP)"> ) "		\
		"( stream.rules=<rules>, use announce-stream and create-stream actions )"

static const struct spa_dict_item module_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "RTP SAP announce/listen" },
	{ PW_KEY_MODULE_USAGE, USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define PTP_MESSAGE_TYPE_MANAGEMENT 0x0d
#define PTP_VERSION_1588_2008_2_1 0x12
#define PTP_DEFAULT_LOG_MESSAGE_INTERVAL 127
#define PTP_MGMT_ACTION_GET 0
#define PTP_MGMT_ACTION_RESPONSE 2
#define PTP_TLV_TYPE_MGMT 0x0001
#define PTP_MGMT_ID_PARENT_DATA_SET 0x2002

struct sdp_info {
	uint16_t hash;
	uint32_t session_id;
	uint32_t session_version;
	uint32_t t_ntp;

	char *origin;
	char *session_name;
	char *media_type;
	char *mime_type;
	char channelmap[512];

	uint16_t dst_port;
	struct sockaddr_storage dst_addr;
	socklen_t dst_len;
	uint16_t ttl;

	uint16_t port;
	uint8_t payload;

	uint32_t rate;
	uint32_t channels;

	float ptime;
	uint32_t framecount;

	uint32_t ssrc;
	uint32_t ts_offset;
	char *ts_refclk;
};

struct session {
	struct spa_list link;

	bool announce;
	uint64_t timestamp;
	bool ts_refclk_ptp;

	struct impl *impl;
	struct node *node;

	struct sdp_info info;

	unsigned has_sent_sap:1;
	unsigned has_sdp:1;
	char sdp[MAX_SDP];

	struct pw_properties *props;

	struct pw_impl_module *module;
	struct spa_hook module_listener;
};

struct node {
	struct impl *impl;

	uint32_t id;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook node_listener;

	struct pw_node_info *info;
	struct session *session;
};

struct impl {
	struct pw_properties *props;

	struct pw_loop *loop;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;
	unsigned int do_disconnect:1;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_source *timer;

	char *ifname;
	uint32_t ttl;
	bool mcast_loop;

	struct sockaddr_storage src_addr;
	socklen_t src_len;

	uint16_t sap_port;
	struct sockaddr_storage sap_addr;
	socklen_t sap_len;
	int sap_fd;
	struct spa_source *sap_source;
	uint32_t cleanup_interval;

	uint32_t max_sessions;
	uint32_t n_sessions;
	struct spa_list sessions;

	char *extra_attrs_preamble;
	char *extra_attrs_end;

	char *ptp_mgmt_socket;
	int ptp_fd;
	uint32_t ptp_seq;
	uint8_t clock_id[8];
	uint8_t gm_id[8];
};

struct format_info {
	uint32_t media_subtype;
	uint32_t format;
	uint32_t size;
	const char *mime;
	const char *media_type;
	const char *format_str;
};

static const struct format_info audio_format_info[] = {
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_U8, 1, "L8", "audio", "U8" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_ALAW, 1, "PCMA", "audio", "ALAW" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_ULAW, 1, "PCMU", "audio", "ULAW" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S16_BE, 2, "L16", "audio", "S16BE" },
	{ SPA_MEDIA_SUBTYPE_raw, SPA_AUDIO_FORMAT_S24_BE, 3, "L24", "audio", "S24BE" },
	{ SPA_MEDIA_SUBTYPE_control, 0, 1, "rtp-midi", "midi", NULL },
	{ SPA_MEDIA_SUBTYPE_opus, 0, 1, "opus", "opus", NULL },
};

static const struct format_info *find_audio_format_info(const char *mime)
{
	SPA_FOR_EACH_ELEMENT_VAR(audio_format_info, f)
		if (spa_streq(f->mime, mime))
			return f;
	return NULL;
}

static int send_sap(struct impl *impl, struct session *sess, bool bye);


static void clear_sdp_info(struct sdp_info *info)
{
	free(info->origin);
	free(info->session_name);
	free(info->media_type);
	free(info->mime_type);
	free(info->ts_refclk);
	spa_zero(*info);
}

static uint64_t get_time_nsec(struct impl *impl)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

static void session_touch(struct session *sess)
{
	sess->timestamp = get_time_nsec(sess->impl);
}

static void session_free(struct session *sess)
{
	struct impl *impl = sess->impl;

	if (sess->impl) {
		if (sess->announce)
			send_sap(impl, sess, 1);
		spa_list_remove(&sess->link);
		impl->n_sessions--;
	}
	if (sess->node && sess->node->session != NULL)
		sess->node->session = NULL;

	if (sess->module) {
		spa_hook_remove(&sess->module_listener);
		pw_impl_module_destroy(sess->module);
	}

	pw_properties_free(sess->props);
	clear_sdp_info(&sess->info);
	free(sess);
}

static bool is_multicast(struct sockaddr *sa, socklen_t salen)
{
	if (sa->sa_family == AF_INET) {
		static const uint32_t ipv4_mcast_mask = 0xe0000000;
		struct sockaddr_in *sa4 = (struct sockaddr_in*)sa;
		return (ntohl(sa4->sin_addr.s_addr) & ipv4_mcast_mask) == ipv4_mcast_mask;
	} else if (sa->sa_family == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)sa;
		return sa6->sin6_addr.s6_addr[0] == 0xff;
	}
	return false;
}

static int make_unix_socket(const char *path) {
	struct sockaddr_un addr;

	spa_autoclose int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		pw_log_warn("Failed to create PTP management socket");
		return -1;
	}

	int val = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &val, sizeof(val)) < 0) {
		pw_log_warn("Failed to bind PTP management socket");
		return -1;
	}

	spa_zero(addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		pw_log_warn("Failed to connect PTP management socket");
		return -1;
	}

	return spa_steal_fd(fd);
}

static int make_send_socket(
		struct sockaddr_storage *src, socklen_t src_len,
		struct sockaddr_storage *sa, socklen_t salen,
		bool loop, int ttl)
{
	int af, fd, val, res;

	af = src->ss_family;
	if ((fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}
	if (bind(fd, (struct sockaddr*)src, src_len) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error;
	}
	if (connect(fd, (struct sockaddr*)sa, salen) < 0) {
		res = -errno;
		pw_log_error("connect() failed: %m");
		goto error;
	}
	if (is_multicast((struct sockaddr*)sa, salen)) {
		if (sa->ss_family == AF_INET) {
			val = loop;
			if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val)) < 0)
				pw_log_warn("setsockopt(IP_MULTICAST_LOOP) failed: %m");

			val = ttl;
			if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof(val)) < 0)
				pw_log_warn("setsockopt(IP_MULTICAST_TTL) failed: %m");
		} else {
			val = loop;
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &val, sizeof(val)) < 0)
				pw_log_warn("setsockopt(IPV6_MULTICAST_LOOP) failed: %m");

			val = ttl;
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &val, sizeof(val)) < 0)
				pw_log_warn("setsockopt(IPV6_MULTICAST_HOPS) failed: %m");
		}
	}
	return fd;
error:
	close(fd);
	return res;
}

static int make_recv_socket(struct sockaddr_storage *sa, socklen_t salen,
		char *ifname)
{
	int af, fd, val, res;
	struct ifreq req;
	struct sockaddr_storage ba = *sa;
	bool do_connect = false;
	char addr[128];

	af = sa->ss_family;
	if ((fd = socket(af, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		pw_log_error("socket failed: %m");
		return -errno;
	}
	val = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
		res = -errno;
		pw_log_error("setsockopt failed: %m");
		goto error;
	}
	spa_zero(req);
	if (ifname) {
		snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", ifname);
		res = ioctl(fd, SIOCGIFINDEX, &req);
	        if (res < 0)
	                pw_log_warn("SIOCGIFINDEX %s failed: %m", ifname);
	}
	res = 0;
	if (af == AF_INET) {
		static const uint32_t ipv4_mcast_mask = 0xe0000000;
		struct sockaddr_in *sa4 = (struct sockaddr_in*)sa;
		if ((ntohl(sa4->sin_addr.s_addr) & ipv4_mcast_mask) == ipv4_mcast_mask) {
			struct ip_mreqn mr4;
			memset(&mr4, 0, sizeof(mr4));
			mr4.imr_multiaddr = sa4->sin_addr;
			mr4.imr_ifindex = req.ifr_ifindex;
			pw_net_get_ip(sa, addr, sizeof(addr), NULL, NULL);
			pw_log_info("join IPv4 group: %s iface:%d", addr, req.ifr_ifindex);
			res = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr4, sizeof(mr4));
		} else {
			struct sockaddr_in *ba4 = (struct sockaddr_in*)&ba;
			if (ba4->sin_addr.s_addr != INADDR_ANY) {
				ba4->sin_addr.s_addr = INADDR_ANY;
				do_connect = true;
			}
		}
	} else if (af == AF_INET6) {
		struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)sa;
		if (sa6->sin6_addr.s6_addr[0] == 0xff) {
			struct ipv6_mreq mr6;
			memset(&mr6, 0, sizeof(mr6));
			mr6.ipv6mr_multiaddr = sa6->sin6_addr;
			mr6.ipv6mr_interface = req.ifr_ifindex;
			pw_net_get_ip(sa, addr, sizeof(addr), NULL, NULL);
			pw_log_info("join IPv6 group: %s iface:%d", addr, req.ifr_ifindex);
			res = setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mr6, sizeof(mr6));
		} else {
			struct sockaddr_in6 *ba6 = (struct sockaddr_in6*)&ba;
			ba6->sin6_addr = in6addr_any;
		}
	} else {
		res = -EINVAL;
		goto error;
	}

	if (res < 0) {
		res = -errno;
		pw_log_error("join mcast failed: %m");
		goto error;
	}

	if (bind(fd, (struct sockaddr*)&ba, salen) < 0) {
		res = -errno;
		pw_log_error("bind() failed: %m");
		goto error;
	}
	if (do_connect) {
		if (connect(fd, (struct sockaddr*)sa, salen) < 0) {
			res = -errno;
			pw_log_error("connect() failed: %m");
			goto error;
		}
	}
	return fd;
error:
	close(fd);
	return res;
}

static void update_ts_refclk(struct impl *impl)
{
	if (!impl->ptp_mgmt_socket || impl->ptp_fd < 0)
		return;

	// Read if something is left in the socket
	int avail;
	uint8_t tmp;

	ioctl(impl->ptp_fd, FIONREAD, &avail);
	pw_log_debug("Flushing stale data: %u bytes", avail);
	while (avail-- && read(impl->ptp_fd, &tmp, 1));

	struct ptp_management_msg req;
	spa_zero(req);

	req.major_sdo_id_message_type = PTP_MESSAGE_TYPE_MANAGEMENT;
	req.ver = PTP_VERSION_1588_2008_2_1;
	req.message_length_be = htobe16(sizeof(struct ptp_management_msg));
	spa_zero(req.clock_identity);
	req.source_port_id_be = htobe16(getpid());
	req.log_message_interval = 127;
	req.sequence_id_be = htobe16(impl->ptp_seq++);
	memset(req.target_port_identity, 0xff, 8);
	req.target_port_id_be = htobe16(0xffff);
	req.starting_boundary_hops = 1;
	req.boundary_hops = 1;
	req.action = PTP_MGMT_ACTION_GET;
	req.tlv_type_be = htobe16(PTP_TLV_TYPE_MGMT);
	// sent empty TLV, only sending management_id
	req.management_message_length_be = htobe16(2);
	req.management_id_be = htobe16(PTP_MGMT_ID_PARENT_DATA_SET);

	if (write(impl->ptp_fd, &req, sizeof(req)) == -1) {
		pw_log_warn("Failed to send PTP management request: %m");
		return;
	}

	uint8_t buf[sizeof(struct ptp_management_msg) + sizeof(struct ptp_parent_data_set)];
	if (read(impl->ptp_fd, &buf, sizeof(buf)) == -1) {
		pw_log_warn("Failed to receive PTP management response: %m");
		return;
	}

	struct ptp_management_msg res = *(struct ptp_management_msg *)buf;
	struct ptp_parent_data_set parent =
		*(struct ptp_parent_data_set *)(buf + sizeof(struct ptp_management_msg));

	if ((res.ver & 0x0f) != 2) {
		pw_log_warn("PTP major version is %d, expected 2", res.ver);
		return;
	}

	if ((res.major_sdo_id_message_type & 0x0f) != PTP_MESSAGE_TYPE_MANAGEMENT) {
		pw_log_warn("PTP management returned type %x, expected management", res.major_sdo_id_message_type);
		return;
	}

	if (res.action != PTP_MGMT_ACTION_RESPONSE) {
		pw_log_warn("PTP management returned action %d, expected response", res.action);
		return;
	}

	if (be16toh(res.tlv_type_be) != PTP_TLV_TYPE_MGMT) {
		pw_log_warn("PTP management returned tlv type %d, expected management", be16toh(res.tlv_type_be));
		return;
	}

	if (be16toh(res.management_id_be) != PTP_MGMT_ID_PARENT_DATA_SET) {
		pw_log_warn("PTP management returned ID %d, expected PARENT_DATA_SET", be16toh(res.management_id_be));
		return;
	}

	uint16_t data_len = be16toh(res.management_message_length_be) - 2;
	if (data_len != sizeof(struct ptp_parent_data_set))
		pw_log_warn("Unexpected PTP GET PARENT_DATA_SET response length %u, expected %zu", data_len, sizeof(struct ptp_parent_data_set));

	uint8_t *cid = res.clock_identity;
	if (memcmp(cid, impl->clock_id, 8) != 0)
		pw_log_info(
			"Local clock ID: IEEE1588-2008:%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%d",
			cid[0],
			cid[1],
			cid[2],
			cid[3],
			cid[4],
			cid[5],
			cid[6],
			cid[7],
			0 /* domain */
	  );

	uint8_t *gmid = parent.gm_clock_id;
	if (memcmp(gmid, impl->gm_id, 8) != 0)
		pw_log_info(
			"GM ID: IEEE1588-2008:%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%d",
			gmid[0],
			gmid[1],
			gmid[2],
			gmid[3],
			gmid[4],
			gmid[5],
			gmid[6],
			gmid[7],
			0 /* domain */
	  );

	// When GM is not equal to own clock we are clocked by external master
	pw_log_debug("Synced to GM: %s", (memcmp(cid, gmid, 8) != 0) ? "true" : "false");

	memcpy(impl->clock_id, cid, 8);
	memcpy(impl->gm_id, gmid, 8);
}

static int make_sdp(struct impl *impl, struct session *sess, char *buffer, size_t buffer_size)
{
	char src_addr[64], dst_addr[64], dst_ttl[8], ptime[32];
	struct sdp_info *sdp = &sess->info;
	bool src_ip4, dst_ip4;
	bool multicast;
	const char *user_name;
	struct spa_strbuf buf;
	int res;

	if ((res = pw_net_get_ip(&impl->src_addr, src_addr, sizeof(src_addr), &src_ip4, NULL)) < 0)
		return res;

	if ((res = pw_net_get_ip(&sdp->dst_addr, dst_addr, sizeof(dst_addr), &dst_ip4, NULL)) < 0)
		return res;

	if ((user_name = pw_get_user_name()) == NULL)
		user_name = "-";

	multicast = is_multicast((struct sockaddr*)&sdp->dst_addr, sdp->dst_len);

	spa_zero(dst_ttl);
	if (multicast)
		snprintf(dst_ttl, sizeof(dst_ttl), "/%d", sdp->ttl);

	spa_strbuf_init(&buf, buffer, buffer_size);
	/* Don't add any sdp records in between this definition or change the order
	   it will break compatibility with Dante/AES67 devices. Add new records to
	   the end. */
	spa_strbuf_append(&buf,
			"v=0\n"
			"o=%s %u %u IN %s %s\n"
			"s=%s\n"
			"c=IN %s %s%s\n"
			"t=%u 0\n"
			"m=%s %u RTP/AVP %i\n",
			user_name, sdp->session_id, sdp->session_version, src_ip4 ? "IP4" : "IP6", src_addr,
			sdp->session_name,
			(multicast ? dst_ip4 : src_ip4) ? "IP4" : "IP6", multicast ? dst_addr : src_addr, dst_ttl,
			sdp->t_ntp,
			sdp->media_type, sdp->dst_port, sdp->payload);

	if (impl->extra_attrs_preamble)
		spa_strbuf_append(&buf, "%s", impl->extra_attrs_preamble);

	if (sdp->channels) {
		if (sdp->channelmap[0] != 0) {
			// Produce Audinate format channel record. It's recognized by RAVENNA
			spa_strbuf_append(&buf,
				"i=%d channels: %s\n", sdp->channels,
				sdp->channelmap);
		} else {
			spa_strbuf_append(&buf, "i=%d channels:", sdp->channels);
			for (uint i = 1; i <= sdp->channels; i++) {
				if (i > 1) spa_strbuf_append(&buf, ",");
				spa_strbuf_append(&buf, " AUX%u", i);
			}
			spa_strbuf_append(&buf, "\n");
		}
		spa_strbuf_append(&buf,
			"a=recvonly\n"
			"a=rtpmap:%i %s/%u/%u\n",
				sdp->payload, sdp->mime_type,
				sdp->rate, sdp->channels);
	} else {
		spa_strbuf_append(&buf,
			"a=rtpmap:%i %s/%u\n",
				sdp->payload, sdp->mime_type, sdp->rate);
	}

	if (is_multicast((struct sockaddr*)&sdp->dst_addr, sdp->dst_len))
		spa_strbuf_append(&buf,
			"a=source-filter: incl IN %s %s %s\n", dst_ip4 ? "IP4" : "IP6",
				dst_addr, src_addr);

	if (sdp->ssrc > 0)
		spa_strbuf_append(&buf, "a=ssrc:%u\n", sdp->ssrc);

	if (sdp->ptime > 0)
		spa_strbuf_append(&buf,
			"a=ptime:%s\n", spa_dtoa(ptime, sizeof(ptime), sdp->ptime));

	if (sdp->framecount > 0)
		spa_strbuf_append(&buf,
			"a=framecount:%u\n", sdp->framecount);

	if (sdp->ts_refclk != NULL || sess->ts_refclk_ptp) {
		// Only broadcast the GM ID when we are synced to external time source
		if (sess->ts_refclk_ptp && memcmp(impl->clock_id, impl->gm_id, 8) != 0) {
			spa_strbuf_append(&buf,
					"a=ts-refclk:ptp=IEEE1588-2008:%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X:%d\n",
					impl->gm_id[0],
					impl->gm_id[1],
					impl->gm_id[2],
					impl->gm_id[3],
					impl->gm_id[4],
					impl->gm_id[5],
					impl->gm_id[6],
					impl->gm_id[7],
					0/* domain */);
		} else if (sdp->ts_refclk != NULL) {
			spa_strbuf_append(&buf, "a=ts-refclk:%s\n", sdp->ts_refclk);
		}
		spa_strbuf_append(&buf, "a=mediaclk:direct=%u\n", sdp->ts_offset);
	} else {
		spa_strbuf_append(&buf, "a=mediaclk:sender\n");
	}

	spa_strbuf_append(&buf,
		"a=tool:PipeWire %s\n"
		"a=type:broadcast\n",
		pw_get_library_version());

	if (impl->extra_attrs_end)
		spa_strbuf_append(&buf, "%s", impl->extra_attrs_end);

	return 0;
}

static int send_sap(struct impl *impl, struct session *sess, bool bye)
{
	struct sap_header header;
	struct iovec iov[4];
	struct msghdr msg;
	struct sdp_info *sdp = &sess->info;
	int res;

	if (!sess->has_sent_sap && bye)
		return 0;

	if (impl->sap_fd == -1) {
		int fd;
		char addr[64];
		const char *str;

		if ((str = pw_properties_get(sess->props, "source.ip")) == NULL) {
			if (impl->ifname) {
				int fd = socket(impl->sap_addr.ss_family, SOCK_DGRAM, 0);
				if (fd >= 0) {
					struct ifreq req;
					spa_zero(req);
					req.ifr_addr.sa_family = impl->sap_addr.ss_family;
					snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", impl->ifname);
					res = ioctl(fd, SIOCGIFADDR, &req);
					if (res < 0)
						pw_log_warn("SIOCGIFADDR %s failed: %m", impl->ifname);
					str = inet_ntop(req.ifr_addr.sa_family,
							&((struct sockaddr_in *)&req.ifr_addr)->sin_addr,
							addr, sizeof(addr));
					if (str == NULL) {
						pw_log_warn("can't parse interface ip: %m");
					} else {
						pw_log_info("interface %s IP: %s", impl->ifname, str);
					}
					close(fd);
				}
			}
			if (str == NULL)
				str = impl->sap_addr.ss_family == AF_INET ?
					DEFAULT_SOURCE_IP : DEFAULT_SOURCE_IP6;
		}
		if ((res = pw_net_parse_address(str, 0, &impl->src_addr, &impl->src_len)) < 0) {
			pw_log_error("invalid source.ip %s: %s", str, spa_strerror(res));
			return res;
		}
		if ((fd = make_send_socket(&impl->src_addr, impl->src_len,
						&impl->sap_addr, impl->sap_len,
						impl->mcast_loop, impl->ttl)) < 0)
			return fd;

		impl->sap_fd = fd;
	}

        /* For the first session, we might not yet have an SDP because the
         * socket needs to be open for us to get the interface address (which
         * happens above. So let's create the SDP now, if needed. */
        if (!sess->has_sdp) {
		res = make_sdp(impl, sess, sess->sdp, sizeof(sess->sdp));
		if (res != 0) {
			pw_log_error("Failed to create SDP: %s", spa_strerror(res));
			return res;
		}
		sess->has_sdp = true;
	}

	spa_zero(header);
	header.v = 1;
	header.t = bye;
	header.msg_id_hash = sdp->hash;

	iov[0].iov_base = &header;
	iov[0].iov_len = sizeof(header);

	if (impl->src_addr.ss_family == AF_INET) {
		iov[1].iov_base = &((struct sockaddr_in*) &impl->src_addr)->sin_addr;
		iov[1].iov_len = 4U;
	} else {
		iov[1].iov_base = &((struct sockaddr_in6*) &impl->src_addr)->sin6_addr;
		iov[1].iov_len = 16U;
		header.a = 1;
	}
	iov[2].iov_base = SAP_MIME_TYPE;
	iov[2].iov_len = sizeof(SAP_MIME_TYPE);
	iov[3].iov_base = sess->sdp;
	iov[3].iov_len = strlen(sess->sdp);

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = iov;
	msg.msg_iovlen = 4;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	pw_log_debug("sending SAP for %u %s", sess->node->id, sess->sdp);

	res = sendmsg(impl->sap_fd, &msg, MSG_NOSIGNAL);
	if (res < 0)
		res = -errno;
	else
		sess->has_sent_sap = true;

	return res;
}

static void on_timer_event(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	struct session *sess, *tmp;
	uint64_t timestamp, interval;

	timestamp = get_time_nsec(impl);
	interval = impl->cleanup_interval * SPA_NSEC_PER_SEC;
	update_ts_refclk(impl);

	spa_list_for_each_safe(sess, tmp, &impl->sessions, link) {
		if (sess->announce) {
			send_sap(impl, sess, 0);
		} else {
			if (sess->timestamp + interval < timestamp) {
				pw_log_info("session %s timeout",
						sess->info.session_name);
				session_free(sess);
			}

		}
	}
}

static struct session *session_find(struct impl *impl, const struct sdp_info *info)
{
	struct session *sess;
	spa_list_for_each(sess, &impl->sessions, link) {
		if (info->hash == sess->info.hash &&
		    spa_streq(info->origin, sess->info.origin))
			return sess;
	}
	return NULL;
}

static inline void replace_str(char **dst, const char *val)
{
	free(*dst);
	*dst = val ? strdup(val) : NULL;
}

static struct session *session_new_announce(struct impl *impl, struct node *node,
		struct pw_properties *props)
{
	char buffer[MAX_SDP];
	struct session *sess = NULL;
	struct sdp_info *sdp;
	const char *str;
	uint32_t port;
	int res;

	sess = node->session;
	if (sess == NULL) {
		if (impl->n_sessions >= impl->max_sessions) {
			pw_log_warn("too many sessions (%u >= %u)", impl->n_sessions, impl->max_sessions);
			errno = EMFILE;
			return NULL;
		}
		sess = calloc(1, sizeof(struct session));
		if (sess == NULL)
			return NULL;

		pw_log_info("created new session for node:%u", node->id);
		node->session = sess;
		sess->node = node;
		sess->impl = impl;
		sess->announce = true;
		spa_list_append(&impl->sessions, &sess->link);
		impl->n_sessions++;
	}

	sdp = &sess->info;

	pw_properties_free(sess->props);
	sess->props = props;

	if ((str = pw_properties_get(props, "sess.name")) == NULL)
		str = pw_get_host_name();
	replace_str(&sdp->session_name, str);

	if ((str = pw_properties_get(props, "rtp.destination.port")) == NULL)
		goto error_free;
	if (!spa_atou32(str, &port, 0))
		goto error_free;
	sdp->dst_port = port;

	if ((str = pw_properties_get(props, "rtp.destination.ip")) == NULL)
		goto error_free;
	if ((res = pw_net_parse_address(str, sdp->dst_port, &sdp->dst_addr, &sdp->dst_len)) < 0) {
		pw_log_error("invalid destination.ip %s: %s", str, spa_strerror(res));
		goto error_free;
	}
	sdp->ttl = pw_properties_get_int32(props, "rtp.ttl", DEFAULT_TTL);
	sdp->payload = pw_properties_get_int32(props, "rtp.payload", 127);

	if ((str = pw_properties_get(props, "rtp.ptime")) != NULL)
		if (!spa_atof(str, &sdp->ptime))
			sdp->ptime = 0.0;

	if ((str = pw_properties_get(props, "rtp.framecount")) != NULL)
		if (!spa_atou32(str, &sdp->framecount, 0))
			sdp->framecount = 0;

	str = pw_properties_get(props, "rtp.media");
	replace_str(&sdp->media_type, str);
	str = pw_properties_get(props, "rtp.mime");
	replace_str(&sdp->mime_type, str);

	if ((str = pw_properties_get(props, "rtp.rate")) != NULL)
		sdp->rate = atoi(str);
	if ((str = pw_properties_get(props, "rtp.channels")) != NULL)
		sdp->channels = atoi(str);
	if ((str = pw_properties_get(props, "rtp.ssrc")) != NULL)
		sdp->ssrc = atoi(str);
	else
		sdp->ssrc = 0;
	if ((str = pw_properties_get(props, "rtp.ts-offset")) != NULL)
		sdp->ts_offset = atoi(str);
	str = pw_properties_get(props, "rtp.ts-refclk");
	replace_str(&sdp->ts_refclk, str);

	sess->ts_refclk_ptp = pw_properties_get_bool(props, "rtp.fetch-ts-refclk", false);
	if ((str = pw_properties_get(props, PW_KEY_NODE_CHANNELNAMES)) != NULL) {
		struct spa_strbuf buf;
		struct spa_json it[1];
		char v[256];
		int count = 0;

		spa_strbuf_init(&buf, sdp->channelmap, sizeof(sdp->channelmap));

		if (spa_json_begin_array_relax(&it[0], str, strlen(str)) > 0) {
			while (spa_json_get_string(&it[0], v, sizeof(v)) > 0)
				spa_strbuf_append(&buf, "%s%s", count++ > 0 ? ", " : "", v);
		}
	}

	/* see if we can make an SDP, will fail for the first session because we
	 * haven't got the SAP socket open yet */
	res = make_sdp(impl, sess, buffer, sizeof(buffer));

	/* we had no sdp or something changed */
	if (res == 0 && (!sess->has_sdp || strcmp(buffer, sess->sdp) != 0)) {
		/*  send bye on the old session */
		send_sap(impl, sess, 1);

		/* update the version and hash */
		sdp->hash = pw_rand32();
		if ((str = pw_properties_get(props, "sess.id")) != NULL) {
			if (!spa_atou32(str, &sdp->session_id, 10)) {
				pw_log_error("Invalid session id: %s (must be a uint32)", str);
				goto error_free;
			}
			sdp->t_ntp = pw_properties_get_uint32(props, "rtp.ntp",
					(uint32_t) time(NULL) + 2208988800U + impl->n_sessions);
		} else {
			sdp->session_id = (uint32_t) time(NULL) + 2208988800U + impl->n_sessions;
			sdp->t_ntp = pw_properties_get_uint32(props, "rtp.ntp", sdp->session_id);
		}
		if ((str = pw_properties_get(props, "sess.version")) != NULL) {
			if (!spa_atou32(str, &sdp->session_version, 10)) {
				pw_log_error("Invalid session version: %s (must be a uint32)", str);
				goto error_free;
			}
		} else {
			sdp->session_version = sdp->t_ntp;
		}

		/* make an updated SDP for sending, this should not actually fail */
		res = make_sdp(impl, sess, sess->sdp, sizeof(sess->sdp));

		if (res == 0)
			sess->has_sdp = true;
		else
			pw_log_error("Failed to create SDP: %s", spa_strerror(res));
	}

	send_sap(impl, sess, 0);

	return sess;

error_free:
	pw_log_warn("invalid session props");
	session_free(sess);
	return NULL;
}

static void session_module_destroy(void *d)
{
	struct session *sess = d;
	spa_hook_remove(&sess->module_listener);
	sess->module = NULL;
	session_free(sess);
}

static const struct pw_impl_module_events session_module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = session_module_destroy,
};

static int session_load_source(struct session *session, struct pw_properties *props)
{
	struct impl *impl = session->impl;
	struct pw_context *context = pw_impl_module_get_context(impl->module);
	FILE *f = NULL;
	char *args = NULL;
	size_t size;
	const char *str, *media;
	int res;

	if ((f = open_memstream(&args, &size)) == NULL) {
		res = -errno;
		pw_log_error("Can't open memstream: %m");
		goto done;
	}
	fprintf(f, "{");

	if ((str = pw_properties_get(props, "rtp.destination.ip")) != NULL)
		fprintf(f, "\"source.ip\" = \"%s\", ", str);
	if ((str = pw_properties_get(props, "rtp.destination.port")) != NULL)
		fprintf(f, "\"source.port\" = %s, ", str);
	if ((str = pw_properties_get(props, "rtp.session")) != NULL)
		fprintf(f, "\"sess.name\" = \"%s\", ", str);

	/* Use an interface if explicitly specified, else use the SAP interface if that was specified */
	if ((str = pw_properties_get(props, "local.ifname")) != NULL || (str = impl->ifname) != NULL) {
		fprintf(f, "\"local.ifname\" = \"%s\", ", str);
	}

	if ((media = pw_properties_get(props, "sess.media")) == NULL)
		media = "audio";

	if ((str = pw_properties_get(props, "cleanup.sec")) != NULL) {
		fprintf(f, "\"cleanup.sec\" = \"%s\", ", str);
	}

	if (spa_streq(media, "audio")) {
		const char *mime;
		const struct format_info *format_info;

		if ((mime = pw_properties_get(props, "rtp.mime")) == NULL) {
			pw_log_error("missing rtp.mime property");
			res = -EINVAL;
			goto done;
		}
		format_info = find_audio_format_info(mime);
		if (format_info == NULL) {
			pw_log_error("unknown rtp.mime type %s", mime);
			res = -ENOTSUP;
			goto done;
		}
		fprintf(f, "\"sess.media\" = \"%s\", ", format_info->media_type);
		if (format_info->format_str != NULL) {
			pw_properties_set(props, "audio.format", format_info->format_str);
			if ((str = pw_properties_get(props, "rtp.rate")) != NULL)
				pw_properties_set(props, "audio.rate", str);
			if ((str = pw_properties_get(props, "rtp.channels")) != NULL)
				pw_properties_set(props, "audio.channels", str);
		}
		if ((str = pw_properties_get(props, "rtp.ssrc")) != NULL)
			fprintf(f, "\"rtp.receiver-ssrc\" = \"%s\", ", str);
	} else {
		pw_log_error("Unhandled media %s", media);
		res = -EINVAL;
		goto done;
	}
	if ((str = pw_properties_get(props, "rtp.ts-offset")) != NULL)
		fprintf(f, "\"sess.ts-offset\" = %s, ", str);

	fprintf(f, " stream.props = {");
	pw_properties_serialize_dict(f, &props->dict, 0);
	fprintf(f, " }");
	fprintf(f, "}");
        fclose(f);
	f = NULL;

	pw_log_info("loading new RTP source");
	session->module = pw_context_load_module(context,
				"libpipewire-module-rtp-source",
				args, NULL);

	if (session->module == NULL) {
		res = -errno;
		pw_log_error("Can't load module: %m");
		goto done;
	}

	pw_impl_module_add_listener(session->module,
			&session->module_listener,
			&session_module_events, session);

	res = 0;
done:
	if (f != NULL)
		fclose(f);
	free(args);
	return res;
}

struct match_info {
	struct impl *impl;
	struct session *session;
	struct node *node;
	struct pw_properties *props;
	bool matched;
};

static int rule_matched(void *data, const char *location, const char *action,
                        const char *str, size_t len)
{
	struct match_info *i = data;
	int res = 0;

	i->matched = true;
	if (i->session && spa_streq(action, "create-stream")) {
		pw_properties_update_string(i->props, str, len);

		session_load_source(i->session, i->props);
	}
	else if (i->node && spa_streq(action, "announce-stream")) {
		struct pw_properties *props;

		if ((props = pw_properties_new_dict(i->node->info->props)) == NULL)
			return -errno;

		pw_properties_update_string(props, str, len);

		session_new_announce(i->impl, i->node, props);
	}
	return res;
}

static struct session *session_new(struct impl *impl, struct sdp_info *info)
{
	struct session *session;
	struct pw_properties *props;
	const char *str;
	char dst_addr[64], tmp[64];

	if (impl->n_sessions >= impl->max_sessions) {
		pw_log_warn("too many sessions (%u >= %u)", impl->n_sessions, impl->max_sessions);
		errno = EMFILE;
		return NULL;
	}

	session = calloc(1, sizeof(struct session));
	if (session == NULL)
		return NULL;

	session->announce = false;
	session->info = *info;
	spa_zero(*info);
	info = &session->info;

	props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		goto error;

	session->impl = impl;
	spa_list_append(&impl->sessions, &session->link);
	impl->n_sessions++;

	pw_properties_set(props, "rtp.origin", info->origin);
	if (info->session_name != NULL) {
		pw_properties_set(props, "rtp.session", info->session_name);
		pw_properties_setf(props, PW_KEY_MEDIA_NAME, "RTP Stream (%s)",
				info->session_name);
		pw_properties_setf(props, PW_KEY_NODE_NAME, "%s",
				info->session_name);
	} else {
		pw_properties_set(props, PW_KEY_MEDIA_NAME, "RTP Stream");
	}

	pw_net_get_ip(&info->dst_addr, dst_addr, sizeof(dst_addr), NULL, NULL);
	pw_properties_setf(props, "rtp.destination.ip", "%s", dst_addr);
	pw_properties_setf(props, "rtp.destination.port", "%u", info->dst_port);
	pw_properties_setf(props, "rtp.payload", "%u", info->payload);
	pw_properties_set(props, "rtp.ptime", spa_dtoa(tmp, sizeof(tmp), info->ptime));
	pw_properties_setf(props, "rtp.framecount", "%u", info->framecount);
	pw_properties_setf(props, "rtp.media", "%s", info->media_type);
	pw_properties_setf(props, "rtp.mime", "%s", info->mime_type);
	pw_properties_setf(props, "rtp.rate", "%u", info->rate);
	pw_properties_setf(props, "rtp.channels", "%u", info->channels);

	pw_properties_setf(props, "rtp.ts-offset", "%u", info->ts_offset);
	pw_properties_set(props, "rtp.ts-refclk", info->ts_refclk);

	if (info->ssrc > 0)
		pw_properties_setf(props, "rtp.ssrc", "%u", info->ssrc);

	if (info->channelmap[0])
		pw_properties_set(props, PW_KEY_NODE_CHANNELNAMES, info->channelmap);

	if ((str = pw_properties_get(impl->props, "stream.rules")) == NULL)
		str = DEFAULT_CREATE_RULES;
	if (str != NULL) {
		struct match_info minfo = {
			.impl = impl,
			.session = session,
			.props = props,
		};
		pw_conf_match_rules(str, strlen(str), NAME, &props->dict,
				rule_matched, &minfo);
	}
	session->props = props;
	session_touch(session);

	return NULL;
error:
	session_free(session);
	return NULL;
}

static int parse_sdp_c(struct impl *impl, char *c, struct sdp_info *info)
{
	int res;

	c[strcspn(c, "/")] = 0;
	if (spa_strstartswith(c, "c=IN IP4 ")) {
		struct sockaddr_in *sa = (struct sockaddr_in*) &info->dst_addr;

		c += strlen("c=IN IP4 ");
		if (inet_pton(AF_INET, c, &sa->sin_addr) <= 0) {
			res = -errno;
			pw_log_warn("inet_pton(%s) failed: %m", c);
			goto error;
		}
		sa->sin_family = AF_INET;
		info->dst_len = sizeof(struct sockaddr_in);
	}
	else if (spa_strstartswith(c, "c=IN IP6 ")) {
		struct sockaddr_in6 *sa = (struct sockaddr_in6*) &info->dst_addr;

		c += strlen("c=IN IP6 ");
		if (inet_pton(AF_INET6, c, &sa->sin6_addr) <= 0) {
			res = -errno;
			pw_log_warn("inet_pton(%s) failed: %m", c);
			goto error;
		}

		sa->sin6_family = AF_INET6;
		info->dst_len = sizeof(struct sockaddr_in6);
	} else
		return -EINVAL;


	res= 0;
error:
	return res;
}

static int parse_sdp_m(struct impl *impl, char *c, struct sdp_info *info)
{
	int port, payload;
	char media_type[12];

	if (!spa_strstartswith(c, "m="))
		return -EINVAL;

	c += strlen("m=");
	if (sscanf(c, "%11s %i RTP/AVP %i", media_type, &port, &payload) != 3)
		return -EINVAL;

	if (port <= 0 || port > 0xFFFF)
		return -EINVAL;

	if (payload < 0 || payload > 127)
		return -EINVAL;

	info->media_type = strdup(media_type);
	info->dst_port = (uint16_t) port;
	info->payload = (uint8_t) payload;

	return 0;
}

/* some AES67 devices have channelmap encoded in i=*
 * if `i` record is found, it matches the template
 * and channel count matches, name the channels respectively
 * `i=2 channels: 01, 08` is the format
 * This is Audinate format. TODO: parse RAVENNA `i=CH1,CH2,CH3` format */
static int parse_sdp_i(struct impl *impl, char *c, struct sdp_info *info)
{
	if (!strstr(c, " channels: ")) {
		return 0;
	}

	c += strlen("i=");
	c[strcspn(c, " ")] = '\0';

	uint32_t channels;
	if (sscanf(c, "%u", &channels) != 1 || channels <= 0 || channels > SPA_AUDIO_MAX_CHANNELS)
		return 0;

	c += strcspn(c, "\0");
	c += strlen(" channels: ");

	strncpy(info->channelmap, c, sizeof(info->channelmap) - 1);

	return 0;
}

static int parse_sdp_a_rtpmap(struct impl *impl, char *c, struct sdp_info *info)
{
	int payload, len, rate, channels;

	if (!spa_strstartswith(c, "a=rtpmap:"))
		return 0;

	c += strlen("a=rtpmap:");

	if (sscanf(c, "%i %n", &payload, &len) != 1)
		return -EINVAL;

	if (payload < 0 || payload > 127)
		return -EINVAL;

	if (payload != info->payload)
		return 0;

	c += len;
	c[strcspn(c, "/")] = 0;
	info->mime_type = strdup(c);
	c += strlen(c) + 1;

	if (sscanf(c, "%u/%u", &rate, &channels) == 2) {
		info->channels = channels;
		info->rate = rate;
	} else if (sscanf(c, "%u", &rate) == 1) {
		info->rate = rate;
		info->channels = 1;
	} else
		return -EINVAL;

	pw_log_debug("a=rtpmap: rate: %d, ch: %d", info->rate, info->channels);

	return 0;
}

static int parse_sdp_a_ssrc(struct impl *impl, char *c, struct sdp_info *info)
{
	if (!spa_strstartswith(c, "a=ssrc:"))
		return 0;

	c += strlen("a=ssrc:");
	if (!spa_atou32(c, &info->ssrc, 10))
		return -EINVAL;
	return 0;
}

static int parse_sdp_a_ptime(struct impl *impl, char *c, struct sdp_info *info)
{
	if (!spa_strstartswith(c, "a=ptime:"))
		return 0;

	c += strlen("a=ptime:");
	if (!spa_atof(c, &info->ptime))
		return -EINVAL;
	return 0;
}

static int parse_sdp_a_mediaclk(struct impl *impl, char *c, struct sdp_info *info)
{
	if (!spa_strstartswith(c, "a=mediaclk:"))
		return 0;

	c += strlen("a=mediaclk:");

	if (spa_strstartswith(c, "direct=")) {
		int offset;
		c += strlen("direct=");
		if (sscanf(c, "%i", &offset) != 1)
			return -EINVAL;
		info->ts_offset = offset;
	} else if (spa_strstartswith(c, "sender")) {
		info->ts_offset = 0;
	}
	return 0;
}

static int parse_sdp_a_ts_refclk(struct impl *impl, char *c, struct sdp_info *info)
{
	if (!spa_strstartswith(c, "a=ts-refclk:"))
		return 0;

	c += strlen("a=ts-refclk:");
	info->ts_refclk = strdup(c);
	return 0;
}

static int parse_sdp(struct impl *impl, char *sdp, struct sdp_info *info)
{
	char *s = sdp;
	int count = 0, res = 0;
	size_t l;

	while (*s) {
		if ((l = strcspn(s, "\r\n")) < 2)
			goto too_short;

		s[l] = 0;
		pw_log_debug("SDP line: %d: %s", count, s);

		if (count++ == 0 && strcmp(s, "v=0") != 0)
			goto invalid_version;

		if (spa_strstartswith(s, "o="))
			info->origin = strdup(&s[2]);
		else if (spa_strstartswith(s, "s="))
			info->session_name = strdup(&s[2]);
		else if (spa_strstartswith(s, "c="))
			res = parse_sdp_c(impl, s, info);
		else if (spa_strstartswith(s, "m="))
			res = parse_sdp_m(impl, s, info);
		else if (spa_strstartswith(s, "a=rtpmap:"))
			res = parse_sdp_a_rtpmap(impl, s, info);
		else if (spa_strstartswith(s, "a=ssrc:"))
			res = parse_sdp_a_ssrc(impl, s, info);
		else if (spa_strstartswith(s, "a=ptime:"))
			res = parse_sdp_a_ptime(impl, s, info);
		else if (spa_strstartswith(s, "a=mediaclk:"))
			res = parse_sdp_a_mediaclk(impl, s, info);
		else if (spa_strstartswith(s, "a=ts-refclk:"))
			res = parse_sdp_a_ts_refclk(impl, s, info);
		else if (spa_strstartswith(s, "i="))
			res = parse_sdp_i(impl, s, info);

		if (res < 0)
			goto error;
		s += l + 1;
		while (isspace(*s))
			s++;
        }
	if (((struct sockaddr*) &info->dst_addr)->sa_family == AF_INET)
		((struct sockaddr_in*) &info->dst_addr)->sin_port = htons(info->dst_port);
	else
		((struct sockaddr_in6*) &info->dst_addr)->sin6_port = htons(info->dst_port);

	return 0;
too_short:
	pw_log_warn("SDP: line starting with `%.6s...' too short", s);
	return -EINVAL;
invalid_version:
	pw_log_warn("SDP: invalid first version line `%*s'", (int)l, s);
	return -EINVAL;
error:
	pw_log_warn("SDP: error: %s", spa_strerror(res));
	return res;
}

static int parse_sap(struct impl *impl, void *data, size_t len)
{
	struct sap_header *header;
	char *mime, *sdp;
	struct sdp_info info;
	struct session *sess;
	int res;
	size_t offs;
	bool bye;

	if (len < 8)
		return -EINVAL;

	header = (struct sap_header*) data;
	if (header->v != 1)
		return -EINVAL;

	if (header->e)
		return -ENOTSUP;
	if (header->c)
		return -ENOTSUP;

	offs = header->a ? 20 : 8;
	offs += header->auth_len * 4;
	if (len <= offs)
		return -EINVAL;

	mime = SPA_PTROFF(data, offs, char);
	if (spa_strstartswith(mime, "v=0")) {
		sdp = mime;
		mime = SAP_MIME_TYPE;
	} else if (spa_streq(mime, SAP_MIME_TYPE))
		sdp = SPA_PTROFF(mime, strlen(mime)+1, char);
	else
		return -EINVAL;

	pw_log_debug("got SAP: %s %s", mime, sdp);

	spa_zero(info);
	if ((res = parse_sdp(impl, sdp, &info)) < 0)
		return res;

	bye = header->t;

	sess = session_find(impl, &info);
	if (sess == NULL) {
		if (!bye)
			session_new(impl, &info);
	} else {
		if (bye)
			session_free(sess);
		else
			session_touch(sess);
	}
	clear_sdp_info(&info);
	return res;
}

static void
on_sap_io(void *data, int fd, uint32_t mask)
{
	struct impl *impl = data;
	int res;

	if (mask & SPA_IO_IN) {
		uint8_t buffer[MAX_SDP];
		ssize_t len;

		if ((len = recv(fd, buffer, sizeof(buffer), 0)) < 0) {
			pw_log_warn("recv error: %m");
			return;
		}
		if ((size_t)len >= sizeof(buffer))
			return;

		buffer[len] = 0;
		if ((res = parse_sap(impl, buffer, len)) < 0)
			pw_log_warn("error parsing SAP: %s", spa_strerror(res));
	}
}

static int start_sap(struct impl *impl)
{
	int fd = -1, res;
	struct timespec value, interval;
	char addr[128] = "invalid";

	pw_log_info("starting SAP timer");
	impl->timer = pw_loop_add_timer(impl->loop, on_timer_event, impl);
	if (impl->timer == NULL) {
		res = -errno;
		pw_log_error("can't create timer source: %m");
		goto error;
	}
	value.tv_sec = 0;
	value.tv_nsec = 1;
	interval.tv_sec = SAP_INTERVAL_SEC;
	interval.tv_nsec = 0;
	pw_loop_update_timer(impl->loop, impl->timer, &value, &interval, false);

	if ((fd = make_recv_socket(&impl->sap_addr, impl->sap_len, impl->ifname)) < 0)
		return fd;

	pw_net_get_ip(&impl->sap_addr, addr, sizeof(addr), NULL, NULL);
	pw_log_info("starting SAP listener on %s", addr);
	impl->sap_source = pw_loop_add_io(impl->loop, fd,
				SPA_IO_IN, true, on_sap_io, impl);
	if (impl->sap_source == NULL) {
		res = -errno;
		goto error;
	}

	return 0;
error:
	if (fd > 0)
		close(fd);
	return res;
}

static void node_event_info(void *data, const struct pw_node_info *info)
{
	struct node *n = data;
	struct impl *impl = n->impl;
	const char *str;

	if (info == NULL)
		return;

	// We only really want to do anything here if properties are updated,
	// or if we don't have a session for this node already
	if (!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) && n->session != NULL)
		return;

	n->info = pw_node_info_merge(n->info, info, true);
	if (n->info == NULL)
		return;

	pw_log_debug("node %d changed", n->id);

	if ((str = pw_properties_get(impl->props, "stream.rules")) == NULL)
		str = DEFAULT_ANNOUNCE_RULES;
	if (str != NULL) {
		struct match_info minfo = {
			.impl = impl,
			.node = n,
		};
		pw_conf_match_rules(str, strlen(str), NAME, n->info->props,
				rule_matched, &minfo);
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
};

static void
proxy_removed(void *data)
{
	struct node *n = data;
	pw_log_debug("node %d removed", n->id);
	pw_proxy_destroy(n->proxy);
}

static void
proxy_destroy(void *data)
{
	struct node *n = data;
	pw_log_debug("node %d destroy", n->id);
	spa_hook_remove(&n->node_listener);
	spa_hook_remove(&n->proxy_listener);
	n->proxy = NULL;
	if (n->session != NULL) {
		session_free(n->session);
		n->session = NULL;
	}
	if (n->info) {
		pw_node_info_free(n->info);
		n->info = NULL;
	}
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy,
};

static void registry_event_global(void *data, uint32_t id,
			uint32_t permissions, const char *type, uint32_t version,
			const struct spa_dict *props)
{
	struct impl *impl = data;
	struct pw_proxy *proxy;
	struct node *node;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node))
		return;

	proxy = pw_registry_bind(impl->registry, id, type, PW_VERSION_NODE, sizeof(struct node));
	if (proxy == NULL)
		return;

	node = pw_proxy_get_user_data(proxy);
	node->impl = impl;
	node->id = id;
	node->proxy = proxy;

	pw_proxy_add_object_listener(proxy,
			&node->node_listener, &node_events, node);
        pw_proxy_add_listener(proxy,
                        &node->proxy_listener, &proxy_events, node);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
};

static void core_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	struct session *sess;

	spa_list_consume(sess, &impl->sessions, link)
		session_free(sess);

	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->registry);
		impl->registry = NULL;
	}
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->timer)
		pw_loop_destroy_source(impl->loop, impl->timer);
	if (impl->sap_source)
		pw_loop_destroy_source(impl->loop, impl->sap_source);

	if (impl->sap_fd != -1)
		close(impl->sap_fd);
	if (impl->ptp_fd != -1)
		close(impl->ptp_fd);

	pw_properties_free(impl->props);

	free(impl->extra_attrs_preamble);
	free(impl->extra_attrs_end);

	free(impl->ptp_mgmt_socket);
	free(impl->ifname);
	free(impl);
}

static void module_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void on_core_error(void *d, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = d;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props = NULL;
	uint32_t port;
	const char *str;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->sap_fd = -1;
	spa_list_init(&impl->sessions);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto out;
	}
	impl->props = props;

	impl->module = module;
	impl->loop = pw_context_get_main_loop(context);

	str = pw_properties_get(props, "local.ifname");
	impl->ifname = str ? strdup(str) : NULL;

	str = pw_properties_get(props, "ptp.management-socket");
	impl->ptp_mgmt_socket = str ? strdup(str) : NULL;

	// TODO: support UDP management access as well
	if (impl->ptp_mgmt_socket)
		impl->ptp_fd = make_unix_socket(impl->ptp_mgmt_socket);

	if ((str = pw_properties_get(props, "sap.ip")) == NULL)
		str = DEFAULT_SAP_IP;
	port = pw_properties_get_uint32(props, "sap.port", DEFAULT_SAP_PORT);
	if ((res = pw_net_parse_address(str, port, &impl->sap_addr, &impl->sap_len)) < 0) {
		pw_log_error("invalid sap.ip %s: %s", str, spa_strerror(res));
		goto out;
	}
	impl->cleanup_interval = pw_properties_get_uint32(impl->props,
			"sap.cleanup.sec", DEFAULT_CLEANUP_SEC);

	impl->ttl = pw_properties_get_uint32(props, "net.ttl", DEFAULT_TTL);
	impl->mcast_loop = pw_properties_get_bool(props, "net.loop", DEFAULT_LOOP);
	impl->max_sessions = pw_properties_get_uint32(props, "sap.max-sessions", DEFAULT_MAX_SESSIONS);

	impl->extra_attrs_preamble = NULL;
	impl->extra_attrs_end = NULL;
	char buffer[MAX_SDP];
	struct spa_strbuf buf;
	if ((str = pw_properties_get(props, "sap.preamble-extra")) != NULL) {
		spa_strbuf_init(&buf, buffer, sizeof(buffer));
		struct spa_json it[1];
		char line[256];

		if (spa_json_begin_array_relax(&it[0], str, strlen(str)) > 0) {
			while (spa_json_get_string(&it[0], line, sizeof(line)) > 0)
				spa_strbuf_append(&buf, "%s\n", line);
		}
		impl->extra_attrs_preamble = strdup(buffer);
	}
	if ((str = pw_properties_get(props, "sap.end-extra")) != NULL) {
		spa_strbuf_init(&buf, buffer, sizeof(buffer));
		struct spa_json it[1];
		char line[256];

		if (spa_json_begin_array_relax(&it[0], str, strlen(str)) > 0) {
			while (spa_json_get_string(&it[0], line, sizeof(line)) > 0)
				spa_strbuf_append(&buf, "%s\n", line);
		}
		impl->extra_attrs_end = strdup(buffer);
	}

	impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto out;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	if ((res = start_sap(impl)) < 0)
		goto out;

	impl->registry = pw_core_get_registry(impl->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(impl->registry, &impl->registry_listener,
			&registry_events, impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_info));

	pw_log_info("Successfully loaded module-rtp-sap");

	return 0;
out:
	impl_destroy(impl);
	return res;
}
