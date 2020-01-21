/*	$OpenBSD: dt_dev.c,v 1.1 2020/01/21 16:16:23 mpi Exp $ */

/*
 * Copyright (c) 2019 Martin Pieuchot <mpi@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/param.h>
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/proc.h>

#include <dev/dt/dtvar.h>

/*
 * How many frames are used by the profiling code?  For example
 * on amd64:
 *
 * From syscall provider:
 *
 *	dt_prov_syscall_entry+0x141
 *	syscall+0x205		<--- start here
 *	Xsyscall+0x128
 *
 * From profile provider:
 *
 *	dt_prov_profile_enter+0x6e
 *	hardclock+0x12c
 *	clockintr+0x59
 *	intr_handler+0x6e
 *	Xresume_legacy0+0x1d3
 *	cpu_idle_cycle+0x1b	<---- start here.
 *	proc_trampoline+0x1c
 */
#if notyet
#define DT_HOOK_FRAME_ADDRESS	__builtin_frame_address(4)
#else
#define DT_HOOK_FRAME_ADDRESS	__builtin_frame_address(0)
#endif

#define DT_EVTRING_SIZE	16	/* # of slots in per PCB event ring */

#define DPRINTF(x...) /* nothing */

/*
 * Descriptor associated with each program opening /dev/dt.  It is used
 * to keep track of enabled PCBs.
 *
 *  Locks used to protect struct members in this file:
 *	m	per-softc mutex
 *	k	kernel lock
 */
struct dt_softc {
	SLIST_ENTRY(dt_softc)	 ds_next;	/* [k] descriptor list */
	int			 ds_unit;	/* [I] D_CLONE unique unit */
	pid_t			 ds_pid;	/* [I] PID of tracing program */

	struct mutex		 ds_mtx;

	struct dt_pcb_list	 ds_pcbs;	/* [k] list of enabled PCBs */
	struct dt_evt		*ds_bufqueue;	/* [k] copy evts to userland */
	size_t			 ds_bufqlen;	/* [k] length of the queue */
	int			 ds_recording;	/* [k] currently recording? */
	int			 ds_evtcnt;	/* [m] # of readable evts */

	/* Counters */
	uint64_t		 ds_readevt;	/* [m] # of events read */
	uint64_t		 ds_dropevt;	/* [m] # of events dropped */
};

SLIST_HEAD(, dt_softc) dtdev_list;	/* [k] list of open /dev/dt nodes */

/*
 * Probes are created during dt_attach() and never modified/freed during
 * the lifetime of the system.  That's why we consider them as [I]mmutable.
 */
unsigned int			dt_nprobes;	/* [I] # of probes available */
SIMPLEQ_HEAD(, dt_probe)	dt_probe_list;	/* [I] list of probes */

struct rwlock			dt_lock = RWLOCK_INITIALIZER("dtlk");
volatile uint32_t		dt_tracing = 0;	/* [d] # of processes tracing */

void	dtattach(struct device *, struct device *, void *);
int	dtopen(dev_t, int, int, struct proc *);
int	dtclose(dev_t, int, int, struct proc *);
int	dtread(dev_t, struct uio *, int);
int	dtioctl(dev_t, u_long, caddr_t, int, struct proc *);

struct	dt_softc *dtlookup(int);

int	dt_ioctl_list_probes(struct dt_softc *, struct dtioc_probe *);
int	dt_ioctl_get_stats(struct dt_softc *, struct dtioc_stat *);
int	dt_ioctl_record_start(struct dt_softc *);
void	dt_ioctl_record_stop(struct dt_softc *);
int	dt_ioctl_probe_enable(struct dt_softc *, struct dtioc_req *);
void	dt_ioctl_probe_disable(struct dt_softc *, struct dtioc_req *);

int	dt_enter(void);
void	dt_leave(uint32_t);

int	dt_pcb_ring_copy(struct dt_pcb *, struct dt_evt *, size_t, uint64_t *);

void
dtattach(struct device *parent, struct device *self, void *aux)
{
	SLIST_INIT(&dtdev_list);
	SIMPLEQ_INIT(&dt_probe_list);

	/* Init providers */
	dt_nprobes += dt_prov_profile_init();
	dt_nprobes += dt_prov_syscall_init();
	dt_nprobes += dt_prov_static_init();

	printf("dt: %u probes\n", dt_nprobes);
}

