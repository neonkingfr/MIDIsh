/*
 * Copyright (c) 2003-2005 Alexandre Ratchov
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
 * 	  materials  provided with the distribution.
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
 * implements all built-in functions
 * available through the interpreter
 */

#include "dbg.h"
#include "default.h"
#include "tree.h"
#include "data.h"

#include "textio.h"
#include "lex.h"
#include "parse.h"

#include "mux.h"
#include "mididev.h"

#include "trackop.h"
#include "track.h"
#include "song.h"
#include "user.h"
#include "smf.h"
#include "saveload.h"
#include "rmidi.h"	/* for rmidi_debug */

struct song_s *user_song;
struct textout_s *user_stdout;
unsigned user_flag_norc = 0;

/* -------------------------------------------------- some tools --- */

unsigned
user_parsefile(struct exec_s *exec, char *filename) {
	struct parse_s *parse;
	struct var_s **locals;
	unsigned res;
	
	parse = parse_new(filename);	
	if (!parse) {
		return 0;
	}
	
	locals = exec->locals;
	exec->locals = &exec->globals;
	res = parse_prog(parse, exec);
	exec->locals = locals;
	
	parse_delete(parse);
	return res;
}

/* -------------------------------------------------------- console --- */

void
user_printstr(char *str) {
	textout_putstr(user_stdout, str);
}

void
user_printlong(long l) {
	if (l < 0) {
		textout_putstr(user_stdout, "-");
		l = -l;
	}
	textout_putlong(user_stdout, (unsigned long)l);
}

void
user_error(char *str) {
	textout_putstr(user_stdout, str);
}

void
exec_printdata(struct exec_s *o, struct data_s *d) {
	struct data_s *i;
	
	switch(d->type) {
	case DATA_NIL:
		user_printstr("(nil)");
		break;
	case DATA_LONG:
		user_printlong(d->val.num);
		break;
	case DATA_STRING:
		user_printstr(d->val.str);
		break;
	case DATA_REF:
		user_printstr(d->val.ref);			
		break;
	case DATA_LIST:
		for (i = d->val.list; i != 0; i = i->next) {
			exec_printdata(o, i);
			if (i->next) {
				user_printstr(" ");
			}
		}
		break;
	default:
		dbg_puts("exec_printdata: unknown type\n");
		break;
	}
}


unsigned
exec_lookuptrack(struct exec_s *o, char *var, struct songtrk_s **res) {
	char *name;	
	struct songtrk_s *t;
	if (!exec_lookupname(o, var, &name)) {
		return 0;
	}
	t = song_trklookup(user_song, name);
	if (t == 0) {
		user_printstr(name);
		user_printstr(": no such track\n");
		return 0;
	}
	*res = t;
	return 1;
}

unsigned
data_list2chan(struct data_s *o, unsigned *dev, unsigned *ch) {
	struct songchan_s *i;

	if (o->type == DATA_LIST) {
		if (!o->val.list || 
		    !o->val.list->next || 
		    o->val.list->next->next ||
		    o->val.list->type != DATA_LONG || 
		    o->val.list->next->type != DATA_LONG) {
			user_printstr("bad {dev midichan} in spec\n");
			return 0;
		}
		*dev = o->val.list->val.num;
		*ch = o->val.list->next->val.num;
		if (*ch < 0 || *ch > EV_MAXCH || 
		    *dev < 0 || *dev > EV_MAXDEV) {
			user_printstr("bad dev/midichan ranges\n");
		}
		return 1;
	} else if (o->type == DATA_REF) {
		i = song_chanlookup(user_song, o->val.ref);
		if (i == 0) {
			user_printstr("no such chan name\n");
			return 0;
		}
		*dev = i->dev;
		*ch = i->ch;
		return 1;
	} else {
		user_printstr("bad channel specification\n");
		return 0;
	}
}


unsigned
exec_lookupchan_getnum(struct exec_s *o, char *var, 
    unsigned *dev, unsigned *ch) {
	struct var_s *arg;
	
	arg = exec_varlookup(o, var);
	if (!arg) {
		dbg_puts("exec_lookupchan_getnum: no such var\n");
		dbg_panic();
	}
	if (!data_list2chan(arg->data, dev, ch)) {
		return 0;
	}
	return 1;
}

unsigned
exec_lookupchan_getref(struct exec_s *o, char *var, struct songchan_s **res) {
	struct var_s *arg;
	struct songchan_s *i;
	
	arg = exec_varlookup(o, var);
	if (!arg) {
		dbg_puts("exec_lookupchan: no such var\n");
		dbg_panic();
	}
	if (arg->data->type == DATA_REF) {
		i = song_chanlookup(user_song, arg->data->val.ref);
	} else {
		user_printstr("bad channel name\n");
		return 0;
	}
	if (i == 0) {
		user_printstr("no such chan\n");
		return 0;
	}
	*res = i;
	return 1;
}


unsigned
exec_lookupfilt(struct exec_s *o, char *var, struct songfilt_s **res) {
	char *name;	
	struct songfilt_s *f;
	if (!exec_lookupname(o, var, &name)) {
		return 0;
	}
	f = song_filtlookup(user_song, name);
	if (f == 0) {
		user_printstr(name);
		user_printstr(": no such filt\n");
		return 0;
	}
	*res = f;
	return 1;
}


unsigned
exec_lookupev(struct exec_s *o, char *name, struct ev_s *ev) {
	struct var_s *arg;
	struct data_s *d;
	unsigned dev, ch;

	arg = exec_varlookup(o, name);
	if (!arg) {
		dbg_puts("exec_lookupev: no such var\n");
		dbg_panic();
	}
	d = arg->data;

	if (d->type != DATA_LIST) {
		user_printstr("event spec must be a list\n");
		return 0;
	}
	d = d->val.list;
	if (!d || d->type != DATA_REF || 
	    !ev_str2cmd(ev, d->val.ref) ||
	    !EV_ISVOICE(ev)) {
		user_printstr("bad status in event spec\n");
		return 0;
	}
	d = d->next;
	if (!d) {
		user_printstr("no channel in event spec\n");
		return 0;
	}
	if (!data_list2chan(d, &dev, &ch)) {
		return 0;
	}
	ev->data.voice.dev = dev;
	ev->data.voice.ch = ch;
	d = d->next;
	if (!d || d->type != DATA_LONG) {
		user_printstr("bad byte0 in event spec\n");
		return 0;
	}
	if (ev->cmd == EV_BEND) {
		EV_SETBEND(ev, d->val.num);
	} else {
		ev->data.voice.b0 = d->val.num;
	}		
	d = d->next;
	if (ev->cmd != EV_PC && ev->cmd != EV_CAT && ev->cmd != EV_BEND) {
		if (!d || d->type != DATA_LONG) {
			user_printstr("bad byte1 in event spec\n");
			return 0;
		}
		ev->data.voice.b1 = d->val.num;
	} else {
		if (d) {
			user_printstr("extra data in event spec\n");
			return 0;
		}
	}
	return 1;
}


unsigned
data_list2range(struct data_s *d, unsigned min, unsigned max, 
    unsigned *lo, unsigned *hi) {
    	if (d->type == DATA_LONG) {
		*lo = *hi = d->val.num;
	} else if (d->type == DATA_LIST) {
		d = d->val.list;
		if (!d) {
			*lo = min;
			*hi = max;
			return 1;
		} 
		if (!d->next || d->next->next || 
		    d->type != DATA_LONG || d->next->type != DATA_LONG) {
			user_printstr("exactly 0 ore 2 numbers expected in range spec\n");
			return 0;
		}
		*lo = d->val.num;
		*hi = d->next->val.num;
	} else {
		user_printstr("list or number expected in range spec\n");
		return 0;
	}
	if (*lo < min || *lo > max || *hi < min || *hi > max || *lo > *hi) {
		user_printstr("range values out of bounds\n");
		return 0;
	}
	return 1;
}


