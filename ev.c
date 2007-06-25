/*
 * Copyright (c) 2003-2007 Alexandre Ratchov <alex@caoua.org>
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
 * ev is an "extended" midi event
 */

#include "dbg.h"
#include "ev.h"
#include "str.h"

char *ev_cmdstr[EV_NUMCMD] = {
	"nil",		NULL,		"tempo",	"timesig",
	"nrpn",		"rpn",		"xctl",		"xpc",
	"noff",		"non",		"kat",		"ctl",
	"pc",		"cat",		"bend"
};

char *evspec_cmdstr[] = {
	"any", "note", "ctl", "pc", "cat", "bend", "nrpn", "rpn", 
	"xctl", "xpc", NULL
};

struct evctl evctl_tab[128];

char *
ev_getstr(struct ev *ev) {
	if (ev->cmd >= EV_NUMCMD) {
		return NULL;
		/*
		dbg_puts("ev_getstr: invalid cmd\n");
		dbg_panic();
		*/
	}
	return ev_cmdstr[ev->cmd];
}

unsigned
ev_str2cmd(struct ev *ev, char *str) {
	unsigned i;
	for (i = 0; i < EV_NUMCMD; i++) {
		if (ev_cmdstr[i] && str_eq(ev_cmdstr[i], str)) {
			ev->cmd = i;
			return 1;
		}
	}
	return 0;
}

/*
 * return the phase of the event within a frame:
 *
 *    -	EV_PHASE_FIRST is set if the event can be the first event in a
 *	sequence (example: note-on, bender != 0x4000)
 *
 *    -	EV_PHASE_NEXT is set if the given event can be the next event
 *	in a frame after a 'first event' but not the last one
 *	(example: key after-touch, beder != 0x4000)
 *
 *    -	EV_PHASE_LAST is set if the given event can be the last event
 *	in a frame (example: note-off, any unknown controller)
 */
unsigned
ev_phase(struct ev *ev) {
	unsigned phase;
	
	switch(ev->cmd) {
	case EV_NOFF:
		phase = EV_PHASE_LAST;
		break;
	case EV_NON:
		phase = EV_PHASE_FIRST;
		break;
	case EV_KAT:
		phase = EV_PHASE_NEXT;
		break;
	case EV_CAT:
		if (ev->cat_val != EV_CAT_DEFAULT) {
			phase = EV_PHASE_FIRST | EV_PHASE_NEXT;
		} else {
			phase = EV_PHASE_LAST;
		}
		break;
	case EV_XCTL:
		if (!EV_CTL_ISFRAME(ev)) {
			phase = EV_PHASE_FIRST | EV_PHASE_LAST;
		} else {
			if (ev->ctl_val != EV_CTL_DEFVAL(ev)) {
				phase = EV_PHASE_FIRST | EV_PHASE_NEXT;
			} else {
				phase = EV_PHASE_LAST;
			}
		}
		break;
	case EV_BEND:
		if (ev->bend_val != EV_BEND_DEFAULT) {
			phase = EV_PHASE_FIRST | EV_PHASE_NEXT;
		} else {
			phase = EV_PHASE_LAST;		
		}
		break;
	default:
		phase = EV_PHASE_FIRST | EV_PHASE_LAST;
		break;
	}
	return phase;
}

void
ev_dbg(struct ev *ev) {
	char *cmdstr;
	cmdstr = ev_getstr(ev);
	if (cmdstr == NULL) {
		dbg_puts("unkw(");
		dbg_putu(ev->cmd);
		dbg_puts(")");
	} else {
		dbg_puts(cmdstr);
		switch(ev->cmd) {
		case EV_NON:
		case EV_NOFF:
		case EV_KAT:
		case EV_CTL:
		case EV_NRPN:
		case EV_RPN:
		case EV_XPC:
		case EV_XCTL:
			dbg_puts(" {");
			dbg_putx(ev->dev);
			dbg_puts(" ");
			dbg_putx(ev->ch);
			dbg_puts("} ");
			dbg_putx(ev->v0);
			dbg_puts(" ");
			dbg_putx(ev->v1);
			break;
		case EV_BEND:
		case EV_CAT:
		case EV_PC:
			dbg_puts(" {");
			dbg_putx(ev->dev);
			dbg_puts(" ");
			dbg_putx(ev->ch);
			dbg_puts("} ");
			dbg_putx(ev->v0);
			break;
		case EV_TEMPO:
			dbg_puts(" ");
			dbg_putu((unsigned)ev->tempo_usec24);
			break;
		case EV_TIMESIG:
			dbg_puts(" ");
			dbg_putx(ev->timesig_beats);
			dbg_puts(" ");
			dbg_putx(ev->timesig_tics);
			break;
		}
	}
}


