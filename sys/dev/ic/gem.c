/*	$OpenBSD: gem.c,v 1.16 2002/02/22 20:15:28 jason Exp $	*/
/*	$NetBSD: gem.c,v 1.1 2001/09/16 00:11:43 eeh Exp $ */

/*
 * 
 * Copyright (C) 2001 Eduardo Horvath.
 * All rights reserved.
 *
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR  ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR  BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/*
 * Driver for Sun GEM ethernet controllers.
 */

#include "bpfilter.h"
#include "vlan.h"

#include <sys/param.h>
#include <sys/systm.h> 
#include <sys/timeout.h>
#include <sys/mbuf.h>   
#include <sys/syslog.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <sys/device.h>

#include <machine/endian.h>

#include <uvm/uvm_extern.h>
 
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>

#ifdef INET
#include <netinet/in.h>
#include <netinet/if_ether.h>
#endif

#if NBPFILTER > 0 
#include <net/bpf.h>
#endif 

#if NVLAN > 0
#include <net/if_vlan_var.h>
#endif

#include <machine/bus.h>
#include <machine/intr.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>
#include <dev/mii/mii_bitbang.h>

#include <dev/ic/gemreg.h>
#include <dev/ic/gemvar.h>

#define TRIES	10000

struct cfdriver gem_cd = {
	NULL, "gem", DV_IFNET
};

void		gem_start __P((struct ifnet *));
void		gem_stop __P((struct ifnet *, int));
int		gem_ioctl __P((struct ifnet *, u_long, caddr_t));
void		gem_tick __P((void *));
void		gem_watchdog __P((struct ifnet *));
void		gem_shutdown __P((void *));
int		gem_init __P((struct ifnet *));
void		gem_init_regs(struct gem_softc *sc);
static int	gem_ringsize(int sz);
int		gem_meminit __P((struct gem_softc *));
void		gem_mifinit __P((struct gem_softc *));
void		gem_reset __P((struct gem_softc *));
int		gem_reset_rx(struct gem_softc *sc);
int		gem_reset_tx(struct gem_softc *sc);
int		gem_disable_rx(struct gem_softc *sc);
int		gem_disable_tx(struct gem_softc *sc);
void		gem_rxdrain(struct gem_softc *sc);
int		gem_add_rxbuf(struct gem_softc *sc, int idx);
void		gem_setladrf __P((struct gem_softc *));
int		gem_encap __P((struct gem_softc *, struct mbuf *, u_int32_t *));

/* MII methods & callbacks */
static int	gem_mii_readreg __P((struct device *, int, int));
static void	gem_mii_writereg __P((struct device *, int, int, int));
static void	gem_mii_statchg __P((struct device *));

int		gem_mediachange __P((struct ifnet *));
void		gem_mediastatus __P((struct ifnet *, struct ifmediareq *));

struct mbuf	*gem_get __P((struct gem_softc *, int, int));
int		gem_put __P((struct gem_softc *, int, struct mbuf *));
void		gem_read __P((struct gem_softc *, int, int));
int		gem_eint __P((struct gem_softc *, u_int));
int		gem_rint __P((struct gem_softc *));
int		gem_tint __P((struct gem_softc *, u_int32_t));
void		gem_power __P((int, void *));

static int	ether_cmp __P((u_char *, u_char *));

#ifdef GEM_DEBUG
#define	DPRINTF(sc, x)	if ((sc)->sc_arpcom.ac_if.if_flags & IFF_DEBUG) \
				printf x
#else
#define	DPRINTF(sc, x)	/* nothing */
#endif


/*
 * gem_config:
 *
 *	Attach a Gem interface to the system.
 */
void
gem_config(sc)
	struct gem_softc *sc;
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	struct mii_data *mii = &sc->sc_mii;
	struct mii_softc *child;
	int i, error;

	bcopy(sc->sc_enaddr, sc->sc_arpcom.ac_enaddr, ETHER_ADDR_LEN);

	/* Make sure the chip is stopped. */
	ifp->if_softc = sc;
	gem_reset(sc);

	/*
	 * Allocate the control data structures, and create and load the
	 * DMA map for it.
	 */
	if ((error = bus_dmamem_alloc(sc->sc_dmatag,
	    sizeof(struct gem_control_data), PAGE_SIZE, 0, &sc->sc_cdseg,
	    1, &sc->sc_cdnseg, 0)) != 0) {
		printf("%s: unable to allocate control data, error = %d\n",
		    sc->sc_dev.dv_xname, error);
		goto fail_0;
	}

	/* XXX should map this in with correct endianness */
	if ((error = bus_dmamem_map(sc->sc_dmatag, &sc->sc_cdseg, sc->sc_cdnseg,
	    sizeof(struct gem_control_data), (caddr_t *)&sc->sc_control_data,
	    BUS_DMA_COHERENT)) != 0) {
		printf("%s: unable to map control data, error = %d\n",
		    sc->sc_dev.dv_xname, error);
		goto fail_1;
	}

	if ((error = bus_dmamap_create(sc->sc_dmatag,
	    sizeof(struct gem_control_data), 1,
	    sizeof(struct gem_control_data), 0, 0, &sc->sc_cddmamap)) != 0) {
		printf("%s: unable to create control data DMA map, "
		    "error = %d\n", sc->sc_dev.dv_xname, error);
		goto fail_2;
	}

	if ((error = bus_dmamap_load(sc->sc_dmatag, sc->sc_cddmamap,
	    sc->sc_control_data, sizeof(struct gem_control_data), NULL,
	    0)) != 0) {
		printf("%s: unable to load control data DMA map, error = %d\n",
		    sc->sc_dev.dv_xname, error);
		goto fail_3;
	}

	/*
	 * Create the receive buffer DMA maps.
	 */
	for (i = 0; i < GEM_NRXDESC; i++) {
		if ((error = bus_dmamap_create(sc->sc_dmatag, MCLBYTES, 1,
		    MCLBYTES, 0, 0, &sc->sc_rxsoft[i].rxs_dmamap)) != 0) {
			printf("%s: unable to create rx DMA map %d, "
			    "error = %d\n", sc->sc_dev.dv_xname, i, error);
			goto fail_5;
		}
		sc->sc_rxsoft[i].rxs_mbuf = NULL;
	}

	/*
	 * From this point forward, the attachment cannot fail.  A failure
	 * before this point releases all resources that may have been
	 * allocated.
	 */

	/* Announce ourselves. */
	printf("%s: Ethernet address %s\n", sc->sc_dev.dv_xname,
	    ether_sprintf(sc->sc_enaddr));

	/* Initialize ifnet structure. */
	strcpy(ifp->if_xname, sc->sc_dev.dv_xname);
	ifp->if_softc = sc;
	ifp->if_flags =
	    IFF_BROADCAST | IFF_SIMPLEX | IFF_NOTRAILERS | IFF_MULTICAST;
	ifp->if_start = gem_start;
	ifp->if_ioctl = gem_ioctl;
	ifp->if_watchdog = gem_watchdog;
	IFQ_SET_READY(&ifp->if_snd);

	/* Initialize ifmedia structures and MII info */
	mii->mii_ifp = ifp;
	mii->mii_readreg = gem_mii_readreg; 
	mii->mii_writereg = gem_mii_writereg;
	mii->mii_statchg = gem_mii_statchg;

	ifmedia_init(&mii->mii_media, 0, gem_mediachange, gem_mediastatus);

	gem_mifinit(sc);

	mii_attach(&sc->sc_dev, mii, 0xffffffff,
			MII_PHY_ANY, MII_OFFSET_ANY, 0);

	child = LIST_FIRST(&mii->mii_phys);
	if (child == NULL) {
		/* No PHY attached */
		ifmedia_add(&sc->sc_media, IFM_ETHER|IFM_MANUAL, 0, NULL);
		ifmedia_set(&sc->sc_media, IFM_ETHER|IFM_MANUAL);
	} else {
		/*
		 * Walk along the list of attached MII devices and
		 * establish an `MII instance' to `phy number'
		 * mapping. We'll use this mapping in media change
		 * requests to determine which phy to use to program
		 * the MIF configuration register.
		 */
		for (; child != NULL; child = LIST_NEXT(child, mii_list)) {
			/*
			 * Note: we support just two PHYs: the built-in
			 * internal device and an external on the MII
			 * connector.
			 */
			if (child->mii_phy > 1 || child->mii_inst > 1) {
				printf("%s: cannot accomodate MII device %s"
				       " at phy %d, instance %d\n",
				       sc->sc_dev.dv_xname,
				       child->mii_dev.dv_xname,
				       child->mii_phy, child->mii_inst);
				continue;
			}

			sc->sc_phys[child->mii_inst] = child->mii_phy;
		}

		/*
		 * Now select and activate the PHY we will use.
		 *
		 * The order of preference is External (MDI1),
		 * Internal (MDI0), Serial Link (no MII).
		 */
		if (sc->sc_phys[1]) {
#ifdef DEBUG
			printf("using external phy\n");
#endif
			sc->sc_mif_config |= GEM_MIF_CONFIG_PHY_SEL;
		} else {
#ifdef DEBUG
			printf("using internal phy\n");
#endif
			sc->sc_mif_config &= ~GEM_MIF_CONFIG_PHY_SEL;
		}
		bus_space_write_4(sc->sc_bustag, sc->sc_h, GEM_MIF_CONFIG, 
			sc->sc_mif_config);

		/*
		 * XXX - we can really do the following ONLY if the
		 * phy indeed has the auto negotiation capability!!
		 */
		ifmedia_set(&sc->sc_media, IFM_ETHER|IFM_AUTO);
	}

	/* Attach the interface. */
	if_attach(ifp);
	ether_ifattach(ifp);

	sc->sc_sh = shutdownhook_establish(gem_shutdown, sc);
	if (sc->sc_sh == NULL)
		panic("gem_config: can't establish shutdownhook");

	timeout_set(&sc->sc_tick_ch, gem_tick, sc);
	return;

	/*
	 * Free any resources we've allocated during the failed attach
	 * attempt.  Do this in reverse order and fall through.
	 */
 fail_5:
	for (i = 0; i < GEM_NRXDESC; i++) {
		if (sc->sc_rxsoft[i].rxs_dmamap != NULL)
			bus_dmamap_destroy(sc->sc_dmatag,
			    sc->sc_rxsoft[i].rxs_dmamap);
	}
	bus_dmamap_unload(sc->sc_dmatag, sc->sc_cddmamap);
 fail_3:
	bus_dmamap_destroy(sc->sc_dmatag, sc->sc_cddmamap);
 fail_2:
	bus_dmamem_unmap(sc->sc_dmatag, (caddr_t)sc->sc_control_data,
	    sizeof(struct gem_control_data));
 fail_1:
	bus_dmamem_free(sc->sc_dmatag, &sc->sc_cdseg, sc->sc_cdnseg);
 fail_0:
	return;
}


