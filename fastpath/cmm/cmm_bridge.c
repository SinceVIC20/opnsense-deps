/*
 * cmm_bridge.c — L2 bridge offload module
 *
 * Three logical sections:
 *   1. Bridge detection and port enumeration (FreeBSD ioctls)
 *   2. L2 flow offload (auto_bridge consumer → CDX programming)
 *   3. CDX timeout callback (hardware flow expiry)
 *
 * Combines the functionality of Linux module_rx.c (L2 flow offload)
 * and ffbridge.c (bridge port resolution).
 *
 * Copyright 2026 Mono Technologies Inc.
 * Copyright (C) 2007 Mindspeed Technologies, Inc.
 * Copyright 2014-2016 Freescale Semiconductor, Inc.
 * Copyright 2017, 2021 NXP
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_bridgevar.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "cmm.h"
#include "cmm_bridge.h"
#include "cmm_itf.h"
#include "cmm_route.h"
#include "libfci.h"

/* L2 flow hash table */
static struct list_head l2flow_table[CMM_L2FLOW_HASH_SIZE];

/* Detected bridges */
static struct list_head bridge_list;

/* auto_bridge device fd */
static int autobridge_fd = -1;

/* Default flow timeout in CDX (seconds) */
#define CMM_L2FLOW_TIMEOUT_DEFAULT	30

/* ------------------------------------------------------------------ */
/* Section 1: Bridge detection and port enumeration                   */
/* ------------------------------------------------------------------ */

/*
 * Is ifname a bridge?  A FreeBSD bridge reports IFT_ETHER, not
 * IFT_BRIDGE, both in the link address and in if_data, so the type
 * cannot tell.  if_clone puts every bridge into the "bridge" interface
 * group on creation, and the group stays across a rename.
 * sd may be -1; a socket is then opened for the query.
 */
int
cmm_bridge_is_bridge(int sd, const char *ifname)
{
	struct ifgroupreq ifgr;
	struct ifg_req *g;
	u_int len, i;
	int own = 0, found = 0;

	if (sd < 0) {
		sd = socket(AF_LOCAL, SOCK_DGRAM, 0);
		if (sd < 0)
			return (0);
		own = 1;
	}

	memset(&ifgr, 0, sizeof(ifgr));
	strlcpy(ifgr.ifgr_name, ifname, sizeof(ifgr.ifgr_name));
	if (ioctl(sd, SIOCGIFGROUP, &ifgr) < 0 || ifgr.ifgr_len == 0)
		goto out;

	len = ifgr.ifgr_len;
	ifgr.ifgr_groups = calloc(1, len);
	if (ifgr.ifgr_groups == NULL)
		goto out;
	if (ioctl(sd, SIOCGIFGROUP, &ifgr) == 0) {
		g = ifgr.ifgr_groups;
		for (i = 0; i + sizeof(*g) <= ifgr.ifgr_len; i += sizeof(*g), g++) {
			if (strcmp(g->ifgrq_group, "bridge") == 0) {
				found = 1;
				break;
			}
		}
	}
	free(ifgr.ifgr_groups);
out:
	if (own)
		close(sd);
	return (found);
}

/*
 * Query bridge member ports via SIOCGDRVSPEC / BRDGGIFS ioctl.
 * Populates br->ports list.
 */
static int
bridge_get_ports(struct cmm_bridge *br, int sd)
{
	struct ifdrv ifd;
	struct ifbifconf bif;
	struct ifbreq *breqs;
	uint32_t len;
	int i, count;

	/* First call with len=0 to get required buffer size */
	memset(&ifd, 0, sizeof(ifd));
	strlcpy(ifd.ifd_name, br->ifname, sizeof(ifd.ifd_name));
	ifd.ifd_cmd = BRDGGIFS;
	ifd.ifd_len = sizeof(bif);
	ifd.ifd_data = &bif;

	memset(&bif, 0, sizeof(bif));
	bif.ifbic_len = 0;
	bif.ifbic_buf = NULL;

	if (ioctl(sd, SIOCGDRVSPEC, &ifd) < 0) {
		cmm_print(CMM_LOG_DEBUG,
		    "bridge: BRDGGIFS size query failed for %s: %s",
		    br->ifname, strerror(errno));
		return (-1);
	}

	len = bif.ifbic_len;
	if (len == 0)
		return (0);	/* no member ports */

	breqs = calloc(1, len);
	if (breqs == NULL)
		return (-1);

	bif.ifbic_len = len;
	bif.ifbic_req = breqs;

	if (ioctl(sd, SIOCGDRVSPEC, &ifd) < 0) {
		cmm_print(CMM_LOG_WARN,
		    "bridge: BRDGGIFS failed for %s: %s",
		    br->ifname, strerror(errno));
		free(breqs);
		return (-1);
	}

	count = bif.ifbic_len / sizeof(struct ifbreq);

	for (i = 0; i < count; i++) {
		struct cmm_bridge_port *port;
		int idx;

		idx = if_nametoindex(breqs[i].ifbr_ifsname);
		if (idx == 0)
			continue;

		port = calloc(1, sizeof(*port));
		if (port == NULL)
			continue;

		strlcpy(port->ifname, breqs[i].ifbr_ifsname,
		    sizeof(port->ifname));
		port->ifindex = idx;
		list_add(&br->ports, &port->entry);

		cmm_print(CMM_LOG_DEBUG,
		    "bridge: %s member port: %s (ifindex %d)",
		    br->ifname, port->ifname, port->ifindex);
	}

	free(breqs);
	return (count);
}

