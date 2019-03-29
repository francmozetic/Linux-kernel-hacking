/**
 * @file: monevent1.c
 * @author: Aleksander Mozetic
 * @date: 28 February 2019
 * @version: 1.2.2.0
 * @copyright: 2019 IndigoSoft
 * @brief: A userspace application for wireless interface scanning.
 *
 * Resources:
 * https://git.kernel.org/pub/scm/linux/kernel/git/jberg/iw.git
 * https://stackoverflow.com/questions/18062268/using-nl80211-h-to-scan-access-points
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>    /* isprint */
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#include <sys/uio.h>
#include <net/if.h>

#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/nl80211.h>

#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

#define NETLINK_EXT_ACK			11

int *time_numbers ()
{
  const struct tm *tm_ptr;
  time_t now;
  int *value;

  now = time(0);
  tm_ptr = localtime(&now);

  value = (int *)malloc(6 * sizeof(int));

  value[0] = 1900 + tm_ptr->tm_year;
  value[1] = 1 + tm_ptr->tm_mon;
  value[2] = tm_ptr->tm_mday;
  value[3] = tm_ptr->tm_hour;
  value[4] = tm_ptr->tm_min;
  value[5] = tm_ptr->tm_sec;

  return value;
}

void mac_addr_n2a(char *mac_addr, unsigned char *arg) {
    // From http://git.kernel.org/cgit/linux/kernel/git/jberg/iw.git/tree/util.c.
    int i, l;

    l = 0;
    for (i = 0; i < 6; i++) {
        if (i == 0) {
            sprintf(mac_addr+l, "%02x", arg[i]);
            l += 2;
        } else {
            sprintf(mac_addr+l, ":%02x", arg[i]);
            l += 3;
        }
    }
}

struct trigger_results {
    int done;
    int aborted;
};

// For family_handler() and nl_get_multicast_id().
struct handler_args {
    const char *group;
    int id;
};

static int error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err, void *arg) {
	// Callback for errors.
    printf("error_handler() called.\n");
    int *ret = arg;
    *ret = err->error;
    return NL_STOP;
}

static int finish_handler(struct nl_msg *msg, void *arg) {
    // Callback for NL_CB_FINISH.
    int *ret = arg;
    *ret = 0;
    return NL_SKIP;
}

static int ack_handler(struct nl_msg *msg, void *arg) {
	// Callback for NL_CB_ACK.
	int *ret = arg;
    *ret = 0;
    return NL_STOP;
}

static int no_seq_check(struct nl_msg *msg, void *arg) {
	// Callback for NL_CB_SEQ_CHECK.
	return NL_OK;
}

static int family_handler(struct nl_msg *msg, void *arg) {
	// Callback for NL_CB_VALID within nl_get_multicast_id().
	struct handler_args *grp = arg;
	struct nlattr *tb[CTRL_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    struct nlattr *mcgrp;
    int rem_mcgrp;

    nla_parse(tb, CTRL_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);

    if (!tb[CTRL_ATTR_MCAST_GROUPS]) return NL_SKIP;

    nla_for_each_nested(mcgrp, tb[CTRL_ATTR_MCAST_GROUPS], rem_mcgrp) {  // This is a loop.
    	struct nlattr *tb_mcgrp[CTRL_ATTR_MCAST_GRP_MAX + 1];

    	/* Create attribute index based on a stream of attributes.
         * Iterates over the stream of attributes and stores a pointer to each attribute
         * in the index array using the attribute type as index to the array.
         */
        nla_parse(tb_mcgrp, CTRL_ATTR_MCAST_GRP_MAX, nla_data(mcgrp), nla_len(mcgrp), NULL);

        if (!tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME] || !tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID]) continue;
        if (strncmp(nla_data(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME]), grp->group, nla_len(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME]))) {
        	continue;
        }

        grp->id = nla_get_u32(tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID]);
        break;
    }

    return NL_SKIP;
}

int nl_get_multicast_id(struct nl_sock *sock, const char *family, const char *group) {
	struct nl_msg *msg;
    struct nl_cb *cb;
    int ret, ctrlid;
    struct handler_args grp = {
    		.group = group,
			.id = -ENOENT,
    };

    msg = nlmsg_alloc();
    if (!msg) return -ENOMEM;

    cb = nl_cb_alloc(NL_CB_DEFAULT);
    if (!cb) {
        ret = -ENOMEM;
        goto out_fail_cb;
    }

    ctrlid = genl_ctrl_resolve(sock, "nlctrl");
    genlmsg_put(msg, 0, 0, ctrlid, 0, 0, CTRL_CMD_GETFAMILY, 0);

    ret = -ENOBUFS;
    NLA_PUT_STRING(msg, CTRL_ATTR_FAMILY_NAME, family);

    ret = nl_send_auto_complete(sock, msg);
    if (ret < 0) goto out;

    ret = 1;
    nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &ret);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &ret);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, family_handler, &grp);

    /* Repeatedly calls nl_recv() or the respective replacement if provided
     * by the application and parses the received data as netlink messages.
     */
    while (ret > 0) nl_recvmsgs(sock, cb);
    if (ret == 0) ret = grp.id;

nla_put_failure:
out:
	nl_cb_put(cb);
out_fail_cb:
	nlmsg_free(msg);
	return ret;
}

static int callback_trigger(struct nl_msg *msg, void *arg) {
    // Called by the kernel when the scan is done or has been aborted.
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    struct trigger_results *results = arg;

    if (gnlh->cmd == NL80211_CMD_SCAN_ABORTED)
    {
    	printf("Got NL80211_CMD_SCAN_ABORTED.\n");
        results->done = 1;
        results->aborted = 1;
    }
    else if (gnlh->cmd == NL80211_CMD_NEW_SCAN_RESULTS)
    {
        printf("Got NL80211_CMD_NEW_SCAN_RESULTS.\n");
        results->done = 1;
        results->aborted = 0;
    }

    return NL_SKIP;
}

#define BIT(x) (1ULL<<(x))

enum print_ie_type {
	PRINT_SCAN,
	PRINT_LINK,
};

struct scan_params {
	bool unknown;
	enum print_ie_type type;
	bool show_both_ie_sets;
};