unsigned
exec_lookupevspec(struct exec_s *o, char *name, struct evspec_s *e) {
	struct var_s *arg;
	struct data_s *d;
	struct songchan_s *i;
	unsigned lo, hi;

	arg = exec_varlookup(o, name);
	if (!arg) {
		dbg_puts("exec_lookupev: no such var\n");
		dbg_panic();
	}
	d = arg->data;
	if (d->type != DATA_LIST) {
		user_printstr("list expected in event range spec\n");
		return 0;
	}

	/* default match any event */
	evspec_reset(e);

	d = d->val.list;
	if (!d) {
		return 1;
	}
	if (d->type != DATA_REF ||
	    !evspec_str2cmd(e, d->val.ref)) {
		user_printstr("bad status in event spec\n");
		return 0;
	}
	
	d = d->next;
	if (!d) {
		return 1;
	}
	if (d->type == DATA_REF) {
		i = song_chanlookup(user_song, d->val.ref);
		if (i == 0) {
			user_printstr("no such chan name\n");
			return 0;
		}
		e->dev_min = e->dev_max = i->dev;
		e->ch_min = e->ch_max = i->ch;
	} else if (d->type == DATA_LIST) {
		if (!d->val.list) {		/* empty list = any chan/dev */
			/* nothing */
		} else if (d->val.list && 
		    d->val.list->next && 
		    !d->val.list->next->next) {
			if (!data_list2range(d->val.list, 0, EV_MAXDEV, &lo, &hi)) {
				return 0;
			}
			e->dev_min = lo;
			e->dev_max = hi;
			if (!data_list2range(d->val.list->next, 0, EV_MAXCH, &lo, &hi)) {
				return 0;
			}
			e->ch_min = lo;
			e->ch_max = hi;	
		} else {
			user_printstr("bad channel range spec\n");
			return 0;
		}
	}

	d = d->next;
	if (!d) {
		return 1;
	}
	if (e->cmd == EVSPEC_ANY) {
		goto toomany;
	}
	if (e->cmd == EVSPEC_BEND) {
		if (!data_list2range(d, 0, EV_MAXBEND, &lo, &hi)) {
			return 0;
		}
		e->b0_min = lo;
		e->b0_max = hi;
	} else {
		if (!data_list2range(d, 0, EV_MAXB0, &lo, &hi)) {
			return 0;
		}
		e->b0_min = lo;
		e->b0_max = hi;
	}
	d = d->next;
	if (!d) {
		return 1;
	}
	if (e->cmd != EVSPEC_PC && 
	    e->cmd != EVSPEC_CAT && 
	    e->cmd != EVSPEC_BEND) {
		if (!data_list2range(d, 0, EV_MAXB1, &lo, &hi)) {
			return 0;
		}
		e->b1_min = lo;
		e->b1_max = hi;
		d = d->next;
		if (!d) {
			return 1;
		}
	}
toomany:
	user_printstr("too many ranges in event spec\n");
	return 0;				
}



/* ---------------------------------------- interpreter functions --- */

unsigned
user_func_ev(struct exec_s *o) {
	struct evspec_s ev;
	if (!exec_lookupevspec(o, "ev", &ev)) {
		return 0;
	}
	evspec_dbg(&ev);
	dbg_puts("\n");
	return 1;
}

unsigned
user_func_panic(struct exec_s *o) {
	dbg_panic();
	/* not reached */
	return 0;
}

unsigned
user_func_debug(struct exec_s *o) {
	char *flag;
	long value;
	
	if (!exec_lookupname(o, "flag", &flag) ||
	    !exec_lookuplong(o, "value", &value)) {
		return 0;
	}
	if (str_eq(flag, "parse")) {
		parse_debug = value;
	} else if (str_eq(flag, "tree")) {
		tree_debug = value;
	} else if (str_eq(flag, "rmidi")) {
		rmidi_debug = value;
	} else if (str_eq(flag, "filt")) {
		filt_debug = value;
	} else {
		user_printstr("debug: unknuwn debug-flag\n");
		return 0;
	}
	return 1;
}

unsigned
user_func_exec(struct exec_s *o) {
	char *filename;		
	if (!exec_lookupstring(o, "filename", &filename)) {
		return 0;
	}	
	return user_parsefile(o, filename);
}


unsigned
user_func_print(struct exec_s *o) {
	struct var_s *arg;
	arg = exec_varlookup(o, "value");
	if (!arg) {
		dbg_puts("user_func_print: 'value': no such param\n");
		return 0;
	}
	exec_printdata(o, arg->data);
	user_printstr("\n");
	exec_putacc(o, data_newnil());
	return 1;
}

unsigned
user_func_help(struct exec_s *o) {
	exec_dumpprocs(o);
	return 1;
}

/* -------------------------------------------------- track stuff --- */

unsigned
user_func_tracklist(struct exec_s *o) {
	struct data_s *d, *n;
	struct songtrk_s *i;

	d = data_newlist(0);
	for (i = user_song->trklist; i != 0; i = (struct songtrk_s *)i->name.next) {
		n = data_newref(i->name.str);
		data_listadd(d, n);
	}
	exec_putacc(o, d);
	return 1;
}


unsigned
user_func_tracknew(struct exec_s *o) {
	char *trkname;
	struct songtrk_s *t;
	
	if (!exec_lookupname(o, "trackname", &trkname)) {
		return 0;
	}
	t = song_trklookup(user_song, trkname);
	if (t != 0) {
		user_printstr("tracknew: track already exists\n");
		return 0;
	}
	t = songtrk_new(trkname);
	song_trkadd(user_song, t);
	return 1;
}


unsigned
user_func_trackdelete(struct exec_s *o) {
	struct songtrk_s *t;
	if (!exec_lookuptrack(o, "trackname", &t)) {
		return 0;
	}
	if (!song_trkrm(user_song, t)) {
		return 0;
	}
	songtrk_delete(t);
	return 1;
}


unsigned
user_func_trackrename(struct exec_s *o) {
	struct songtrk_s *t;
	char *name;
	
	if (!exec_lookuptrack(o, "trackname", &t) ||
	    !exec_lookupname(o, "newname", &name)) {
		return 0;
	}
	if (song_trklookup(user_song, name)) {
		user_printstr("name already used by another track\n");
		return 0;
	}
	str_delete(t->name.str);
	t->name.str = str_new(name);
	return 1;
}


unsigned
user_func_trackexists(struct exec_s *o) {
	char *name;
	struct songtrk_s *t;
	
	if (!exec_lookupname(o, "trackname", &name)) {
		return 0;
	}
	t = song_trklookup(user_song, name);
	exec_putacc(o, data_newlong(t != 0 ? 1 : 0));
	return 1;
}


unsigned
user_func_trackaddev(struct exec_s *o) {
	long measure, beat, tic;
	struct ev_s ev;
	struct seqptr_s tp;
	struct songtrk_s *t;
	unsigned pos, bpm, tpb;
	unsigned long dummy;

	if (!exec_lookuptrack(o, "trackname", &t) || 
	    !exec_lookuplong(o, "measure", &measure) || 
	    !exec_lookuplong(o, "beat", &beat) || 
	    !exec_lookuplong(o, "tic", &tic) || 
	    !exec_lookupev(o, "event", &ev)) {
		return 0;
	}

	pos = track_opfindtic(&user_song->meta, measure);
	track_optimeinfo(&user_song->meta, pos, &dummy, &bpm, &tpb);
	
	if (beat < 0 || beat >= bpm || tic < 0 || tic >= tpb) {
		user_printstr("beat and tic must fit in the selected measure\n");
		return 0;
	}

	pos += beat * tpb + tic;	

	track_rew(&t->track, &tp);
	track_seekblank(&t->track, &tp, pos);
	track_evlast(&t->track, &tp);
	track_evput(&t->track, &tp, &ev);
	return 1;
}


unsigned
user_func_tracksetcurfilt(struct exec_s *o) {
	struct songtrk_s *t;
	struct songfilt_s *f;
	struct var_s *arg;
	
	if (!exec_lookuptrack(o, "trackname", &t)) {
		return 0;
	}	
	arg = exec_varlookup(o, "filtname");
	if (!arg) {
		dbg_puts("user_func_tracksetcurfilt: 'filtname': no such param\n");
		return 0;
	}
	if (arg->data->type == DATA_NIL) {
		t->curfilt = 0;
		return 1;
	} else if (arg->data->type == DATA_REF) {
		f = song_filtlookup(user_song, arg->data->val.ref);
		if (!f) {
			user_printstr("no such filt\n");
			return 0;
		}
		t->curfilt = f;
		return 1;
	}
	return 0;
}

unsigned
user_func_trackgetcurfilt(struct exec_s *o) {
	struct songtrk_s *t;
	
	if (!exec_lookuptrack(o, "trackname", &t)) {
		return 0;
	}
	if (t->curfilt) {
		exec_putacc(o, data_newref(t->curfilt->name.str));
	} else {
		exec_putacc(o, data_newnil());
	}		
	return 1;
}


unsigned
user_func_trackcheck(struct exec_s *o) {
	char *trkname;
	struct songtrk_s *t;
	
	if (!exec_lookupname(o, "trackname", &trkname)) {
		return 0;
	}
	t = song_trklookup(user_song, trkname);
	if (t == 0) {
		user_printstr("trackcheck: no such track\n");
		return 0;
	}
	track_opcheck(&t->track);
	return 1;
}