/*
 * Free all port entries from a bridge.
 */
static void
bridge_free_ports(struct cmm_bridge *br)
{
	struct list_head *pos, *tmp;

	for (pos = list_first(&br->ports); pos != &br->ports;) {
		tmp = list_next(pos);
		list_del(pos);
		free(container_of(pos, struct cmm_bridge_port, entry));
		pos = tmp;
	}
}

/*
 * Free all bridges and the bridge ports on a list.
 */
static void
bridge_free_all(struct list_head *list)
{
	struct list_head *pos, *tmp;

	for (pos = list_first(list); pos != list;) {
		tmp = list_next(pos);
		struct cmm_bridge *br = container_of(pos,
		    struct cmm_bridge, entry);
		bridge_free_ports(br);
		list_del(pos);
		free(br);
		pos = tmp;
	}
}

/*
 * Find a bridge record by ifindex.
 */
static struct cmm_bridge *
bridge_find(struct list_head *list, int ifindex)
{
	struct list_head *pos;

	for (pos = list_first(list); pos != list; pos = list_next(pos)) {
		struct cmm_bridge *br = container_of(pos,
		    struct cmm_bridge, entry);
		if (br->ifindex == ifindex)
			return (br);
	}
	return (NULL);
}

/*
 * Find the bridge a member port belongs to, by port name.
 */
static struct cmm_bridge *
bridge_find_port(struct list_head *list, const char *ifname)
{
	struct list_head *bpos, *ppos;

	for (bpos = list_first(list); bpos != list; bpos = list_next(bpos)) {
		struct cmm_bridge *br = container_of(bpos,
		    struct cmm_bridge, entry);

		for (ppos = list_first(&br->ports); ppos != &br->ports;
		    ppos = list_next(ppos)) {
			struct cmm_bridge_port *port = container_of(ppos,
			    struct cmm_bridge_port, entry);
			if (strcmp(port->ifname, ifname) == 0)
				return (br);
		}
	}
	return (NULL);
}

static int
bridge_port_count(struct cmm_bridge *br)
{
	struct list_head *pos;
	int n = 0;

	for (pos = list_first(&br->ports); pos != &br->ports;
	    pos = list_next(pos))
		n++;
	return (n);
}

/*
 * Is ifname a member port of a bridge cmm knows about?
 */
int
cmm_bridge_is_member(const char *ifname)
{
	return (bridge_find_port(&bridge_list, ifname) != NULL);
}

/*
 * Scan the system for bridges and the member ports into list.
 */
static void
bridge_scan(int sd, struct list_head *list)
{
	struct ifaddrs *ifap, *ifa;

	if (getifaddrs(&ifap) < 0) {
		cmm_print(CMM_LOG_ERR, "bridge: getifaddrs: %s",
		    strerror(errno));
		return;
	}

	for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
		struct sockaddr_dl *sdl;
		struct cmm_bridge *br;
		int idx;

		if (ifa->ifa_addr == NULL ||
		    ifa->ifa_addr->sa_family != AF_LINK)
			continue;

		sdl = (struct sockaddr_dl *)ifa->ifa_addr;
		if (!cmm_bridge_is_bridge(sd, ifa->ifa_name))
			continue;

		idx = if_nametoindex(ifa->ifa_name);
		if (idx == 0)
			continue;

		/* Already seen (multiple addrs per interface) */
		if (bridge_find(list, idx) != NULL)
			continue;

		br = calloc(1, sizeof(*br));
		if (br == NULL)
			continue;

		strlcpy(br->ifname, ifa->ifa_name, sizeof(br->ifname));
		br->ifindex = idx;
		if (sdl->sdl_alen == ETHER_ADDR_LEN)
			memcpy(br->macaddr, LLADDR(sdl), ETHER_ADDR_LEN);
		list_head_init(&br->ports);
		list_add(list, &br->entry);

		bridge_get_ports(br, sd);
	}

	freeifaddrs(ifap);
}

/*
 * Query bridge FDB to resolve a destination MAC to a physical
 * member port name.  Used by cmm_fe_route_register() when the
 * output interface is a bridge.
 */