int
dtopen(dev_t dev, int flags, int mode, struct proc *p)
{
	struct dt_softc *sc;
	int unit = minor(dev);

	KASSERT(dtlookup(unit) == NULL);

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK|M_CANFAIL|M_ZERO);
	if (sc == NULL)
		return ENOMEM;

	/*
	 * Enough space to empty 2 full rings of events in a single read.
	 */
	sc->ds_bufqlen = 2 * DT_EVTRING_SIZE;
	sc->ds_bufqueue = mallocarray(sc->ds_bufqlen, sizeof(*sc->ds_bufqueue),
	    M_DEVBUF, M_WAITOK|M_CANFAIL);
	if (sc->ds_bufqueue == NULL)
		goto bad;

	sc->ds_unit = unit;
	sc->ds_pid = p->p_p->ps_pid;
	TAILQ_INIT(&sc->ds_pcbs);
	mtx_init(&sc->ds_mtx, IPL_HIGH);
	sc->ds_evtcnt = 0;
	sc->ds_readevt = 0;
	sc->ds_dropevt = 0;

	SLIST_INSERT_HEAD(&dtdev_list, sc, ds_next);

	DPRINTF("dt%d: pid %d open\n", sc->ds_unit, sc->ds_pid);

	return 0;

bad:
	free(sc, M_DEVBUF, sizeof(*sc));
	return ENOMEM;
}

int
dtclose(dev_t dev, int flags, int mode, struct proc *p)
{
	struct dt_softc *sc;
	int unit = minor(dev);

	sc = dtlookup(unit);
	KASSERT(sc != NULL);

	DPRINTF("dt%d: pid %d close\n", sc->ds_unit, sc->ds_pid);

	SLIST_REMOVE(&dtdev_list, sc, dt_softc, ds_next);
	dt_ioctl_record_stop(sc);
	dt_pcb_purge(&sc->ds_pcbs);

	free(sc->ds_bufqueue, M_DEVBUF,
	    sc->ds_bufqlen * sizeof(*sc->ds_bufqueue));
	free(sc, M_DEVBUF, sizeof(*sc));

	return 0;
}

int
dtread(dev_t dev, struct uio *uio, int flags)
{
	struct dt_softc *sc;
	struct dt_evt *estq;
	struct dt_pcb *dp;
	int error, unit = minor(dev);
	size_t qlen, count, read = 0;
	uint64_t dropped = 0;

	sc = dtlookup(unit);
	KASSERT(sc != NULL);

	count = howmany(uio->uio_resid, sizeof(struct dt_evt));
	if (count < 1)
		return (EMSGSIZE);

	mtx_enter(&sc->ds_mtx);
	while (!sc->ds_evtcnt) {
		error = msleep(sc, &sc->ds_mtx, PWAIT|PCATCH, "dtread", 0);
		if (error == EINTR || error == ERESTART)
			break;
	}
	mtx_leave(&sc->ds_mtx);

	if (error)
		return error;

	estq = sc->ds_bufqueue;
	qlen = MIN(sc->ds_bufqlen, count);

	KERNEL_ASSERT_LOCKED();
	TAILQ_FOREACH(dp, &sc->ds_pcbs, dp_snext) {
		count = dt_pcb_ring_copy(dp, estq, qlen, &dropped);
		read += count;
		estq += count; /* pointer aritmetic */
		qlen -= count;
		if (qlen == 0)
			break;
	}
	if (read > 0)
		uiomove(sc->ds_bufqueue, read * sizeof(struct dt_evt), uio);

	mtx_enter(&sc->ds_mtx);
	sc->ds_evtcnt -= read;
	sc->ds_readevt += read;
	sc->ds_dropevt += dropped;
	mtx_leave(&sc->ds_mtx);

	return 0;
}

int
dtioctl(dev_t dev, u_long cmd, caddr_t addr, int flag, struct proc *p)
{
	struct dt_softc *sc;
	int unit = minor(dev);
	int on, error = 0;

	sc = dtlookup(unit);
	KASSERT(sc != NULL);

	switch (cmd) {
	case DTIOCGPLIST:
		return dt_ioctl_list_probes(sc, (struct dtioc_probe *)addr);
	case DTIOCGSTATS:
		return dt_ioctl_get_stats(sc, (struct dtioc_stat *)addr);
	case DTIOCRECORD:
	case DTIOCPRBENABLE:
		/* root only ioctl(2) */
		break;
	default:
		return ENOTTY;
	}

	if ((error = suser(p)) != 0)
		return error;

	switch (cmd) {
	case DTIOCRECORD:
		on = *(int *)addr;
		if (on)
			error = dt_ioctl_record_start(sc);
		else
			dt_ioctl_record_stop(sc);
		break;
	case DTIOCPRBENABLE:
		error = dt_ioctl_probe_enable(sc, (struct dtioc_req *)addr);
		break;
	default:
		KASSERT(0);
	}

	return error;
}