unsigned
user_func_trackgetlen(struct exec_s *o) {
	char *trkname;
	struct songtrk_s *t;
	unsigned len;
	
	if (!exec_lookupname(o, "trackname", &trkname)) {
		return 0;
	}
	t = song_trklookup(user_song, trkname);
	if (t == 0) {
		user_printstr("trackgetlen: no such track\n");
		return 0;
	}
	len = track_numtic(&t->track);
	exec_putacc(o, data_newlong((long)len));
	return 1;
}


unsigned
user_func_tracksave(struct exec_s *o) {
	char *trkname, *filename;
	struct songtrk_s *t;
	if (!exec_lookupname(o, "trackname", &trkname) ||
	    !exec_lookupstring(o, "filename", &filename)) {
		return 0;
	}
	t = song_trklookup(user_song, trkname);
	if (t == 0) {
		user_printstr("tracksave: no such track\n");
		return 0;
	}
	track_save(&t->track, filename);
	return 1;
}


unsigned
user_func_trackload(struct exec_s *o) {
	char *trkname, *filename;
	struct songtrk_s *t;
	if (!exec_lookupname(o, "trackname", &trkname) ||  
	    !exec_lookupstring(o, "filename", &filename)) {
		return 0;
	}
	t = song_trklookup(user_song, trkname);
	if (t == 0) {
		user_printstr("trackload: no such track\n");
		return 0;
	}
	track_load(&t->track, filename);
	return 1;
}


unsigned
user_func_trackcut(struct exec_s *o) {
	struct songtrk_s *t;
	long from, amount, quant;
	unsigned tic, len;
	struct seqptr_s op;
	
	if (!exec_lookuptrack(o, "trackname", &t) ||
	    !exec_lookuplong(o, "from", &from) ||
	    !exec_lookuplong(o, "amount", &amount) ||
	    !exec_lookuplong(o, "quantum", &quant)) {
		return 0;
	}
	tic = song_measuretotic(user_song, from);
	len = song_measuretotic(user_song, from + amount) - tic;

	if (tic > quant/2) {
		tic -= quant/2;
	}

	track_rew(&t->track, &op);
	track_seek(&t->track, &op, tic);
	track_opcut(&t->track, &op, len);
	return 1;
}


unsigned
user_func_trackblank(struct exec_s *o) {
	struct songtrk_s *t;
	struct track_s null;
	struct seqptr_s tp;
	struct evspec_s es;
	long from, amount, quant;
	unsigned tic, len;
	
	if (!exec_lookuptrack(o, "trackname", &t) ||
	    !exec_lookuplong(o, "from", &from) ||
	    !exec_lookuplong(o, "amount", &amount) ||
	    !exec_lookuplong(o, "quantum", &quant) ||
	    !exec_lookupevspec(o, "evspec", &es)) {
		return 0;
	}
	tic = song_measuretotic(user_song, from);
	len = song_measuretotic(user_song, from + amount) - tic;
	
	if (tic > quant/2) {
		tic -= quant/2;
	}

	track_init(&null);
	track_rew(&t->track, &tp);
	track_seek(&t->track, &tp, tic);
	track_opextract(&t->track, &tp, len, &null, &es);
	track_done(&null);
	return 1;
}


unsigned
user_func_trackcopy(struct exec_s *o) {
	struct songtrk_s *t, *t2;
	struct track_s null, null2;
	struct seqptr_s tp, tp2;
	struct evspec_s es;
	long from, where, amount, quant;
	unsigned tic, tic2, len;
	
	if (!exec_lookuptrack(o, "trackname", &t) ||
	    !exec_lookuplong(o, "from", &from) ||
	    !exec_lookuplong(o, "amount", &amount) ||
	    !exec_lookuptrack(o, "trackname2", &t2) ||
	    !exec_lookuplong(o, "where", &where) ||
	    !exec_lookuplong(o, "quantum", &quant) ||
	    !exec_lookupevspec(o, "evspec", &es)) {
		return 0;
	}
	tic  = song_measuretotic(user_song, from);
	len  = song_measuretotic(user_song, from + amount) - tic;
	tic2 = song_measuretotic(user_song, where);

	if (tic > quant/2 && tic > quant/2) {
		tic -= quant/2;
		tic2 -= quant/2;
	}

	track_init(&null);
	track_init(&null2);
	track_rew(&t->track, &tp);
	track_seek(&t->track, &tp, tic);
	track_rew(&t2->track, &tp2);
	track_seekblank(&t2->track, &tp2, tic2);
	track_opextract(&t->track, &tp, len, &null, &es);
	track_framecp(&null, &null2);
	track_frameins(&t->track, &tp, &null);
	track_frameins(&t2->track, &tp2, &null2);
	track_done(&null);
	return 1;
}


unsigned
user_func_trackinsert(struct exec_s *o) {
	char *trkname;
	struct songtrk_s *t;
	struct seqptr_s tp;
	long from, amount, quant;
	unsigned tic, len;
	
	if (!exec_lookupname(o, "trackname", &trkname) ||
	    !exec_lookuplong(o, "from", &from) ||
	    !exec_lookuplong(o, "amount", &amount) ||
	    !exec_lookuplong(o, "quantum", &quant)) {
		return 0;
	}
	t = song_trklookup(user_song, trkname);
	if (t == 0) {
		user_printstr("trackinsert: no such track\n");
		return 0;
	}

	tic = song_measuretotic(user_song, from);
	len = song_measuretotic(user_song, from + amount) - tic;

	if (tic > quant/2) {
		tic -= quant/2;
	}

	track_rew(&t->track, &tp);
	track_seekblank(&t->track, &tp, tic);
	track_opinsert(&t->track, &tp, len);
	return 1;
}


unsigned
user_func_trackquant(struct exec_s *o) {
	char *trkname;
	struct songtrk_s *t;
	struct seqptr_s tp;
	long from, amount;
	unsigned tic, len, first;
	long quantum, rate;
	
	if (!exec_lookupname(o, "trackname", &trkname) ||
	    !exec_lookuplong(o, "from", &from) ||
	    !exec_lookuplong(o, "amount", &amount) ||
	    !exec_lookuplong(o, "quantum", &quantum) || 
	    !exec_lookuplong(o, "rate", &rate)) {
		return 0;
	}	
	t = song_trklookup(user_song, trkname);
	if (t == 0) {
		user_printstr("trackquant: no such track\n");
		return 0;
	}
	if (rate > 100) {
		user_printstr("trackquant: rate must be between 0 and 100\n");
		return 0;
	}

	tic = song_measuretotic(user_song, from);
	len = song_measuretotic(user_song, from + amount) - tic;
	
	if (tic > quantum / 2) {
		tic -= quantum / 2;
		first = quantum / 2;
	} else {
		first = 0;
		len -= quantum / 2;
	}
	
	track_rew(&t->track, &tp);
	track_seek(&t->track, &tp, tic);
	track_opquantise(&t->track, &tp, first, len, (unsigned)quantum, (unsigned)rate);
	return 1;
}

/* -------------------------------------------------- chan stuff --- */


unsigned
user_func_chanlist(struct exec_s *o) {
	struct data_s *d, *n;
	struct songchan_s *i;

	d = data_newlist(0);
	for (i = user_song->chanlist; i != 0; i = (struct songchan_s *)i->name.next) {
		n = data_newref(i->name.str);
		data_listadd(d, n);
	}
	exec_putacc(o, d);
	return 1;
}

unsigned
user_func_channew(struct exec_s *o) {
	char *name;
	struct var_s *arg;
	struct songchan_s *i;
	unsigned dev, ch;
	
	if (!exec_lookupname(o, "channame", &name)) {
		return 0;
	}
	i = song_chanlookup(user_song, name);
	if (i != 0) {
		user_printstr("channew: chan already exists\n");
		return 0;
	}
	arg = exec_varlookup(o, "channum");
	if (!arg) {
		dbg_puts("exec_lookupchan: no such var\n");
		dbg_panic();
	}
	if (!data_list2chan(arg->data, &dev, &ch)) {
		return 0;
	}
	i = song_chanlookup_bynum(user_song, dev, ch);
	if (i != 0) {
		user_printstr("channew: dev/chan number already used by '");
		user_printstr(i->name.str);
		user_printstr("'\n");
		return 0;
	}
	
	i = songchan_new(name);
	if (dev > EV_MAXDEV || ch > EV_MAXCH) {
		user_printstr("channew: dev/chan number out of bounds\n");
		return 0;
	}
	i->dev = dev;
	i->ch = ch;
	song_chanadd(user_song, i);
	return 1;
}

unsigned
user_func_chandelete(struct exec_s *o) {
	struct songchan_s *c;
	if (!exec_lookupchan_getref(o, "channame", &c)) {
		return 0;
	}
	if (!song_chanrm(user_song, c)) {
		return 0;
	}
	songchan_delete(c);
	return 1;
}