int
cmm_bridge_resolve_port(int bridge_ifindex, const uint8_t *dst_mac,
    char *out_ifname, size_t namelen)
{
	struct ifdrv ifd;
	struct ifbaconf bac;
	struct ifbareq *bareqs;
	char brname[IFNAMSIZ];
	uint32_t len;
	int sd, found, i, count;

	if (if_indextoname(bridge_ifindex, brname) == NULL)
		return (-1);

	sd = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (sd < 0)
		return (-1);

	/* Query FDB size */
	memset(&ifd, 0, sizeof(ifd));
	strlcpy(ifd.ifd_name, brname, sizeof(ifd.ifd_name));
	ifd.ifd_cmd = BRDGRTS;
	ifd.ifd_len = sizeof(bac);
	ifd.ifd_data = &bac;

	memset(&bac, 0, sizeof(bac));
	bac.ifbac_len = 0;
	bac.ifbac_buf = NULL;

	if (ioctl(sd, SIOCGDRVSPEC, &ifd) < 0) {
		close(sd);
		return (-1);
	}

	len = bac.ifbac_len;
	if (len == 0) {
		close(sd);
		return (-1);
	}

	bareqs = calloc(1, len);
	if (bareqs == NULL) {
		close(sd);
		return (-1);
	}

	bac.ifbac_len = len;
	bac.ifbac_req = bareqs;

	if (ioctl(sd, SIOCGDRVSPEC, &ifd) < 0) {
		free(bareqs);
		close(sd);
		return (-1);
	}
	close(sd);

	count = bac.ifbac_len / sizeof(struct ifbareq);
	found = 0;

	for (i = 0; i < count; i++) {
		if (memcmp(bareqs[i].ifba_dst, dst_mac,
		    ETHER_ADDR_LEN) == 0) {
			strlcpy(out_ifname, bareqs[i].ifba_ifsname, namelen);
			found = 1;
			cmm_print(CMM_LOG_DEBUG,
			    "bridge: resolved MAC "
			    "%02x:%02x:%02x:%02x:%02x:%02x → %s "
			    "on %s",
			    dst_mac[0], dst_mac[1], dst_mac[2],
			    dst_mac[3], dst_mac[4], dst_mac[5],
			    out_ifname, brname);
			break;
		}
	}

	free(bareqs);
	return (found ? 0 : -1);
}

static void l2flow_flush_port(struct cmm_global *g, const char *ifname);

/*
 * Tell CDX that a port joined (on = 1) or left (on = 0) a bridge:
 * L2 bridging on the port input, and the bridged state with the bridge's MAC.
 */
static void
bridge_port_notify(struct cmm_global *g, const char *ifname,
    const uint8_t *br_mac, int on)
{
	fpp_l2_bridge_enable_cmd_t ecmd;
	fpp_bridged_itf_cmd_t bcmd;

	memset(&ecmd, 0, sizeof(ecmd));
	ecmd.enable_flag = on ? 1 : 0;
	strlcpy(ecmd.input_name, ifname, sizeof(ecmd.input_name));
	if (fci_write(g->fci_handle, FPP_CMD_RX_L2BRIDGE_ENABLE,
	    sizeof(ecmd), (unsigned short *)&ecmd) != 0)
		cmm_print(CMM_LOG_WARN, "bridge: L2BRIDGE_ENABLE %d "
		    "failed for %s", on, ifname);

	memset(&bcmd, 0, sizeof(bcmd));
	strlcpy(bcmd.ifname, ifname, sizeof(bcmd.ifname));
	bcmd.is_bridged = on ? 1 : 0;
	if (on)
		memcpy(bcmd.br_macaddr, br_mac, ETHER_ADDR_LEN);
	if (fci_write(g->fci_handle, FPP_CMD_BRIDGED_ITF_UPDATE,
	    sizeof(bcmd), (unsigned short *)&bcmd) != 0)
		cmm_print(CMM_LOG_WARN, "bridge: BRIDGED_ITF_UPDATE %d "
		    "failed for %s", on, ifname);
}

/*
 * Read the bridges again and bring CDX in line with what changed since
 * the last read.  Called at start and on every interface message for a
 * bridge or a port that is or was a member, so a bridge set up at boot
 * and one built or changed later end in the same state.  Only changes
 * are sent: a port that joined is enabled, a port that left is disabled
 * and the port's L2 flows leave the hardware (the FMan would otherwise go on
 * bridging frames between ports that are no longer bridged).
 */
void
cmm_bridge_itf_update(struct cmm_global *g)
{
	struct list_head old, *bpos, *ppos;
	int sd;

	sd = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (sd < 0)
		return;

	/* The previous read becomes "old"; read the system into bridge_list. */
	list_head_init(&old);
	if (!list_empty(&bridge_list)) {
		old.next = bridge_list.next;
		old.prev = bridge_list.prev;
		old.next->prev = &old;
		old.prev->next = &old;
		list_head_init(&bridge_list);
	}
	bridge_scan(sd, &bridge_list);
	close(sd);

	/* Bridges and ports that are new, or whose bridge MAC changed. */
	for (bpos = list_first(&bridge_list); bpos != &bridge_list;
	    bpos = list_next(bpos)) {
		struct cmm_bridge *br = container_of(bpos,
		    struct cmm_bridge, entry);
		struct cmm_bridge *obr = bridge_find(&old, br->ifindex);
		int changed = (obr == NULL ||
		    memcmp(obr->macaddr, br->macaddr, ETHER_ADDR_LEN) != 0 ||
		    bridge_port_count(obr) != bridge_port_count(br));

		if (obr == NULL)
			cmm_print(CMM_LOG_INFO, "bridge: detected %s "
			    "(ifindex %d, mac %02x:%02x:%02x:%02x:%02x:%02x)",
			    br->ifname, br->ifindex,
			    br->macaddr[0], br->macaddr[1], br->macaddr[2],
			    br->macaddr[3], br->macaddr[4], br->macaddr[5]);

		for (ppos = list_first(&br->ports); ppos != &br->ports;
		    ppos = list_next(ppos)) {
			struct cmm_bridge_port *port = container_of(ppos,
			    struct cmm_bridge_port, entry);
			struct cmm_bridge *was = bridge_find_port(&old,
			    port->ifname);

			if (was != NULL && was->ifindex == br->ifindex &&
			    memcmp(was->macaddr, br->macaddr,
			    ETHER_ADDR_LEN) == 0)
				continue;	/* unchanged */
			if (was != NULL && was->ifindex != br->ifindex)
				l2flow_flush_port(g, port->ifname);
			cmm_print(CMM_LOG_INFO, "bridge: %s port %s %s",
			    br->ifname, port->ifname,
			    was == NULL ? "added" : "updated");
			bridge_port_notify(g, port->ifname, br->macaddr, 1);
			changed = 1;
		}

		/*
		 * Routes out of this bridge were registered with a member
		 * port and the bridge's MAC as before: register those again.
		 */
		if (changed && obr != NULL)
			cmm_route_invalidate_by_oif(g, br->ifindex);
	}

	/* Ports that left a bridge, or whose bridge is gone. */
	for (bpos = list_first(&old); bpos != &old; bpos = list_next(bpos)) {
		struct cmm_bridge *obr = container_of(bpos,
		    struct cmm_bridge, entry);

		for (ppos = list_first(&obr->ports); ppos != &obr->ports;
		    ppos = list_next(ppos)) {
			struct cmm_bridge_port *port = container_of(ppos,
			    struct cmm_bridge_port, entry);

			if (bridge_find_port(&bridge_list, port->ifname) != NULL)
				continue;
			cmm_print(CMM_LOG_INFO, "bridge: %s port %s removed",
			    obr->ifname, port->ifname);
			l2flow_flush_port(g, port->ifname);
			bridge_port_notify(g, port->ifname, NULL, 0);
		}
		if (bridge_find(&bridge_list, obr->ifindex) == NULL)
			cmm_print(CMM_LOG_INFO, "bridge: %s gone",
			    obr->ifname);
	}

	bridge_free_all(&old);
}