void
gem_tick(arg)
	void *arg;
{
	struct gem_softc *sc = arg;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t mac = sc->sc_h;
	int s;

	s = splimp();

	/* unload collisions counters */
	ifp->if_collisions +=
	    bus_space_read_4(t, mac, GEM_MAC_NORM_COLL_CNT) +
	    bus_space_read_4(t, mac, GEM_MAC_FIRST_COLL_CNT) +
	    bus_space_read_4(t, mac, GEM_MAC_EXCESS_COLL_CNT) +
	    bus_space_read_4(t, mac, GEM_MAC_LATE_COLL_CNT);

	/* clear the hardware counters */
	bus_space_write_4(t, mac, GEM_MAC_NORM_COLL_CNT, 0);
	bus_space_write_4(t, mac, GEM_MAC_FIRST_COLL_CNT, 0);
	bus_space_write_4(t, mac, GEM_MAC_EXCESS_COLL_CNT, 0);
	bus_space_write_4(t, mac, GEM_MAC_LATE_COLL_CNT, 0);

	mii_tick(&sc->sc_mii);
	splx(s);

	timeout_add(&sc->sc_tick_ch, hz);
}

void
gem_reset(sc)
	struct gem_softc *sc;
{
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t h = sc->sc_h;
	int i;
	int s;

	s = splimp();
	DPRINTF(sc, ("%s: gem_reset\n", sc->sc_dev.dv_xname));
	gem_reset_rx(sc);
	gem_reset_tx(sc);

	/* Do a full reset */
	bus_space_write_4(t, h, GEM_RESET, GEM_RESET_RX|GEM_RESET_TX);
	for (i=TRIES; i--; delay(100))
		if ((bus_space_read_4(t, h, GEM_RESET) & 
			(GEM_RESET_RX|GEM_RESET_TX)) == 0)
			break;
	if ((bus_space_read_4(t, h, GEM_RESET) &
		(GEM_RESET_RX|GEM_RESET_TX)) != 0) {
		printf("%s: cannot reset device\n",
			sc->sc_dev.dv_xname);
	}
	splx(s);
}


/*
 * gem_rxdrain:
 *
 *	Drain the receive queue.
 */
void
gem_rxdrain(struct gem_softc *sc)
{
	struct gem_rxsoft *rxs;
	int i;

	for (i = 0; i < GEM_NRXDESC; i++) {
		rxs = &sc->sc_rxsoft[i];
		if (rxs->rxs_mbuf != NULL) {
			bus_dmamap_unload(sc->sc_dmatag, rxs->rxs_dmamap);
			m_freem(rxs->rxs_mbuf);
			rxs->rxs_mbuf = NULL;
		}
	}
}

/* 
 * Reset the whole thing.
 */
void
gem_stop(struct ifnet *ifp, int disable)
{
	struct gem_softc *sc = (struct gem_softc *)ifp->if_softc;
	struct gem_sxd *sd;
	u_int32_t i;

	DPRINTF(sc, ("%s: gem_stop\n", sc->sc_dev.dv_xname));

	timeout_del(&sc->sc_tick_ch);
	mii_down(&sc->sc_mii);

	/* XXX - Should we reset these instead? */
	gem_disable_rx(sc);
	gem_disable_rx(sc);

	/*
	 * Release any queued transmit buffers.
	 */
	for (i = 0; i < GEM_NTXDESC; i++) {
		sd = &sc->sc_txd[i];
		if (sd->sd_map != NULL) {
			bus_dmamap_sync(sc->sc_dmatag, sd->sd_map, 0,
			    sd->sd_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmatag, sd->sd_map);
			bus_dmamap_destroy(sc->sc_dmatag, sd->sd_map);
			sd->sd_map = NULL;
		}
		if (sd->sd_mbuf != NULL) {
			m_freem(sd->sd_mbuf);
			sd->sd_mbuf = NULL;
		}
	}
	sc->sc_tx_cnt = sc->sc_tx_prod = sc->sc_tx_cons = 0;

	if (disable) {
		gem_rxdrain(sc);
	}

	/*
	 * Mark the interface down and cancel the watchdog timer.
	 */
	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
	ifp->if_timer = 0;
}


