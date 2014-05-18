/*-
 * Copyright (c) 2014 Jakub Wojciech Klama <jceel@FreeBSD.org>
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/poll.h>
#include <sys/selinfo.h>
#include <sys/malloc.h>

#include <dev/evdev/input.h>
#include <dev/evdev/evdev.h>

#define	DEBUG
#ifdef DEBUG
#define	debugf(fmt, args...)	printf("evdev: " fmt "\n", ##args);
#else
#define	debugf(fmt, args...)
#endif

#define	IOCNUM(x)	(x & 0xff)

static int evdev_open(struct cdev *, int, int, struct thread *);
static int evdev_close(struct cdev *, int, int, struct thread *);
static int evdev_read(struct cdev *, struct uio *, int);
static int evdev_write(struct cdev *, struct uio *, int);
static int evdev_ioctl(struct cdev *, u_long, caddr_t, int, struct thread *);
static int evdev_poll(struct cdev *, int, struct thread *);
static int evdev_kqfilter(struct cdev *, struct knote *);
static int evdev_kqread(struct knote *kn, long hint);
static void evdev_kqdetach(struct knote *kn);
static void evdev_dtor(void *);

static void evdev_notify_event(struct evdev_client *, void *);
static int evdev_ioctl_eviocgbit(struct evdev_dev *, int, int, caddr_t);

static struct cdevsw evdev_cdevsw = {
	.d_version = D_VERSION,
	.d_open = evdev_open,
	.d_close = evdev_close,
	.d_read = evdev_read,
	.d_write = evdev_write,
	.d_ioctl = evdev_ioctl,
	.d_poll = evdev_poll,
	.d_kqfilter = evdev_kqfilter,
	.d_name = "evdev",
};

static struct filterops evdev_cdev_filterops = {
	.f_isfd = 1,
	.f_attach = NULL,
	.f_detach = evdev_kqdetach,
	.f_event = evdev_kqread,
};

struct evdev_cdev_softc
{
	struct evdev_dev *	ecs_evdev;
	int			ecs_open_count;

	LIST_ENTRY(evdev_cdev_softc) ecs_link;
};

struct evdev_cdev_state
{
	struct mtx		ecs_mtx;
	struct evdev_client *	ecs_client;
	struct selinfo		ecs_selp;
	struct sigio *		ecs_sigio;
};

static int evdev_cdev_count = 0;

static int
evdev_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	struct evdev_cdev_softc *sc = dev->si_drv1;
	struct evdev_cdev_state *state;
	int ret;

	state = malloc(sizeof(struct evdev_cdev_state), M_EVDEV, M_WAITOK | M_ZERO);
	
	ret = evdev_register_client(sc->ecs_evdev, &state->ecs_client);
	if (ret != 0) {
		debugf("cdev: cannot register evdev client");
		return (ret);
	}

	state->ecs_client->ec_ev_notify = &evdev_notify_event;
	state->ecs_client->ec_ev_arg = state;

	knlist_init_mtx(&state->ecs_selp.si_note, NULL);

	sc->ecs_open_count++;
	devfs_set_cdevpriv(state, evdev_dtor);
	return (0);
}

static int
evdev_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	struct evdev_cdev_softc *sc = dev->si_drv1;

	sc->ecs_open_count--;
	return (0);
}

static void
evdev_dtor(void *data)
{
	struct evdev_cdev_state *state = (struct evdev_cdev_state *)data;

	seldrain(&state->ecs_selp);
	evdev_dispose_client(state->ecs_client);
	free(data, M_EVDEV);
}

static int
evdev_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct evdev_cdev_state *state;
	struct evdev_client *client;
	struct input_event *event;
	int ret = 0;
	int remaining;

	debugf("cdev: read %ld bytes by thread %d", uio->uio_resid,
	    uio->uio_td->td_tid);

	ret = devfs_get_cdevpriv((void **)&state);
	if (ret != 0)
		return (ret);

	client = state->ecs_client;

	if (uio->uio_resid % sizeof(struct input_event) != 0) {
		debugf("read size not multiple of struct input_event size");
		return (EINVAL);
	}

	remaining = uio->uio_resid / sizeof(struct input_event);

	EVDEV_CLIENT_LOCKQ(client);

	if (EVDEV_CLIENT_EMPTYQ(client))
		mtx_sleep(client, &client->ec_buffer_mtx, 0, "evrea", 0);

	for (;;) {
		if (EVDEV_CLIENT_EMPTYQ(client))
			/* Short read :-( */
			break;
	
		event = &client->ec_buffer[client->ec_buffer_head];
		client->ec_buffer_head = (client->ec_buffer_head + 1) % client->ec_buffer_size;
		remaining--;

		EVDEV_CLIENT_UNLOCKQ(client);
		uiomove(event, sizeof(struct input_event), uio);
		EVDEV_CLIENT_LOCKQ(client);

		if (remaining == 0)
			break;
	}

	EVDEV_CLIENT_UNLOCKQ(client);

	return (0);
}