/* ------------------------------------------------------------------ */
/* Section 2: L2 flow offload (auto_bridge consumer)                  */
/* ------------------------------------------------------------------ */

static inline unsigned int
l2flow_hash(const struct abm_l2flow *flow)
{
	return (jhash(flow, sizeof(*flow), 0x12345678) &
	    (CMM_L2FLOW_HASH_SIZE - 1));
}

static struct cmm_l2flow *
l2flow_find(const struct abm_l2flow *flow)
{
	struct list_head *bucket, *pos;
	unsigned int h;

	h = l2flow_hash(flow);
	bucket = &l2flow_table[h];

	for (pos = list_first(bucket); pos != bucket;
	    pos = list_next(pos)) {
		struct cmm_l2flow *entry = container_of(pos,
		    struct cmm_l2flow, hash_entry);
		if (memcmp(&entry->flow, flow, sizeof(*flow)) == 0)
			return (entry);
	}
	return (NULL);
}

static struct cmm_l2flow *
l2flow_add(const struct abm_l2flow *flow)
{
	struct cmm_l2flow *entry;
	unsigned int h;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return (NULL);

	memcpy(&entry->flow, flow, sizeof(*flow));
	h = l2flow_hash(flow);
	list_add(&l2flow_table[h], &entry->hash_entry);

	return (entry);
}

static void
l2flow_del(struct cmm_l2flow *entry)
{
	list_del(&entry->hash_entry);
	free(entry);
}

/*
 * Deregister a single L2 flow from CDX.
 */
static int
l2flow_fpp_deregister(struct cmm_global *g, struct cmm_l2flow *entry)
{
	fpp_l2_bridge_flow_entry_cmd_t cmd;
	int rc;

	if (!entry->fpp_programmed)
		return (0);

	memset(&cmd, 0, sizeof(cmd));
	cmd.action = FPP_ACTION_DEREGISTER;
	memcpy(cmd.srcaddr, entry->flow.saddr, ETHER_ADDR_LEN);
	memcpy(cmd.destaddr, entry->flow.daddr, ETHER_ADDR_LEN);
	cmd.ethertype = entry->flow.ethertype;
	cmd.svlan_tag = entry->flow.svlan_tag;
	cmd.cvlan_tag = entry->flow.cvlan_tag;
	cmd.session_id = entry->flow.session_id;
	strlcpy(cmd.input_name, entry->input_name,
	    sizeof(cmd.input_name));

	rc = fci_write(g->fci_handle, FPP_CMD_RX_L2FLOW_ENTRY,
	    sizeof(cmd), (unsigned short *)&cmd);
	if (rc != 0) {
		cmm_print(CMM_LOG_WARN,
		    "bridge: L2FLOW deregister failed: %d", rc);
	}

	entry->fpp_programmed = 0;
	return (rc == 0 ? 0 : -1);
}

/*
 * Register or update a L2 flow in CDX.
 */