/*
 * Reset the receiver
 */
int
gem_reset_rx(struct gem_softc *sc)
{
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t h = sc->sc_h;
	int i;


	/*
	 * Resetting while DMA is in progress can cause a bus hang, so we
	 * disable DMA first.
	 */
	gem_disable_rx(sc);
	bus_space_write_4(t, h, GEM_RX_CONFIG, 0);
	/* Wait till it finishes */
	for (i = TRIES; i--; delay(100))
		if ((bus_space_read_4(t, h, GEM_RX_CONFIG) & 1) == 0)
			break;
	if ((bus_space_read_4(t, h, GEM_RX_CONFIG) & 1) != 0)
		printf("%s: cannot disable read dma\n",
			sc->sc_dev.dv_xname);

	/* Wait 5ms extra. */
	delay(5000);

	/* Finally, reset the ERX */
	bus_space_write_4(t, h, GEM_RESET, GEM_RESET_RX);
	/* Wait till it finishes */
	for (i = TRIES; i--; delay(100))
		if ((bus_space_read_4(t, h, GEM_RESET) & GEM_RESET_RX) == 0)
			break;
	if ((bus_space_read_4(t, h, GEM_RESET) & GEM_RESET_RX) != 0) {
		printf("%s: cannot reset receiver\n",
			sc->sc_dev.dv_xname);
		return (1);
	}
	return (0);
}


/*
 * Reset the transmitter
 */
int
gem_reset_tx(struct gem_softc *sc)
{
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t h = sc->sc_h;
	int i;

	/*
	 * Resetting while DMA is in progress can cause a bus hang, so we
	 * disable DMA first.
	 */
	gem_disable_tx(sc);
	bus_space_write_4(t, h, GEM_TX_CONFIG, 0);
	/* Wait till it finishes */
	for (i = TRIES; i--; delay(100))
		if ((bus_space_read_4(t, h, GEM_TX_CONFIG) & 1) == 0)
			break;
	if ((bus_space_read_4(t, h, GEM_TX_CONFIG) & 1) != 0)
		printf("%s: cannot disable read dma\n",
			sc->sc_dev.dv_xname);

	/* Wait 5ms extra. */
	delay(5000);

	/* Finally, reset the ETX */
	bus_space_write_4(t, h, GEM_RESET, GEM_RESET_TX);
	/* Wait till it finishes */
	for (i = TRIES; i--; delay(100))
		if ((bus_space_read_4(t, h, GEM_RESET) & GEM_RESET_TX) == 0)
			break;
	if ((bus_space_read_4(t, h, GEM_RESET) & GEM_RESET_TX) != 0) {
		printf("%s: cannot reset receiver\n",
			sc->sc_dev.dv_xname);
		return (1);
	}
	return (0);
}

/*
 * disable receiver.
 */
int
gem_disable_rx(struct gem_softc *sc)
{
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t h = sc->sc_h;
	int i;
	u_int32_t cfg;

	/* Flip the enable bit */
	cfg = bus_space_read_4(t, h, GEM_MAC_RX_CONFIG);
	cfg &= ~GEM_MAC_RX_ENABLE;
	bus_space_write_4(t, h, GEM_MAC_RX_CONFIG, cfg);

	/* Wait for it to finish */
	for (i = TRIES; i--; delay(100)) 
		if ((bus_space_read_4(t, h, GEM_MAC_RX_CONFIG) &
			GEM_MAC_RX_ENABLE) == 0)
			return (0);
	return (1);
}

/*
 * disable transmitter.
 */
int
gem_disable_tx(struct gem_softc *sc)
{
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t h = sc->sc_h;
	int i;
	u_int32_t cfg;

	/* Flip the enable bit */
	cfg = bus_space_read_4(t, h, GEM_MAC_TX_CONFIG);
	cfg &= ~GEM_MAC_TX_ENABLE;
	bus_space_write_4(t, h, GEM_MAC_TX_CONFIG, cfg);

	/* Wait for it to finish */
	for (i = TRIES; i--; delay(100)) 
		if ((bus_space_read_4(t, h, GEM_MAC_TX_CONFIG) &
			GEM_MAC_TX_ENABLE) == 0)
			return (0);
	return (1);
}

/*
 * Initialize interface.
 */
int
gem_meminit(struct gem_softc *sc)
{
	struct gem_rxsoft *rxs;
	int i, error;

	/*
	 * Initialize the transmit descriptor ring.
	 */
	memset((void *)sc->sc_txdescs, 0, sizeof(sc->sc_txdescs));
	for (i = 0; i < GEM_NTXDESC; i++) {
		sc->sc_txdescs[i].gd_flags = 0;
		sc->sc_txdescs[i].gd_addr = 0;
	}
	GEM_CDTXSYNC(sc, 0, GEM_NTXDESC,
	    BUS_DMASYNC_PREREAD|BUS_DMASYNC_PREWRITE);

	/*
	 * Initialize the receive descriptor and receive job
	 * descriptor rings.
	 */
	for (i = 0; i < GEM_NRXDESC; i++) {
		rxs = &sc->sc_rxsoft[i];
		if (rxs->rxs_mbuf == NULL) {
			if ((error = gem_add_rxbuf(sc, i)) != 0) {
				printf("%s: unable to allocate or map rx "
				    "buffer %d, error = %d\n",
				    sc->sc_dev.dv_xname, i, error);
				/*
				 * XXX Should attempt to run with fewer receive
				 * XXX buffers instead of just failing.
				 */
				gem_rxdrain(sc);
				return (1);
			}
		} else
			GEM_INIT_RXDESC(sc, i);
	}
	sc->sc_rxptr = 0;

	return (0);
}

static int
gem_ringsize(int sz)
{
	int v;

	switch (sz) {
	case 32:
		v = GEM_RING_SZ_32;
		break;
	case 64:
		v = GEM_RING_SZ_64;
		break;
	case 128:
		v = GEM_RING_SZ_128;
		break;
	case 256:
		v = GEM_RING_SZ_256;
		break;
	case 512:
		v = GEM_RING_SZ_512;
		break;
	case 1024:
		v = GEM_RING_SZ_1024;
		break;
	case 2048:
		v = GEM_RING_SZ_2048;
		break;
	case 4096:
		v = GEM_RING_SZ_4096;
		break;
	case 8192:
		v = GEM_RING_SZ_8192;
		break;
	default:
		v = GEM_RING_SZ_32;
		printf("gem: invalid Receive Descriptor ring size\n");
		break;
	}
	return (v);
}

/*
 * Initialization of interface; set up initialization block
 * and transmit/receive descriptor rings.
 */
