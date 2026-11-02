/*
 * cdx_ifstats_freebsd.c — Interface statistics (MURAM-based) for FreeBSD
 *
 * Port of cdx-5.03.1/cdx_ifstats.c.  Allocates a contiguous block of
 * FMan MURAM to hold per-interface packet/byte counters that FMan
 * hardware updates atomically during flow offload processing.
 *
 * Two pools of slots:
 *   - PPPoE interfaces: cdx_pppoe_iface_ifinfo (with timestamp)
 *   - All other interfaces: cdx_iface_ifinfo (without timestamp)
 *
 * Which slots are in use is kept in kernel memory, not as list links in
 * MURAM like the original, since FMan writes counters into that memory.
 *
 * Copyright 2014-2016 Freescale Semiconductor, Inc.
 * Copyright 2017,2021 NXP
 * Copyright 2026 Mono Technologies Inc.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/callout.h>
#include <sys/endian.h>
#include <sys/epoch.h>
#include <sys/eventhandler.h>
#include <sys/jail.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/socket.h>
#include <sys/ucred.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_private.h>
#include <net/vnet.h>

#include "portdefs.h"
#include "cdx.h"
#include "cdx_ioctl.h"
#include "fm_ehash.h"

#include <contrib/ncsw/inc/Peripherals/fm_muram_ext.h>

#pragma GCC diagnostic ignored "-Wmissing-prototypes"

DEFINE_SPINLOCK(dpa_statslist_lock);

/* Base of stats area (virtual MURAM address) */
static void *stats_mem;

/* Stats area physical MURAM offset */
static uint32_t stats_mem_phys;

#define NUM_IFSTATS_SLOTS (MAX_LOGICAL_INTERFACES - MAX_PPPoE_INTERFACES)

/* Slots for PPPoE, then for all other interfaces, and which are in use */
static struct cdx_pppoe_iface_ifinfo *pppoe_slots;
static struct cdx_iface_ifinfo *ifstats_slots;
static uint8_t pppoe_slot_used[MAX_PPPoE_INTERFACES];
static uint8_t ifstats_slot_used[NUM_IFSTATS_SLOTS];

/*
 * When each slot was last freed (0 = never). Old flows can still write a
 * freed record for a while, so a new interface gets the slot freed longest ago.
 */
static uint64_t pppoe_slot_freed[MAX_PPPoE_INTERFACES];
static uint64_t ifstats_slot_freed[NUM_IFSTATS_SLOTS];
static uint64_t slot_free_seq;

/* The free slot freed longest ago, or -1. Called with dpa_statslist_lock. */
static int
pick_free_slot(const uint8_t *used, const uint64_t *freed, int count)
{
	int i, best = -1;

	for (i = 0; i < count; i++)
		if (!used[i] && (best < 0 || freed[i] < freed[best]))
			best = i;
	return (best);
}

/* MURAM handle for cleanup */
static void *stats_muram_handle;

extern void *FmMurambaseAddr;

/*
 * Offloaded frames never reach a VLAN or PPPoE interface's own counters,
 * so its counter function is wrapped to add what FMan counted in its
 * record. One entry per record, PPPoE pool first, like ASK's ifstats_wide.
 * Guarded by dpa_statslist_lock.
 */
struct cdx_ifcounter {
	if_t		 ifp;
	if_get_counter_t orig;
	uint32_t	 rx_raw, tx_raw;	/* last 32-bit packet counts read */
	uint64_t	 rx_pkts, tx_pkts;	/* the same carried past 2^32 */
};

static struct cdx_ifcounter ifcounters[MAX_LOGICAL_INTERFACES];

/* 2^32 minimum-size frames take 289 s at 10G, so a 30 s read sees every wrap. */
#define IFCOUNTER_SAMPLE_PERIOD	30
static struct callout ifcounter_sampler;
static void ifcounter_sampler_run(void *);
static eventhandler_tag ifcounter_departure_tag;

struct cdx_ifcounter_totals {
	uint64_t rx_bytes, rx_pkts, tx_bytes, tx_pkts;
};

/* The record's entry in ifcounters, or -1 for an address that is not one. */
static int
ifcounter_index(const void *record)
{
	uintptr_t p = (uintptr_t)record;
	uintptr_t pppoe_base = (uintptr_t)pppoe_slots;
	uintptr_t if_base = (uintptr_t)ifstats_slots;

	if (record == NULL || stats_mem == NULL)
		return (-1);
	if (p >= pppoe_base && p < if_base)
		return ((p - pppoe_base) / sizeof(struct cdx_pppoe_iface_ifinfo));
	if (p >= if_base && p < if_base +
	    NUM_IFSTATS_SLOTS * sizeof(struct cdx_iface_ifinfo))
		return (MAX_PPPoE_INTERFACES +
		    (p - if_base) / sizeof(struct cdx_iface_ifinfo));
	return (-1);
}