static int
l2flow_fpp_register(struct cmm_global *g, struct cmm_l2flow *entry,
    int update)
{
	fpp_l2_bridge_flow_entry_cmd_t cmd;
	int rc;

	memset(&cmd, 0, sizeof(cmd));

	if (update && entry->fpp_programmed)
		cmd.action = FPP_ACTION_UPDATE;
	else
		cmd.action = FPP_ACTION_REGISTER;

	cmd.ethertype = entry->flow.ethertype;
	memcpy(cmd.destaddr, entry->flow.daddr, ETHER_ADDR_LEN);
	memcpy(cmd.srcaddr, entry->flow.saddr, ETHER_ADDR_LEN);
	cmd.svlan_tag = entry->flow.svlan_tag;
	cmd.cvlan_tag = entry->flow.cvlan_tag;
	cmd.session_id = entry->flow.session_id;
	cmd.mark = entry->mark;

	/* L3/L4 optional fields */
	cmd.proto = entry->flow.proto;
	memcpy(cmd.saddr, entry->flow.sip, sizeof(cmd.saddr));
	memcpy(cmd.daddr, entry->flow.dip, sizeof(cmd.daddr));
	cmd.sport = entry->flow.sport;
	cmd.dport = entry->flow.dport;

	strlcpy(cmd.input_name, entry->input_name,
	    sizeof(cmd.input_name));
	strlcpy(cmd.output_name, entry->output_name,
	    sizeof(cmd.output_name));

	rc = fci_write(g->fci_handle, FPP_CMD_RX_L2FLOW_ENTRY,
	    sizeof(cmd), (unsigned short *)&cmd);

	if (rc == 0 || rc == FPP_ERR_BRIDGE_ENTRY_ALREADY_EXISTS) {
		entry->fpp_programmed = 1;
		cmm_print(CMM_LOG_DEBUG,
		    "bridge: L2FLOW %s "
		    "%02x:%02x:%02x:%02x:%02x:%02x -> "
		    "%02x:%02x:%02x:%02x:%02x:%02x "
		    "etype=0x%04x in=%s out=%s",
		    cmd.action == FPP_ACTION_REGISTER ?
		    "registered" : "updated",
		    entry->flow.saddr[0], entry->flow.saddr[1],
		    entry->flow.saddr[2], entry->flow.saddr[3],
		    entry->flow.saddr[4], entry->flow.saddr[5],
		    entry->flow.daddr[0], entry->flow.daddr[1],
		    entry->flow.daddr[2], entry->flow.daddr[3],
		    entry->flow.daddr[4], entry->flow.daddr[5],
		    ntohs(entry->flow.ethertype),
		    entry->input_name, entry->output_name);
		return (0);
	}

	cmm_print(CMM_LOG_WARN,
	    "bridge: L2FLOW register failed: %d", rc);
	return (-1);
}

/*
 * Send a response back to auto_bridge.ko via /dev/autobridge.
 */
static void
autobridge_respond(const struct abm_l2flow *flow, uint32_t flags)
{
	struct abm_response resp;

	if (autobridge_fd < 0)
		return;

	memset(&resp, 0, sizeof(resp));
	memcpy(&resp.flow, flow, sizeof(resp.flow));
	resp.flags = flags;

	if (write(autobridge_fd, &resp, sizeof(resp)) !=
	    (ssize_t)sizeof(resp)) {
		cmm_print(CMM_LOG_WARN,
		    "bridge: write to autobridge failed: %s",
		    strerror(errno));
	}
}

/*
 * Flush all L2 flows — deregister from CDX and free entries.
 */
static void
l2flow_flush_all(struct cmm_global *g)
{
	struct list_head *pos, *tmp;
	int i;

	for (i = 0; i < CMM_L2FLOW_HASH_SIZE; i++) {
		for (pos = list_first(&l2flow_table[i]);
		    pos != &l2flow_table[i];) {
			tmp = list_next(pos);
			struct cmm_l2flow *entry = container_of(pos,
			    struct cmm_l2flow, hash_entry);
			l2flow_fpp_deregister(g, entry);
			l2flow_del(entry);
			pos = tmp;
		}
	}
}

/*
 * Take every L2 flow that enters or leaves ifname out of the hardware,
 * and tell auto_bridge to forget the flow, so frames go back through the
 * software bridge (which applies the new membership) and a flow that
 * is still bridged is offloaded again from the next frame.
 */
static void
l2flow_flush_port(struct cmm_global *g, const char *ifname)
{
	struct list_head *pos, *tmp;
	int i, n = 0;

	for (i = 0; i < CMM_L2FLOW_HASH_SIZE; i++) {
		for (pos = list_first(&l2flow_table[i]);
		    pos != &l2flow_table[i];) {
			tmp = list_next(pos);
			struct cmm_l2flow *entry = container_of(pos,
			    struct cmm_l2flow, hash_entry);
			if (strcmp(entry->input_name, ifname) == 0 ||
			    strcmp(entry->output_name, ifname) == 0) {
				l2flow_fpp_deregister(g, entry);
				autobridge_respond(&entry->flow,
				    ABM_FLAG_REMOVED | ABM_FLAG_ACK);
				l2flow_del(entry);
				n++;
			}
			pos = tmp;
		}
	}
	if (n > 0)
		cmm_print(CMM_LOG_INFO, "bridge: %d L2 flow(s) on %s "
		    "taken out of the hardware", n, ifname);
}

/*
 * A learned address moved to another member port: every L2 flow to or
 * from that address is programmed for the old port.  Take those out of the
 * hardware (the next frames offload those flows again, through
 * the new port), and re-offload routed connections to that address.
 */
