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
 * This module provides to midish events that are self-contained, and
 * that do not depend on any context. Standard MIDI events/messages
 * aren't context free: for instance the meaning of a "data entry"
 * controller depends of the last NRPN/RPN controller; dealing with
 * contexts would overcomplicate most midish code (filters, tracks),
 * that's why we define those context-free events: XCTL, NRPN, RPN,
 * and XPC.
 *
 * This module make convertions between context-free events (XPC,
 * XCTL, RPN, NRPN) and basic events (PC, CTL). For instatance a bank
 * controller followed by a prog change will be converted to a
 * "extended" prog change (XPC) that contains also the bank number.
 * In order to generate XPCs whose context is the current bank number,
 * we keep the bank number into a statelist. Similarly the current
 * NRPN and RPN numbers are kept.
 *
 * To keep the state, we use statelist_xxx() functions. However since
 * we store only controllers, we roll simplified lookup() and update()
 * functions.
 */

#include "dbg.h"
#include "state.h"
#include "conv.h"

#define CHAN_MATCH(e1, e2) \
	((e1)->ch == (e2)->ch && (e1)->dev == (e2)->dev)

#define CTL_MATCH(e1, e2) \
	((e1)->ctl_num == (e2)->ctl_num && CHAN_MATCH((e1), (e2)))

/*
 * create a state for the given controller event. If there is already
 * one, then update it.
 */
void
conv_setctl(struct statelist *slist, struct ev *ev) {
	struct state *i;

	for (i = slist->first; i != NULL; i = i->next) {
		if (CTL_MATCH(&i->ev, ev)) {
			i->ev.ctl_val = ev->ctl_val;
			return;
		}
	}
	i = state_new();
	statelist_add(slist, i);
	i->ev = *ev;
}

/*
 * return the state (ie the value) of the given controller number with
 * the same channel/device as the given event. If there is no state
 * recorded, then return EV_UNDEF
 */
unsigned
conv_getctl(struct statelist *slist, struct ev *ev, unsigned num) {
	struct state *i;

	for (i = slist->first; i != NULL; i = i->next) {
		if (i->ev.ctl_num == num && CHAN_MATCH(&i->ev, ev)) {
			return i->ev.ctl_val;
		}
	}
	return EV_UNDEF;
}

/*
 * delete the state of the given controller number with the
 * same channel/device as the given event.
 */
void
conv_rmctl(struct statelist *slist, struct ev *ev, unsigned num) {
	struct state *i;

	for (i = slist->first; i != NULL; i = i->next) {
		if (i->ev.ctl_num == num && CHAN_MATCH(&i->ev, ev)) {
			statelist_rm(slist, i);
			state_del(i);
			break;
		}
	}
}

/*
 * return the 14bit value of a pair of (high, low) controllers with
 * the same device/channel as the given event. If the state of of one
 * of the high or low controllers is missing, then EV_UNDEF is
 * returned.
 */
unsigned
conv_getctx(struct statelist *slist, struct ev *ev, unsigned hi, unsigned lo) {
	unsigned vhi, vlo;

	vlo = conv_getctl(slist, ev, lo);
	if (vlo == EV_UNDEF) {
		return EV_UNDEF;
	}
	vhi = conv_getctl(slist, ev, hi);
	if (vhi == EV_UNDEF) {
		return EV_UNDEF;
	}
	return vlo + (vhi << 7);
}

/*
 * convert an old-style event (ie CTL, PC) to a context-free event (ie
 * XCTL, NRPN, RPN, XPC). If an event is available, 'rev' parameter is
 * filled and 1 is returned.
 */
unsigned
conv_packev(struct statelist *l, unsigned xctlset, 
	    struct ev *ev, struct ev *rev) 
{
	unsigned num, val;

	if (ev->cmd == EV_PC) {
		rev->cmd = EV_XPC;
		rev->dev = ev->dev;
		rev->ch = ev->ch;
		rev->pc_prog = ev->pc_prog;
		rev->pc_bank = conv_getctx(l, ev, BANK_HI, BANK_LO);
		return 1;
	} else if (ev->cmd == EV_CTL) {
		switch (ev->ctl_num) {
		case BANK_HI:
			conv_rmctl(l, ev, BANK_LO);
			conv_setctl(l, ev);
			break;
		case RPN_HI:
			conv_rmctl(l, ev, NRPN_LO);
			conv_rmctl(l, ev, RPN_LO);
			conv_setctl(l, ev);
			break;
		case NRPN_HI:
			conv_rmctl(l, ev, RPN_LO);
			conv_rmctl(l, ev, NRPN_LO);
			conv_setctl(l, ev);
			break;
		case DATAENT_HI:
			conv_rmctl(l, ev, DATAENT_LO);
			conv_setctl(l, ev);
			break;
		case BANK_LO:
			conv_setctl(l, ev);
			break;
		case NRPN_LO:
			conv_rmctl(l, ev, RPN_LO);
			conv_setctl(l, ev);
			break;
		case RPN_LO:
			conv_rmctl(l, ev, NRPN_LO);
			conv_setctl(l, ev);
			break;
		case DATAENT_LO:
			num = conv_getctx(l, ev, NRPN_HI, NRPN_LO);
			if (num != EV_UNDEF) {
				rev->cmd = EV_NRPN;
			} else {
				num = conv_getctx(l, ev, RPN_HI, NRPN_LO);
				if (num == EV_UNDEF) {
					break;
				}
				rev->cmd = EV_RPN;
			}
			val = conv_getctl(l, ev, DATAENT_HI);
			if (val == EV_UNDEF) {
				break;
			}
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			rev->ctl_num = num;
			rev->ctl_val = ev->ctl_val + (val << 7);
			return 1;
		default:
			if (ev->ctl_num < 32) {
				if (EVCTL_ISFINE(xctlset, ev->ctl_num)) {
					conv_setctl(l, ev);
					break;
				}				
				rev->ctl_num = ev->ctl_num;
				rev->ctl_val = ev->ctl_val << 7;
			} else if (ev->ctl_num < 64) {
				num = ev->ctl_num - 32;
				if (!EVCTL_ISFINE(xctlset, num)) {
					break;
				}
				val = conv_getctl(l, ev, num);
				if (val == EV_UNDEF) {
					break;
				}
				rev->ctl_num = num;
				rev->ctl_val = ev->ctl_val + (val << 7);
			} else {
				rev->ctl_num = ev->ctl_num;
				rev->ctl_val = ev->ctl_val << 7;
			}
			rev->cmd = EV_XCTL;
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			return 1;
		}
		return 0;
	} else {
		*rev = *ev;
		return 1;
	}
}