/* FMan writes the counts big-endian; read each word once so it can't tear. */
static uint64_t
ifcounter_rd64(const uint8_t *p)
{
	return (be64toh(*(const volatile uint64_t *)(const void *)p));
}

static uint32_t
ifcounter_rd32(const uint8_t *p)
{
	return (be32toh(*(const volatile uint32_t *)(const void *)p));
}

/* Read one record and carry its packet counts past 32 bits. Caller holds
 * dpa_statslist_lock. */
static void
ifcounter_sample(int index, struct cdx_ifcounter_totals *t)
{
	struct cdx_ifcounter *c = &ifcounters[index];
	const uint8_t *rx, *tx;
	uint32_t raw;

	if (index < MAX_PPPoE_INTERFACES) {
		rx = (const uint8_t *)&pppoe_slots[index];
		tx = rx + sizeof(struct en_ehash_stats_with_ts);
	} else {
		rx = (const uint8_t *)&ifstats_slots[index -
		    MAX_PPPoE_INTERFACES];
		tx = rx + sizeof(struct en_ehash_stats);
	}

	t->rx_bytes = ifcounter_rd64(rx + offsetof(struct en_ehash_stats, bytes));
	t->tx_bytes = ifcounter_rd64(tx + offsetof(struct en_ehash_stats, bytes));
	raw = ifcounter_rd32(rx + offsetof(struct en_ehash_stats, pkts));
	c->rx_pkts += (uint32_t)(raw - c->rx_raw);
	c->rx_raw = raw;
	raw = ifcounter_rd32(tx + offsetof(struct en_ehash_stats, pkts));
	c->tx_pkts += (uint32_t)(raw - c->tx_raw);
	c->tx_raw = raw;
	t->rx_pkts = c->rx_pkts;
	t->tx_pkts = c->tx_pkts;
}

/* The interface's own count plus what FMan counted for it. */
static uint64_t
cdx_ifcounter_get(if_t ifp, ift_counter cnt)
{
	struct cdx_ifcounter_totals t;
	uint64_t value;
	int i;

	spin_lock(&dpa_statslist_lock);
	for (i = 0; i < MAX_LOGICAL_INTERFACES; i++)
		if (ifcounters[i].ifp == ifp)
			break;
	if (i == MAX_LOGICAL_INTERFACES) {
		spin_unlock(&dpa_statslist_lock);
		return (if_get_counter_default(ifp, cnt));
	}
	value = ifcounters[i].orig(ifp, cnt);
	switch (cnt) {
	case IFCOUNTER_IPACKETS:
	case IFCOUNTER_IBYTES:
	case IFCOUNTER_OPACKETS:
	case IFCOUNTER_OBYTES:
		ifcounter_sample(i, &t);
		value += cnt == IFCOUNTER_IPACKETS ? t.rx_pkts :
		    cnt == IFCOUNTER_IBYTES ? t.rx_bytes :
		    cnt == IFCOUNTER_OPACKETS ? t.tx_pkts : t.tx_bytes;
		break;
	default:
		break;
	}
	spin_unlock(&dpa_statslist_lock);
	return (value);
}

/*
 * Unwrap the interface and return its reference for the caller to drop.
 * The record's totals move into the interface's own counters first, so its
 * counts don't step back when the record goes. Caller holds
 * dpa_statslist_lock.
 */
static if_t
ifcounter_detach_locked(int index)
{
	struct cdx_ifcounter *c = &ifcounters[index];
	struct cdx_ifcounter_totals t;
	if_t ifp = c->ifp;

	if (ifp == NULL)
		return (NULL);
	ifcounter_sample(index, &t);
	if_inc_counter(ifp, IFCOUNTER_IPACKETS, t.rx_pkts);
	if_inc_counter(ifp, IFCOUNTER_IBYTES, t.rx_bytes);
	if_inc_counter(ifp, IFCOUNTER_OPACKETS, t.tx_pkts);
	if_inc_counter(ifp, IFCOUNTER_OBYTES, t.tx_bytes);
	if (ifp->if_get_counter == cdx_ifcounter_get)
		if_setgetcounterfn(ifp, c->orig);
	memset(c, 0, sizeof(*c));
	return (ifp);
}