static void
handle_fdb_move(struct cmm_global *g, const struct abm_event *ev)
{
	const uint8_t *mac = ev->flow.saddr;
	struct list_head *pos, *tmp;
	int i, n = 0;

	for (i = 0; i < CMM_L2FLOW_HASH_SIZE; i++) {
		for (pos = list_first(&l2flow_table[i]);
		    pos != &l2flow_table[i];) {
			tmp = list_next(pos);
			struct cmm_l2flow *entry = container_of(pos,
			    struct cmm_l2flow, hash_entry);
			if (memcmp(entry->flow.saddr, mac, ETHER_ADDR_LEN) == 0 ||
			    memcmp(entry->flow.daddr, mac, ETHER_ADDR_LEN) == 0) {
				l2flow_fpp_deregister(g, entry);
				autobridge_respond(&entry->flow,
				    ABM_FLAG_REMOVED | ABM_FLAG_ACK);
				l2flow_del(entry);
				n++;
			}
			pos = tmp;
		}
	}

	cmm_print(CMM_LOG_INFO, "bridge: %02x:%02x:%02x:%02x:%02x:%02x moved "
	    "from ifindex %u to %u, %d L2 flow(s) taken out of the hardware",
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
	    ev->iif_index, ev->oif_index, n);

	cmm_route_invalidate_by_mac(g, mac);
}

/*
 * Handle a FLOW_NEW event from auto_bridge.
 */
static void
handle_flow_new(struct cmm_global *g, const struct abm_event *ev)
{
	struct cmm_l2flow *entry;
	struct cmm_interface *iif, *oif;

	/* Resolve interface indexes to names */
	iif = cmm_itf_find_by_index(ev->iif_index);
	oif = cmm_itf_find_by_index(ev->oif_index);

	if (iif == NULL || oif == NULL) {
		cmm_print(CMM_LOG_WARN,
		    "bridge: flow_new unknown iif=%u or oif=%u",
		    ev->iif_index, ev->oif_index);
		autobridge_respond(&ev->flow,
		    ABM_FLAG_DENIED | ABM_FLAG_ACK);
		return;
	}

	/* Check for existing entry */
	entry = l2flow_find(&ev->flow);
	if (entry != NULL) {
		/* Update if interfaces changed */
		if (strcmp(entry->input_name, iif->ifname) != 0 ||
		    strcmp(entry->output_name, oif->ifname) != 0) {
			strlcpy(entry->input_name, iif->ifname,
			    sizeof(entry->input_name));
			strlcpy(entry->output_name, oif->ifname,
			    sizeof(entry->output_name));
			entry->mark = ev->mark;
			l2flow_fpp_register(g, entry, 1);
		}
		autobridge_respond(&ev->flow,
		    ABM_FLAG_OFFLOADED | ABM_FLAG_ACK);
		return;
	}

	/* Create new entry */
	entry = l2flow_add(&ev->flow);
	if (entry == NULL) {
		autobridge_respond(&ev->flow,
		    ABM_FLAG_DENIED | ABM_FLAG_ACK);
		return;
	}

	strlcpy(entry->input_name, iif->ifname,
	    sizeof(entry->input_name));
	strlcpy(entry->output_name, oif->ifname,
	    sizeof(entry->output_name));
	entry->mark = ev->mark;

	/* Program CDX */
	if (l2flow_fpp_register(g, entry, 0) == 0) {
		autobridge_respond(&ev->flow,
		    ABM_FLAG_OFFLOADED | ABM_FLAG_ACK);
	} else {
		autobridge_respond(&ev->flow,
		    ABM_FLAG_DENIED | ABM_FLAG_ACK);
		l2flow_del(entry);
	}
}

/*
 * Handle a FLOW_UPDATE event from auto_bridge.
 */
static void
handle_flow_update(struct cmm_global *g, const struct abm_event *ev)
{
	struct cmm_l2flow *entry;
	struct cmm_interface *iif, *oif;

	entry = l2flow_find(&ev->flow);
	if (entry == NULL) {
		/* Treat as new */
		handle_flow_new(g, ev);
		return;
	}

	iif = cmm_itf_find_by_index(ev->iif_index);
	oif = cmm_itf_find_by_index(ev->oif_index);

	if (iif == NULL || oif == NULL) {
		cmm_print(CMM_LOG_WARN,
		    "bridge: flow_update unknown interfaces");
		return;
	}

	/* Update interfaces if changed */
	if (strcmp(entry->input_name, iif->ifname) != 0 ||
	    strcmp(entry->output_name, oif->ifname) != 0 ||
	    entry->mark != ev->mark) {
		strlcpy(entry->input_name, iif->ifname,
		    sizeof(entry->input_name));
		strlcpy(entry->output_name, oif->ifname,
		    sizeof(entry->output_name));
		entry->mark = ev->mark;
		l2flow_fpp_register(g, entry, 1);
	}

	autobridge_respond(&ev->flow,
	    ABM_FLAG_OFFLOADED | ABM_FLAG_ACK);
}

/*
 * Handle a FLOW_DEL event from auto_bridge.
 */