struct dt_softc *
dtlookup(int unit)
{
	struct dt_softc *sc;

	KERNEL_ASSERT_LOCKED();

	SLIST_FOREACH(sc, &dtdev_list, ds_next) {
		if (sc->ds_unit == unit)
			break;
	}

	return sc;
}

int
dtioc_req_isvalid(struct dtioc_req *dtrq)
{
	switch (dtrq->dtrq_filter.dtf_operand) {
	case DT_OP_NONE:
	case DT_OP_EQ:
	case DT_OP_NE:
		break;
	default:
		return 0;
	}

	switch (dtrq->dtrq_filter.dtf_variable) {
	case DT_FV_NONE:
	case DT_FV_PID:
	case DT_FV_TID:
		break;
	default:
		return 0;
	}

	return 1;
}

int
dt_ioctl_list_probes(struct dt_softc *sc, struct dtioc_probe *dtpr)
{
	struct dtioc_probe_info info, *dtpi;
	struct dt_probe *dtp;
	size_t size;
	int error = 0;

	if (dtpr->dtpr_size == 0) {
		dtpr->dtpr_size = dt_nprobes * sizeof(*dtpi);
		return 0;
	}

	size = dtpr->dtpr_size;
	dtpi = dtpr->dtpr_probes;
	memset(&info, 0, sizeof(info));
	SIMPLEQ_FOREACH(dtp, &dt_probe_list, dtp_next) {
		if (size < sizeof(*dtpi)) {
			error = ENOSPC;
			break;
		}
		info.dtpi_pbn = dtp->dtp_pbn;
		strlcpy(info.dtpi_prov, dtp->dtp_prov->dtpv_name,
		    sizeof(info.dtpi_prov));
		strlcpy(info.dtpi_func, dtp->dtp_func, sizeof(info.dtpi_func));
		strlcpy(info.dtpi_name, dtp->dtp_name, sizeof(info.dtpi_name));
		error = copyout(&info, dtpi, sizeof(*dtpi));
		if (error)
			break;
		size -= sizeof(*dtpi);
		dtpi++;
	};

	return error;
}

int
dt_ioctl_get_stats(struct dt_softc *sc, struct dtioc_stat *dtst)
{
	mtx_enter(&sc->ds_mtx);
	dtst->dtst_readevt = sc->ds_readevt;
	dtst->dtst_dropevt = sc->ds_dropevt;
	mtx_leave(&sc->ds_mtx);

	return 0;
}

int
dt_ioctl_record_start(struct dt_softc *sc)
{
	struct dt_pcb *dp;
	int count;

	if (sc->ds_recording)
		return EBUSY;

	KERNEL_ASSERT_LOCKED();
 	if (TAILQ_EMPTY(&sc->ds_pcbs))
		return ENOENT;

	count = dt_enter();
	TAILQ_FOREACH(dp, &sc->ds_pcbs, dp_snext) {
		struct dt_probe *dtp = dp->dp_dtp;

		rw_assert_wrlock(&dt_lock);
		SMR_SLIST_INSERT_HEAD_LOCKED(&dtp->dtp_pcbs, dp, dp_pnext);
		dtp->dtp_recording++;
		dtp->dtp_prov->dtpv_recording++;
	}
	dt_leave(count);

	sc->ds_recording = 1;
	dt_tracing++;

	return 0;
}

void
dt_ioctl_record_stop(struct dt_softc *sc)
{
	struct dt_pcb *dp;
	int count;

	KASSERT(suser(curproc) == 0);

	if (!sc->ds_recording)
		return;

	DPRINTF("dt%d: pid %d disable\n", sc->ds_unit, sc->ds_pid);

	dt_tracing--;
	sc->ds_recording = 0;

	count = dt_enter();
	TAILQ_FOREACH(dp, &sc->ds_pcbs, dp_snext) {
		struct dt_probe *dtp = dp->dp_dtp;

		rw_assert_wrlock(&dt_lock);
		dtp->dtp_recording--;
		dtp->dtp_prov->dtpv_recording--;
		SMR_SLIST_REMOVE_LOCKED(&dtp->dtp_pcbs, dp, dt_pcb, dp_pnext);
	}
	dt_leave(count);
}