unsigned
user_func_chanrename(struct exec_s *o) {
	struct songchan_s *c;
	char *name;
	
	if (!exec_lookupchan_getref(o, "channame", &c) ||
	    !exec_lookupname(o, "newname", &name)) {
		return 0;
	}
	if (song_chanlookup(user_song, name)) {
		user_printstr("name already used by another chan\n");
		return 0;
	}
	str_delete(c->name.str);
	c->name.str = str_new(name);
	return 1;
}

unsigned
user_func_chanexists(struct exec_s *o) {
	char *name;
	struct songchan_s *i;
	if (!exec_lookupname(o, "channame", &name)) {
		return 0;
	}
	i = song_chanlookup(user_song, name);
	exec_putacc(o, data_newlong(i != 0 ? 1 : 0));
	return 1;
}

unsigned
user_func_changetch(struct exec_s *o) {
	struct songchan_s *i;
	
	if (!exec_lookupchan_getref(o, "channame", &i)) {
		return 0;
	}
	exec_putacc(o, data_newlong(i->ch));
	return 1;
}

unsigned
user_func_changetdev(struct exec_s *o) {
	struct songchan_s *i;
	
	if (!exec_lookupchan_getref(o, "channame", &i)) {
		return 0;
	}
	exec_putacc(o, data_newlong(i->dev));
	return 1;
}

unsigned
user_func_chanconfev(struct exec_s *o) {
	struct songchan_s *c;
	struct seqptr_s cp;
	struct ev_s ev;
	
	if (!exec_lookupchan_getref(o, "channame", &c) ||
	    !exec_lookupev(o, "event", &ev)) {
		return 0;
	}
	track_rew(&c->conf, &cp);
	track_evlast(&c->conf, &cp);
	track_evput(&c->conf, &cp, &ev);
	track_opcheck(&c->conf);
	return 1;
}

unsigned
user_func_chaninfo(struct exec_s *o) {
	struct songchan_s *c;
	
	if (!exec_lookupchan_getref(o, "channame", &c)) {
		return 0;
	}
	track_output(&c->conf, user_stdout);
	user_printstr("\n");
	return 1;
}

/* -------------------------------------------------- filt stuff --- */


unsigned
user_func_filtlist(struct exec_s *o) {
	struct data_s *d, *n;
	struct songfilt_s *i;

	d = data_newlist(0);
	for (i = user_song->filtlist; i != 0; i = (struct songfilt_s *)i->name.next) {
		n = data_newref(i->name.str);
		data_listadd(d, n);
	}
	exec_putacc(o, d);
	return 1;
}

unsigned
user_func_filtnew(struct exec_s *o) {
	char *name;
	struct songfilt_s *i;
	
	if (!exec_lookupname(o, "filtname", &name)) {
		return 0;
	}	
	i = song_filtlookup(user_song, name);
	if (i != 0) {
		user_printstr("filtnew: filt already exists\n");
		return 0;
	}
	i = songfilt_new(name);
	song_filtadd(user_song, i);
	return 1;
}

unsigned
user_func_filtdelete(struct exec_s *o) {
	struct songfilt_s *f;
	if (!exec_lookupfilt(o, "filtname", &f)) {
		return 0;
	}
	if (!song_filtrm(user_song, f)) {
		return 0;
	}
	songfilt_delete(f);
	return 1;
}

unsigned
user_func_filtrename(struct exec_s *o) {
	struct songfilt_s *f;
	char *name;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupname(o, "newname", &name)) {
		return 0;
	}
	if (song_filtlookup(user_song, name)) {
		user_printstr("name already used by another filt\n");
		return 0;
	}
	str_delete(f->name.str);
	f->name.str = str_new(name);
	return 1;
}

unsigned
user_func_filtexists(struct exec_s *o) {
	char *name;
	struct songfilt_s *i;
	if (!exec_lookupname(o, "filtname", &name)) {
		return 0;
	}
	i = song_filtlookup(user_song, name);
	exec_putacc(o, data_newlong(i != 0 ? 1 : 0));
	return 1;
}

unsigned
user_func_filtinfo(struct exec_s *o) {
	struct songfilt_s *f;
	struct rule_s *i;
	
	if (!exec_lookupfilt(o, "filtname", &f)) {
		return 0;
	}
	for (i = f->filt.voice_rules; i != 0; i = i->next) {
		rule_output(i, user_stdout);
	}
	for (i = f->filt.chan_rules; i != 0; i = i->next) {
		rule_output(i, user_stdout);
	}
	for (i = f->filt.dev_rules; i != 0; i = i->next) {
		rule_output(i, user_stdout);
	}
	return 1;
}


unsigned
user_func_filtdevdrop(struct exec_s *o) {
	struct songfilt_s *f;
	long idev;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookuplong(o, "indev", &idev)) {
		return 0;
	}
	if (idev < 0 || idev > EV_MAXDEV) {
	    	user_printstr("device number out of range\n");
		return 0;
	}
	filt_conf_devdrop(&f->filt, idev);
	return 1;
}



unsigned
user_func_filtnodevdrop(struct exec_s *o) {
	struct songfilt_s *f;
	long idev;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookuplong(o, "indev", &idev)) {
		return 0;
	}
	if (idev < 0 || idev > EV_MAXDEV) {
	    	user_printstr("device number out of range\n");
		return 0;
	}
	filt_conf_nodevdrop(&f->filt, idev);
	return 1;
}


unsigned
user_func_filtdevmap(struct exec_s *o) {
	struct songfilt_s *f;
	long idev, odev;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookuplong(o, "indev", &idev) ||
	    !exec_lookuplong(o, "outdev", &odev)) {
		return 0;
	}
	if (idev < 0 || idev > EV_MAXDEV ||
	    odev < 0 || odev > EV_MAXDEV) {
	    	user_printstr("device number out of range\n");
		return 0;
	}
	filt_conf_devmap(&f->filt, idev, odev);
	return 1;
}


unsigned
user_func_filtnodevmap(struct exec_s *o) {
	struct songfilt_s *f;
	long odev;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookuplong(o, "outdev", &odev)) {
		return 0;
	}
	if (odev < 0 || odev > EV_MAXDEV) {
	    	user_printstr("device number out of range\n");
		return 0;
	}
	filt_conf_nodevmap(&f->filt, odev);
	return 1;
}


unsigned
user_func_filtchandrop(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned idev, ich;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "inchan", &idev, &ich)) {
		return 0;
	}
	filt_conf_chandrop(&f->filt, idev, ich);
	return 1;
}


unsigned
user_func_filtnochandrop(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned idev, ich;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "inchan", &idev, &ich)) {
		return 0;
	}
	filt_conf_nochandrop(&f->filt, idev, ich);
	return 1;
}

unsigned
user_func_filtchanmap(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned idev, ich, odev, och;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "inchan", &idev, &ich) ||
	    !exec_lookupchan_getnum(o, "outchan", &odev, &och)) {
		return 0;
	}
	filt_conf_chanmap(&f->filt, idev, ich, odev, och);
	return 1;
}

unsigned
user_func_filtnochanmap(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned odev, och;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "outchan", &odev, &och)) {
		return 0;
	}
	filt_conf_nochanmap(&f->filt, odev, och);
	return 1;
}

unsigned
user_func_filtctldrop(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned idev, ich;
	long ictl;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "inchan", &idev, &ich) ||
	    !exec_lookuplong(o, "inctl", &ictl)) {
		return 0;
	}
	if (ictl < 0 || ictl > EV_MAXB0) {
		user_printstr("filtctlmap: controllers must be between 0 and 127\n");
		return 0;
	}
	filt_conf_ctldrop(&f->filt, idev, ich, ictl);
	return 1;
}


unsigned
user_func_filtnoctldrop(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned idev, ich;
	long ictl;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "inchan", &idev, &ich) ||
	    !exec_lookuplong(o, "inctl", &ictl)) {
		return 0;
	}
	if (ictl < 0 || ictl > EV_MAXB0) {
		user_printstr("filtctlmap: controllers must be between 0 and 127\n");
		return 0;
	}
	filt_conf_noctldrop(&f->filt, idev, ich, ictl);
	return 1;
}


unsigned
user_func_filtctlmap(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned idev, ich, odev, och;
	long ictl, octl;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "inchan", &idev, &ich) ||
	    !exec_lookupchan_getnum(o, "outchan", &odev, &och) ||
	    !exec_lookuplong(o, "inctl", &ictl) || 
	    !exec_lookuplong(o, "outctl", &octl)) {
		return 0;
	}
	if (ictl < 0 || ictl > EV_MAXB0 || octl < 0 || octl > EV_MAXB0) {
		user_printstr("filtctlmap: controllers must be between 0 and 127\n");
		return 0;
	}
	filt_conf_ctlmap(&f->filt, idev, ich, odev, och, ictl, octl);
	return 1;
}