/* Wrap a VLAN or PPPoE interface's counters around its new record. */
static void
ifcounter_attach(uint32_t dev_type, struct dpa_iface_info *iface)
{
	if_t ifp;
	int i, index;

	if (!(dev_type & (IF_TYPE_VLAN | IF_TYPE_PPPOE)))
		return;
	CURVNET_SET(TD_TO_VNET(curthread));
	ifp = ifunit_ref((const char *)iface->name);
	CURVNET_RESTORE();
	if (ifp == NULL)
		return;

	spin_lock(&dpa_statslist_lock);
	index = ifcounter_index(iface->stats);
	for (i = 0; i < MAX_LOGICAL_INTERFACES; i++)
		if (ifcounters[i].ifp == ifp)
			break;
	if (index < 0 || i < MAX_LOGICAL_INTERFACES) {
		/* already counted through another record */
		spin_unlock(&dpa_statslist_lock);
		if_rele(ifp);
		return;
	}
	/* the record was just cleared, so its counts start from zero */
	ifcounters[index] = (struct cdx_ifcounter){
		.ifp = ifp,
		.orig = ifp->if_get_counter,
	};
	if_setgetcounterfn(ifp, cdx_ifcounter_get);
	if (!callout_pending(&ifcounter_sampler))
		callout_reset(&ifcounter_sampler, IFCOUNTER_SAMPLE_PERIOD * hz,
		    ifcounter_sampler_run, NULL);
	spin_unlock(&dpa_statslist_lock);
}

/* Read every wrapped record before its packet count can come round. Runs
 * with dpa_statslist_lock held. */
static void
ifcounter_sampler_run(void *arg __unused)
{
	struct cdx_ifcounter_totals t;
	bool active = false;
	int i;

	for (i = 0; i < MAX_LOGICAL_INTERFACES; i++) {
		if (ifcounters[i].ifp != NULL) {
			ifcounter_sample(i, &t);
			active = true;
		}
	}
	if (active)
		callout_reset(&ifcounter_sampler, IFCOUNTER_SAMPLE_PERIOD * hz,
		    ifcounter_sampler_run, NULL);
}

/* An interface going away, or being renamed, is unwrapped before its memory
 * can go. Its counts carry over, so a rename doesn't step them back. */
static void
ifcounter_departure(void *arg __unused, if_t ifp)
{
	if_t gone = NULL;
	int i;

	spin_lock(&dpa_statslist_lock);
	for (i = 0; i < MAX_LOGICAL_INTERFACES; i++) {
		if (ifcounters[i].ifp == ifp) {
			gone = ifcounter_detach_locked(i);
			break;
		}
	}
	spin_unlock(&dpa_statslist_lock);
	if (gone != NULL)
		if_rele(gone);
}

/*
 * cdx_ifcounter_report — One line per wrapped interface: its name and the
 * bytes FMan forwarded in and out of it, which leaves the CPU's share as the
 * rest of its counts.
 */
void
cdx_ifcounter_report(struct sbuf *sb)
{
	struct ifcounter_line {
		char name[IFNAMSIZ];
		uint64_t rx, tx;
	} *line;
	struct cdx_ifcounter_totals t;
	int i, n = 0;

	if (stats_mem == NULL)
		return;
	line = malloc(MAX_LOGICAL_INTERFACES * sizeof(*line), M_TEMP, M_WAITOK);
	/* copied out under the lock, printed after: printing may sleep */
	spin_lock(&dpa_statslist_lock);
	for (i = 0; i < MAX_LOGICAL_INTERFACES; i++) {
		if (ifcounters[i].ifp == NULL)
			continue;
		ifcounter_sample(i, &t);
		strlcpy(line[n].name, if_name(ifcounters[i].ifp),
		    sizeof(line[n].name));
		line[n].rx = t.rx_bytes;
		line[n].tx = t.tx_bytes;
		n++;
	}
	spin_unlock(&dpa_statslist_lock);
	for (i = 0; i < n; i++)
		sbuf_printf(sb, "%s %ju %ju\n", line[i].name,
		    (uintmax_t)line[i].rx, (uintmax_t)line[i].tx);
	free(line, M_TEMP);
}

/*
 * cdxdrv_stop_ifcounters — Unwrap every interface on module unload.
 */
void
cdxdrv_stop_ifcounters(void)
{
	if_t gone[MAX_LOGICAL_INTERFACES];
	int i;

	if (stats_mem == NULL)
		return;
	EVENTHANDLER_DEREGISTER(ifnet_departure_event,
	    ifcounter_departure_tag);
	spin_lock(&dpa_statslist_lock);
	for (i = 0; i < MAX_LOGICAL_INTERFACES; i++)
		gone[i] = ifcounter_detach_locked(i);
	callout_stop(&ifcounter_sampler);
	spin_unlock(&dpa_statslist_lock);
	callout_drain(&ifcounter_sampler);
	/* readers already inside cdx_ifcounter_get finish before unload */
	NET_EPOCH_WAIT();
	for (i = 0; i < MAX_LOGICAL_INTERFACES; i++)
		if (gone[i] != NULL)
			if_rele(gone[i]);
}