#define WLAN_CAPABILITY_ESS    	(1<<0)
#define WLAN_CAPABILITY_IBSS    (1<<1)
#define WLAN_CAPABILITY_CF_POLLABLE    (1<<2)
#define WLAN_CAPABILITY_CF_POLL_REQUEST    (1<<3)
#define WLAN_CAPABILITY_PRIVACY    (1<<4)
#define WLAN_CAPABILITY_SHORT_PREAMBLE    (1<<5)
#define WLAN_CAPABILITY_PBCC    (1<<6)
#define WLAN_CAPABILITY_CHANNEL_AGILITY    (1<<7)
#define WLAN_CAPABILITY_SPECTRUM_MGMT    (1<<8)
#define WLAN_CAPABILITY_QOS    (1<<9)
#define WLAN_CAPABILITY_SHORT_SLOT_TIME    (1<<10)
#define WLAN_CAPABILITY_APSD    (1<<11)
#define WLAN_CAPABILITY_RADIO_MEASURE    (1<<12)
#define WLAN_CAPABILITY_DSSS_OFDM    (1<<13)
#define WLAN_CAPABILITY_DEL_BACK    (1<<14)
#define WLAN_CAPABILITY_IMM_BACK    (1<<15)
#define WLAN_CAPABILITY_DMG_TYPE_MASK    (3<<0)

#define WLAN_CAPABILITY_DMG_TYPE_IBSS    (1<<0) /* Tx by: STA */
#define WLAN_CAPABILITY_DMG_TYPE_PBSS    (2<<0) /* Tx by: PCP */
#define WLAN_CAPABILITY_DMG_TYPE_AP    (3<<0) /* Tx by: AP */

#define WLAN_CAPABILITY_DMG_CBAP_ONLY    (1<<2)
#define WLAN_CAPABILITY_DMG_CBAP_SOURCE    (1<<3)
#define WLAN_CAPABILITY_DMG_PRIVACY    	(1<<4)
#define WLAN_CAPABILITY_DMG_ECPAC    (1<<5)

#define WLAN_CAPABILITY_DMG_SPECTRUM_MGMT    (1<<8)
#define WLAN_CAPABILITY_DMG_RADIO_MEASURE    (1<<12)

struct print_ies_data {
	unsigned char *ie;
	int ielen;
};

struct ie_print {
	const char *name;
	void (*print)(const uint8_t type, uint8_t len, const uint8_t *data, const struct print_ies_data *ie_buffer);
	uint8_t minlen, maxlen;
	uint8_t flags;
};

void print_ssid_escaped(const uint8_t len, const uint8_t *data)
{
	int i;

	for (i = 0; i < len; i++) {
		if (isprint(data[i]) && data[i] != ' ' && data[i] != '\\')
			printf("%c", data[i]);
		else if (data[i] == ' ' &&
			 (i != 0 && i != len -1))
			printf(" ");
		else
			printf("\\x%.2x", data[i]);
	}
}

static void print_ssid(const uint8_t type, uint8_t len, const uint8_t *data, const struct print_ies_data *ie_buffer)
{
	printf(" ");
	print_ssid_escaped(len, data);
	printf("\n");
}

#define BSS_MEMBERSHIP_SELECTOR_VHT_PHY 126
#define BSS_MEMBERSHIP_SELECTOR_HT_PHY 127

static void print_supprates(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	int i;

	printf(" ");

	for (i = 0; i < len; i++) {
		int r = data[i] & 0x7f;

		if (r == BSS_MEMBERSHIP_SELECTOR_VHT_PHY && (data[i] & 0x80))
			printf("VHT");
		else if (r == BSS_MEMBERSHIP_SELECTOR_HT_PHY && (data[i] & 0x80))
			printf("HT");
		else
			printf("%d.%d", r/2, 5*(r&1));

		printf("%s ", data[i] & 0x80 ? "*" : "");
	}
	printf("\n");
}

static void print_ds(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf(" channel %d\n", data[0]);
}

static void print_tim(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf(" DTIM Count %u DTIM Period %u Bitmap Control 0x%x Bitmap[0] 0x%x",
	       data[0], data[1], data[2], data[3]);
	if (len - 4)
		printf(" (+ %u octet%s)", len - 4, len - 4 == 1 ? "" : "s");
	printf("\n");
}

static void print_ibssatim(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf(" %d TUs", (data[1] << 8) + data[0]);
}

static const char *country_env_str(char environment)
{
	switch (environment) {
	case 'I':
		return "Indoor only";
	case 'O':
		return "Outdoor only";
	case ' ':
		return "Indoor/Outdoor";
	default:
		return "bogus";
	}
}

static void print_country(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf(" %.*s", 2, data);

	printf("\tEnvironment: %s\n", country_env_str(data[2]));

	data += 3;
	len -= 3;

	if (len < 3) {
		printf("\t\tNo country IE triplets present\n");
		return;
	}

	while (len >= 3) {
		int end_channel;
		union ieee80211_country_ie_triplet *triplet = (void *) data;

		if (triplet->ext.reg_extension_id >= IEEE80211_COUNTRY_EXTENSION_ID) {
			printf("\t\tExtension ID: %d Regulatory Class: %d Coverage class: %d (up to %dm)\n",
			       triplet->ext.reg_extension_id,
			       triplet->ext.reg_class,
			       triplet->ext.coverage_class,
			       triplet->ext.coverage_class * 450);

			data += 3;
			len -= 3;
			continue;
		}

		/* 2 GHz */
		if (triplet->chans.first_channel <= 14)
			end_channel = triplet->chans.first_channel + (triplet->chans.num_channels - 1);
		else
			end_channel =  triplet->chans.first_channel + (4 * (triplet->chans.num_channels - 1));

		printf("\t\tChannels [%d - %d] @ %d dBm\n", triplet->chans.first_channel, end_channel, triplet->chans.max_power);

		data += 3;
		len -= 3;
	}

	return;
}

static void print_bss_load(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf("\n");
	printf("\t\t * station count: %d\n", (data[1] << 8) | data[0]);
	printf("\t\t * channel utilisation: %d/255\n", data[2]);
	printf("\t\t * available admission capacity: %d [*32us]\n", (data[4] << 8) | data[3]);
}

static void print_powerconstraint(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf(" %d dB\n", data[0]);
}

static void print_tpcreport(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf(" TX power: %d dBm\n", data[0]);
}

static const char *ht_secondary_offset[4] = {
	"no secondary",
	"above",
	"[reserved!]",
	"below",
};

static void print_erp(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	if (data[0] == 0x00)
		printf(" <no flags>");
	if (data[0] & 0x01)
		printf(" NonERP_Present");
	if (data[0] & 0x02)
		printf(" Use_Protection");
	if (data[0] & 0x04)
		printf(" Barker_Preamble_Mode");
	printf("\n");
}

static void print_obss_scan_params(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf("\n");
	printf("\t\t * passive dwell: %d TUs\n", (data[1] << 8) | data[0]);
	printf("\t\t * active dwell: %d TUs\n", (data[3] << 8) | data[2]);
	printf("\t\t * channel width trigger scan interval: %d s\n", (data[5] << 8) | data[4]);
	printf("\t\t * scan passive total per channel: %d TUs\n", (data[7] << 8) | data[6]);
	printf("\t\t * scan active total per channel: %d TUs\n", (data[9] << 8) | data[8]);
	printf("\t\t * BSS width channel transition delay factor: %d\n", (data[11] << 8) | data[10]);
	printf("\t\t * OBSS Scan Activity Threshold: %d.%02d %%\n",
		((data[13] << 8) | data[12]) / 100, ((data[13] << 8) | data[12]) % 100);
}