int
dt_ioctl_probe_enable(struct dt_softc *sc, struct dtioc_req *dtrq)
{
	struct dt_pcb_list plist;
	struct dt_probe *dtp;
	struct dt_pcb *dp;
	int error;

	KASSERT(suser(curproc) == 0);

	if (!dtioc_req_isvalid(dtrq))
		return EINVAL;

	SIMPLEQ_FOREACH(dtp, &dt_probe_list, dtp_next) {
		if (dtp->dtp_pbn == dtrq->dtrq_pbn)
			break;
	}
	if (dtp == NULL)
		return ENOENT;

	TAILQ_INIT(&plist);
	error = dtp->dtp_prov->dtpv_alloc(dtp, sc, &plist, dtrq);
	if (error)
		return error;

	DPRINTF("dt%d: pid %d enable %u : %b\n", sc->ds_unit, sc->ds_pid,
	    dtrq->dtrq_pbn, (unsigned int)dtrq->dtrq_evtflags, DTEVT_FLAG_BITS);

	/* Append all PCBs to this instance */
	while ((dp = TAILQ_FIRST(&plist)) != NULL) {
		TAILQ_REMOVE(&plist, dp, dp_snext);
		TAILQ_INSERT_HEAD(&sc->ds_pcbs, dp, dp_snext);
	}

	return 0;
}

int
dt_enter(void)
{
	uint32_t count;

	rw_enter_write(&dt_lock);
	count = dt_tracing;
	dt_tracing = 0;

	smr_barrier();

	return count;
}

void
dt_leave(uint32_t count)
{
	dt_tracing = count;
	rw_exit_write(&dt_lock);
}

struct dt_probe *
dt_dev_alloc_probe(const char *func, const char *name, struct dt_provider *dtpv)
{
	struct dt_probe *dtp;

	dtp = malloc(sizeof(*dtp), M_DT, M_NOWAIT|M_ZERO);
	if (dtp == NULL)
		return NULL;

	SMR_SLIST_INIT(&dtp->dtp_pcbs);
	dtp->dtp_prov = dtpv;
	dtp->dtp_func = func;
	dtp->dtp_name = name;
	dtp->dtp_sysnum = -1;

	return dtp;
}

void
dt_dev_register_probe(struct dt_probe *dtp)
{
	static uint64_t probe_nb;

	dtp->dtp_pbn = ++probe_nb;
	SIMPLEQ_INSERT_TAIL(&dt_probe_list, dtp, dtp_next);
}

struct dt_pcb *
dt_pcb_alloc(struct dt_probe *dtp, struct dt_softc *sc)
{
	struct dt_pcb *dp;

	dp = malloc(sizeof(*dp), M_DT, M_WAITOK|M_CANFAIL|M_ZERO);
	if (dp == NULL)
		goto bad;

	dp->dp_ring = mallocarray(DT_EVTRING_SIZE, sizeof(*dp->dp_ring), M_DT,
	    M_WAITOK|M_CANFAIL|M_ZERO);
	if (dp->dp_ring == NULL)
		goto bad;

	mtx_init(&dp->dp_mtx, IPL_HIGH);
	dp->dp_sc = sc;
	dp->dp_dtp = dtp;
	return dp;
bad:
	dt_pcb_free(dp);
	return NULL;
}

void
dt_pcb_free(struct dt_pcb *dp)
{
	if (dp == NULL)
		return;
	free(dp->dp_ring, M_DT, DT_EVTRING_SIZE * sizeof(*dp->dp_ring));
	free(dp, M_DT, sizeof(*dp));
}

void
dt_pcb_purge(struct dt_pcb_list *plist)
{
	struct dt_pcb *dp;

	while ((dp = TAILQ_FIRST(plist)) != NULL) {
		TAILQ_REMOVE(plist, dp, dp_snext);
		dt_pcb_free(dp);
	}
}