int
gem_init(struct ifnet *ifp)
{
	struct gem_softc *sc = (struct gem_softc *)ifp->if_softc;
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t h = sc->sc_h;
	int s;
	u_int32_t v;

	s = splimp();

	DPRINTF(sc, ("%s: gem_init: calling stop\n", sc->sc_dev.dv_xname));
	/*
	 * Initialization sequence. The numbered steps below correspond
	 * to the sequence outlined in section 6.3.5.1 in the Ethernet
	 * Channel Engine manual (part of the PCIO manual).
	 * See also the STP2002-STQ document from Sun Microsystems.
	 */

	/* step 1 & 2. Reset the Ethernet Channel */
	gem_stop(ifp, 0);
	gem_reset(sc);
	DPRINTF(sc, ("%s: gem_init: restarting\n", sc->sc_dev.dv_xname));

	/* Re-initialize the MIF */
	gem_mifinit(sc);

	/* Call MI reset function if any */
	if (sc->sc_hwreset)
		(*sc->sc_hwreset)(sc);

	/* step 3. Setup data structures in host memory */
	gem_meminit(sc);

	/* step 4. TX MAC registers & counters */
	gem_init_regs(sc);
	v = (GEM_MTU) | (0x2000 << 16) /* Burst size */;
	bus_space_write_4(t, h, GEM_MAC_MAC_MAX_FRAME, v);

	/* step 5. RX MAC registers & counters */
	gem_setladrf(sc);

	/* step 6 & 7. Program Descriptor Ring Base Addresses */
	bus_space_write_4(t, h, GEM_TX_RING_PTR_HI, 
	    (((uint64_t)GEM_CDTXADDR(sc,0)) >> 32));
	bus_space_write_4(t, h, GEM_TX_RING_PTR_LO, GEM_CDTXADDR(sc, 0));

	bus_space_write_4(t, h, GEM_RX_RING_PTR_HI, 
	    (((uint64_t)GEM_CDRXADDR(sc,0)) >> 32));
	bus_space_write_4(t, h, GEM_RX_RING_PTR_LO, GEM_CDRXADDR(sc, 0));

	/* step 8. Global Configuration & Interrupt Mask */
	bus_space_write_4(t, h, GEM_INTMASK,
		      ~(GEM_INTR_TX_INTME|
			GEM_INTR_TX_EMPTY|
			GEM_INTR_RX_DONE|GEM_INTR_RX_NOBUF|
			GEM_INTR_RX_TAG_ERR|GEM_INTR_PCS|
			GEM_INTR_MAC_CONTROL|GEM_INTR_MIF|
			GEM_INTR_BERR));
	bus_space_write_4(t, h, GEM_MAC_RX_MASK, 0); /* XXXX */
	bus_space_write_4(t, h, GEM_MAC_TX_MASK, 0xffff); /* XXXX */
	bus_space_write_4(t, h, GEM_MAC_CONTROL_MASK, 0); /* XXXX */

	/* step 9. ETX Configuration: use mostly default values */

	/* Enable DMA */
	bus_space_write_4(t, h, GEM_TX_KICK, 0);
	v = gem_ringsize(GEM_NTXDESC /*XXX*/);
	bus_space_write_4(t, h, GEM_TX_CONFIG, 
		v|GEM_TX_CONFIG_TXDMA_EN|
		((0x400<<10)&GEM_TX_CONFIG_TXFIFO_TH));

	/* step 10. ERX Configuration */

	/* Encode Receive Descriptor ring size: four possible values */
	v = gem_ringsize(GEM_NRXDESC /*XXX*/);

	/* Enable DMA */
	bus_space_write_4(t, h, GEM_RX_CONFIG, 
		v|(GEM_THRSH_1024<<GEM_RX_CONFIG_FIFO_THRS_SHIFT)|
		(2<<GEM_RX_CONFIG_FBOFF_SHFT)|GEM_RX_CONFIG_RXDMA_EN|
		(0<<GEM_RX_CONFIG_CXM_START_SHFT));
	/*
	 * The following value is for an OFF Threshold of about 15.5 Kbytes
	 * and an ON Threshold of 4K bytes.
	 */
	bus_space_write_4(t, h, GEM_RX_PAUSE_THRESH, 0xf8 | (0x40 << 12));
	bus_space_write_4(t, h, GEM_RX_BLANKING, (2<<12)|6);

	/* step 11. Configure Media */
	gem_mii_statchg(&sc->sc_dev);

	/* step 12. RX_MAC Configuration Register */
	v = bus_space_read_4(t, h, GEM_MAC_RX_CONFIG);
	v |= GEM_MAC_RX_ENABLE;
	bus_space_write_4(t, h, GEM_MAC_RX_CONFIG, v);

	/* step 14. Issue Transmit Pending command */

	/* Call MI initialization function if any */
	if (sc->sc_hwinit)
		(*sc->sc_hwinit)(sc);


	/* step 15.  Give the reciever a swift kick */
	bus_space_write_4(t, h, GEM_RX_KICK, GEM_NRXDESC-4);

	/* Start the one second timer. */
	timeout_add(&sc->sc_tick_ch, hz);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;
	ifp->if_timer = 0;
	splx(s);

	return (0);
}

/*
 * Compare two Ether/802 addresses for equality, inlined and unrolled for
 * speed.
 */
static __inline__ int
ether_cmp(a, b)
	u_char *a, *b;
{       
        
	if (a[5] != b[5] || a[4] != b[4] || a[3] != b[3] ||
	    a[2] != b[2] || a[1] != b[1] || a[0] != b[0])
		return (0);
	return (1);
}