static void print_ht_capa(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf("\n");
	print_ht_capability(data[0] | (data[1] << 8));
	print_ampdu_length(data[2] & 3);
	print_ampdu_spacing((data[2] >> 2) & 7);
	print_ht_mcs(data + 3);
}

static void print_capabilities(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	int i, base, bit, si_duration = 0, max_amsdu = 0;
	bool s_psmp_support = false, is_vht_cap = false;
	unsigned char *ie = ie_buffer->ie;
	int ielen = ie_buffer->ielen;

	while (ielen >= 2 && ielen >= ie[1]) {
		if (ie[0] == 191) {
			is_vht_cap = true;
			break;
		}
		ielen -= ie[1] + 2;
		ie += ie[1] + 2;
	}

	for (i = 0; i < len; i++) {
		base = i * 8;

		for (bit = 0; bit < 8; bit++) {
			if (!(data[i] & (1 << bit)))
				continue;

			printf("\n\t\t *");

#define CAPA(bit, name) case bit: printf(" " name); break

/* if the capability 'cap' exists add 'val' to 'sum' otherwise print 'Reserved' */
#define ADD_BIT_VAL(bit, cap, sum, val)	case (bit): do {    \
	if (!(cap)) {    \
		printf(" Reserved");    \
		break;    \
	}    \
	sum += val;    	\
	break;    \
} while (0)

			switch (bit + base) {
			CAPA(0, "HT Information Exchange Supported");
			CAPA(1, "reserved (On-demand Beacon)");
			CAPA(2, "Extended Channel Switching");
			CAPA(3, "reserved (Wave Indication)");
			CAPA(4, "PSMP Capability");
			CAPA(5, "reserved (Service Interval Granularity)");

			case 6:
				s_psmp_support = true;
				printf(" S-PSMP Capability");
				break;

			CAPA(7, "Event");
			CAPA(8, "Diagnostics");
			CAPA(9, "Multicast Diagnostics");
			CAPA(10, "Location Tracking");
			CAPA(11, "FMS");
			CAPA(12, "Proxy ARP Service");
			CAPA(13, "Collocated Interference Reporting");
			CAPA(14, "Civic Location");
			CAPA(15, "Geospatial Location");
			CAPA(16, "TFS");
			CAPA(17, "WNM-Sleep Mode");
			CAPA(18, "TIM Broadcast");
			CAPA(19, "BSS Transition");
			CAPA(20, "QoS Traffic Capability");
			CAPA(21, "AC Station Count");
			CAPA(22, "Multiple BSSID");
			CAPA(23, "Timing Measurement");
			CAPA(24, "Channel Usage");
			CAPA(25, "SSID List");
			CAPA(26, "DMS");
			CAPA(27, "UTC TSF Offset");
			CAPA(28, "TDLS Peer U-APSD Buffer STA Support");
			CAPA(29, "TDLS Peer PSM Support");
			CAPA(30, "TDLS channel switching");
			CAPA(31, "Interworking");
			CAPA(32, "QoS Map");
			CAPA(33, "EBR");
			CAPA(34, "SSPN Interface");
			CAPA(35, "Reserved");
			CAPA(36, "MSGCF Capability");
			CAPA(37, "TDLS Support");
			CAPA(38, "TDLS Prohibited");
			CAPA(39, "TDLS Channel Switching Prohibited");
			CAPA(40, "Reject Unadmitted Frame");

			ADD_BIT_VAL(41, s_psmp_support, si_duration, 1);
			ADD_BIT_VAL(42, s_psmp_support, si_duration, 2);
			ADD_BIT_VAL(43, s_psmp_support, si_duration, 4);

			CAPA(44, "Identifier Location");
			CAPA(45, "U-APSD Coexistence");
			CAPA(46, "WNM-Notification");
			CAPA(47, "Reserved");
			CAPA(48, "UTF-8 SSID");
			CAPA(49, "QMFActivated");
			CAPA(50, "QMFReconfigurationActivated");
			CAPA(51, "Robust AV Streaming");
			CAPA(52, "Advanced GCR");
			CAPA(53, "Mesh GCR");
			CAPA(54, "SCS");
			CAPA(55, "QLoad Report");
			CAPA(56, "Alternate EDCA");
			CAPA(57, "Unprotected TXOP Negotiation");
			CAPA(58, "Protected TXOP egotiation");
			CAPA(59, "Reserved");
			CAPA(60, "Protected QLoad Report");
			CAPA(61, "TDLS Wider Bandwidth");
			CAPA(62, "Operating Mode Notification");

			ADD_BIT_VAL(63, is_vht_cap, max_amsdu, 1);
			ADD_BIT_VAL(64, is_vht_cap, max_amsdu, 2);

			CAPA(65, "Channel Schedule Management");
			CAPA(66, "Geodatabase Inband Enabling Signal");
			CAPA(67, "Network Channel Control");
			CAPA(68, "White Space Map");
			CAPA(69, "Channel Availability Query");
			CAPA(70, "FTM Responder");
			CAPA(71, "FTM Initiator");
			CAPA(72, "Reserved");
			CAPA(73, "Extended Spectrum Management Capable");
			CAPA(74, "Reserved");
			default:
				printf(" %d", bit);
				break;
			}
#undef ADD_BIT_VAL
#undef CAPA
		}
	}

	if (s_psmp_support)
		printf("\n\t\t * Service Interval Granularity is %d ms",
		       (si_duration + 1) * 5);

	if (is_vht_cap) {
		printf("\n\t\t * Max Number Of MSDUs In A-MSDU is ");
		switch (max_amsdu) {
		case 0:
			printf("unlimited");
			break;
		case 1:
			printf("32");
			break;
		case 2:
			printf("16");
			break;
		case 3:
			printf("8");
			break;
		default:
			break;
		}
	}

	printf("\n");
}

static void print_ht_op(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	static const char *protection[4] = {
		"no",
		"nonmember",
		"20 MHz",
		"non-HT mixed",
	};
	static const char *sta_chan_width[2] = {
		"20 MHz",
		"any",
	};

	printf("\n");
	printf("\t\t * primary channel: %d\n", data[0]);
	printf("\t\t * secondary channel offset: %s\n", 	ht_secondary_offset[data[1] & 0x3]);
	printf("\t\t * STA channel width: %s\n", sta_chan_width[(data[1] & 0x4)>>2]);
	printf("\t\t * RIFS: %d\n", (data[1] & 0x8)>>3);
	printf("\t\t * HT protection: %s\n", protection[data[2] & 0x3]);
	printf("\t\t * non-GF present: %d\n", (data[2] & 0x4) >> 2);
	printf("\t\t * OBSS non-GF present: %d\n", (data[2] & 0x10) >> 4);
	printf("\t\t * dual beacon: %d\n", (data[4] & 0x40) >> 6);
	printf("\t\t * dual CTS protection: %d\n", (data[4] & 0x80) >> 7);
	printf("\t\t * STBC beacon: %d\n", data[5] & 0x1);
	printf("\t\t * L-SIG TXOP Prot: %d\n", (data[5] & 0x2) >> 1);
	printf("\t\t * PCO active: %d\n", (data[5] & 0x4) >> 2);
	printf("\t\t * PCO phase: %d\n", (data[5] & 0x8) >> 3);
}

