/*
 * cmm_vlan.c — VLAN interface offload
 *
 * Detects 802.1Q VLAN interfaces and registers them with CDX
 * via FPP_CMD_VLAN_ENTRY so the hardware knows the VLAN-to-port
 * mapping for flow classification.
 *
 * Copyright 2026 Mono Technologies Inc.
 * Copyright (C) 2007 Mindspeed Technologies, Inc.
 * Copyright 2014-2016 Freescale Semiconductor, Inc.
 * Copyright 2017, 2021 NXP
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_vlan_var.h>
#include <string.h>

#include "cmm.h"
#include "cmm_vlan.h"
#include "cmm_itf.h"
#include "cmm_route.h"

static int
cmm_vlan_register(struct cmm_global *g, struct cmm_interface *itf)
{
	fpp_vlan_cmd_t cmd;
	struct cmm_interface *parent;
	int rc;

	if (!(itf->itf_flags & ITF_F_VLAN))
		return (0);
	if (itf->itf_flags & ITF_F_FPP_VLAN)
		return (0);
	/* CDX only inserts 802.1Q tags, so 802.1ad VLANs stay with PF. */
	if (itf->itf_flags & ITF_F_VLAN_QINQ)
		return (0);

	parent = cmm_itf_find_by_index(itf->parent_ifindex);
	if (parent == NULL) {
		cmm_print(CMM_LOG_WARN,
		    "vlan: %s parent idx=%d not found",
		    itf->ifname, itf->parent_ifindex);
		return (-1);
	}
	/* CDX links the VLAN to its parent's entry, so a LAGG parent must
	 * be registered first.  cmm_vlan_register_children() catches up. */
	if ((parent->itf_flags & ITF_F_LAGG) &&
	    !(parent->itf_flags & ITF_F_FPP_LAGG)) {
		cmm_print(CMM_LOG_DEBUG,
		    "vlan: %s waiting for %s to register",
		    itf->ifname, parent->ifname);
		return (0);
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.action = FPP_ACTION_REGISTER;
	cmd.vlan_id = itf->vlan_id;
	strlcpy(cmd.vlan_ifname, itf->ifname, sizeof(cmd.vlan_ifname));
	strlcpy(cmd.vlan_phy_ifname, parent->ifname,
	    sizeof(cmd.vlan_phy_ifname));
#if defined(LS1043)
	memcpy(cmd.macaddr, itf->macaddr, ETHER_ADDR_LEN);
#endif

	rc = fci_write(g->fci_handle, FPP_CMD_VLAN_ENTRY,
	    sizeof(cmd), (unsigned short *)&cmd);
	if (rc == FPP_ERR_VLAN_ENTRY_ALREADY_REGISTERED) {
		cmm_print(CMM_LOG_DEBUG,
		    "vlan: %s already in CDX, reusing", itf->ifname);
	} else if (rc != 0) {
		cmm_print(CMM_LOG_WARN, "vlan: register %s failed: %d",
		    itf->ifname, rc);
		return (-1);
	}

	itf->itf_flags |= ITF_F_FPP_VLAN;
	cmm_print(CMM_LOG_INFO,
	    "vlan: registered %s (vid=%u parent=%s)",
	    itf->ifname, itf->vlan_id, parent->ifname);

	return (0);
}

static int
cmm_vlan_deregister(struct cmm_global *g, struct cmm_interface *itf)
{
	fpp_vlan_cmd_t cmd;
	struct cmm_interface *parent;
	int rc;

	if (!(itf->itf_flags & ITF_F_FPP_VLAN))
		return (0);

	/* CDX drops the VLAN's flows with it; tear down ours to match. */
	cmm_route_invalidate_by_oif(g, itf->ifindex);

	parent = cmm_itf_find_by_index(itf->parent_ifindex);

	memset(&cmd, 0, sizeof(cmd));
	cmd.action = FPP_ACTION_DEREGISTER;
	cmd.vlan_id = itf->vlan_id;
	strlcpy(cmd.vlan_ifname, itf->ifname, sizeof(cmd.vlan_ifname));
	if (parent != NULL)
		strlcpy(cmd.vlan_phy_ifname, parent->ifname,
		    sizeof(cmd.vlan_phy_ifname));

	rc = fci_write(g->fci_handle, FPP_CMD_VLAN_ENTRY,
	    sizeof(cmd), (unsigned short *)&cmd);
	if (rc != 0 && rc != FPP_ERR_VLAN_ENTRY_NOT_FOUND)
		cmm_print(CMM_LOG_WARN, "vlan: deregister %s failed: %d",
		    itf->ifname, rc);

	itf->itf_flags &= ~ITF_F_FPP_VLAN;
	cmm_print(CMM_LOG_INFO, "vlan: deregistered %s (vid=%u)",
	    itf->ifname, itf->vlan_id);

	return (0);
}

