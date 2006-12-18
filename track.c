/*
 * Copyright (c) 2003-2006 Alexandre Ratchov <alex@caoua.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met:
 *
 * 	- Redistributions of source code must retain the above
 * 	  copyright notice, this list of conditions and the
 * 	  following disclaimer.
 *
 * 	- Redistributions in binary form must reproduce the above
 * 	  copyright notice, this list of conditions and the
 * 	  following disclaimer in the documentation and/or other
 * 	  materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * a track (struct track *o) is a singly linked list of
 * events. Each event (struct seqev) is made by 
 *	- a midi event (struct ev)
 *	- the number of tics before the event is to be played
 *
 * Since a track can contain an amount of blank space
 * after the last event (if any), there is always an end-of-track event
 * in the list.
 *
 *	- each clock tic marks the begining of a delta
 *	- each event (struct ev) is played after delta tics
 *
 * a seqptr represents the position of a cursor on the track In play
 * mode, the field 'o->pos' points to the event to be played. The
 * 'delta' field contains the number of tics elapsed since the last
 * played event. Thus when o->delta reaches o->pos->delta, the event
 * is to be played.
 *
 * In record mode, the field 'o->pos' points to the next event that
 * the event being recorded.
 *
 * The seqptr structure contain a cursor (a pos/delta pair) which is
 * used to walk through the track.
 */

#include "dbg.h"
#include "pool.h"
#include "track.h"

struct pool seqev_pool;

void
seqev_pool_init(unsigned size) {
	pool_init(&seqev_pool, "seqev", sizeof(struct seqev), size);
}

void
seqev_pool_done(void) {
	pool_done(&seqev_pool);
}


struct seqev *
seqev_new(void) {
	return (struct seqev *)pool_new(&seqev_pool);
}

void
seqev_del(struct seqev *se) {
	pool_del(&seqev_pool, se);
}

void
seqev_dump(struct seqev *i) {
	dbg_putu(i->delta);
	dbg_puts("\t");
	ev_dbg(&i->ev);
}

/*
 * initialise the track
 */
void
track_init(struct track *o) {
	o->eot.ev.cmd = EV_NULL;
	o->eot.delta = 0;
	o->eot.next = NULL;
	o->eot.prev = &o->first;
	o->first = &o->eot;
}

/*
 * free a track
 */
void
track_done(struct track *o) {
	struct seqev *i, *inext;
	
	for (i = o->first;  i != &o->eot;  i = inext) {
		inext = i->next;
		seqev_del(i);
	}
#ifdef TRACK_DEBUG
	o->first = (void *)0xdeadbeef;
#endif
}

/*
 * dump the track on stderr, for debugging purposes
 */
void
track_dump(struct track *o) {
	struct seqev *i;
	unsigned tic = 0, num = 0;
	
	for (i = o->first; i != NULL; i = i->next) {
		tic += i->delta;
		dbg_putu(num);
		dbg_puts("\t");
		dbg_putu(tic);
		dbg_puts("\t+");
		seqev_dump(i);
		dbg_puts("\n");
		num++;
	}
}

/*
 * return true if an event is available on the track
 */
unsigned
seqev_avail(struct seqev *pos) {
	return (pos->ev.cmd != EV_NULL);
}

/*
 * insert an event (stored in an already allocated seqev
 * structure) just before the event of the given position
 * (the delta field of the given event is ignored)
 */
void
seqev_ins(struct seqev *pos, struct seqev *se) {
	se->delta = pos->delta;
	pos->delta = 0;
	/* link to the list */
	se->next = pos;
	se->prev = pos->prev;
	*(se->prev) = se;
	pos->prev = &se->next;
}

/*
 * remove the event (but not blank space) on the given position
 */
void
seqev_rm(struct seqev *pos) {
#ifdef TRACK_DEBUG
	if (pos->ev.cmd == EV_NULL) {
		dbg_puts("seqev_rm: unexpected end of track\n");
		dbg_panic();
	}	
#endif
	pos->next->delta += pos->delta;
	pos->delta = 0;
	/* since se != &eot, next is never NULL */
	*pos->prev = pos->next;
	pos->next->prev = pos->prev;
}

/*
 * return the number of events in the track
 */
unsigned
track_numev(struct track *o) {
	unsigned n;
	struct seqev *i;
	
	n = 0;
	for(i = o->first; i != &o->eot; i = i->next) n++;

	return n;
}

/*
 * return the number of tics in the track
 * ie its length (eot included, of course)
 */
unsigned
track_numtic(struct track *o) {
	unsigned ntics;
	struct seqev *i;
	ntics = 0;
	for(i = o->first; i != NULL; i = i->next) 
		ntics += i->delta;
	return ntics;
}


/*
 * remove all events from the track
 * XXX: rename this track_clear and put it in track.c,
 * (but first stop using the old track_clear())
 */
void
track_clearall(struct track *o) {
	struct seqev *i, *inext;
	
	for (i = o->first;  i != &o->eot;  i = inext) {
		inext = i->next;
		seqev_del(i);
	}
	o->eot.delta = 0;
	o->eot.prev = &o->first;
	o->first = &o->eot;
}

/*
 * clear 'dst' and attach contents of 'src' to 'dst'
 * (dont forget to copy the 'eot')
 */
void
track_moveall(struct track *dst, struct track *src) {
	track_clearall(dst);
	dst->eot.delta = src->eot.delta;
	if (src->first == &src->eot) {
		dst->first = &dst->eot;
		dst->eot.prev = &dst->first;
	} else {
		dst->first = src->first;
		dst->eot.prev = src->eot.prev;
		dst->first->prev = &dst->first;
		*dst->eot.prev = &dst->eot;
	}
	src->eot.delta = 0;
	src->eot.prev = &src->first;
	src->first = &src->eot;
}



/*
 * set the chan (dev/midichan pair) of
 * all voice events
 */
void
track_setchan(struct track *src, unsigned dev, unsigned ch) {
	struct seqev *i;

	for (i = src->first; i != NULL; i = i->next) {
		if (EV_ISVOICE(&i->ev)) {
			i->ev.data.voice.dev = dev;
			i->ev.data.voice.ch = ch;
		}
	}
}

/*
 * fill a map of used channels/devices
 */
void
track_chanmap(struct track *o, char *map) {
	struct seqev *se;
	unsigned dev, ch, i;
	
	for (i = 0; i < DEFAULT_MAXNCHANS; i++) {
		map[i] = 0;
	}

	for (se = o->first; se != NULL; se = se->next) {
		if (EV_ISVOICE(&se->ev)) {
			dev = se->ev.data.voice.dev;
			ch  = se->ev.data.voice.ch;
			if (dev >= DEFAULT_MAXNDEVS || ch >= 16) {
				dbg_puts("track_chanmap: bogus dev/ch pair, stopping\n");
				break;
			}
			map[dev * 16 + ch] = 1;
		}
	}
}