static void print_mesh_conf(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	printf("\n");
	printf("\t\t * Active Path Selection Protocol ID: %d\n", data[0]);
	printf("\t\t * Active Path Selection Metric ID: %d\n", data[1]);
	printf("\t\t * Congestion Control Mode ID: %d\n", data[2]);
	printf("\t\t * Synchronization Method ID: %d\n", data[3]);
	printf("\t\t * Authentication Protocol ID: %d\n", data[4]);
	printf("\t\t * Mesh Formation Info:\n");
	printf("\t\t\t Number of Peerings: %d\n", (data[5] & 0x7E) >> 1);
	if (data[5] & 0x01)
		printf("\t\t\t Connected to Mesh Gate\n");
	if (data[5] & 0x80)
		printf("\t\t\t Connected to AS\n");
	printf("\t\t * Mesh Capability\n");
	if (data[6] & 0x01)
		printf("\t\t\t Accepting Additional Mesh Peerings\n");
	if (data[6] & 0x02)
		printf("\t\t\t MCCA Supported\n");
	if (data[6] & 0x04)
		printf("\t\t\t MCCA Enabled\n");
	if (data[6] & 0x08)
		printf("\t\t\t Forwarding\n");
	if (data[6] & 0x10)
		printf("\t\t\t MBCA Supported\n");
	if (data[6] & 0x20)
		printf("\t\t\t TBTT Adjusting\n");
	if (data[6] & 0x40)
		printf("\t\t\t Mesh Power Save Level\n");
}

static const char* ntype_11u(uint8_t t)
{
	switch (t) {
	case 0: return "Private";
	case 1: return "Private with Guest";
	case 2: return "Chargeable Public";
	case 3: return "Free Public";
	case 4: return "Personal Device";
	case 5: return "Emergency Services Only";
	case 14: return "Test or Experimental";
	case 15: return "Wildcard";
	default: return "Reserved";
	}
}

static const char* vgroup_11u(uint8_t t)
{
	switch (t) {
	case 0: return "Unspecified";
	case 1: return "Assembly";
	case 2: return "Business";
	case 3: return "Educational";
	case 4: return "Factory and Industrial";
	case 5: return "Institutional";
	case 6: return "Mercantile";
	case 7: return "Residential";
	case 8: return "Storage";
	case 9: return "Utility and Miscellaneous";
	case 10: return "Vehicular";
	case 11: return "Outdoor";
	default: return "Reserved";
	}
}

static void print_interworking(const uint8_t type, uint8_t len, const uint8_t *data,
		const struct print_ies_data *ie_buffer)
{
	/* See Section 7.3.2.92 in the 802.11u spec. */
	printf("\n");
	if (len >= 1) {
		uint8_t ano = data[0];
		printf("\t\tNetwork Options: 0x%hx\n", (unsigned short)(ano));
		printf("\t\t\tNetwork Type: %i (%s)\n", (int)(ano & 0xf), ntype_11u(ano & 0xf));
		if (ano & (1<<4))
			printf("\t\t\tInternet\n");
		if (ano & (1<<5))
			printf("\t\t\tASRA\n");
		if (ano & (1<<6))
			printf("\t\t\tESR\n");
		if (ano & (1<<7))
			printf("\t\t\tUESA\n");
	}
	if ((len == 3) || (len == 9)) {
		printf("\t\tVenue Group: %i (%s)\n", (int)(data[1]), vgroup_11u(data[1]));
		printf("\t\tVenue Type: %i\n", (int)(data[2]));
	}
	if (len == 9)
		printf("\t\tHESSID: %02hx:%02hx:%02hx:%02hx:%02hx:%02hx\n",
		       data[3], data[4], data[5], data[6], data[7], data[8]);
	else if (len == 7)
		printf("\t\tHESSID: %02hx:%02hx:%02hx:%02hx:%02hx:%02hx\n",
		       data[1], data[2], data[3], data[4], data[5], data[6]);
}

static const struct ie_print ieprinters[] = {
    [0] = { "SSID", print_ssid, 0, 32, BIT(PRINT_SCAN) | BIT(PRINT_LINK), },
	[1] = { "Supported rates", print_supprates, 0, 255, BIT(PRINT_SCAN), },
	[3] = { "DS Parameter set", print_ds, 1, 1, BIT(PRINT_SCAN), },
	[5] = { "TIM", print_tim, 4, 255, BIT(PRINT_SCAN), },
	[6] = { "IBSS ATIM window", print_ibssatim, 2, 2, BIT(PRINT_SCAN), },
	[7] = { "Country", print_country, 3, 255, BIT(PRINT_SCAN), },
	[11] = { "BSS Load", print_bss_load, 5, 5, BIT(PRINT_SCAN), },
	[32] = { "Power constraint", print_powerconstraint, 1, 1, BIT(PRINT_SCAN), },
	[35] = { "TPC report", print_tpcreport, 2, 2, BIT(PRINT_SCAN), },
	[42] = { "ERP", print_erp, 1, 255, BIT(PRINT_SCAN), },
	[45] = { "HT capabilities", print_ht_capa, 26, 26, BIT(PRINT_SCAN), },
	[47] = { "ERP D4.0", print_erp, 1, 255, BIT(PRINT_SCAN), },
	[74] = { "Overlapping BSS scan params", print_obss_scan_params, 14, 255, BIT(PRINT_SCAN), },
	[61] = { "HT operation", print_ht_op, 22, 22, BIT(PRINT_SCAN), },
	[62] = { "Secondary Channel Offset", print_secchan_offs, 1, 1, BIT(PRINT_SCAN), },
	[191] = { "VHT capabilities", print_vht_capa, 12, 255, BIT(PRINT_SCAN), },
	[192] = { "VHT operation", print_vht_oper, 5, 255, BIT(PRINT_SCAN), },
	[48] = { "RSN", print_rsn, 2, 255, BIT(PRINT_SCAN), },
	[50] = { "Extended supported rates", print_supprates, 0, 255, BIT(PRINT_SCAN), },
	[113] = { "MESH Configuration", print_mesh_conf, 7, 7, BIT(PRINT_SCAN), },
	[114] = { "MESH ID", print_ssid, 0, 32, BIT(PRINT_SCAN) | BIT(PRINT_LINK), },
	[127] = { "Extended capabilities", print_capabilities, 0, 255, BIT(PRINT_SCAN), },
	[107] = { "802.11u Interworking", print_interworking, 0, 255, BIT(PRINT_SCAN), },
};