static void
handle_flow_del(struct cmm_global *g, const struct abm_event *ev)
{
	struct cmm_l2flow *entry;

	entry = l2flow_find(&ev->flow);
	if (entry == NULL) {
		cmm_print(CMM_LOG_DEBUG,
		    "bridge: flow_del for unknown flow");
		autobridge_respond(&ev->flow, ABM_FLAG_ACK);
		return;
	}

	l2flow_fpp_deregister(g, entry);
	l2flow_del(entry);

	autobridge_respond(&ev->flow, ABM_FLAG_ACK);

	cmm_print(CMM_LOG_DEBUG,
	    "bridge: flow deleted "
	    "%02x:%02x:%02x:%02x:%02x:%02x -> "
	    "%02x:%02x:%02x:%02x:%02x:%02x",
	    ev->flow.saddr[0], ev->flow.saddr[1],
	    ev->flow.saddr[2], ev->flow.saddr[3],
	    ev->flow.saddr[4], ev->flow.saddr[5],
	    ev->flow.daddr[0], ev->flow.daddr[1],
	    ev->flow.daddr[2], ev->flow.daddr[3],
	    ev->flow.daddr[4], ev->flow.daddr[5]);
}

/*
 * Handle a RESET event from auto_bridge.
 */
static void
handle_reset(struct cmm_global *g)
{
	int rc;

	cmm_print(CMM_LOG_INFO, "bridge: reset — flushing all flows");

	rc = fci_write(g->fci_handle, FPP_CMD_RX_L2BRIDGE_FLOW_RESET,
	    0, NULL);
	if (rc != 0)
		cmm_print(CMM_LOG_WARN,
		    "bridge: FLOW_RESET failed: %d", rc);

	l2flow_flush_all(g);
}

/*
 * Read and dispatch events from /dev/autobridge.
 * Called from the kqueue event loop when the fd is readable.
 */