unsigned
user_func_filtnoctlmap(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned odev, och;
	long octl;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "outchan", &odev, &och) || 
	    !exec_lookuplong(o, "outctl", &octl)) {
		return 0;
	}
	if (octl < 0 || octl > EV_MAXB0) {
		user_printstr("filtctlmap: controllers must be between 0 and 127\n");
		return 0;
	}
	filt_conf_noctlmap(&f->filt, odev, och, octl);
	return 1;
}

unsigned
user_func_filtkeydrop(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned idev, ich;
	long kstart, kend;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "inchan", &idev, &ich) ||
	    !exec_lookuplong(o, "keystart", &kstart) || 
	    !exec_lookuplong(o, "keyend", &kend)) {
		return 0;
	}
	if (kstart < 0 || kstart > EV_MAXB0 || kend < 0 || kend > EV_MAXB0) {
		user_printstr("filtkeymap: notes must be between 0 and 127\n");
		return 0;
	}
	filt_conf_keydrop(&f->filt, idev, ich, kstart, kend);
	return 1;
}


unsigned
user_func_filtnokeydrop(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned idev, ich;
	long kstart, kend;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "inchan", &idev, &ich) ||
	    !exec_lookuplong(o, "keystart", &kstart) || 
	    !exec_lookuplong(o, "keyend", &kend)) {
		return 0;
	}
	if (kstart < 0 || kstart > EV_MAXB0 || kend < 0 || kend > EV_MAXB0) {
		user_printstr("filtkeymap: notes must be between 0 and 127\n");
		return 0;
	}
	filt_conf_nokeydrop(&f->filt, idev, ich, kstart, kend);
	return 1;
}


unsigned
user_func_filtkeymap(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned idev, ich, odev, och;
	long kstart, kend, kplus;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "inchan", &idev, &ich) ||
	    !exec_lookupchan_getnum(o, "outchan", &idev, &och) ||
	    !exec_lookuplong(o, "keystart", &kstart) || 
	    !exec_lookuplong(o, "keyend", &kend) ||
	    !exec_lookuplong(o, "keyplus", &kplus)) {
		return 0;
	}
	if (kstart < 0 || kstart > EV_MAXB0 || kend < 0 || kend > EV_MAXB0) {
		user_printstr("filtkeymap: notes must be between 0 and 127\n");
		return 0;
	}
	if (kplus < - EV_MAXB0/2 || kplus > EV_MAXB0/2) {
		user_printstr("filtkeymap: transpose must be between -63 and 63\n");
		return 0;
	}
	filt_conf_keymap(&f->filt, idev, ich, odev, och, kstart, kend, kplus);
	return 1;
}


unsigned
user_func_filtnokeymap(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned odev, och;
	long kstart, kend;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "outchan", &odev, &och) ||
	    !exec_lookuplong(o, "keystart", &kstart) || 
	    !exec_lookuplong(o, "keyend", &kend)) {
		return 0;
	}
	if (kstart < 0 || kstart > EV_MAXB0 || kend < 0 || kend > EV_MAXB0) {
		user_printstr("filtkeymap: notes must be between 0 and 127\n");
		return 0;
	}
	filt_conf_nokeymap(&f->filt, odev, och, kstart, kend);
	return 1;
}



unsigned
user_func_filtreset(struct exec_s *o) {
	struct songfilt_s *f;
	
	if (!exec_lookupfilt(o, "filtname", &f)) {
		return 0;
	}
	filt_reset(&f->filt);
	return 1;
}


unsigned
user_func_filtswapichan(struct exec_s *o) {
	struct songfilt_s *f;
	unsigned olddev, oldch, newdev, newch;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookupchan_getnum(o, "oldchan", &olddev, &oldch) ||
	    !exec_lookupchan_getnum(o, "newchan", &newdev, &newch)) {
		return 0;
	}
	filt_conf_swapichan(&f->filt, olddev, oldch, newdev, newch);
	return 1;
}


unsigned
user_func_filtswapidev(struct exec_s *o) {
	struct songfilt_s *f;
	long olddev, newdev;
	
	if (!exec_lookupfilt(o, "filtname", &f) ||
	    !exec_lookuplong(o, "olddev", &olddev) ||
	    !exec_lookuplong(o, "newdev", &newdev)) {
		return 0;
	}
	if (olddev < 0 || olddev > EV_MAXDEV || 
	    newdev < 0 || newdev > EV_MAXDEV) {
		user_printstr("dev numver out of bounds\n");
	}
	filt_conf_swapidev(&f->filt, olddev, newdev);
	return 1;
}

/* ------------------------------------------------- song stuff --- */

unsigned
user_func_songsetunit(struct exec_s *o) {	/* tics per unit note */
	long tpu;
	if (!exec_lookuplong(o, "tics_per_unit", &tpu)) {
		return 0;
	}
	/* XXX: should check that all tracks are empty (tempo track included) */
	if (user_song->trklist) {
		user_printstr("WARNING: unit must be changed before any tracks are created\n");
	}
	if ((tpu % DEFAULT_TPU) != 0 || tpu < DEFAULT_TPU) {
		user_printstr("unit should multiple of 96\n");
		return 0;
	}
	user_song->tics_per_unit = tpu;
	return 1;
}

unsigned
user_func_songgetunit(struct exec_s *o) {	/* tics per unit note */
	exec_putacc(o, data_newlong(user_song->tics_per_unit));
	return 1;
}


unsigned
user_func_songsetcurpos(struct exec_s *o) {
	long measure;
	
	if (!exec_lookuplong(o, "measure", &measure)) {
		return 0;
	}
	if (measure < 0) {
		user_printstr("measure cant be negative\n");
		return 0;
	}
	user_song->curpos = measure;
	return 1;
}


unsigned
user_func_songgetcurpos(struct exec_s *o) {
	exec_putacc(o, data_newlong(user_song->curpos));
	return 1;
}


unsigned
user_func_songsetcurquant(struct exec_s *o) {
	long quantum;
	
	if (!exec_lookuplong(o, "quantum", &quantum)) {
		return 0;
	}
	if (quantum < 0 || quantum > user_song->tics_per_unit) {
		user_printstr("quantum must be between 0 and tics_per_unit\n");
		return 0;
	}
	user_song->curquant = quantum;
	return 1;
}


unsigned
user_func_songgetcurquant(struct exec_s *o) {
	exec_putacc(o, data_newlong(user_song->curquant));
	return 1;
}



unsigned
user_func_songsetcurtrack(struct exec_s *o) {
	struct songtrk_s *t;
	struct var_s *arg;
	
	arg = exec_varlookup(o, "trackname");
	if (!arg) {
		dbg_puts("user_func_songsetcurtrack: 'trackname': no such param\n");
		return 0;
	}
	if (arg->data->type == DATA_NIL) {
		user_song->curtrk = 0;
		return 1;
	} 
	if (!exec_lookuptrack(o, "trackname", &t)) {
		return 0;
	}
	user_song->curtrk = t;
	return 1;
}


unsigned
user_func_songgetcurtrack(struct exec_s *o) {
	if (user_song->curtrk) {
		exec_putacc(o, data_newref(user_song->curtrk->name.str));
	} else {
		exec_putacc(o, data_newnil());
	}
	return 1;
}


unsigned
user_func_songsetcurfilt(struct exec_s *o) {
	struct songfilt_s *f;
	struct var_s *arg;
	
	arg = exec_varlookup(o, "filtname");
	if (!arg) {
		dbg_puts("user_func_songsetcurfilt: 'filtname': no such param\n");
		return 0;
	}
	if (arg->data->type == DATA_NIL) {
		user_song->curfilt = 0;
		return 1;
	} else if (arg->data->type == DATA_REF) {
		f = song_filtlookup(user_song, arg->data->val.ref);
		if (!f) {
			user_printstr("no such filt\n");
			return 0;
		}
		user_song->curfilt = f;
		return 1;
	}
	return 0;
}


unsigned
user_func_songgetcurfilt(struct exec_s *o) {
	if (user_song->curfilt) {
		exec_putacc(o, data_newref(user_song->curfilt->name.str));
	} else {
		exec_putacc(o, data_newnil());
	}	
	return 1;
}

unsigned
user_func_songinfo(struct exec_s *o) {
	dbg_puts("tics_per_unit=");
	dbg_putu(user_song->tics_per_unit);
	dbg_puts(", ");
	dbg_puts("curpos=");
	dbg_putu(user_song->curpos);
	dbg_puts(", ");
	dbg_puts("curquant=");
	dbg_putu(user_song->curquant);
	dbg_puts("\n");
	return 1;
}

unsigned
user_func_songsave(struct exec_s *o) {
	char *filename;	
	if (!exec_lookupstring(o, "filename", &filename)) {
		return 0;
	}	
	song_save(user_song, filename);
	return 1;
}