static void print_vendor(unsigned char len, unsigned char *data,
			 bool unknown, enum print_ie_type ptype)
{
	int i;

	if (len < 3) {
		printf("\tVendor specific: <too short> data:");
		for(i = 0; i < len; i++)
			printf(" %.02x", data[i]);
		printf("\n");
		return;
	}

	if (len >= 4 && memcmp(data, ms_oui, 3) == 0) {
		if (data[3] < ARRAY_SIZE(wifiprinters) &&
		    wifiprinters[data[3]].name &&
		    wifiprinters[data[3]].flags & BIT(ptype)) {
			print_ie(&wifiprinters[data[3]],
				 data[3], len - 4, data + 4,
				 NULL);
			return;
		}
		if (!unknown)
			return;
		printf("\tMS/WiFi %#.2x, data:", data[3]);
		for(i = 0; i < len - 4; i++)
			printf(" %.02x", data[i + 4]);
		printf("\n");
		return;
	}

	if (len >= 4 && memcmp(data, wfa_oui, 3) == 0) {
		if (data[3] < ARRAY_SIZE(wfa_printers) &&
		    wfa_printers[data[3]].name &&
		    wfa_printers[data[3]].flags & BIT(ptype)) {
			print_ie(&wfa_printers[data[3]],
				 data[3], len - 4, data + 4,
				 NULL);
			return;
		}
		if (!unknown)
			return;
		printf("\tWFA %#.2x, data:", data[3]);
		for(i = 0; i < len - 4; i++)
			printf(" %.02x", data[i + 4]);
		printf("\n");
		return;
	}

	if (!unknown)
		return;

	printf("\tVendor specific: OUI %.2x:%.2x:%.2x, data:",
		data[0], data[1], data[2]);
	for (i = 3; i < len; i++)
		printf(" %.2x", data[i]);
	printf("\n");
}

void print_ies(unsigned char *ie, int ielen, bool unknown, enum print_ie_type ptype)
{
	struct print_ies_data ie_buffer = {
		.ie = ie,
		.ielen = ielen };

	while (ielen >= 2 && ielen >= ie[1]) {
		if (ie[0] < ARRAY_SIZE(ieprinters) &&
		    ieprinters[ie[0]].name &&
		    ieprinters[ie[0]].flags & BIT(ptype)) {
			print_ie(&ieprinters[ie[0]],
				 ie[0], ie[1], ie + 2, &ie_buffer);
		} else if (ie[0] == 221 /* vendor */) {
			print_vendor(ie[1], ie + 2, unknown, ptype);
		} else if (unknown) {
			int i;

			printf("\tUnknown IE (%d):", ie[0]);
			for (i=0; i<ie[1]; i++)
				printf(" %.2x", ie[2+i]);
			printf("\n");
		}
		ielen -= ie[1] + 2;
		ie += ie[1] + 2;
	}
}

static void print_capa_dmg(__u16 capa)
{
	switch (capa & WLAN_CAPABILITY_DMG_TYPE_MASK) {
	case WLAN_CAPABILITY_DMG_TYPE_AP:
		printf(" DMG_ESS");
		break;
	case WLAN_CAPABILITY_DMG_TYPE_PBSS:
		printf(" DMG_PCP");
		break;
	case WLAN_CAPABILITY_DMG_TYPE_IBSS:
		printf(" DMG_IBSS");
		break;
	}

	if (capa & WLAN_CAPABILITY_DMG_CBAP_ONLY)
		printf(" CBAP_Only");
	if (capa & WLAN_CAPABILITY_DMG_CBAP_SOURCE)
		printf(" CBAP_Src");
	if (capa & WLAN_CAPABILITY_DMG_PRIVACY)
		printf(" Privacy");
	if (capa & WLAN_CAPABILITY_DMG_ECPAC)
		printf(" ECPAC");
	if (capa & WLAN_CAPABILITY_DMG_SPECTRUM_MGMT)
		printf(" SpectrumMgmt");
	if (capa & WLAN_CAPABILITY_DMG_RADIO_MEASURE)
		printf(" RadioMeasure");
}

static void print_capa_non_dmg(__u16 capa)
{
	if (capa & WLAN_CAPABILITY_ESS)
		printf(" ESS");
	if (capa & WLAN_CAPABILITY_IBSS)
		printf(" IBSS");
	if (capa & WLAN_CAPABILITY_CF_POLLABLE)
		printf(" CfPollable");
	if (capa & WLAN_CAPABILITY_CF_POLL_REQUEST)
		printf(" CfPollReq");
	if (capa & WLAN_CAPABILITY_PRIVACY)
		printf(" Privacy");
	if (capa & WLAN_CAPABILITY_SHORT_PREAMBLE)
		printf(" ShortPreamble");
	if (capa & WLAN_CAPABILITY_PBCC)
		printf(" PBCC");
	if (capa & WLAN_CAPABILITY_CHANNEL_AGILITY)
		printf(" ChannelAgility");
	if (capa & WLAN_CAPABILITY_SPECTRUM_MGMT)
		printf(" SpectrumMgmt");
	if (capa & WLAN_CAPABILITY_QOS)
		printf(" QoS");
	if (capa & WLAN_CAPABILITY_SHORT_SLOT_TIME)
		printf(" ShortSlotTime");
	if (capa & WLAN_CAPABILITY_APSD)
		printf(" APSD");
	if (capa & WLAN_CAPABILITY_RADIO_MEASURE)
		printf(" RadioMeasure");
	if (capa & WLAN_CAPABILITY_DSSS_OFDM)
		printf(" DSSS-OFDM");
	if (capa & WLAN_CAPABILITY_DEL_BACK)
		printf(" DelayedBACK");
	if (capa & WLAN_CAPABILITY_IMM_BACK)
		printf(" ImmediateBACK");
}