void
gem_init_regs(struct gem_softc *sc)
{
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t h = sc->sc_h;
	u_int32_t v;

	/* These regs are not cleared on reset */
	sc->sc_inited = 0;
	if (!sc->sc_inited) {

		/* Wooo.  Magic values. */
		bus_space_write_4(t, h, GEM_MAC_IPG0, 0);
		bus_space_write_4(t, h, GEM_MAC_IPG1, 8);
		bus_space_write_4(t, h, GEM_MAC_IPG2, 4);

		bus_space_write_4(t, h, GEM_MAC_MAC_MIN_FRAME, ETHER_MIN_LEN);
		/* Max frame and max burst size */
		v = (GEM_MTU) | (0x2000 << 16) /* Burst size */;
		bus_space_write_4(t, h, GEM_MAC_MAC_MAX_FRAME, v);
		bus_space_write_4(t, h, GEM_MAC_PREAMBLE_LEN, 0x7);
		bus_space_write_4(t, h, GEM_MAC_JAM_SIZE, 0x4);
		bus_space_write_4(t, h, GEM_MAC_ATTEMPT_LIMIT, 0x10);
		/* Dunno.... */
		bus_space_write_4(t, h, GEM_MAC_CONTROL_TYPE, 0x8088);
		bus_space_write_4(t, h, GEM_MAC_RANDOM_SEED,
			((sc->sc_enaddr[5]<<8)|sc->sc_enaddr[4])&0x3ff);
		/* Secondary MAC addr set to 0:0:0:0:0:0 */
		bus_space_write_4(t, h, GEM_MAC_ADDR3, 0);
		bus_space_write_4(t, h, GEM_MAC_ADDR4, 0);
		bus_space_write_4(t, h, GEM_MAC_ADDR5, 0);
		/* MAC control addr set to 0:1:c2:0:1:80 */
		bus_space_write_4(t, h, GEM_MAC_ADDR6, 0x0001);
		bus_space_write_4(t, h, GEM_MAC_ADDR7, 0xc200);
		bus_space_write_4(t, h, GEM_MAC_ADDR8, 0x0180);

		/* MAC filter addr set to 0:0:0:0:0:0 */
		bus_space_write_4(t, h, GEM_MAC_ADDR_FILTER0, 0);
		bus_space_write_4(t, h, GEM_MAC_ADDR_FILTER1, 0);
		bus_space_write_4(t, h, GEM_MAC_ADDR_FILTER2, 0);

		bus_space_write_4(t, h, GEM_MAC_ADR_FLT_MASK1_2, 0);
		bus_space_write_4(t, h, GEM_MAC_ADR_FLT_MASK0, 0);

		sc->sc_inited = 1;
	}

	/* Counters need to be zeroed */
	bus_space_write_4(t, h, GEM_MAC_NORM_COLL_CNT, 0);
	bus_space_write_4(t, h, GEM_MAC_FIRST_COLL_CNT, 0);
	bus_space_write_4(t, h, GEM_MAC_EXCESS_COLL_CNT, 0);
	bus_space_write_4(t, h, GEM_MAC_LATE_COLL_CNT, 0);
	bus_space_write_4(t, h, GEM_MAC_DEFER_TMR_CNT, 0);
	bus_space_write_4(t, h, GEM_MAC_PEAK_ATTEMPTS, 0);
	bus_space_write_4(t, h, GEM_MAC_RX_FRAME_COUNT, 0);
	bus_space_write_4(t, h, GEM_MAC_RX_LEN_ERR_CNT, 0);
	bus_space_write_4(t, h, GEM_MAC_RX_ALIGN_ERR, 0);
	bus_space_write_4(t, h, GEM_MAC_RX_CRC_ERR_CNT, 0);
	bus_space_write_4(t, h, GEM_MAC_RX_CODE_VIOL, 0);

	/* Un-pause stuff */
	bus_space_write_4(t, h, GEM_MAC_SEND_PAUSE_CMD, 0);

	/*
	 * Set the station address.
	 */
	bus_space_write_4(t, h, GEM_MAC_ADDR0, 
		(sc->sc_enaddr[4]<<8) | sc->sc_enaddr[5]);
	bus_space_write_4(t, h, GEM_MAC_ADDR1, 
		(sc->sc_enaddr[2]<<8) | sc->sc_enaddr[3]);
	bus_space_write_4(t, h, GEM_MAC_ADDR2, 
		(sc->sc_enaddr[0]<<8) | sc->sc_enaddr[1]);

}

/*
 * Receive interrupt.
 */
int
gem_rint(sc)
	struct gem_softc *sc;
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t h = sc->sc_h;
	struct ether_header *eh;
	struct gem_rxsoft *rxs;
	struct mbuf *m;
	u_int64_t rxstat;
	int i, len;

	/*
	 * XXXX Read the lastrx only once at the top for speed.
	 */
	DPRINTF(sc, ("gem_rint: sc->rxptr %d, complete %d\n",
		sc->sc_rxptr, bus_space_read_4(t, h, GEM_RX_COMPLETION)));
	for (i = sc->sc_rxptr; i != bus_space_read_4(t, h, GEM_RX_COMPLETION);
	     i = GEM_NEXTRX(i)) {
		rxs = &sc->sc_rxsoft[i];

		GEM_CDRXSYNC(sc, i,
		    BUS_DMASYNC_POSTREAD|BUS_DMASYNC_POSTWRITE);

		rxstat = GEM_DMA_READ(sc, sc->sc_rxdescs[i].gd_flags);

		if (rxstat & GEM_RD_OWN) {
			printf("gem_rint: completed descriptor "
				"still owned %d\n", i);
			/*
			 * We have processed all of the receive buffers.
			 */
			break;
		}

		if (rxstat & GEM_RD_BAD_CRC) {
			printf("%s: receive error: CRC error\n",
				sc->sc_dev.dv_xname);
			GEM_INIT_RXDESC(sc, i);
			continue;
		}

		bus_dmamap_sync(sc->sc_dmatag, rxs->rxs_dmamap, 0,
		    rxs->rxs_dmamap->dm_mapsize, BUS_DMASYNC_POSTREAD);
#ifdef GEM_DEBUG
		if (ifp->if_flags & IFF_DEBUG) {
			printf("    rxsoft %p descriptor %d: ", rxs, i);
			printf("gd_flags: 0x%016llx\t", (long long)
				GEM_DMA_READ(sc, sc->sc_rxdescs[i].gd_flags));
			printf("gd_addr: 0x%016llx\n", (long long)
				GEM_DMA_READ(sc, sc->sc_rxdescs[i].gd_addr));
		}
#endif

		/*
		 * No errors; receive the packet.  Note the Gem
		 * includes the CRC with every packet.
		 */
		len = GEM_RD_BUFLEN(rxstat);

		/*
		 * Allocate a new mbuf cluster.  If that fails, we are
		 * out of memory, and must drop the packet and recycle
		 * the buffer that's already attached to this descriptor.
		 */
		m = rxs->rxs_mbuf;
		if (gem_add_rxbuf(sc, i) != 0) {
			ifp->if_ierrors++;
			GEM_INIT_RXDESC(sc, i);
			bus_dmamap_sync(sc->sc_dmatag, rxs->rxs_dmamap, 0,
			    rxs->rxs_dmamap->dm_mapsize, BUS_DMASYNC_PREREAD);
			continue;
		}
		m->m_data += 2; /* We're already off by two */

		ifp->if_ipackets++;
		eh = mtod(m, struct ether_header *);
		m->m_pkthdr.rcvif = ifp;
		m->m_pkthdr.len = m->m_len = len;

#if NBPFILTER > 0
		/*
		 * Pass this up to any BPF listeners, but only
		 * pass it up the stack if its for us.
		 */
		if (ifp->if_bpf)
			bpf_mtap(ifp->if_bpf, m);
#endif /* NPBFILTER > 0 */

		/* Pass it on. */
		ether_input_mbuf(ifp, m);
	}

	/* Update the receive pointer. */
	sc->sc_rxptr = i;
	bus_space_write_4(t, h, GEM_RX_KICK, i);

	DPRINTF(sc, ("gem_rint: done sc->rxptr %d, complete %d\n",
		sc->sc_rxptr, bus_space_read_4(t, h, GEM_RX_COMPLETION)));

	return (1);
}


/*
 * gem_add_rxbuf:
 *
 *	Add a receive buffer to the indicated descriptor.
 */
int
gem_add_rxbuf(struct gem_softc *sc, int idx)
{
	struct gem_rxsoft *rxs = &sc->sc_rxsoft[idx];
	struct mbuf *m;
	int error;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return (ENOBUFS);

	MCLGET(m, M_DONTWAIT);
	if ((m->m_flags & M_EXT) == 0) {
		m_freem(m);
		return (ENOBUFS);
	}

#ifdef GEM_DEBUG
/* bzero the packet to check dma */
	memset(m->m_ext.ext_buf, 0, m->m_ext.ext_size);
#endif

	if (rxs->rxs_mbuf != NULL)
		bus_dmamap_unload(sc->sc_dmatag, rxs->rxs_dmamap);

	rxs->rxs_mbuf = m;

	error = bus_dmamap_load(sc->sc_dmatag, rxs->rxs_dmamap,
	    m->m_ext.ext_buf, m->m_ext.ext_size, NULL,
	    BUS_DMA_READ|BUS_DMA_NOWAIT);
	if (error) {
		printf("%s: can't load rx DMA map %d, error = %d\n",
		    sc->sc_dev.dv_xname, idx, error);
		panic("gem_add_rxbuf");	/* XXX */
	}

	bus_dmamap_sync(sc->sc_dmatag, rxs->rxs_dmamap, 0,
	    rxs->rxs_dmamap->dm_mapsize, BUS_DMASYNC_PREREAD);

	GEM_INIT_RXDESC(sc, idx);

	return (0);
}