unsigned
user_func_songload(struct exec_s *o) {
	char *filename;		
	if (!exec_lookupstring(o, "filename", &filename)) {
		return 0;
	}
	song_done(user_song);
	song_init(user_song);
	return song_load(user_song, filename);
}


unsigned
user_func_songreset(struct exec_s *o) {
	song_done(user_song);
	song_init(user_song);
	return 1;
}



unsigned
user_func_songexportsmf(struct exec_s *o) {
	char *filename;
	if (!exec_lookupstring(o, "filename", &filename)) {
		return 0;
	}	
	return song_exportsmf(user_song, filename);
}


unsigned
user_func_songimportsmf(struct exec_s *o) {
	char *filename;
	struct song_s *sng;
	if (!exec_lookupstring(o, "filename", &filename)) {
		return 0;
	}
	sng = song_importsmf(filename);
	if (sng == 0) {
		return 0;
	}
	song_delete(user_song);
	user_song = sng;
	return 1;
}


unsigned
user_func_songidle(struct exec_s *o) {
	song_idle(user_song);
	return 1;
}
		
unsigned
user_func_songplay(struct exec_s *o) {
	song_play(user_song);
	return 1;
}

unsigned
user_func_songrecord(struct exec_s *o) {
	song_record(user_song);
	return 1;
}


unsigned
user_func_songsettempo(struct exec_s *o) {	/* beat per minute*/
	long tempo, measure;
	struct ev_s ev;
	struct seqptr_s mp;
	unsigned bpm, tpb;
	unsigned long dummy;
	unsigned pos;
	
	if (!exec_lookuplong(o, "measure", &measure) ||
	    !exec_lookuplong(o, "beats_per_minute", &tempo)) {
		return 0;
	}	
	if (tempo < 40 || tempo > 240) {
		user_printstr("tempo must be between 40 and 240 beats per measure\n");
		return 0;
	}
	pos = track_opfindtic(&user_song->meta, measure);
	track_optimeinfo(&user_song->meta, pos, &dummy, &bpm, &tpb);
	
	track_rew(&user_song->meta, &mp);
	track_seekblank(&user_song->meta, &mp, pos);
	
	ev.cmd = EV_TEMPO;
	ev.data.tempo.usec24 = 60L * 24000000L / (tempo * tpb);
	
	track_evlast(&user_song->meta, &mp);
	track_evput(&user_song->meta, &mp, &ev);
	track_opcheck(&user_song->meta);
	return 1;
}

unsigned
user_func_songtimeins(struct exec_s *o) {
	long num, den, amount, from;
	struct ev_s ev;
	struct seqptr_s mp;
	unsigned pos, tics, save_bpm, save_tpb;
	unsigned long dummy_usec24;
	
	if (!exec_lookuplong(o, "from", &from) ||
	    !exec_lookuplong(o, "amount", &amount) ||
	    !exec_lookuplong(o, "numerator", &num) || 
	    !exec_lookuplong(o, "denominator", &den)) {
		return 0;
	}
	if (den != 2 && den != 4 && den != 8) {
		user_printstr("only 2, 4 and 8 are supported as denominator\n");
		return 0;
	}
	if (amount == 0) {
		return 1;
	}

	ev.cmd = EV_TIMESIG;
	ev.data.sign.beats = num;
	ev.data.sign.tics = user_song->tics_per_unit / den;

	pos = track_opfindtic(&user_song->meta, from);
	tics = ev.data.sign.beats * ev.data.sign.tics * amount;

	track_optimeinfo(&user_song->meta, pos, &dummy_usec24, &save_bpm, &save_tpb);
	
	track_rew(&user_song->meta, &mp);
	track_seekblank(&user_song->meta, &mp, pos);
	track_opinsert(&user_song->meta, &mp, tics);
	
	if (ev.data.sign.beats != save_bpm || ev.data.sign.tics != save_tpb) {
		track_rew(&user_song->meta, &mp);
		track_seek(&user_song->meta, &mp, pos);
		track_evput(&user_song->meta, &mp, &ev);
	
		ev.cmd = EV_TIMESIG;
		ev.data.sign.beats = save_bpm;
		ev.data.sign.tics = save_tpb;
		track_seek(&user_song->meta, &mp, tics);
		track_evput(&user_song->meta, &mp, &ev);
	}
	return 1;
}

unsigned
user_func_songtimerm(struct exec_s *o) {
	long amount, from;
	struct ev_s ev;
	struct seqptr_s mp;
	unsigned pos, tics, save_bpm, save_tpb, bpm, tpb;
	unsigned long save_usec24, usec24;
	
	if (!exec_lookuplong(o, "from", &from) ||
	    !exec_lookuplong(o, "amount", &amount)) {
		return 0;
	}
	if (amount == 0) {
		return 1;
	}

	pos = track_opfindtic(&user_song->meta, from);
	tics = track_opfindtic(&user_song->meta, from + amount) - pos;
	track_optimeinfo(&user_song->meta, pos, &save_usec24, &save_bpm, &save_tpb);
		
	track_rew(&user_song->meta, &mp);
	if (track_seek(&user_song->meta, &mp, pos)) {
		return 1;
	}
	track_opcut(&user_song->meta, &mp, tics);
	track_optimeinfo(&user_song->meta, pos, &usec24, &bpm, &tpb);
	
	if (bpm != save_bpm || tpb != save_tpb) {
		ev.cmd = EV_TIMESIG;
		ev.data.sign.beats = save_bpm;
		ev.data.sign.tics = save_tpb;
		track_rew(&user_song->meta, &mp);
		track_seek(&user_song->meta, &mp, pos);
		track_evput(&user_song->meta, &mp, &ev);
	}
	/* 
	 * XXX: other timesig events should be removed, 
	 * but we don't have a routine for this,
	 * so we use track_opcheck 
	 */
	track_opcheck(&user_song->meta);
	return 1;
}


unsigned
user_func_songtimeinfo(struct exec_s *o) {
	track_output(&user_song->meta, user_stdout);
	user_printstr("\n");
	return 1;
}


unsigned
user_func_metroswitch(struct exec_s *o) {
	long onoff;
	if (!exec_lookuplong(o, "onoff", &onoff)) {
		return 0;
	}	
	user_song->metro_enabled = onoff;
	return 1;
}


unsigned
user_func_metroconf(struct exec_s *o) {
	struct ev_s evhi, evlo;
	if (!exec_lookupev(o, "eventhi", &evhi) ||
	    !exec_lookupev(o, "eventlo", &evlo)) {
		return 0;
	}
	if (evhi.cmd != EV_NON && evlo.cmd != EV_NON) {
		user_printstr("note-on event expected\n");
		return 0;
	}
	user_song->metro_hi = evhi;
	user_song->metro_lo = evlo;
	return 1;
}




unsigned
user_func_shut(struct exec_s *o) {
	unsigned i;
	struct ev_s ev;
	struct mididev_s *dev;

	mux_init(0, 0);

	for (dev = mididev_list; dev != 0; dev = dev->next) {
		for (i = 0; i < EV_MAXCH; i++) {
			ev.cmd = EV_CTL;
			ev.data.voice.dev = dev->unit;		
			ev.data.voice.ch = i;
			ev.data.voice.b0 = 121;
			ev.data.voice.b1 = 0;
			mux_putev(&ev);
			ev.cmd = EV_CTL;		
			ev.data.voice.dev = dev->unit;		
			ev.data.voice.ch = i;
			ev.data.voice.b0 = 123;
			ev.data.voice.b1 = 0;
			mux_putev(&ev);
			ev.cmd = EV_BEND;
			ev.data.voice.dev = dev->unit;		
			ev.data.voice.ch = i;
			ev.data.voice.b0 = EV_BEND_DEFAULTLO;
			ev.data.voice.b1 = EV_BEND_DEFAULTHI;
		}
	}
	
	mux_flush();
	mux_done();
	return 1;
}

unsigned
user_func_sendraw(struct exec_s *o) {
	struct var_s *arg;
	struct data_s *i;
	unsigned char byte;
	long device;
	
	arg = exec_varlookup(o, "list");
	if (!arg) {
		dbg_puts("user_func_sendraw: 'list': no such param\n");
		dbg_panic();
	}
	if (arg->data->type != DATA_LIST) {
		user_printstr("argument must be a list\n");
		return 0;
	}
	if (!exec_lookuplong(o, "device", &device)) {
		return 0;
	}
	if (device < 0 || device >= DEFAULT_MAXNDEVS) {
		user_printstr("sendraw: device out of range\n");
	}
	for (i = arg->data->val.list; i != 0; i = i->next) {
		if (i->type != DATA_LONG || i->val.num < 0 || i->val.num > 255) {
			user_printstr("list elements must be integers in 0..255\n");
			return 0;
		}
	}
	
	mux_init(0, 0);
	for (i = arg->data->val.list; i != 0; i = i->next) {
		byte = i->val.num;
		mux_sendraw(device, &byte, 1);
	}
	mux_flush();
	mux_done();
	return 1;
}