static int callback_dump(struct nl_msg *msg, void *arg)
{
	/* callback_dump() prints SSIDs to stdout.
	 * @NL80211_BSS_BSSID: BSSID of the BSS (6 octets)
	 * @NL80211_BSS_FREQUENCY: frequency in MHz (u32)
	 * @NL80211_BSS_TSF: TSF of the received probe response/beacon (u64)
	 *	(if @NL80211_BSS_PRESP_DATA is present then this is known to be
	 *	from a probe response, otherwise it may be from the same beacon
	 *	that the NL80211_BSS_BEACON_TSF will be from)
	 * @NL80211_BSS_BEACON_INTERVAL: beacon interval of the (I)BSS (u16)
	 * @NL80211_BSS_CAPABILITY: capability field (CPU order, u16)
	 * @NL80211_BSS_INFORMATION_ELEMENTS: binary attribute containing the
	 *	raw information elements from the probe response/beacon (bin);
	 * @NL80211_BSS_SIGNAL_MBM: signal strength of probe response/beacon
	 *	in mBm (100 * dBm) (s32)
	 *	@NL80211_BSS_SIGNAL_UNSPEC: signal strength of the probe response/beacon
	 *	in unspecified units, scaled to 0..100 (u8)
	 *	@NL80211_BSS_STATUS: status, if this BSS is "used"
	 *	@NL80211_BSS_SEEN_MS_AGO: age of this BSS entry in ms
	 * @NL80211_BSS_BEACON_IES: binary attribute containing the raw information
	 *	elements from a Beacon frame (bin)
	 */
	struct nlattr *tb[NL80211_ATTR_MAX + 1];
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *bss[NL80211_BSS_MAX + 1];
	char mac_addr[20], dev[20];
	static struct nla_policy bss_policy[NL80211_BSS_MAX + 1] = {
		[NL80211_BSS_TSF] = { .type = NLA_U64 },
		[NL80211_BSS_FREQUENCY] = { .type = NLA_U32 },
		[NL80211_BSS_BSSID] = { },
		[NL80211_BSS_BEACON_INTERVAL] = { .type = NLA_U16 },
		[NL80211_BSS_CAPABILITY] = { .type = NLA_U16 },
		[NL80211_BSS_INFORMATION_ELEMENTS] = { },
		[NL80211_BSS_SIGNAL_MBM] = { .type = NLA_U32 },
		[NL80211_BSS_SIGNAL_UNSPEC] = { .type = NLA_U8 },
		[NL80211_BSS_STATUS] = { .type = NLA_U32 },
		[NL80211_BSS_SEEN_MS_AGO] = { .type = NLA_U32 },
		[NL80211_BSS_BEACON_IES] = { },
	};

	struct scan_params *params = arg;
	int show = params->show_both_ie_sets ? 2 : 1;
	bool is_dmg = false;

	nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
		  genlmsg_attrlen(gnlh, 0), NULL);

	if (!tb[NL80211_ATTR_BSS]) {
		fprintf(stderr, "bss info missing!\n");
		return NL_SKIP;
	}
	if (nla_parse_nested(bss, NL80211_BSS_MAX,
			     tb[NL80211_ATTR_BSS],
			     bss_policy)) {
		fprintf(stderr, "failed to parse nested attributes!\n");
		return NL_SKIP;
	}

	if (!bss[NL80211_BSS_BSSID])
		return NL_SKIP;

	mac_addr_n2a(mac_addr, nla_data(bss[NL80211_BSS_BSSID]));
	printf("BSS %s", mac_addr);
	if (tb[NL80211_ATTR_IFINDEX]) {
		if_indextoname(nla_get_u32(tb[NL80211_ATTR_IFINDEX]), dev);
		printf("(on %s)", dev);
	}

	if (bss[NL80211_BSS_STATUS]) {
		switch (nla_get_u32(bss[NL80211_BSS_STATUS])) {
		case NL80211_BSS_STATUS_AUTHENTICATED:
			printf(" -- authenticated");
			break;
		case NL80211_BSS_STATUS_ASSOCIATED:
			printf(" -- associated");
			break;
		case NL80211_BSS_STATUS_IBSS_JOINED:
			printf(" -- joined");
			break;
		default:
			printf(" -- unknown status: %d",
				nla_get_u32(bss[NL80211_BSS_STATUS]));
			break;
		}
	}
	printf("\n");

	if (bss[NL80211_BSS_LAST_SEEN_BOOTTIME]) {
		unsigned long long bt;
		bt = (unsigned long long)nla_get_u64(bss[NL80211_BSS_LAST_SEEN_BOOTTIME]);
		printf("\tlast seen: %llu.%.3llus [boottime]\n", bt/1000000000, (bt%1000000000)/1000000);
	}

	if (bss[NL80211_BSS_TSF]) {
		unsigned long long tsf;
		tsf = (unsigned long long)nla_get_u64(bss[NL80211_BSS_TSF]);
		printf("\tTSF: %llu usec (%llud, %.2lld:%.2llu:%.2llu)\n",
			tsf, tsf/1000/1000/60/60/24, (tsf/1000/1000/60/60) % 24,
			(tsf/1000/1000/60) % 60, (tsf/1000/1000) % 60);
	}
	if (bss[NL80211_BSS_FREQUENCY]) {
		int freq = nla_get_u32(bss[NL80211_BSS_FREQUENCY]);
		printf("\tfreq: %d\n", freq);
		if (freq > 45000)
			is_dmg = true;
	}
	if (bss[NL80211_BSS_BEACON_INTERVAL])
		printf("\tbeacon interval: %d TUs\n",
			nla_get_u16(bss[NL80211_BSS_BEACON_INTERVAL]));
	if (bss[NL80211_BSS_CAPABILITY]) {
		__u16 capa = nla_get_u16(bss[NL80211_BSS_CAPABILITY]);
		printf("\tcapability:");
		if (is_dmg)
			print_capa_dmg(capa);
		else
			print_capa_non_dmg(capa);
		printf(" (0x%.4x)\n", capa);
	}
	if (bss[NL80211_BSS_SIGNAL_MBM]) {
		int s = nla_get_u32(bss[NL80211_BSS_SIGNAL_MBM]);
		printf("\tsignal: %d.%.2d dBm\n", s/100, s%100);
	}
	if (bss[NL80211_BSS_SIGNAL_UNSPEC]) {
		unsigned char s = nla_get_u8(bss[NL80211_BSS_SIGNAL_UNSPEC]);
		printf("\tsignal: %d/100\n", s);
	}
	if (bss[NL80211_BSS_SEEN_MS_AGO]) {
		int age = nla_get_u32(bss[NL80211_BSS_SEEN_MS_AGO]);
		printf("\tlast seen: %d ms ago\n", age);
	}

	if (bss[NL80211_BSS_INFORMATION_ELEMENTS] && show--) {
		struct nlattr *ies = bss[NL80211_BSS_INFORMATION_ELEMENTS];
		struct nlattr *bcnies = bss[NL80211_BSS_BEACON_IES];

		if (bss[NL80211_BSS_PRESP_DATA] ||
				(bcnies && (nla_len(ies) != nla_len(bcnies) ||
				memcmp(nla_data(ies), nla_data(bcnies), nla_len(ies)))))
			printf("\tInformation elements from Probe response frame:\n");
		print_ies(nla_data(ies), nla_len(ies), params->unknown, params->type);
	}
	if (bss[NL80211_BSS_BEACON_IES] && show--) {
		printf("\tInformation elements from Beacon frame:\n");
		print_ies(nla_data(bss[NL80211_BSS_BEACON_IES]),
				nla_len(bss[NL80211_BSS_BEACON_IES]),
				params->unknown, params->type);
	}

	return NL_SKIP;
}