/*
 * convert a context-free event (XCTL, RPN, NRPN, XPC) to an array of
 * old-style events (CTL, PC). Renturn the number of events filled in
 * the array.
 */
unsigned
conv_unpackev(struct statelist *slist, unsigned xctlset,
	      struct ev *ev, struct ev *rev)
{
	unsigned val, hi;
	unsigned nev = 0;

	if (ev->cmd == EV_XCTL) {
		if (ev->ctl_num < 32 && EVCTL_ISFINE(xctlset, ev->ctl_num)) {
			hi = ev->ctl_val >> 7;
			val = conv_getctl(slist, ev, ev->ctl_num);
			if (val != hi || val == EV_UNDEF) {
				rev->cmd = EV_CTL;
				rev->dev = ev->dev;
				rev->ch = ev->ch;
				rev->ctl_num = ev->ctl_num;
				rev->ctl_val = hi;
				conv_setctl(slist, rev);
				rev++;
				nev++;
			}
			rev->cmd = EV_CTL;
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			rev->ctl_num = ev->ctl_num + 32;
			rev->ctl_val = ev->ctl_val & 0x7f;
			rev++; 
			nev++;
			return nev;
		} else {
			rev->cmd = EV_CTL;
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			rev->ctl_num = ev->ctl_num;
			rev->ctl_val = ev->ctl_val >> 7;
			return 1;
		}
	} else if (ev->cmd == EV_XPC) {
		val = conv_getctx(slist, ev, BANK_HI, BANK_LO);
		if (val != ev->pc_bank && ev->pc_bank != EV_UNDEF) {
			rev->cmd = EV_CTL;
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			rev->ctl_num = BANK_HI;
			rev->ctl_val = ev->pc_bank >> 7;
			conv_setctl(slist, rev);
			rev++; 
			nev++;
			rev->cmd = EV_CTL;
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			rev->ctl_num = BANK_LO;
			rev->ctl_val = ev->pc_bank & 0x7f;
			conv_setctl(slist, rev);
			rev++; 
			nev++;
		}
		rev->cmd = EV_PC;
		rev->dev = ev->dev;
		rev->ch = ev->ch;
		rev->pc_prog = ev->pc_prog;
		rev++;
		nev++;
		return nev;
	} else if (ev->cmd == EV_NRPN) {
		val = conv_getctx(slist, ev, NRPN_HI, NRPN_LO);
		if (val != ev->rpn_num) {
			conv_rmctl(slist, ev, RPN_HI);
			conv_rmctl(slist, ev, RPN_LO);
			rev->cmd = EV_CTL;
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			rev->ctl_num = NRPN_HI;
			rev->ctl_val = ev->rpn_num >> 7;
			conv_setctl(slist, rev);
			rev++; 
			nev++;
			rev->cmd = EV_CTL;
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			rev->ctl_num = NRPN_LO;
			rev->ctl_val = ev->rpn_num & 0x7f;
			conv_setctl(slist, rev);
			rev++; 
			nev++;
		}
	dataentry:
		rev->cmd = EV_CTL;
		rev->dev = ev->dev;
		rev->ch = ev->ch;
		rev->ctl_num = DATAENT_HI;
		rev->ctl_val = ev->rpn_val >> 7;
		rev++;
		nev++;
		rev->cmd = EV_CTL;
		rev->dev = ev->dev;
		rev->ch = ev->ch;
		rev->ctl_num = DATAENT_LO;
		rev->ctl_val = ev->rpn_val & 0x7f;
		rev++;
		nev++;
		return nev;
	} else if (ev->cmd == EV_RPN) {
		val = conv_getctx(slist, ev, RPN_HI, RPN_LO);
		if (val != ev->rpn_num) {
			conv_rmctl(slist, ev, NRPN_HI);
			conv_rmctl(slist, ev, NRPN_LO);
			rev->cmd = EV_CTL;
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			rev->ctl_num = RPN_HI;
			rev->ctl_val = ev->rpn_num >> 7;
			conv_setctl(slist, rev);
			rev++; 
			nev++;
			rev->cmd = EV_CTL;
			rev->dev = ev->dev;
			rev->ch = ev->ch;
			rev->ctl_num = RPN_LO;
			rev->ctl_val = ev->rpn_num & 0x7f;
			conv_setctl(slist, rev);
			rev++; 
			nev++;
		}
		goto dataentry;
	} else {
		*rev = *ev;
		return 1;
	}
}