static int
evdev_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct evdev_cdev_state *state;
	int ret = 0;
	
	debugf("cdev: write %ld bytes by thread %d", uio->uio_resid,
	    uio->uio_td->td_tid);

	ret = devfs_get_cdevpriv((void **)&state);
	if (ret != 0)
		return (ret);

	if (uio->uio_resid % sizeof(struct input_event) != 0) {
		debugf("write size not multiple of struct input_event size");
		return (EINVAL);
	}

	return (0);
}

static int
evdev_poll(struct cdev *dev, int events, struct thread *td)
{
	struct evdev_client *client;
	struct evdev_cdev_state *state;
	int ret;
	int revents = 0;

	debugf("cdev: poll by thread %d", td->td_tid);

	ret = devfs_get_cdevpriv((void **)&state);
	if (ret != 0)
		return (ret);

	client = state->ecs_client;

	if (events & (POLLIN | POLLRDNORM)) {
		EVDEV_CLIENT_LOCKQ(client);
		if (!EVDEV_CLIENT_EMPTYQ(client))
			revents = events & (POLLIN | POLLRDNORM);
		else
			selrecord(td, &state->ecs_selp);
		EVDEV_CLIENT_UNLOCKQ(client);
	}

	return (revents);
}

static int
evdev_kqfilter(struct cdev *dev, struct knote *kn)
{
	struct evdev_cdev_state *state;
	int ret;

	ret = devfs_get_cdevpriv((void **)&state);
	if (ret != 0)
		return (ret);

	kn->kn_hook = (caddr_t)state;
	kn->kn_fop = &evdev_cdev_filterops;

	knlist_add(&state->ecs_selp.si_note, kn, 0);
	return (0);
}

static int
evdev_kqread(struct knote *kn, long hint)
{
	struct evdev_client *client;
	struct evdev_cdev_state *state;
	int ret;

	state = (struct evdev_cdev_state *)kn->kn_hook;
	client = state->ecs_client;

	EVDEV_CLIENT_LOCKQ(client);
	ret = !EVDEV_CLIENT_EMPTYQ(client);
	EVDEV_CLIENT_UNLOCKQ(client);
	return (ret);
}

static void
evdev_kqdetach(struct knote *kn)
{
	struct evdev_cdev_state *state;

	state = (struct evdev_cdev_state *)kn->kn_hook;
	knlist_remove(&state->ecs_selp.si_note, kn, 0);
}