int do_scan_trigger(struct nl_sock *socket, int if_index, int driver_id) {
	// Starts the scan and waits for it to finish. Does not return until the scan is done or has been aborted.
	struct trigger_results results = {
			.done = 0,
			.aborted = 0
	};
	struct nl_msg *msg;
    struct nl_cb *cb;
    struct nl_msg *ssids_to_scan;
    int err;
    int ret;

    int mcid = nl_get_multicast_id(socket, "nl80211", "scan");
    nl_socket_add_membership(socket, mcid);

    // Allocate the messages and callback handler.
    msg = nlmsg_alloc();
    if (!msg) {
        printf("Failed to allocate netlink message for msg.\n");
        return -ENOMEM;
    }
    ssids_to_scan = nlmsg_alloc();
    if (!ssids_to_scan) {
        printf("Failed to allocate netlink message for ssids_to_scan.\n");
        nlmsg_free(msg);
        return -ENOMEM;
    }
    cb = nl_cb_alloc(NL_CB_DEFAULT);
    if (!cb) {
        printf("Failed to allocate netlink callbacks.\n");
        nlmsg_free(msg);
        nlmsg_free(ssids_to_scan);
        return -ENOMEM;
    }

    // Setup the messages and callback handler.
    genlmsg_put(msg, 0, 0, driver_id, 0, 0, NL80211_CMD_TRIGGER_SCAN, 0);    // Setup which command to run
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, if_index);    // Add message attribute, which interface to use
    nla_put(ssids_to_scan, 1, 0, "");    // Scan all SSIDs
    nla_put_nested(msg, NL80211_ATTR_SCAN_SSIDS, ssids_to_scan);
    nlmsg_free(ssids_to_scan);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, callback_trigger, &results);    // Add the callback
    nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);
    nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check, NULL);    // No sequence checking for multicast messages

    // Send NL80211_CMD_TRIGGER_SCAN to start the scan. The kernel may reply with NL80211_CMD_NEW_SCAN_RESULTS on
    // success or NL80211_CMD_SCAN_ABORTED if another scan was started by another process.
    err = 1;
    ret = nl_send_auto(socket, msg);    // Send the message
    printf("NL80211_CMD_TRIGGER_SCAN sent %d bytes to the kernel.\n", ret);
    printf("Waiting for scan to complete...\n");
    while (err > 0) ret = nl_recvmsgs(socket, cb);
    if (err < 0) {
    	printf("Warning: err has a value of %d.\n", err);
    }
    if (ret < 0) {
    	printf("Error: nl_recvmsgs() returned %d (%s).\n", ret, nl_geterror(-ret));
    	return ret;
    }

    // Now wait until the scan is done or aborted.
    while (!results.done) nl_recvmsgs(socket, cb);
    if (results.aborted) {
    	printf("Error: Kernel aborted scan.\n");
    	return 1;
    }
    printf("Scan is done.\n");

    // Cleanup
    nlmsg_free(msg);
    nl_cb_put(cb);
    nl_socket_drop_membership(socket, mcid);
    return 0;
}

static int (*registered_handler)(struct nl_msg *, void *);
static void *registered_handler_data;

void register_handler(int (*handler)(struct nl_msg *, void *), void *data)
{
	registered_handler = handler;
	registered_handler_data = data;
}

struct nl80211_state {
	struct nl_sock *nl_sock;
	int nl80211_id;
};

struct print_event_args {
	struct timeval ts; /* internal */
	bool have_ts; /* must be set false */
	bool frame, time, reltime;
};

struct wait_event {
	int n_cmds, n_prints;
	const __u32 *cmds;
	const __u32 *prints;
	__u32 cmd;
	struct print_event_args *pargs;
};

static int nl80211_print(struct nl_msg* msg, void* arg) {
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	struct nlattr *tb[NL80211_ATTR_MAX + 1];
	char macbuf[6*3];
	__u16 status;

	printf("event command: %d\n", gnlh->cmd);

	switch(gnlh->cmd) {
	case NL80211_CMD_NEW_STATION:
		mac_addr_n2a(macbuf, nla_data(tb[NL80211_ATTR_MAC]));
		printf("new station %s\n", macbuf);
		break;
	case NL80211_CMD_DEL_STATION:
		mac_addr_n2a(macbuf, nla_data(tb[NL80211_ATTR_MAC]));
		printf("del station %s\n", macbuf);
		break;
	case NL80211_CMD_JOIN_IBSS:
		mac_addr_n2a(macbuf, nla_data(tb[NL80211_ATTR_MAC]));
		printf("IBSS %s joined\n", macbuf);
		break;
	case NL80211_CMD_AUTHENTICATE:
		printf("auth");
		if (tb[NL80211_ATTR_FRAME])
			printf(": print frame");
		else if (tb[NL80211_ATTR_TIMED_OUT])
			printf(": timed out");
		else
			printf(": unknown event");
		printf("\n");
		break;
	case NL80211_CMD_ASSOCIATE:
		printf("assoc");
		if (tb[NL80211_ATTR_FRAME])
			printf(": print frame");
		else if (tb[NL80211_ATTR_TIMED_OUT])
			printf(": timed out");
		else
			printf(": unknown event");
		printf("\n");
		break;
	case NL80211_CMD_DEAUTHENTICATE:
		printf("deauth");
		printf(": print frame");
		printf("\n");
		break;
	case NL80211_CMD_DISASSOCIATE:
		printf("disassoc");
		printf(": print frame");
		printf("\n");
		break;
	case NL80211_CMD_CONNECT:
		status = 0;
		if (tb[NL80211_ATTR_TIMED_OUT])
			printf("timed out");
		else if (!tb[NL80211_ATTR_STATUS_CODE])
			printf("unknown connect status");
		else if (nla_get_u16(tb[NL80211_ATTR_STATUS_CODE]) == 0)
			printf("connected");
		else {
			status = nla_get_u16(tb[NL80211_ATTR_STATUS_CODE]);
			printf("failed to connect");
		}
		if (tb[NL80211_ATTR_MAC]) {
			mac_addr_n2a(macbuf, nla_data(tb[NL80211_ATTR_MAC]));
			printf(" to %s", macbuf);
		}
		if (status)
			printf(", status: %d: %s", status, get_status_str(status));
		printf("\n");
		break;
	case NL80211_CMD_ROAM:
		printf("roamed");
		if (tb[NL80211_ATTR_MAC]) {
			mac_addr_n2a(macbuf, nla_data(tb[NL80211_ATTR_MAC]));
			printf(" to %s", macbuf);
		}
		printf("\n");
		break;
	case NL80211_CMD_DISCONNECT:
		printf("disconnected");
		if (tb[NL80211_ATTR_DISCONNECTED_BY_AP])
			printf(" (by AP)");
		else
			printf(" (local request)");
		if (tb[NL80211_ATTR_REASON_CODE])
			printf(" reason: %d: %s", nla_get_u16(tb[NL80211_ATTR_REASON_CODE]),
				get_reason_str(nla_get_u16(tb[NL80211_ATTR_REASON_CODE])));
		printf("\n");
		break;

    default:
    	printf("default multicast event: %d\n", gnlh->cmd);
        return NL_SKIP;
    }

	return 0;
}