unsigned
user_func_devlist(struct exec_s *o) {
	struct data_s *d, *n;
	struct mididev_s *i;

	d = data_newlist(0);
	for (i = mididev_list; i != 0; i = i->next) {
		n = data_newlong(i->unit);
		data_listadd(d, n);
	}
	exec_putacc(o, d);
	return 1;
}


unsigned
user_func_devattach(struct exec_s *o) {
	long unit;
	char *path;
	if (!exec_lookuplong(o, "unit", &unit) || 
	    !exec_lookupstring(o, "path", &path)) {
		return 0;
	}
	return mididev_attach(unit, path, 1, 1);
}

unsigned
user_func_devdetach(struct exec_s *o) {
	long unit;
	if (!exec_lookuplong(o, "unit", &unit)) {
		return 0;
	}
	return mididev_detach(unit);
}

unsigned
user_func_devsetmaster(struct exec_s *o) {
	struct var_s *arg;
	long unit;
	
	arg = exec_varlookup(o, "unit");
	if (!arg) {
		dbg_puts("user_func_devsetmaster: no such var\n");
		dbg_panic();
	}
	if (arg->data->type == DATA_NIL) {
		mididev_master = 0;
		return 0;
	} else if (arg->data->type == DATA_LONG) {
		unit = arg->data->val.num;
		if (unit < 0 || unit >= DEFAULT_MAXNDEVS || !mididev_byunit[unit]) {
			user_printstr("no such device\n");
			return 0;		
		}
		mididev_master = mididev_byunit[unit];
		return 1;
	}
	
	user_printstr("bad argument type for 'unit'\n");
	return 0;
}


unsigned
user_func_devgetmaster(struct exec_s *o) {
	if (mididev_master) {
		exec_putacc(o, data_newlong(mididev_master->unit));
	} else {
		exec_putacc(o, data_newnil());
	}
	return 1;
}

unsigned
user_func_devsendrt(struct exec_s *o) {
	long unit, sendrt;

	if (!exec_lookuplong(o, "unit", &unit) || 
	    !exec_lookupbool(o, "sendrt", &sendrt)) {
		return 0;
	}
	if (unit < 0 || unit >= DEFAULT_MAXNDEVS || !mididev_byunit[unit]) {
		user_printstr("no such device\n");
		return 0;		
	}
	mididev_byunit[unit]->sendrt = sendrt;
	return 1;
}


unsigned
user_func_devticrate(struct exec_s *o) {
	long unit, tpu;
	
	if (!exec_lookuplong(o, "unit", &unit) || 
	    !exec_lookuplong(o, "tics_per_unit", &tpu)) {
		return 0;
	}
	if (unit < 0 || unit >= DEFAULT_MAXNDEVS || !mididev_byunit[unit]) {
		user_printstr("no such device\n");
		return 0;		
	}
	if (tpu < DEFAULT_TPU || (tpu % DEFAULT_TPU)) {
		user_printstr("device tpu must be multiple of 96\n");
		return 0;
	}
	mididev_byunit[unit]->ticrate = tpu;
	return 1;
}


unsigned
user_func_devinfo(struct exec_s *o) {
	long unit;
	
	if (!exec_lookuplong(o, "unit", &unit)) {
		return 0;
	}
	if (unit < 0 || unit >= DEFAULT_MAXNDEVS || !mididev_byunit[unit]) {
		user_printstr("no such device\n");
		return 0;		
	}
	user_printstr("device = ");
	user_printlong(unit);
	if (mididev_master == mididev_byunit[unit]) {
		user_printstr(", master");
	}
	user_printstr(", tics_per_unit = ");
	user_printlong(mididev_byunit[unit]->ticrate);
	if (mididev_byunit[unit]->sendrt) {
		user_printstr(", sending real-time events");
	}
	user_printstr("\n");
	return 1;
}	