/*
 * cdxdrv_init_stats — Allocate MURAM and create free-lists.
 *
 * Called during CDX bridge init after the MURAM handle and
 * FmMurambaseAddr are set up.
 */
int
cdxdrv_init_stats(void *muram_handle)
{
	uint32_t size;

	size = (MAX_PPPoE_INTERFACES *
	    sizeof(struct cdx_pppoe_iface_ifinfo)) +
	    ((MAX_LOGICAL_INTERFACES - MAX_PPPoE_INTERFACES) *
	    sizeof(struct cdx_iface_ifinfo));

	stats_mem = FM_MURAM_AllocMem(muram_handle, size, sizeof(uint64_t));
	if (stats_mem == NULL) {
		printf("cdx: cdxdrv_init_stats: unable to allocate MURAM "
		    "for iface stats, size %u\n", size);
		return (-1);
	}

	stats_muram_handle = muram_handle;
	stats_mem_phys = (uint32_t)((uint8_t *)stats_mem -
	    (uint8_t *)FmMurambaseAddr);

	printf("cdx: cdxdrv_init_stats: mem=%p phys=0x%x size=%u\n",
	    stats_mem, stats_mem_phys, size);

	/* DEFINE_SPINLOCK only declares the lock on FreeBSD (unlike real
	 * Linux, which statically initializes it) -- it still needs an
	 * explicit spin_lock_init before first use. This is the only
	 * caller of cdxdrv_init_stats, so init it here rather than adding
	 * a separate module-init function just for one lock. */
	spin_lock_init(&dpa_statslist_lock);

	spin_lock(&dpa_statslist_lock);
	pppoe_slots = (struct cdx_pppoe_iface_ifinfo *)stats_mem;
	ifstats_slots = (struct cdx_iface_ifinfo *)(pppoe_slots +
	    MAX_PPPoE_INTERFACES);
	memset(pppoe_slot_used, 0, sizeof(pppoe_slot_used));
	memset(ifstats_slot_used, 0, sizeof(ifstats_slot_used));
	memset(pppoe_slot_freed, 0, sizeof(pppoe_slot_freed));
	memset(ifstats_slot_freed, 0, sizeof(ifstats_slot_freed));
	slot_free_seq = 0;
	spin_unlock(&dpa_statslist_lock);

	callout_init_mtx(&ifcounter_sampler, &dpa_statslist_lock.mtx, 0);
	ifcounter_departure_tag = EVENTHANDLER_REGISTER(ifnet_departure_event,
	    ifcounter_departure, NULL, EVENTHANDLER_PRI_ANY);

	printf("cdx: cdxdrv_init_stats: %u PPPoE + %u regular slots\n",
	    MAX_PPPoE_INTERFACES, NUM_IFSTATS_SLOTS);

	return (0);
}

/*
 * alloc_iface_stats — Allocate a MURAM stats slot for an interface.
 *
 * Takes the free slot freed longest ago and computes the rxstats_index
 * and txstats_index relative to stats_mem.  Also allocates a heap-side
 * last_stats snapshot for delta calculation.
 */