static int nl80211_wait(struct nl_msg *msg, void *arg)
{
	struct wait_event *wait = arg;
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
	int i;

	if (wait->pargs) {
		for (i = 0; i < wait->n_prints; i++) {
			if (gnlh->cmd == wait->prints[i])
				nl80211_print(msg, wait->pargs);
		}
	}

	for (i = 0; i < wait->n_cmds; i++) {
		if (gnlh->cmd == wait->cmds[i])
			wait->cmd = gnlh->cmd;
	}

	return NL_SKIP;
}

static int nl80211_init(struct nl80211_state *state)
{
	int err;

	state->nl_sock = nl_socket_alloc();
	if (!state->nl_sock) {
		fprintf(stderr, "Failed to allocate netlink socket.\n");
		return -ENOMEM;
	}

	if (genl_connect(state->nl_sock)) {
		fprintf(stderr, "Failed to connect to generic netlink.\n");
		err = -ENOLINK;
		goto out_handle_destroy;
	}

	/* The buffer size used when reading from the netlink socket and thus limiting the
	 * maximum size of a netlink message that can be read defaults to the size of a memory page.
	 */
	nl_socket_set_buffer_size(state->nl_sock, 8192, 8192);

	/* try to set NETLINK_EXT_ACK to 1, ignoring errors */
	err = 1;
	setsockopt(nl_socket_get_fd(state->nl_sock), SOL_NETLINK, NETLINK_EXT_ACK, &err, sizeof(err));

	/* This method resolves the generic netlink family name ("nl80211") to the
	 * corresponding numeric family identifier. The userspace application must
	 * send its subsequent messages to the kernel, specifying this id.
	 */
	state->nl80211_id = genl_ctrl_resolve(state->nl_sock, "nl80211");
	if (state->nl80211_id < 0) {
		fprintf(stderr, "nl80211 not found.\n");
		err = -ENOENT;
		goto out_handle_destroy;
	}

	return 0;

out_handle_destroy:
	nl_socket_free(state->nl_sock);
	return err;
}

static void nl80211_cleanup(struct nl80211_state *state)
{
	nl_socket_free(state->nl_sock);
}

static int nl80211_listen_events(struct nl80211_state *state, struct print_event_args *args)
{
	int mcid, ret;

	/* Configuration multicast group
	 * Joins the specified groups using the modern socket option which is
	 * available since kernel version 2.6.14. It allows joining an almost arbitary
	 * number of groups without limitation.
	 */
	mcid = nl_get_multicast_id(state->nl_sock, "nl80211", "config");
	if (mcid >= 0) {
		ret = nl_socket_add_membership(state->nl_sock, mcid);
		if (ret)
			return ret;
	}

	/* Scan multicast group */
	mcid = nl_get_multicast_id(state->nl_sock, "nl80211", "scan");
	if (mcid >= 0) {
		ret = nl_socket_add_membership(state->nl_sock, mcid);
		if (ret)
			return ret;
	}

	/* Regulatory multicast group */
	mcid = nl_get_multicast_id(state->nl_sock, "nl80211", "regulatory");
	if (mcid >= 0) {
		ret = nl_socket_add_membership(state->nl_sock, mcid);
		if (ret)
			return ret;
	}

	/* MLME multicast group */
	mcid = nl_get_multicast_id(state->nl_sock, "nl80211", "mlme");
	if (mcid >= 0) {
		ret = nl_socket_add_membership(state->nl_sock, mcid);
		if (ret)
			return ret;
	}

	const __u32 *waits = NULL;
	const int n_waits = 0;
	const __u32 *prints = NULL;
	const int n_prints = 0;

	struct nl_cb *cb = nl_cb_alloc(NL_CB_DEFAULT);
	struct wait_event wait_ev;

	if (n_waits && waits) {
			wait_ev.cmds = waits;
			wait_ev.n_cmds = n_waits;
			wait_ev.prints = prints;
			wait_ev.n_prints = n_prints;
			register_handler(nl80211_wait, &wait_ev);
		}
	else {
		register_handler(nl80211_print, args);
	}

	wait_ev.cmd = 0;

	while (!wait_ev.cmd)
		nl_recvmsgs(state->nl_sock, cb);

	nl_cb_put(cb);

	return wait_ev.cmd;
}

int main(void)
{
	// Use this wireless interface for scanning.
	int if_index = if_nametoindex("wlp1s0");
	// Open socket to kernel.
	// Allocate new netlink socket in memory.
	struct nl_sock *socket = nl_socket_alloc();
	// Create file descriptor and bind socket.
	genl_connect(socket);
	// Resolve Generic Netlink family name to numeric identifier (driver_id in this case).
	int driver_id = genl_ctrl_resolve(socket, "nl80211");

	// Issue NL80211_CMD_TRIGGER_SCAN to the kernel and wait for it to finish.
	int err = do_scan_trigger(socket, if_index, driver_id);
    if (err != 0) {
    	printf("do_scan_trigger() failed with %d.\n", err);
    	return err;
    }

    // Now get info for all SSIDs detected.
    struct nl_msg *msg = nlmsg_alloc();    // Allocate a message
    genlmsg_put(msg, 0, 0, driver_id, 0, NLM_F_DUMP, NL80211_CMD_GET_SCAN, 0);    // Setup which command to run
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, if_index);    // Add message attribute, which interface to use
    nl_socket_modify_cb(socket, NL_CB_VALID, NL_CB_CUSTOM, callback_dump, NULL);    // Add the callback
    int ret = nl_send_auto(socket, msg);    // Send the message
    printf("NL80211_CMD_GET_SCAN sent %d bytes to the kernel.\n", ret);
    ret = nl_recvmsgs_default(socket);    // Retrieve the kernel's answer (callback_dump() prints SSIDs to stdout)
    nlmsg_free(msg);

    if (ret < 0) {
    	printf("Error: nl_recvmsgs_default() returned %d (%s).\n", ret, nl_geterror(-ret));
    	return ret;
    }

    return 0;
}
/*
struct nl80211_state nlstate;
int errnl;

errnl = nl80211_init(&nlstate);
if (errnl)
	return 1;

struct print_event_args args;

errnl = nl80211_listen_events(&nlstate, &args);
if (errnl)
	return 1;
*/

/*
nl80211_cleanup(&nlstate);
*/