static int
evdev_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct evdev_cdev_softc *sc = dev->si_drv1;
	struct evdev_dev *evdev = sc->ecs_evdev;
	int len, num;

	len = IOCPARM_LEN(cmd);
	cmd = IOCBASECMD(cmd);
	num = IOCNUM(cmd);

	debugf("cdev: ioctl called: cmd=0x%08lx, data=%p", cmd, data);

	switch (cmd) {
	case EVIOCGVERSION:
		data = (caddr_t)EV_VERSION;
		break;

	case EVIOCGID:
		break;

	case EVIOCGREP:
		break;

	case EVIOCSREP:
		break;

	case EVIOCGKEYCODE:
		break;

	case EVIOCGKEYCODE_V2:
		break;

	case EVIOCSKEYCODE:
		break;

	case EVIOCSKEYCODE_V2:
		break;

	case EVIOCGNAME(0):
		debugf("EVIOCGNAME: data=%p, ev_name=%s, len=%d", data, evdev->ev_name, len);
		strlcpy(data, evdev->ev_name, len);
		break;

	case EVIOCGPHYS(0):
		strlcpy(data, evdev->ev_shortname, len);
		break;

	case EVIOCGUNIQ(0):
		strlcpy(data, evdev->ev_serial, len);
		break;

	case EVIOCGPROP(0):
		memcpy(data, evdev->ev_type_flags, len);
		break;

	case EVIOCGKEY(0):
		memcpy(data, evdev->ev_key_flags, len);
		break;

	case EVIOCGLED(0):
		break;

	case EVIOCGSND(0):
		break;

	case EVIOCGSW(0):
		break;
	}

	if (IOCGROUP(cmd) != 'E')
		return (EINVAL);

	/* Handle EVIOCGBIT variants */
	if (num >= IOCNUM(EVIOCGBIT(0, 0)) &&
	    num < IOCNUM(EVIOCGBIT(EV_MAX, 0))) {
		int type_num = num - IOCNUM(EVIOCGBIT(0, 0));
		debugf("cdev: EVIOCGBIT(%d): data=%p, len=%d", type_num, data, len);
		return (evdev_ioctl_eviocgbit(evdev, type_num, len, data));
	}

	return (0);
}

static int
evdev_ioctl_eviocgbit(struct evdev_dev *evdev, int type, int len, caddr_t data)
{
	uint32_t *bitmap;
	int limit;

	switch (type) {
	case 0:
		bitmap = evdev->ev_type_flags;
		limit = EV_CNT;
		break;
	case EV_KEY:
		bitmap = evdev->ev_key_flags;
		limit = KEY_CNT;
		break;
	case EV_REL:
		bitmap = evdev->ev_rel_flags;
		limit = REL_CNT;
		break;
	case EV_ABS:
		bitmap = evdev->ev_abs_flags;
		limit = ABS_CNT;
		break;
	case EV_MSC:
		bitmap = evdev->ev_msc_flags;
		limit = MSC_CNT;
		break;
	case EV_LED:
		bitmap = evdev->ev_led_flags;
		limit = LED_CNT;
		break;
	case EV_SND:
		bitmap = evdev->ev_snd_flags;
		limit = SND_CNT;
		break;
	case EV_SW:
		bitmap = evdev->ev_sw_flags;
		limit = SW_CNT;
		break;

	default:
		return (ENOTTY);
	}

	/* 
	 * Clear ioctl data buffer in case it's bigger than
	 * bitmap size
	 */
	bzero(data, len);

	limit = howmany(limit, 8);
	len = MIN(limit, len);
	memcpy(data, bitmap, len);
	return (0);
}

static void
evdev_notify_event(struct evdev_client *client, void *data)
{
	struct evdev_cdev_state *state = (struct evdev_cdev_state *)data;

	selwakeup(&state->ecs_selp);
}

int
evdev_cdev_create(struct evdev_dev *evdev)
{
	struct evdev_cdev_softc *sc;
	struct cdev *cdev;

	cdev = make_dev(&evdev_cdevsw, 0, UID_ROOT, GID_WHEEL, 0600,
	    "input/event%d", evdev_cdev_count++);

	sc = malloc(sizeof(struct evdev_cdev_softc), M_EVDEV, M_WAITOK | M_ZERO);
	
	sc->ecs_evdev = evdev;
	evdev->ev_cdev = cdev;
	cdev->si_drv1 = sc;
	return (0);
}

int
evdev_cdev_destroy(struct evdev_dev *evdev)
{
	destroy_dev(evdev->ev_cdev);
	return (0);
}