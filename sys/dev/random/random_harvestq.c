/*-
 * Copyright (c) 2000-2013 Mark R V Murray
 * Copyright (c) 2013 Arthur Mesh
 * Copyright (c) 2004 Robert N. M. Watson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_random.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/linker.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/random.h>
#include <sys/selinfo.h>
#include <sys/sysctl.h>
#include <sys/unistd.h>

#include <machine/cpu.h>

#include <dev/random/randomdev.h>
#include <dev/random/random_adaptors.h>
#include <dev/random/random_harvestq.h>
#include <dev/random/live_entropy_sources.h>
#include <dev/random/rwfile.h>

/*
 * How many events to queue up. We create this many items in
 * an 'empty' queue, then transfer them to the 'harvest' queue with
 * supplied junk. When used, they are transferred back to the
 * 'empty' queue.
 */
#define RANDOM_FIFO_MAX	1024

/*
 * The harvest mutex protects the consistency of the entropy fifos and
 * empty fifo and other associated structures.
 */
struct mtx	harvest_mtx;

/* Lockable FIFO queue holding entropy buffers */
struct entropyfifo {
	STAILQ_HEAD(harvestlist, harvest) head;
};

/* Empty entropy buffers */
static struct entropyfifo emptyfifo;

/* Harvested entropy */
static struct entropyfifo harvestfifo;

/* Function called to process one harvested stochastic event */
void (*harvest_process_event)(struct harvest *);

/* <0 to end the kthread, 0 to let it run, 1 to flush the harvest queues */
int random_kthread_control = 0;

static struct proc *random_kthread_proc;

#ifdef RANDOM_RWFILE
static struct entropy_cache {
	const char *filename;
	int already_read;
} entropy_files[] = {
	{ "/entropy", 0 },
	{ "/var/db/entropy-file", 0 },
	{ "/var/db/entropy/saved-entropy.1", 0 },
	{ "/var/db/entropy/saved-entropy.2", 0 },
	{ "/var/db/entropy/saved-entropy.3", 0 },
	{ "/var/db/entropy/saved-entropy.4", 0 },
	{ "/var/db/entropy/saved-entropy.5", 0 },
	{ "/var/db/entropy/saved-entropy.6", 0 },
	{ "/var/db/entropy/saved-entropy.7", 0 },
	{ "/var/db/entropy/saved-entropy.8", 0 },
	{ NULL, 0 }
};

/* Deal with entropy cached externally if this is present.
 * Lots of policy may eventually arrive in this function.
 * Called after any volume is mounted; this way we can keep
 * looking for files in (say) /var/db/entropy/...
 * 
 * We keep a cache of files read so we don't keep re-reading them.
 *
 * Optionally (a compile-time option) overwrite these files.
 */
static void
random_harvestq_cache(void *arg1 __unused, struct mount *arg2 __unused,
    struct vnode *arg3 __unused, struct thread *arg4 __unused)
{
	static int caches_read = 0;
	struct entropy_cache *entropy_file;
	int error;
	size_t i;
	uint8_t *data;

	/* Read and attempt to overwrite the entropy cache files.
	 * If the file exists, can be read and then overwritten,
	 * then use it. Ignore it otherwise, but print out what is
	 * going on.
	 */
	data = malloc(PAGE_SIZE, M_ENTROPY, M_WAITOK);
	for (entropy_file = entropy_files; entropy_file->filename; entropy_file++) {
		if (entropy_file->already_read)
			break;
		error = randomdev_read_file(entropy_file->filename, data, PAGE_SIZE);
		if (error == 0) {
			printf("random: entropy cache '%s' provides %ld bytes\n", entropy_file->filename, (long)PAGE_SIZE);
#ifdef RANDOM_RWFILE_WRITE_OK /* Not defined so writes disabled for now */
			error = randomdev_write_file(entropy_file->filename, zero_region, PAGE_SIZE);
			if (error == 0) {
				printf("random: entropy cache '%s' successfully overwritten\n", entropy_file->filename);
#endif
				for (i = 0; i < PAGE_SIZE; i += 16)
					random_harvestq_internal(data + i, 16, 16, RANDOM_CACHED);
				entropy_file->already_read = 1;
				caches_read++;
#ifdef RANDOM_RWFILE_WRITE_OK /* Not defined so writes disabled for now */
			}
			else
				printf("random: entropy cache '%s' not overwritten and therefore not used; error = %d\n", entropy_file->filename, error);
#endif
		}
		else
			if (bootverbose)
				printf("random: entropy cache '%s' not present or unreadable; error = %d\n", entropy_file->filename, error);
	}
	bzero(data, PAGE_SIZE);
	free(data, M_ENTROPY);

	if (bootverbose)
		printf("random: total entropy cache files read so far = %d\n", caches_read);
}
/* Invoke the above every time a FS is mounted; this way we can keep going after cached entropy */
EVENTHANDLER_DEFINE(vfs_mounted, random_harvestq_cache, NULL, 0);
#endif /* RANDOM_RWFILE */