int
alloc_iface_stats(uint32_t dev_type, struct dpa_iface_info *iface)
{

	iface->last_stats = kzalloc(sizeof(struct iface_stats), GFP_KERNEL);
	if (iface->last_stats == NULL) {
		DPA_ERROR("cdx: alloc_iface_stats: "
		    "memory alloc failed for last_stats\n");
		return (FAILURE);
	}

	if (dev_type == IF_TYPE_PPPOE) {
		struct cdx_pppoe_iface_ifinfo *pppoe_stats;
		int i;

		pppoe_stats = NULL;
		spin_lock(&dpa_statslist_lock);
		i = pick_free_slot(pppoe_slot_used, pppoe_slot_freed,
		    MAX_PPPoE_INTERFACES);
		if (i >= 0) {
			pppoe_slot_used[i] = 1;
			pppoe_stats = &pppoe_slots[i];
		}
		if (pppoe_stats != NULL) {
			memset(pppoe_stats, 0,
			    sizeof(struct cdx_pppoe_iface_ifinfo));
			iface->rxstats_index =
			    (((uint32_t)((uint8_t *)&pppoe_stats->stats.rxstats -
			    (uint8_t *)stats_mem) /
			    sizeof(struct en_ehash_stats_with_ts)) |
			    STATS_WITH_TS);
			iface->txstats_index =
			    (((uint32_t)((uint8_t *)&pppoe_stats->stats.txstats -
			    (uint8_t *)stats_mem) /
			    sizeof(struct en_ehash_stats_with_ts)) |
			    STATS_WITH_TS);
		}
		iface->stats = pppoe_stats;
		spin_unlock(&dpa_statslist_lock);
	} else {
		struct cdx_iface_ifinfo *ifstats;
		int i;

		ifstats = NULL;
		spin_lock(&dpa_statslist_lock);
		i = pick_free_slot(ifstats_slot_used, ifstats_slot_freed,
		    NUM_IFSTATS_SLOTS);
		if (i >= 0) {
			ifstats_slot_used[i] = 1;
			ifstats = &ifstats_slots[i];
		}
		if (ifstats != NULL) {
			memset(ifstats, 0, sizeof(struct cdx_iface_ifinfo));
			iface->rxstats_index =
			    ((uint32_t)((uint8_t *)&ifstats->stats.rxstats -
			    (uint8_t *)stats_mem) /
			    sizeof(struct en_ehash_stats));
			iface->txstats_index =
			    ((uint32_t)((uint8_t *)&ifstats->stats.txstats -
			    (uint8_t *)stats_mem) /
			    sizeof(struct en_ehash_stats));
		}
		iface->stats = ifstats;
		spin_unlock(&dpa_statslist_lock);
	}

	if (iface->stats == NULL) {
		kfree(iface->last_stats);
		iface->last_stats = NULL;
		DPA_ERROR("cdx: alloc_iface_stats: "
		    "no free stats slots for type 0x%x\n", dev_type);
		return (FAILURE);
	}

	ifcounter_attach(dev_type, iface);
	return (SUCCESS);
}

/*
 * free_iface_stats — Return a MURAM stats slot to its pool.
 *
 * The pool comes from the slot's address, not dev_type: callers pass
 * if_flags, which also carries IF_STATS_ENABLED.  Freeing a slot that is
 * already free does nothing.
 */
void
free_iface_stats(uint32_t dev_type __unused, struct dpa_iface_info *iface)
{
	uintptr_t p, pppoe_base, if_base, slot;
	if_t ifp = NULL;
	int index;

	if (iface->stats == NULL)
		return;

	p = (uintptr_t)iface->stats;
	pppoe_base = (uintptr_t)pppoe_slots;
	if_base = (uintptr_t)ifstats_slots;

	spin_lock(&dpa_statslist_lock);
	index = ifcounter_index(iface->stats);
	if (index >= 0)
		ifp = ifcounter_detach_locked(index);
	if (p >= pppoe_base && p < if_base) {
		slot = (p - pppoe_base) / sizeof(struct cdx_pppoe_iface_ifinfo);
		if (pppoe_slot_used[slot]) {
			pppoe_slot_used[slot] = 0;
			pppoe_slot_freed[slot] = ++slot_free_seq;
		}
	} else if (p >= if_base && p < if_base +
	    NUM_IFSTATS_SLOTS * sizeof(struct cdx_iface_ifinfo)) {
		slot = (p - if_base) / sizeof(struct cdx_iface_ifinfo);
		if (ifstats_slot_used[slot]) {
			ifstats_slot_used[slot] = 0;
			ifstats_slot_freed[slot] = ++slot_free_seq;
		}
	} else {
		DPA_ERROR("cdx: free_iface_stats: slot %p is not a stats "
		    "slot\n", iface->stats);
	}
	spin_unlock(&dpa_statslist_lock);
	if (ifp != NULL)
		if_rele(ifp);

	iface->stats = NULL;

	if (iface->last_stats != NULL) {
		kfree(iface->last_stats);
		iface->last_stats = NULL;
	}
}

/*
 * get_logical_ifstats_base — Return the MURAM physical offset of the
 * stats area.  Used by cdx_ehash.c to compute 24-bit Action Descriptor
 * stats pointers.
 */
uint32_t
get_logical_ifstats_base(void)
{

	return (stats_mem_phys);
}

/*
 * cdxdrv_cleanup_stats — Free MURAM stats allocation.
 *
 * Called during CDX module unload.
 */
void
cdxdrv_cleanup_stats(void)
{

	if (stats_mem != NULL && stats_muram_handle != NULL) {
		FM_MURAM_FreeMem(stats_muram_handle, stats_mem);
		stats_mem = NULL;
		stats_muram_handle = NULL;
		printf("cdx: cdxdrv_cleanup_stats: freed MURAM stats\n");
	}
}