int
dt_pcb_filter(struct dt_pcb *dp)
{
	struct dt_filter *dtf = &dp->dp_filter;
	struct proc *p = curproc;
	unsigned int var;
	int match = 1;

	/* Filter out tracing program. */
	if (dp->dp_sc->ds_pid == p->p_p->ps_pid)
		return 1;

	switch (dtf->dtf_variable) {
	case DT_FV_PID:
		var = p->p_p->ps_pid;
		break;
	case DT_FV_TID:
		var = p->p_tid;
		break;
	case DT_FV_NONE:
		break;
	default:
		KASSERT(0);
	}

	switch (dtf->dtf_operand) {
	case DT_OP_EQ:
		match = !!(var == dtf->dtf_value);
		break;
	case DT_OP_NE:
		match = !!(var != dtf->dtf_value);
		break;
	case DT_OP_NONE:
		break;
	default:
		KASSERT(0);
	}

	return !match;
}


/*
 * Get a reference to the next free event state from the ring.
 */
struct dt_evt *
dt_pcb_ring_get(struct dt_pcb *dp)
{
	struct proc *p = curproc;
	struct dt_evt *dtev;
	int distance;

	if (dt_pcb_filter(dp))
		return NULL;

	mtx_enter(&dp->dp_mtx);
	distance = dp->dp_prod - dp->dp_cons;
	if (distance == 1 || distance == (1 - DT_EVTRING_SIZE)) {
		/* read(2) isn't finished */
		dp->dp_dropevt++;
		mtx_leave(&dp->dp_mtx);
		return NULL;
	}

	/*
	 * Save states in next free event slot.
	 */
	dtev = &dp->dp_ring[dp->dp_cons];
	memset(dtev, 0, sizeof(*dtev));

	dtev->dtev_pbn = dp->dp_dtp->dtp_pbn;
	dtev->dtev_cpu = cpu_number();
	dtev->dtev_pid = p->p_p->ps_pid;
	dtev->dtev_tid = p->p_tid;
	nanotime(&dtev->dtev_tsp);

	if (ISSET(dp->dp_evtflags, DTEVT_EXECNAME))
		memcpy(dtev->dtev_comm, p->p_p->ps_comm, DTMAXCOMLEN - 1);

	if (ISSET(dp->dp_evtflags, DTEVT_KSTACK|DTEVT_USTACK)) {
#if notyet
		stacktrace_save_at(&dtev->dtev_kstack, DT_HOOK_FRAME_ADDRESS);
#else
		stacktrace_save(&dtev->dtev_kstack);
#endif
	}

	return dtev;
}

void
dt_pcb_ring_consume(struct dt_pcb *dp, struct dt_evt *dtev)
{
	MUTEX_ASSERT_LOCKED(&dp->dp_mtx);
	KASSERT(dtev == &dp->dp_ring[dp->dp_cons]);

	dp->dp_cons = (dp->dp_cons + 1) % DT_EVTRING_SIZE;
	mtx_leave(&dp->dp_mtx);

	mtx_enter(&dp->dp_sc->ds_mtx);
	dp->dp_sc->ds_evtcnt++;
	mtx_leave(&dp->dp_sc->ds_mtx);
	wakeup(dp->dp_sc);
}

/*
 * Copy at most `qlen' events from `dp', producing the same amount
 * of free slots.
 */
int
dt_pcb_ring_copy(struct dt_pcb *dp, struct dt_evt *estq, size_t qlen,
    uint64_t *dropped)
{
	size_t count, copied = 0;
	unsigned int cons, prod;

	KASSERT(qlen > 0);

	mtx_enter(&dp->dp_mtx);
	cons = dp->dp_cons;
	prod = dp->dp_prod;

	if (cons < prod)
		count = DT_EVTRING_SIZE - prod;
	else
		count = cons - prod;

	if (count == 0)
		goto out;

	*dropped += dp->dp_dropevt;
	dp->dp_dropevt = 0;

	count = MIN(count, qlen);

	memcpy(&estq[0], &dp->dp_ring[prod], count * sizeof(*estq));
	copied += count;

	/* Produce */
	prod = (prod + count) % DT_EVTRING_SIZE;

	/* If the queue is full or the ring didn't wrap, stop here. */
	if (qlen == copied || prod != 0 || cons == 0)
		goto out;

	count = MIN(cons, (qlen - copied));
	memcpy(&estq[copied], &dp->dp_ring[0], count * sizeof(*estq));
	copied += count;
	prod += count;

out:
	dp->dp_prod = prod;
	mtx_leave(&dp->dp_mtx);
	return copied;
}