static void
random_kthread(void *arg __unused)
{
	STAILQ_HEAD(, harvest) local_queue;
	struct harvest *event = NULL;

	STAILQ_INIT(&local_queue);

	/* Process until told to stop */
	mtx_lock_spin(&harvest_mtx);
	for (; random_kthread_control >= 0;) {

		/*
		 * Grab all the entropy events.
		 * Drain entropy source records into a thread-local
		 * queue for processing while not holding the mutex.
		 */
		STAILQ_CONCAT(&local_queue, &harvestfifo.head);

		/*
		 * Deal with events, if any.
		 * Then transfer the used events back into the empty fifo.
		 */
		if (!STAILQ_EMPTY(&local_queue)) {
			mtx_unlock_spin(&harvest_mtx);
			STAILQ_FOREACH(event, &local_queue, next)
				harvest_process_event(event);
			mtx_lock_spin(&harvest_mtx);
			STAILQ_CONCAT(&emptyfifo.head, &local_queue);
		}

		/*
		 * Give the fast hardware sources a go
		 */
		mtx_unlock_spin(&harvest_mtx);
		live_entropy_sources_feed();
		mtx_lock_spin(&harvest_mtx);

		/*
		 * If a queue flush was commanded, it has now happened,
		 * and we can mark this by resetting the command.
		 */

		if (random_kthread_control == 1)
			random_kthread_control = 0;

		/* Work done, so don't belabour the issue */
		msleep_spin_sbt(&random_kthread_control, &harvest_mtx,
		    "-", SBT_1S/10, 0, C_PREL(1));

	}
	mtx_unlock_spin(&harvest_mtx);

	randomdev_set_wakeup_exit(&random_kthread_control);
	/* NOTREACHED */
}

void
random_harvestq_init(void (*event_processor)(struct harvest *))
{
	uint8_t *keyfile, *data;
	int error, i;
	size_t size, j;
	struct harvest *np;

	/* Initialise the harvest fifos */

	/* Contains the currently unused event structs. */
	STAILQ_INIT(&emptyfifo.head);
	for (i = 0; i < RANDOM_FIFO_MAX; i++) {
		np = malloc(sizeof(struct harvest), M_ENTROPY, M_WAITOK);
		STAILQ_INSERT_TAIL(&emptyfifo.head, np, next);
	}

	/* Will contain the queued-up events. */
	STAILQ_INIT(&harvestfifo.head);

	/* Point to the correct event_processing function */
	harvest_process_event = event_processor;

	mtx_init(&harvest_mtx, "entropy harvest mutex", NULL, MTX_SPIN);

	/* Start the hash/reseed thread */
	error = kproc_create(random_kthread, NULL,
	    &random_kthread_proc, RFHIGHPID, 0, "rand_harvestq"); /* RANDOM_CSPRNG_NAME */

	if (error != 0)
		panic("Cannot create entropy maintenance thread.");

	/* Get entropy that may have been preloaded by loader(8)
	 * and use it to pre-charge the entropy harvest queue.
	 */
	keyfile = preload_search_by_type("/boot/entropy");
	if (keyfile != NULL) {
		data = preload_fetch_addr(keyfile);
		size = preload_fetch_size(keyfile);
		if (data != NULL && size != 0) {
			for (j = 0; j < size; j += 16)
				random_harvestq_internal(data + j, 16, 16, RANDOM_CACHED);
			printf("random: read %zu bytes from preloaded cache\n", size);
			bzero(data, size);
		}
		else
			printf("random: no preloaded entropy cache\n");
	}

}

void
random_harvestq_deinit(void)
{
	struct harvest *np;

	/* Destroy the harvest fifos */
	while (!STAILQ_EMPTY(&emptyfifo.head)) {
		np = STAILQ_FIRST(&emptyfifo.head);
		STAILQ_REMOVE_HEAD(&emptyfifo.head, next);
		free(np, M_ENTROPY);
	}
	while (!STAILQ_EMPTY(&harvestfifo.head)) {
		np = STAILQ_FIRST(&harvestfifo.head);
		STAILQ_REMOVE_HEAD(&harvestfifo.head, next);
		free(np, M_ENTROPY);
	}

	mtx_destroy(&harvest_mtx);
}

/*
 * Entropy harvesting routine.
 * This is supposed to be fast; do not do anything slow in here!
 *
 * It is also illegal (and morally reprehensible) to insert any
 * high-rate data here. "High-rate" is define as a data source
 * that will usually cause lots of failures of the "Lockless read"
 * check a few lines below. This includes the "always-on" sources
 * like the Intel "rdrand" or the VIA Nehamiah "xstore" sources.
 */
/* XXXRW: get_cyclecount() is cheap on most modern hardware, where cycle
 * counters are built in, but on older hardware it will do a real time clock
 * read which can be quite expensive.
 */
void
random_harvestq_internal(const void *entropy, u_int count, u_int bits,
    enum esource origin)
{
	static u_int destination[ENTROPYSOURCE];
	struct harvest *event;

	KASSERT(origin >= RANDOM_START && origin < ENTROPYSOURCE,
	    ("random_harvest_internal: origin %d invalid\n", origin));

	/* Lockless check to avoid lock operations if queue is empty. */
	if (STAILQ_EMPTY(&emptyfifo.head))
		return;

	mtx_lock_spin(&harvest_mtx);

	event = STAILQ_FIRST(&emptyfifo.head);
	if (event != NULL) {
		/* Add the harvested data to the fifo */
		STAILQ_REMOVE_HEAD(&emptyfifo.head, next);
		event->somecounter = get_cyclecount();
		event->size = count;
		event->bits = bits;
		event->source = origin;
		event->destination = destination[origin]++;
		memcpy(event->entropy, entropy,
		    MIN(count, HARVESTSIZE));

		STAILQ_INSERT_TAIL(&harvestfifo.head,
		    event, next);
	}

	mtx_unlock_spin(&harvest_mtx);
}