int
gem_eint(sc, status)
	struct gem_softc *sc;
	u_int status;
{
	if ((status & GEM_INTR_MIF) != 0) {
		printf("%s: XXXlink status changed\n", sc->sc_dev.dv_xname);
		return (1);
	}

	printf("%s: status=%b\n", sc->sc_dev.dv_xname, status, GEM_INTR_BITS);
	return (1);
}


int
gem_intr(v)
	void *v;
{
	struct gem_softc *sc = (struct gem_softc *)v;
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t seb = sc->sc_h;
	u_int32_t status;
	int r = 0;

	status = bus_space_read_4(t, seb, GEM_STATUS);
	DPRINTF(sc, ("%s: gem_intr: cplt %xstatus %b\n",
		sc->sc_dev.dv_xname, (status>>19), status, GEM_INTR_BITS));

	if ((status & (GEM_INTR_RX_TAG_ERR | GEM_INTR_BERR)) != 0)
		r |= gem_eint(sc, status);

	if ((status & (GEM_INTR_TX_EMPTY | GEM_INTR_TX_INTME)) != 0)
		r |= gem_tint(sc, status);

	if ((status & (GEM_INTR_RX_DONE | GEM_INTR_RX_NOBUF)) != 0)
		r |= gem_rint(sc);

	/* We should eventually do more than just print out error stats. */
	if (status & GEM_INTR_TX_MAC) {
		int txstat = bus_space_read_4(t, seb, GEM_MAC_TX_STATUS);
		if (txstat & ~GEM_MAC_TX_XMIT_DONE)
			printf("MAC tx fault, status %x\n", txstat);
	}
	if (status & GEM_INTR_RX_MAC) {
		int rxstat = bus_space_read_4(t, seb, GEM_MAC_RX_STATUS);
		if (rxstat & ~GEM_MAC_RX_DONE)
			printf("MAC rx fault, status %x\n", rxstat);
	}
	return (r);
}


void
gem_watchdog(ifp)
	struct ifnet *ifp;
{
	struct gem_softc *sc = ifp->if_softc;

	DPRINTF(sc, ("gem_watchdog: GEM_RX_CONFIG %x GEM_MAC_RX_STATUS %x "
		"GEM_MAC_RX_CONFIG %x\n",
		bus_space_read_4(sc->sc_bustag, sc->sc_h, GEM_RX_CONFIG),
		bus_space_read_4(sc->sc_bustag, sc->sc_h, GEM_MAC_RX_STATUS),
		bus_space_read_4(sc->sc_bustag, sc->sc_h, GEM_MAC_RX_CONFIG)));

	log(LOG_ERR, "%s: device timeout\n", sc->sc_dev.dv_xname);
	++ifp->if_oerrors;

	/* Try to get more packets going. */
	gem_init(ifp);
}

/*
 * Initialize the MII Management Interface
 */
void
gem_mifinit(sc)
	struct gem_softc *sc;
{
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t mif = sc->sc_h;

	/* Configure the MIF in frame mode */
	sc->sc_mif_config = bus_space_read_4(t, mif, GEM_MIF_CONFIG);
	sc->sc_mif_config &= ~GEM_MIF_CONFIG_BB_ENA;
	bus_space_write_4(t, mif, GEM_MIF_CONFIG, sc->sc_mif_config);
}

/*
 * MII interface
 *
 * The GEM MII interface supports at least three different operating modes:
 *
 * Bitbang mode is implemented using data, clock and output enable registers.
 *
 * Frame mode is implemented by loading a complete frame into the frame
 * register and polling the valid bit for completion.
 *
 * Polling mode uses the frame register but completion is indicated by
 * an interrupt.
 *
 */
static int
gem_mii_readreg(self, phy, reg)
	struct device *self;
	int phy, reg;
{
	struct gem_softc *sc = (void *)self;
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t mif = sc->sc_h;
	int n;
	u_int32_t v;

#ifdef GEM_DEBUG1
	if (sc->sc_debug)
		printf("gem_mii_readreg: phy %d reg %d\n", phy, reg);
#endif

	/* Construct the frame command */
	v = (reg << GEM_MIF_REG_SHIFT)	| (phy << GEM_MIF_PHY_SHIFT) |
		GEM_MIF_FRAME_READ;

	bus_space_write_4(t, mif, GEM_MIF_FRAME, v);
	for (n = 0; n < 100; n++) {
		DELAY(1);
		v = bus_space_read_4(t, mif, GEM_MIF_FRAME);
		if (v & GEM_MIF_FRAME_TA0)
			return (v & GEM_MIF_FRAME_DATA);
	}

	printf("%s: mii_read timeout\n", sc->sc_dev.dv_xname);
	return (0);
}

static void
gem_mii_writereg(self, phy, reg, val)
	struct device *self;
	int phy, reg, val;
{
	struct gem_softc *sc = (void *)self;
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t mif = sc->sc_h;
	int n;
	u_int32_t v;

#ifdef GEM_DEBUG1
	if (sc->sc_debug)
		printf("gem_mii_writereg: phy %d reg %d val %x\n", 
			phy, reg, val);
#endif

#if 0
	/* Select the desired PHY in the MIF configuration register */
	v = bus_space_read_4(t, mif, GEM_MIF_CONFIG);
	/* Clear PHY select bit */
	v &= ~GEM_MIF_CONFIG_PHY_SEL;
	if (phy == GEM_PHYAD_EXTERNAL)
		/* Set PHY select bit to get at external device */
		v |= GEM_MIF_CONFIG_PHY_SEL;
	bus_space_write_4(t, mif, GEM_MIF_CONFIG, v);
#endif
	/* Construct the frame command */
	v = GEM_MIF_FRAME_WRITE			|
	    (phy << GEM_MIF_PHY_SHIFT)		|
	    (reg << GEM_MIF_REG_SHIFT)		|
	    (val & GEM_MIF_FRAME_DATA);

	bus_space_write_4(t, mif, GEM_MIF_FRAME, v);
	for (n = 0; n < 100; n++) {
		DELAY(1);
		v = bus_space_read_4(t, mif, GEM_MIF_FRAME);
		if (v & GEM_MIF_FRAME_TA0)
			return;
	}

	printf("%s: mii_write timeout\n", sc->sc_dev.dv_xname);
}

static void
gem_mii_statchg(dev)
	struct device *dev;
{
	struct gem_softc *sc = (void *)dev;
#ifdef GEM_DEBUG
	int instance = IFM_INST(sc->sc_mii.mii_media.ifm_cur->ifm_media);
#endif
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t mac = sc->sc_h;
	u_int32_t v;

#ifdef GEM_DEBUG
	if (sc->sc_debug)
		printf("gem_mii_statchg: status change: phy = %d\n",
		    sc->sc_phys[instance]);
#endif