void
cmm_bridge_event(struct cmm_global *g)
{
	struct abm_event ev;
	ssize_t n;

	for (;;) {
		n = read(autobridge_fd, &ev, sizeof(ev));
		if (n != (ssize_t)sizeof(ev)) {
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (n == 0) {
				cmm_print(CMM_LOG_WARN,
				    "bridge: /dev/autobridge closed");
				break;
			}
			cmm_print(CMM_LOG_WARN,
			    "bridge: short read from autobridge: %zd", n);
			break;
		}

		switch (ev.type) {
		case ABM_EVENT_FLOW_NEW:
			handle_flow_new(g, &ev);
			break;
		case ABM_EVENT_FLOW_UPDATE:
			handle_flow_update(g, &ev);
			break;
		case ABM_EVENT_FLOW_DEL:
			handle_flow_del(g, &ev);
			break;
		case ABM_EVENT_RESET:
			handle_reset(g);
			break;
		case ABM_EVENT_FDB_MOVE:
			handle_fdb_move(g, &ev);
			break;
		default:
			cmm_print(CMM_LOG_WARN,
			    "bridge: unknown event type %u", ev.type);
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Section 3: CDX timeout callback                                    */
/* ------------------------------------------------------------------ */

/*
 * FCI event handler for FPP_CMD_RX_L2FLOW_ENTRY.
 * CDX sends ACTION_REMOVED when a flow saw no traffic for the flow
 * timeout.  CDX only reports the timeout: the hardware entry stays until
 * deregistered (as in ASK's cmm, __cmm_l2flow_deregister()).  So send
 * DEREGISTER, by the tuple alone, also for a flow this cmm never
 * registered (left from a cmm that ran before); then drop the record
 * and tell auto_bridge the flow is gone, so a flow that comes back is
 * seen as new and offloaded again.
 *
 * Called from the top-level FCI event dispatcher in cmm.c.
 * Returns FCI_CB_CONTINUE for unrecognized events.
 */
int
cmm_bridge_fci_event(unsigned short fcode, unsigned short len,
    unsigned short *payload)
{
	fpp_l2_bridge_flow_entry_cmd_t *cmd;
	struct abm_l2flow flow;
	struct cmm_l2flow *entry;

	if (fcode != FPP_CMD_RX_L2FLOW_ENTRY)
		return (FCI_CB_CONTINUE);

	if (len < sizeof(*cmd))
		return (FCI_CB_CONTINUE);

	cmd = (fpp_l2_bridge_flow_entry_cmd_t *)payload;

	if (cmd->action != FPP_ACTION_REMOVED)
		return (FCI_CB_CONTINUE);

	cmm_print(CMM_LOG_DEBUG,
	    "bridge: CDX removed flow "
	    "%02x:%02x:%02x:%02x:%02x:%02x -> "
	    "%02x:%02x:%02x:%02x:%02x:%02x (timeout)",
	    cmd->srcaddr[0], cmd->srcaddr[1],
	    cmd->srcaddr[2], cmd->srcaddr[3],
	    cmd->srcaddr[4], cmd->srcaddr[5],
	    cmd->destaddr[0], cmd->destaddr[1],
	    cmd->destaddr[2], cmd->destaddr[3],
	    cmd->destaddr[4], cmd->destaddr[5]);

	/*
	 * Build the flow key to look up in our hash table.
	 * The CDX command contains the same fields we need.
	 */
	memset(&flow, 0, sizeof(flow));
	memcpy(flow.saddr, cmd->srcaddr, ETHER_ADDR_LEN);
	memcpy(flow.daddr, cmd->destaddr, ETHER_ADDR_LEN);
	flow.ethertype = cmd->ethertype;
	flow.svlan_tag = cmd->svlan_tag;
	flow.cvlan_tag = cmd->cvlan_tag;
	flow.session_id = cmd->session_id;
	flow.proto = cmd->proto;
	flow.sport = cmd->sport;
	flow.dport = cmd->dport;
	memcpy(flow.sip, cmd->saddr, sizeof(flow.sip));
	memcpy(flow.dip, cmd->daddr, sizeof(flow.dip));

	{
		fpp_l2_bridge_flow_entry_cmd_t dcmd;
		int rc;

		memset(&dcmd, 0, sizeof(dcmd));
		dcmd.action = FPP_ACTION_DEREGISTER;
		memcpy(dcmd.srcaddr, cmd->srcaddr, ETHER_ADDR_LEN);
		memcpy(dcmd.destaddr, cmd->destaddr, ETHER_ADDR_LEN);
		dcmd.ethertype = cmd->ethertype;
		dcmd.svlan_tag = cmd->svlan_tag;
		dcmd.cvlan_tag = cmd->cvlan_tag;
		dcmd.session_id = cmd->session_id;
		rc = fci_write(cmm_g.fci_handle, FPP_CMD_RX_L2FLOW_ENTRY,
		    sizeof(dcmd), (unsigned short *)&dcmd);
		if (rc != 0 && rc != FPP_ERR_BRIDGE_ENTRY_NOT_FOUND)
			cmm_print(CMM_LOG_WARN,
			    "bridge: deregister after CDX timeout failed: %d",
			    rc);
	}

	entry = l2flow_find(&flow);
	if (entry != NULL) {
		entry->fpp_programmed = 0;	/* deregistered above */
		l2flow_del(entry);
	}

	/* auto_bridge drops the flow; the next frame starts a new one. */
	autobridge_respond(&flow, ABM_FLAG_REMOVED | ABM_FLAG_ACK);

	return (FCI_CB_CONTINUE);
}

/* ------------------------------------------------------------------ */
/* Module lifecycle                                                    */
/* ------------------------------------------------------------------ */

int
cmm_bridge_init(struct cmm_global *g)
{
	fpp_l2_bridge_control_cmd_t ctrl;
	int i;

	/* Initialize hash table */
	for (i = 0; i < CMM_L2FLOW_HASH_SIZE; i++)
		list_head_init(&l2flow_table[i]);

	list_head_init(&bridge_list);

	/*
	 * L2 flows a cmm that ran before left in the hardware have no
	 * record here and would never be deregistered: clear those flows.
	 */
	if (fci_write(g->fci_handle, FPP_CMD_RX_L2BRIDGE_FLOW_RESET,
	    0, NULL) != 0)
		cmm_print(CMM_LOG_WARN, "bridge: FLOW_RESET at start failed");

	/* Open /dev/autobridge */
	autobridge_fd = open(ABM_DEV_PATH, O_RDWR | O_NONBLOCK);
	if (autobridge_fd < 0) {
		cmm_print(CMM_LOG_WARN,
		    "bridge: open %s: %s — running in manual mode",
		    ABM_DEV_PATH, strerror(errno));

		/* Manual mode: still enable bridge in CDX without
		 * auto flow detection */
		ctrl.mode_timeout = FPP_L2_BRIDGE_MODE_MANUAL;
		fci_write(g->fci_handle, FPP_CMD_RX_L2BRIDGE_MODE,
		    sizeof(ctrl), (unsigned short *)&ctrl);

		cmm_bridge_itf_update(g);

		cmm_print(CMM_LOG_INFO,
		    "bridge: initialized (manual mode)");
		return (0);
	}

	/* Store fd in global state for kqueue registration */
	g->autobridge_fd = autobridge_fd;

	/* First read: every bridge and port found is new to CDX. */
	cmm_bridge_itf_update(g);

	/* Set auto mode */
	ctrl.mode_timeout = FPP_L2_BRIDGE_MODE_AUTO;
	if (fci_write(g->fci_handle, FPP_CMD_RX_L2BRIDGE_MODE,
	    sizeof(ctrl), (unsigned short *)&ctrl) != 0) {
		cmm_print(CMM_LOG_WARN,
		    "bridge: failed to set AUTO mode");
	} else {
		cmm_print(CMM_LOG_INFO,
		    "bridge: set AUTO mode in CDX");
	}

	/* Set default flow timeout */
	ctrl.mode_timeout = CMM_L2FLOW_TIMEOUT_DEFAULT;
	if (fci_write(g->fci_handle, FPP_CMD_RX_L2BRIDGE_FLOW_TIMEOUT,
	    sizeof(ctrl), (unsigned short *)&ctrl) != 0) {
		cmm_print(CMM_LOG_WARN,
		    "bridge: failed to set flow timeout");
	} else {
		cmm_print(CMM_LOG_INFO,
		    "bridge: set flow timeout to %ds",
		    CMM_L2FLOW_TIMEOUT_DEFAULT);
	}

	cmm_print(CMM_LOG_INFO,
	    "bridge: initialized (auto mode, fd=%d)", autobridge_fd);
	return (0);
}

void
cmm_bridge_fini(struct cmm_global *g)
{
	/* Deregister all flows from CDX */
	l2flow_flush_all(g);

	/* Free bridge records */
	bridge_free_all(&bridge_list);

	/* Close autobridge device */
	if (autobridge_fd >= 0) {
		close(autobridge_fd);
		autobridge_fd = -1;
		g->autobridge_fd = -1;
	}

	cmm_print(CMM_LOG_INFO, "bridge: shut down");
}