void
user_mainloop(void) {
	struct parse_s *parse;
	struct exec_s *exec;
	
	user_stdout = textout_new(0);
	user_song = song_new();
	exec = exec_new();

	exec_newbuiltin(exec, "ev", user_func_ev, 
			name_newarg("ev", 0));
	exec_newbuiltin(exec, "print", user_func_print, 
			name_newarg("value", 0));
	exec_newbuiltin(exec, "exec", user_func_exec, 
			name_newarg("filename", 0));
	exec_newbuiltin(exec, "debug", user_func_debug,
			name_newarg("flag", 
			name_newarg("value", 0)));
	exec_newbuiltin(exec, "panic", user_func_panic, 0);
	exec_newbuiltin(exec, "help", user_func_help, 0);
		
	exec_newbuiltin(exec, "tracklist", user_func_tracklist, 0);
	exec_newbuiltin(exec, "tracknew", user_func_tracknew, 
			name_newarg("trackname", 0));
	exec_newbuiltin(exec, "trackdelete", user_func_trackdelete, 
			name_newarg("trackname", 0));
	exec_newbuiltin(exec, "trackrename", user_func_trackrename,
			name_newarg("trackname",
			name_newarg("newname", 0)));
	exec_newbuiltin(exec, "trackexists", user_func_trackexists, 
			name_newarg("trackname", 0));
	exec_newbuiltin(exec, "trackaddev", user_func_trackaddev,
			name_newarg("trackname", 
			name_newarg("measure", 
			name_newarg("beat", 
			name_newarg("tic", 
			name_newarg("event", 0))))));
	exec_newbuiltin(exec, "tracksetcurfilt", user_func_tracksetcurfilt, 
			name_newarg("trackname", 
			name_newarg("filtname", 0)));
	exec_newbuiltin(exec, "trackgetcurfilt", user_func_trackgetcurfilt,
			name_newarg("trackname", 0));
	exec_newbuiltin(exec, "trackcheck", user_func_trackcheck,
			name_newarg("trackname", 0));
	exec_newbuiltin(exec, "trackgetlen", user_func_trackgetlen, 
			name_newarg("trackname", 0));
	exec_newbuiltin(exec, "tracksave", user_func_tracksave, 
			name_newarg("trackname", 
			name_newarg("filename", 0)));
	exec_newbuiltin(exec, "trackload", user_func_trackload, 
			name_newarg("trackname", 
			name_newarg("filename", 0)));
	exec_newbuiltin(exec, "trackcut", user_func_trackcut,
			name_newarg("trackname", 
			name_newarg("from", 
			name_newarg("amount", 
			name_newarg("quantum", 0)))));
	exec_newbuiltin(exec, "trackblank", user_func_trackblank,
			name_newarg("trackname", 
			name_newarg("from", 
			name_newarg("amount", 
			name_newarg("quantum", 
			name_newarg("evspec", 0))))));
	exec_newbuiltin(exec, "trackcopy", user_func_trackcopy,
			name_newarg("trackname", 
			name_newarg("from", 
			name_newarg("amount", 
			name_newarg("trackname2", 
			name_newarg("where", 
			name_newarg("quantum", 
			name_newarg("evspec",  0))))))));
	exec_newbuiltin(exec, "trackinsert", user_func_trackinsert,
			name_newarg("trackname", 
			name_newarg("from", 
			name_newarg("amount", 
			name_newarg("quantum", 0)))));
	exec_newbuiltin(exec, "trackquant", user_func_trackquant, 
			name_newarg("trackname", 
			name_newarg("from", 
			name_newarg("amount", 
			name_newarg("rate", 
			name_newarg("quantum", 0))))));

	exec_newbuiltin(exec, "chanlist", user_func_chanlist, 0);
	exec_newbuiltin(exec, "channew", user_func_channew, 
			name_newarg("channame",
			name_newarg("channum", 0)));
	exec_newbuiltin(exec, "chandelete", user_func_chandelete, 
			name_newarg("channame", 0));
	exec_newbuiltin(exec, "chanrename", user_func_chanrename,
			name_newarg("channame",
			name_newarg("newname", 0)));
	exec_newbuiltin(exec, "chanexists", user_func_chanexists, 
			name_newarg("channame", 0));
	exec_newbuiltin(exec, "chaninfo", user_func_chaninfo, 
			name_newarg("channame", 0));
	exec_newbuiltin(exec, "changetch", user_func_changetch,
			name_newarg("channame", 0));
	exec_newbuiltin(exec, "changetdev", user_func_changetdev,
			name_newarg("channame", 0));
	exec_newbuiltin(exec, "chanconfev", user_func_chanconfev,
			name_newarg("channame",
			name_newarg("event", 0)));

	exec_newbuiltin(exec, "filtlist", user_func_filtlist, 0);
	exec_newbuiltin(exec, "filtnew", user_func_filtnew, 
			name_newarg("filtname", 0));
	exec_newbuiltin(exec, "filtdelete", user_func_filtdelete, 
			name_newarg("filtname", 0));
	exec_newbuiltin(exec, "filtrename", user_func_filtrename,
			name_newarg("filtname",
			name_newarg("newname", 0)));
	exec_newbuiltin(exec, "filtinfo", user_func_filtinfo, 
			name_newarg("filtname", 0));
	exec_newbuiltin(exec, "filtexists", user_func_filtexists, 
			name_newarg("filtname", 0));
	exec_newbuiltin(exec, "filtreset", user_func_filtreset, 
			name_newarg("filtname", 0));
	exec_newbuiltin(exec, "filtdevdrop", user_func_filtdevdrop,
			name_newarg("filtname",
			name_newarg("indev", 0)));
	exec_newbuiltin(exec, "filtnodevdrop", user_func_filtnodevdrop,
			name_newarg("filtname",
			name_newarg("indev", 0)));
	exec_newbuiltin(exec, "filtdevmap", user_func_filtdevmap,
			name_newarg("filtname",
			name_newarg("indev", 
			name_newarg("outdev", 0))));
	exec_newbuiltin(exec, "filtnodevmap", user_func_filtnodevmap,
			name_newarg("filtname",
			name_newarg("outdev", 0)));
	exec_newbuiltin(exec, "filtchandrop", user_func_filtchandrop,
			name_newarg("filtname",
			name_newarg("inchan", 0)));
	exec_newbuiltin(exec, "filtnochandrop", user_func_filtnochandrop,
			name_newarg("filtname",
			name_newarg("inchan", 0)));
	exec_newbuiltin(exec, "filtchanmap", user_func_filtchanmap,
			name_newarg("filtname",
			name_newarg("inchan", 
			name_newarg("outchan", 0))));
	exec_newbuiltin(exec, "filtnochanmap", user_func_filtnochanmap,
			name_newarg("filtname",
			name_newarg("outchan", 0)));
	exec_newbuiltin(exec, "filtkeydrop", user_func_filtkeydrop,
			name_newarg("filtname",
			name_newarg("inchan", 
			name_newarg("keystart", 
			name_newarg("keyend", 0)))));
	exec_newbuiltin(exec, "filtnokeydrop", user_func_filtnokeydrop,
			name_newarg("filtname",
			name_newarg("inchan", 
			name_newarg("keystart", 
			name_newarg("keyend", 0)))));
	exec_newbuiltin(exec, "filtkeymap", user_func_filtkeymap,
			name_newarg("filtname",
			name_newarg("inchan", 
			name_newarg("outchan", 
			name_newarg("keystart", 
			name_newarg("keyend", 
			name_newarg("keyplus", 0)))))));
	exec_newbuiltin(exec, "filtnokeymap", user_func_filtnokeymap,
			name_newarg("filtname",
			name_newarg("outchan", 
			name_newarg("keystart", 
			name_newarg("keyend", 0)))));
	exec_newbuiltin(exec, "filtctldrop", user_func_filtctldrop,
			name_newarg("filtname",
			name_newarg("inchan", 
			name_newarg("inctl", 0))));
	exec_newbuiltin(exec, "filtnoctldrop", user_func_filtnoctldrop,
			name_newarg("filtname",
			name_newarg("inchan", 
			name_newarg("inctl", 0))));
	exec_newbuiltin(exec, "filtctlmap", user_func_filtctlmap,
			name_newarg("filtname",
			name_newarg("inchan", 
			name_newarg("outchan", 
			name_newarg("inctl", 
			name_newarg("outctl", 0))))));
	exec_newbuiltin(exec, "filtnoctlmap", user_func_filtnoctlmap,
			name_newarg("filtname",
			name_newarg("outchan", 
			name_newarg("outctl", 0))));
	exec_newbuiltin(exec, "filtswapichan", user_func_filtswapichan,
			name_newarg("filtname",
			name_newarg("oldchan", 
			name_newarg("newchan", 0))));
	exec_newbuiltin(exec, "filtswapidev", user_func_filtswapidev,
			name_newarg("filtname",
			name_newarg("olddev", 
			name_newarg("newdev", 0))));

	exec_newbuiltin(exec, "songgetunit", user_func_songgetunit, 0);
	exec_newbuiltin(exec, "songsetunit", user_func_songsetunit, 
			name_newarg("tics_per_unit", 0));
	exec_newbuiltin(exec, "songgetcurpos", user_func_songgetcurpos, 0);
	exec_newbuiltin(exec, "songsetcurpos", user_func_songsetcurpos, 
			name_newarg("measure", 0));
	exec_newbuiltin(exec, "songgetcurquant", user_func_songgetcurquant, 0);
	exec_newbuiltin(exec, "songsetcurquant", user_func_songsetcurquant, 
			name_newarg("quantum", 0));
	exec_newbuiltin(exec, "songgetcurtrack", user_func_songgetcurtrack, 0);
	exec_newbuiltin(exec, "songsetcurtrack", user_func_songsetcurtrack, 
			name_newarg("trackname", 0));
	exec_newbuiltin(exec, "songgetcurfilt", user_func_songgetcurfilt, 0);
	exec_newbuiltin(exec, "songsetcurfilt", user_func_songsetcurfilt, 
			name_newarg("filtname", 0));
	exec_newbuiltin(exec, "songinfo", user_func_songinfo, 0);
	exec_newbuiltin(exec, "songsave", user_func_songsave, 
			name_newarg("filename", 0));
	exec_newbuiltin(exec, "songload", user_func_songload, 
			name_newarg("filename", 0));
	exec_newbuiltin(exec, "songreset", user_func_songreset, 0);
	exec_newbuiltin(exec, "songexportsmf", user_func_songexportsmf, 
			name_newarg("filename", 0));
	exec_newbuiltin(exec, "songimportsmf", user_func_songimportsmf, 
			name_newarg("filename", 0));
	exec_newbuiltin(exec, "songidle", user_func_songidle, 0);
	exec_newbuiltin(exec, "songplay", user_func_songplay, 0);
	exec_newbuiltin(exec, "songrecord", user_func_songrecord, 0);
	exec_newbuiltin(exec, "songsetunit", user_func_songsetunit, 0);
	exec_newbuiltin(exec, "songsettempo", user_func_songsettempo, 
			name_newarg("measure", 
			name_newarg("beats_per_minute", 0)));
	exec_newbuiltin(exec, "songtimeins", user_func_songtimeins, 
			name_newarg("from", 
			name_newarg("amount", 
			name_newarg("numerator", 
			name_newarg("denominator", 0)))));
	exec_newbuiltin(exec, "songtimerm", user_func_songtimerm, 
			name_newarg("from", 
			name_newarg("amount", 0)));
	exec_newbuiltin(exec, "songtimeinfo", user_func_songtimeinfo, 0);

	exec_newbuiltin(exec, "metroswitch", user_func_metroswitch, 
			name_newarg("onoff", 0));
	exec_newbuiltin(exec, "metroconf", user_func_metroconf, 
			name_newarg("eventhi",
			name_newarg("eventlo", 0)));
	exec_newbuiltin(exec, "shut", user_func_shut, 0);
	exec_newbuiltin(exec, "sendraw", user_func_sendraw, 
			name_newarg("device", 
			name_newarg("list", 0)));

	exec_newbuiltin(exec, "devattach", user_func_devattach,
			name_newarg("unit", 
			name_newarg("path", 0)));
	exec_newbuiltin(exec, "devdetach", user_func_devdetach,
			name_newarg("unit", 0));
	exec_newbuiltin(exec, "devlist", user_func_devlist, 0);
	exec_newbuiltin(exec, "devsetmaster", user_func_devsetmaster,
			name_newarg("unit", 0));
	exec_newbuiltin(exec, "devgetmaster", user_func_devgetmaster, 0);
	exec_newbuiltin(exec, "devsendrt", user_func_devsendrt,
			name_newarg("unit", 
			name_newarg("sendrt", 0)));
	exec_newbuiltin(exec, "devticrate", user_func_devticrate,
			name_newarg("unit", 
			name_newarg("tics_per_unit", 0)));
	exec_newbuiltin(exec, "devinfo", user_func_devinfo,
			name_newarg("unit", 0));

	if (!user_flag_norc) {
		user_parsefile(exec, user_rcname());	/* parse rc file */
	}

	parse = parse_new(0);
	if (parse == 0) {
		return;
	}
	while (!parse_prog(parse, exec)) {
		/* nothing */
	}

	parse_delete(parse);
	exec_delete(exec);
	song_delete(user_song);
	user_song = 0;
	
	textout_delete(user_stdout);
}