int
cmm_vlan_init(struct cmm_global *g)
{
	int rc;

	/* Reset all VLAN entries in CDX */
	rc = fci_write(g->fci_handle, FPP_CMD_VLAN_RESET, 0, NULL);
	if (rc != 0)
		cmm_print(CMM_LOG_WARN, "vlan: reset failed: %d", rc);

	/* Register all existing UP VLAN interfaces */
	cmm_itf_foreach_vlan(g, cmm_vlan_register);

	cmm_print(CMM_LOG_INFO, "vlan: initialized");
	return (0);
}

void
cmm_vlan_fini(struct cmm_global *g)
{
	cmm_itf_foreach_vlan(g, cmm_vlan_deregister);
}

/*
 * OPNsense tags a new VLAN after creating it and changes a VLAN's tag or
 * parent in place, and the kernel sends no message for either.  Read the
 * VLAN again and replace a stale CDX entry, or offloaded packets keep the
 * old tag or port.
 */
int
cmm_vlan_recheck(struct cmm_global *g, struct cmm_interface *itf)
{
	static int sd = -1;
	struct ifreq ifr;
	struct vlanreq vlr;
	int parent_ifindex;

	if (!(itf->itf_flags & ITF_F_VLAN_DEV))
		return (0);
	if (sd < 0 && (sd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return (0);

	memset(&ifr, 0, sizeof(ifr));
	memset(&vlr, 0, sizeof(vlr));
	strlcpy(ifr.ifr_name, itf->ifname, sizeof(ifr.ifr_name));
	ifr.ifr_data = (caddr_t)&vlr;
	if (ioctl(sd, SIOCGETVLAN, &ifr) < 0)
		return (0);

	parent_ifindex = vlr.vlr_tag != 0 ? (int)if_nametoindex(vlr.vlr_parent) : 0;
	if (vlr.vlr_tag == 0 ? !(itf->itf_flags & ITF_F_VLAN) :
	    ((itf->itf_flags & ITF_F_VLAN) && itf->vlan_id == vlr.vlr_tag &&
	    itf->parent_ifindex == parent_ifindex &&
	    itf->vlan_proto == vlr.vlr_proto))
		return (0);

	cmm_print(CMM_LOG_INFO,
	    "vlan: %s changed: vid %u -> %u, parent idx %d -> %d (%s)",
	    itf->ifname, itf->vlan_id, vlr.vlr_tag, itf->parent_ifindex,
	    parent_ifindex, vlr.vlr_tag != 0 ? vlr.vlr_parent : "none");

	/* Remove the old entry while the old tag and parent are known. */
	cmm_vlan_deregister(g, itf);

	itf->itf_flags &= ~(ITF_F_VLAN | ITF_F_VLAN_QINQ);
	itf->vlan_id = vlr.vlr_tag;
	itf->vlan_proto = vlr.vlr_proto;
	itf->parent_ifindex = parent_ifindex;
	if (vlr.vlr_tag != 0) {
		itf->itf_flags |= ITF_F_VLAN;
		if (vlr.vlr_proto == ETHERTYPE_QINQ)
			itf->itf_flags |= ITF_F_VLAN_QINQ;
	}

	cmm_vlan_notify(g, itf);
	return (1);
}

void
cmm_vlan_notify(struct cmm_global *g, struct cmm_interface *itf)
{
	if (!(itf->itf_flags & ITF_F_VLAN))
		return;

	if ((itf->flags & IFF_UP) && !(itf->itf_flags & ITF_F_FPP_VLAN))
		cmm_vlan_register(g, itf);
	else if (!(itf->flags & IFF_UP) && (itf->itf_flags & ITF_F_FPP_VLAN))
		cmm_vlan_deregister(g, itf);
}

static int
vlan_notify_cb(struct cmm_global *g, struct cmm_interface *itf)
{
	cmm_vlan_notify(g, itf);
	return (0);
}

/*
 * CDX links each VLAN to its parent's entry, which is freed when the
 * parent is deregistered.  Linux takes VLANs down with their parent;
 * FreeBSD leaves them up, so the LAGG code does it through these.
 */
void
cmm_vlan_deregister_children(struct cmm_global *g, int parent_ifindex)
{
	cmm_itf_foreach_vlan_of(g, parent_ifindex, cmm_vlan_deregister);
}

void
cmm_vlan_register_children(struct cmm_global *g, int parent_ifindex)
{
	cmm_itf_foreach_vlan_of(g, parent_ifindex, vlan_notify_cb);
}