	/* Set tx full duplex options */
	bus_space_write_4(t, mac, GEM_MAC_TX_CONFIG, 0);
	delay(10000); /* reg must be cleared and delay before changing. */
	v = GEM_MAC_TX_ENA_IPG0|GEM_MAC_TX_NGU|GEM_MAC_TX_NGU_LIMIT|
		GEM_MAC_TX_ENABLE;
	if ((IFM_OPTIONS(sc->sc_mii.mii_media_active) & IFM_FDX) != 0) {
		v |= GEM_MAC_TX_IGN_CARRIER|GEM_MAC_TX_IGN_COLLIS;
	}
	bus_space_write_4(t, mac, GEM_MAC_TX_CONFIG, v);

	/* XIF Configuration */
 /* We should really calculate all this rather than rely on defaults */
	v = bus_space_read_4(t, mac, GEM_MAC_XIF_CONFIG);
	v = GEM_MAC_XIF_LINK_LED;
	v |= GEM_MAC_XIF_TX_MII_ENA;
	/* If an external transceiver is connected, enable its MII drivers */
	sc->sc_mif_config = bus_space_read_4(t, mac, GEM_MIF_CONFIG);
	if ((sc->sc_mif_config & GEM_MIF_CONFIG_MDI1) != 0) {
		/* External MII needs echo disable if half duplex. */
		if ((IFM_OPTIONS(sc->sc_mii.mii_media_active) & IFM_FDX) != 0)
			/* turn on full duplex LED */
			v |= GEM_MAC_XIF_FDPLX_LED;
 			else
	 			/* half duplex -- disable echo */
		 		v |= GEM_MAC_XIF_ECHO_DISABL;
	} else 
		/* Internal MII needs buf enable */
		v |= GEM_MAC_XIF_MII_BUF_ENA;
	bus_space_write_4(t, mac, GEM_MAC_XIF_CONFIG, v);
}

int
gem_mediachange(ifp)
	struct ifnet *ifp;
{
	struct gem_softc *sc = ifp->if_softc;

	if (IFM_TYPE(sc->sc_media.ifm_media) != IFM_ETHER)
		return (EINVAL);

	return (mii_mediachg(&sc->sc_mii));
}

void
gem_mediastatus(ifp, ifmr)
	struct ifnet *ifp;
	struct ifmediareq *ifmr;
{
	struct gem_softc *sc = ifp->if_softc;

	if ((ifp->if_flags & IFF_UP) == 0)
		return;

	mii_pollstat(&sc->sc_mii);
	ifmr->ifm_active = sc->sc_mii.mii_media_active;
	ifmr->ifm_status = sc->sc_mii.mii_media_status;
}

int gem_ioctldebug = 0;
/*
 * Process an ioctl request.
 */
int
gem_ioctl(ifp, cmd, data)
	struct ifnet *ifp;
	u_long cmd;
	caddr_t data;
{
	struct gem_softc *sc = ifp->if_softc;
	struct ifaddr *ifa = (struct ifaddr *)data;
	struct ifreq *ifr = (struct ifreq *)data;
	int s, error = 0;

	s = splimp();

	switch (cmd) {

	case SIOCSIFADDR:
		ifp->if_flags |= IFF_UP;

		switch (ifa->ifa_addr->sa_family) {
#ifdef INET
		case AF_INET:
			gem_init(ifp);
			arp_ifinit(&sc->sc_arpcom, ifa);
			break;
#endif
#ifdef NS
		case AF_NS:
		    {
			struct ns_addr *ina = &IA_SNS(ifa)->sns_addr;

			if (ns_nullhost(*ina))
				ina->x_host =
				    *(union ns_host *)LLADDR(ifp->if_sadl);
			else {
				memcpy(LLADDR(ifp->if_sadl),
				    ina->x_host.c_host, sizeof(sc->sc_enaddr));
			}	
			/* Set new address. */
			gem_init(ifp);
			break;
		    }
#endif
		default:
			gem_init(ifp);
			break;
		}
		break;

	case SIOCSIFFLAGS:
		if ((ifp->if_flags & IFF_UP) == 0 &&
		    (ifp->if_flags & IFF_RUNNING) != 0) {
			/*
			 * If interface is marked down and it is running, then
			 * stop it.
			 */
			gem_stop(ifp, 1);
			ifp->if_flags &= ~IFF_RUNNING;
		} else if ((ifp->if_flags & IFF_UP) != 0 &&
		    	   (ifp->if_flags & IFF_RUNNING) == 0) {
			/*
			 * If interface is marked up and it is stopped, then
			 * start it.
			 */
			gem_init(ifp);
		} else if ((ifp->if_flags & IFF_UP) != 0) {
			/*
			 * Reset the interface to pick up changes in any other
			 * flags that affect hardware registers.
			 */
			/*gem_stop(sc);*/
			gem_init(ifp);
		}
#ifdef HMEDEBUG
		sc->sc_debug = (ifp->if_flags & IFF_DEBUG) != 0 ? 1 : 0;
#endif
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		error = (cmd == SIOCADDMULTI) ?
		    ether_addmulti(ifr, &sc->sc_arpcom) :
		    ether_delmulti(ifr, &sc->sc_arpcom);

		if (error == ENETRESET) {
			/*
			 * Multicast list has changed; set the hardware filter
			 * accordingly.
			 */
			gem_setladrf(sc);
			error = 0;
		}
		break;

	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->sc_media, cmd);
		break;

	default:
		error = EINVAL;
		break;
	}

	splx(s);
	return (error);
}


void
gem_shutdown(arg)
	void *arg;
{
	struct gem_softc *sc = (struct gem_softc *)arg;
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;

	gem_stop(ifp, 1);
}

/*
 * Set up the logical address filter.
 */
void
gem_setladrf(sc)
	struct gem_softc *sc;
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	struct ether_multi *enm;
	struct ether_multistep step;
	struct arpcom *ac = &sc->sc_arpcom;
	bus_space_tag_t t = sc->sc_bustag;
	bus_space_handle_t h = sc->sc_h;
	u_char *cp;
	u_int32_t crc;
	u_int32_t hash[16];
	u_int32_t v;
	int len;

	/* Clear hash table */
	memset(hash, 0, sizeof(hash));

	/* Get current RX configuration */
	v = bus_space_read_4(t, h, GEM_MAC_RX_CONFIG);

	if ((ifp->if_flags & IFF_PROMISC) != 0) {
		/* Turn on promiscuous mode; turn off the hash filter */
		v |= GEM_MAC_RX_PROMISCUOUS;
		v &= ~GEM_MAC_RX_HASH_FILTER;
		ifp->if_flags |= IFF_ALLMULTI;
		goto chipit;
	}

	/* Turn off promiscuous mode; turn on the hash filter */
	v &= ~GEM_MAC_RX_PROMISCUOUS;
	v |= GEM_MAC_RX_HASH_FILTER;

	/*
	 * Set up multicast address filter by passing all multicast addresses
	 * through a crc generator, and then using the high order 6 bits as an
	 * index into the 256 bit logical address filter.  The high order bit
	 * selects the word, while the rest of the bits select the bit within
	 * the word.
	 */

	ETHER_FIRST_MULTI(step, ac, enm);
	while (enm != NULL) {
		if (ether_cmp(enm->enm_addrlo, enm->enm_addrhi)) {
			/*
			 * We must listen to a range of multicast addresses.
			 * For now, just accept all multicasts, rather than
			 * trying to set only those filter bits needed to match
			 * the range.  (At this time, the only use of address
			 * ranges is for IP multicast routing, for which the
			 * range is big enough to require all bits set.)
			 */
			hash[3] = hash[2] = hash[1] = hash[0] = 0xffff;
			ifp->if_flags |= IFF_ALLMULTI;
			goto chipit;
		}

		cp = enm->enm_addrlo;
		crc = 0xffffffff;
		for (len = sizeof(enm->enm_addrlo); --len >= 0;) {
			int octet = *cp++;
			int i;

#define MC_POLY_LE	0xedb88320UL	/* mcast crc, little endian */
			for (i = 0; i < 8; i++) {
				if ((crc & 1) ^ (octet & 1)) {
					crc >>= 1;
					crc ^= MC_POLY_LE;
				} else {
					crc >>= 1;
				}
				octet >>= 1;
			}
		}
		/* Just want the 8 most significant bits. */
		crc >>= 24;

		/* Set the corresponding bit in the filter. */
		hash[crc >> 4] |= 1 << (crc & 0xf);

		ETHER_NEXT_MULTI(step, enm);
	}

	ifp->if_flags &= ~IFF_ALLMULTI;