/* ------------------------------------------------------------------ */


unsigned
evspec_str2cmd(struct evspec *ev, char *str) {
	unsigned i;

	for (i = 0; evspec_cmdstr[i]; i++) {
		if (str_eq(evspec_cmdstr[i], str)) {
			ev->cmd = i;
			return 1;
		}
	}
	return 0;
}

void
evspec_reset(struct evspec *o) {
	o->cmd = EVSPEC_ANY;
	o->dev_min = 0;
	o->dev_max = EV_MAXDEV;
	o->ch_min  = 0;
	o->ch_max  = EV_MAXCH;
	o->b0_min  = 0;
	o->b0_max  = EV_MAXFINE;
	o->b1_min  = 0;
	o->b1_max  = EV_MAXFINE;
}

void
evspec_dbg(struct evspec *o) {
	unsigned i;
	
	i = 0;
	for (;;) {
		if (evspec_cmdstr[i] == 0) {
			dbg_puts("unknown");
			break;
		}
		if (o->cmd == i) {
			dbg_puts(evspec_cmdstr[i]);
			break;
		}
		i++;
	}
	dbg_puts(" ");
	dbg_putu(o->dev_min);
	dbg_puts(":");
	dbg_putu(o->dev_max);
		
	dbg_puts(" ");
	dbg_putu(o->ch_min);
	dbg_puts(":");
	dbg_putu(o->ch_max);

	if (o->cmd != EVSPEC_ANY) {
		dbg_puts(" ");
		dbg_putu(o->b0_min);
		dbg_puts(":");
		dbg_putu(o->b0_max);

		if (o->cmd != EVSPEC_CAT && 
		    o->cmd != EVSPEC_PC &&
		    o->cmd != EVSPEC_BEND) {
			dbg_puts(" ");
			dbg_putu(o->b1_min);
			dbg_puts(":");
			dbg_putu(o->b1_max);
		}
	}
}

/*
 * configure a controller (set the name and default value)
 */
void
evctl_conf(unsigned num, char *name, unsigned defval) {
	struct evctl *ctl = &evctl_tab[num];

	if (name) {
		ctl->name = str_new(name);
	}
	ctl->defval = defval;
}

/*
 * unconfigure a controller (clear its name unset set its default
 * value tu "unknown")
 */
void
evctl_unconf(unsigned i) {
	struct evctl *ctl = &evctl_tab[i];

	if (ctl->name != NULL) {
		str_delete(ctl->name);
		ctl->name = NULL;
	}
	ctl->defval = EV_UNDEF;
}

/*
 * find the controller number corresponding the given controller
 * name. Return 1 if found, 0 if not
 */
unsigned
evctl_lookup(char *name, unsigned *ret) {
	unsigned i;
	struct evctl *ctl;
	
	for (i = 0; i < 128; i++) {
		ctl = &evctl_tab[i];
		if (ctl->name != NULL && str_eq(ctl->name, name)) {
			*ret = i;
			return 1;
		}
	}
	return 0;
}

/*
 * initialize the controller table
 */
void
evctl_init(void) {
	unsigned i;
	
	for (i = 0; i < 128; i++) {
		evctl_tab[i].name = NULL;
		evctl_tab[i].defval = EV_UNDEF;
	}

	/*
	 * some defaults, for testing ...
	 */
	evctl_conf(1,   "mod", 0);
	evctl_conf(7,   "vol", EV_UNDEF);
	evctl_conf(64,  "sustain", 0);
}

/*
 * free the controller table 
 */
void
evctl_done(void) {
	unsigned i;

	for (i = 0; i < 128; i++) {
		if (evctl_tab[i].name != NULL) {
			str_delete(evctl_tab[i].name);
		}
	}
}


/*
 * return 1 if the controller is reserved
 */
unsigned
evctl_isreserved(unsigned num) {
	if (num == BANK_HI || num == DATAENT_HI || (num >= 32 && num < 64) ||
	    num == RPN_HI || num == RPN_LO || 
	    num == NRPN_HI || num == NRPN_LO) {
		return 1;
	} else {
		return 0;
	}	
}
