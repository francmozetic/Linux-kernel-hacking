/**
 * @file: uevent1.c
 * @author: Aleksander Mozetic
 * @date: 15 February 2019
 * @version: 1.2.2.0
 * @copyright: 2019 IndigoSoft
 * @brief: A userspace application for wireless interface scanning.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>    /* isprint */
#include <fcntl.h>
#include <errno.h>

#include <sys/uio.h>
#include <net/if.h>

#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/nl80211.h>

#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

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

        nla_parse(tb_mcgrp, CTRL_ATTR_MCAST_GRP_MAX, nla_data(mcgrp), nla_len(mcgrp), NULL);

        if (!tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME] || !tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID]) continue;
        if (strncmp(nla_data(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME]), grp->group,
                nla_len(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME]))) {
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
    struct handler_args grp = { .group = group, .id = -ENOENT, };

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

    while (ret > 0) nl_recvmsgs(sock, cb);

    if (ret == 0) ret = grp.id;

    nla_put_failure:
        out:
            nl_cb_put(cb);
        out_fail_cb:
            nlmsg_free(msg);
            return ret;
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

void print_ssid(unsigned char *ie, int ielen) {
    uint8_t len;
    uint8_t *data;
    int i;

    while (ielen >= 2 && ielen >= ie[1]) {
        if (ie[0] == 0 && ie[1] >= 0 && ie[1] <= 32) {
            len = ie[1];
            data = ie + 2;
            for (i = 0; i < len; i++) {
                if (isprint(data[i]) && data[i] != ' ' && data[i] != '\\') printf("%c", data[i]);
                else if (data[i] == ' ' && (i != 0 && i != len -1)) printf(" ");
                else printf("\\x%.2x", data[i]);
            }
            break;
        }
        ielen -= ie[1] + 2;
        ie += ie[1] + 2;
    }
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

static int callback_dump(struct nl_msg *msg, void *arg) {
	/* Called by the kernel with a dump of the successful scan's data. Called for each SSID.
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
	 *	elements from a Beacon frame (bin); not present if no Beacon frame has
	 *	yet been received
	 */
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    char mac_addr[20];
    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    struct nlattr *bss[NL80211_BSS_MAX + 1];
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

    // Parse and error check
    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);
    if (!tb[NL80211_ATTR_BSS]) {
        printf("bss info missing\n");
        return NL_SKIP;
    }
    if (nla_parse_nested(bss, NL80211_BSS_MAX, tb[NL80211_ATTR_BSS], bss_policy)) {
        printf("failed to parse nested attributes\n");
        return NL_SKIP;
    }
    if (!bss[NL80211_BSS_BSSID]) return NL_SKIP;
    if (!bss[NL80211_BSS_INFORMATION_ELEMENTS]) return NL_SKIP;

    if (bss[NL80211_BSS_FREQUENCY])
      printf("%d,", nla_get_u32(bss[NL80211_BSS_FREQUENCY]));
    else
      printf("NaN,");
    if (bss[NL80211_BSS_BSSID]) {
         mac_addr_n2a(mac_addr, nla_data(bss[NL80211_BSS_BSSID]));
         printf("%s,", mac_addr);
       }
     else
       printf("NaN,");
    if (bss[NL80211_BSS_SEEN_MS_AGO]) {
    	int s = nla_get_u32(bss[NL80211_BSS_SEEN_MS_AGO]);
    	printf("%d,", s);
    }
    else
    	printf("NaN,");
    if (bss[NL80211_BSS_SIGNAL_MBM]) {
    	int s = nla_get_u32(bss[NL80211_BSS_SIGNAL_MBM]);
    	printf("%d.%.2d,", s/100, s%100);
    }
    else
    	printf("NaN,");
    if (bss[NL80211_BSS_INFORMATION_ELEMENTS])
    	print_ssid(nla_data(bss[NL80211_BSS_INFORMATION_ELEMENTS]), nla_len(bss[NL80211_BSS_INFORMATION_ELEMENTS]));
    else
    	printf("NaN,");



    printf("\n");

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

    // Allocate the messages and callback handler
    msg = nlmsg_alloc();
    if (!msg) {
        printf("Failed to allocate netlink message for msg\n");
        return -ENOMEM;
    }
    ssids_to_scan = nlmsg_alloc();
    if (!ssids_to_scan) {
        printf("Failed to allocate netlink message for ssids_to_scan\n");
        nlmsg_free(msg);
        return -ENOMEM;
    }
    cb = nl_cb_alloc(NL_CB_DEFAULT);
    if (!cb) {
        printf("Failed to allocate netlink callbacks\n");
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
    nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check, NULL);    // No sequence checking for multicast messages.

    // Send NL80211_CMD_TRIGGER_SCAN to start the scan. The kernel may reply with NL80211_CMD_NEW_SCAN_RESULTS on
    // success or NL80211_CMD_SCAN_ABORTED if another scan was started by another process.
    err = 1;
    ret = nl_send_auto(socket, msg);    // Send the message.
    printf("NL80211_CMD_TRIGGER_SCAN sent %d bytes to the kernel\n", ret);
    printf("Waiting for scan to complete...\n");
    while (err > 0) ret = nl_recvmsgs(socket, cb);
    if (err < 0) {
    	printf("Warning: err has a value of %d\n", err);
    }
    if (ret < 0) {
    	printf("Error: nl_recvmsgs() returned %d (%s)\n", ret, nl_geterror(-ret));
    	return ret;
    }
    while (!results.done) nl_recvmsgs(socket, cb);    // Now wait until the scan is done or aborted
    if (results.aborted) {
    	printf("Error: Kernel aborted scan\n");
    	return 1;
    }
    printf("Scan is done\n");

    // Cleanup
    nlmsg_free(msg);
    nl_cb_put(cb);
    nl_socket_drop_membership(socket, mcid);    // No longer need this
    return 0;
}

int main(void)
{
	// Use this wireless interface for scanning
	int if_index = if_nametoindex("wlan0");
	// Open socket to kernel
	// Allocate new netlink socket in memory
	struct nl_sock *socket = nl_socket_alloc();
	// Create file descriptor and bind socket
	genl_connect(socket);
    int driver_id = genl_ctrl_resolve(socket, "nl80211");

    // Issue NL80211_CMD_TRIGGER_SCAN to the kernel and wait for it to finish
    int err = do_scan_trigger(socket, if_index, driver_id);
    if (err != 0) {
    	printf("do_scan_trigger() failed with %d\n", err);
    	return err;
    }

    // Now get info for all SSIDs detected.
    struct nl_msg *msg = nlmsg_alloc();    // Allocate a message
    genlmsg_put(msg, 0, 0, driver_id, 0, NLM_F_DUMP, NL80211_CMD_GET_SCAN, 0);    // Setup which command to run
    nla_put_u32(msg, NL80211_ATTR_IFINDEX, if_index);    // Add message attribute, which interface to use
    nl_socket_modify_cb(socket, NL_CB_VALID, NL_CB_CUSTOM, callback_dump, NULL);    // Add the callback
    int ret = nl_send_auto(socket, msg);    // Send the message
    printf("NL80211_CMD_GET_SCAN sent %d bytes to the kernel\n", ret);
    ret = nl_recvmsgs_default(socket);    // Retrieve the kernel's answer (callback_dump() prints SSIDs to stdout)
    nlmsg_free(msg);

    if (ret < 0) {
    	printf("Error: nl_recvmsgs_default() returned %d (%s)\n", ret, nl_geterror(-ret));
    	return ret;
    }

    return 0;
}