chipit:
	/* Now load the hash table into the chip */
	bus_space_write_4(t, h, GEM_MAC_HASH0, hash[0]);
	bus_space_write_4(t, h, GEM_MAC_HASH1, hash[1]);
	bus_space_write_4(t, h, GEM_MAC_HASH2, hash[2]);
	bus_space_write_4(t, h, GEM_MAC_HASH3, hash[3]);
	bus_space_write_4(t, h, GEM_MAC_HASH4, hash[4]);
	bus_space_write_4(t, h, GEM_MAC_HASH5, hash[5]);
	bus_space_write_4(t, h, GEM_MAC_HASH6, hash[6]);
	bus_space_write_4(t, h, GEM_MAC_HASH7, hash[7]);
	bus_space_write_4(t, h, GEM_MAC_HASH8, hash[8]);
	bus_space_write_4(t, h, GEM_MAC_HASH9, hash[9]);
	bus_space_write_4(t, h, GEM_MAC_HASH10, hash[10]);
	bus_space_write_4(t, h, GEM_MAC_HASH11, hash[11]);
	bus_space_write_4(t, h, GEM_MAC_HASH12, hash[12]);
	bus_space_write_4(t, h, GEM_MAC_HASH13, hash[13]);
	bus_space_write_4(t, h, GEM_MAC_HASH14, hash[14]);
	bus_space_write_4(t, h, GEM_MAC_HASH15, hash[15]);

	bus_space_write_4(t, h, GEM_MAC_RX_CONFIG, v);
}

int
gem_encap(sc, mhead, bixp)
	struct gem_softc *sc;
	struct mbuf *mhead;
	u_int32_t *bixp;
{
	u_int64_t flags;
	u_int32_t cur, frag, i;
	bus_dmamap_t map;

	cur = frag = *bixp;

	if (bus_dmamap_create(sc->sc_dmatag, MCLBYTES, GEM_NTXDESC,
	    MCLBYTES, 0, BUS_DMA_NOWAIT, &map) != 0) {
		return (ENOBUFS);
	}

	if (bus_dmamap_load_mbuf(sc->sc_dmatag, map, mhead,
	    BUS_DMA_NOWAIT) != 0) {
		bus_dmamap_destroy(sc->sc_dmatag, map);
		return (ENOBUFS);
	}

	if ((sc->sc_tx_cnt + map->dm_nsegs) > (GEM_NTXDESC - 2)) {
		bus_dmamap_unload(sc->sc_dmatag, map);
		bus_dmamap_destroy(sc->sc_dmatag, map);
		return (ENOBUFS);
	}

	bus_dmamap_sync(sc->sc_dmatag, map, 0, map->dm_mapsize,
	    BUS_DMASYNC_PREWRITE);

	for (i = 0; i < map->dm_nsegs; i++) {
		sc->sc_txdescs[frag].gd_addr =
		    GEM_DMA_WRITE(sc, map->dm_segs[i].ds_addr);
		flags = (map->dm_segs[i].ds_len & GEM_TD_BUFSIZE) |
		    (i == 0 ? GEM_TD_START_OF_PACKET : 0) |
		    ((i == (map->dm_nsegs - 1)) ? GEM_TD_END_OF_PACKET : 0);
		sc->sc_txdescs[frag].gd_flags = GEM_DMA_WRITE(sc, flags);
		bus_dmamap_sync(sc->sc_dmatag, sc->sc_cddmamap,
		    GEM_CDTXOFF(frag), sizeof(struct gem_desc),
		    BUS_DMASYNC_PREWRITE);
		cur = frag;
		if (++frag == GEM_NTXDESC)
			frag = 0;
	}

	sc->sc_tx_cnt += map->dm_nsegs;
	sc->sc_txd[cur].sd_map = map;
	sc->sc_txd[cur].sd_mbuf = mhead;

	bus_space_write_4(sc->sc_bustag, sc->sc_h, GEM_TX_KICK, frag);

	*bixp = frag;

	/* sync descriptors */

	return (0);
}

/*
 * Transmit interrupt.
 */
int
gem_tint(sc, status)
	struct gem_softc *sc;
	u_int32_t status;
{
	struct ifnet *ifp = &sc->sc_arpcom.ac_if;
	struct gem_sxd *sd;
	u_int32_t cons, hwcons;

	hwcons = status >> 19;
	cons = sc->sc_tx_cons;
	while (cons != hwcons) {
		sd = &sc->sc_txd[cons];
		if (sd->sd_map != NULL) {
			bus_dmamap_sync(sc->sc_dmatag, sd->sd_map, 0,
			    sd->sd_map->dm_mapsize, BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->sc_dmatag, sd->sd_map);
			bus_dmamap_destroy(sc->sc_dmatag, sd->sd_map);
			sd->sd_map = NULL;
		}
		if (sd->sd_mbuf != NULL) {
			m_freem(sd->sd_mbuf);
			sd->sd_mbuf = NULL;
		}
		sc->sc_tx_cnt--;
		if (++cons == GEM_NTXDESC)
			cons = 0;
	}
	sc->sc_tx_cons = cons;

	gem_start(ifp);

	if (sc->sc_tx_cnt == 0)
		ifp->if_timer = 0;

	return (1);
}

void
gem_start(ifp)
	struct ifnet *ifp;
{
	struct gem_softc *sc = ifp->if_softc;
	struct mbuf *m;
	u_int32_t bix;

	if ((ifp->if_flags & (IFF_RUNNING | IFF_OACTIVE)) != IFF_RUNNING)
		return;

	bix = sc->sc_tx_prod;
	while (sc->sc_txd[bix].sd_mbuf == NULL) {
		IFQ_POLL(&ifp->if_snd, m);
		if (m == NULL)
			break;

#if NBPFILTER > 0
		/*
		 * If BPF is listening on this interface, let it see the
		 * packet before we commit it to the wire.
		 */
		if (ifp->if_bpf)
			bpf_mtap(ifp->if_bpf, m);
#endif

		/*
		 * Encapsulate this packet and start it going...
		 * or fail...
		 */
		if (gem_encap(sc, m, &bix)) {
			ifp->if_flags |= IFF_OACTIVE;
			break;
		}

		IFQ_DEQUEUE(&ifp->if_snd, m);
		ifp->if_timer = 5;
	}

	sc->sc_tx_prod = bix;
}
