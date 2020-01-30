#ifdef DEDUP
#include "rl_json.h"

//#define _GNU_SOURCE

#ifndef ffsll
#define ffsll	ffsll_polyfill
static int ffsll_polyfill(long long x) //{{{
{
	int i=0;
	long long mask = 1;
	for(i=0; i<sizeof(long long)*8;++i, mask <<= 1) {
		if(x & mask) {
			return i+1;
		}
	}
	return 0;
}

//}}}
#endif

static int first_free(long long* freemap) //{{{
{
	int	i=0, bit, res;
	while ((bit = ffsll(freemap[i])) == 0) {
		i++;
	}
	res = i * (sizeof(long long)*8) + (bit-1);
	return res;
}

//}}}
static void mark_used(long long* freemap, int idx) //{{{
{
	int	i = idx / (sizeof(long long)*8);
	int bit = idx - (i * (sizeof(long long)*8));
	freemap[i] &= ~(1LL << bit);
}

//}}}
static void mark_free(long long* freemap, int idx) //{{{
{
	int	i = idx / (sizeof(long long)*8);
	int bit = idx - (i * (sizeof(long long)*8));
	freemap[i] |= 1LL << bit;
}

//}}}
void free_cache(struct interp_cx* l) //{{{
{
	Tcl_HashEntry*		he;
	Tcl_HashSearch		search;
	struct kc_entry*	e;

	he = Tcl_FirstHashEntry(&l->kc, &search);
	while (he) {
		ptrdiff_t	idx = (ptrdiff_t)Tcl_GetHashValue(he);

		//if (idx >= KC_ENTRIES) Tcl_Panic("age_cache: idx (%ld) is out of bounds, KC_ENTRIES: %d", idx, KC_ENTRIES);
		//printf("age_cache: kc_count: %d", l->kc_count);
		e = &l->kc_entries[idx];

		Tcl_DeleteHashEntry(he);
		Tcl_DecrRefCount(e->val);
		Tcl_DecrRefCount(e->val);	// Two references - one for the cache table and one on loan to callers' interim processing
		mark_free(l->freemap, idx);
		e->val = NULL;
		he = Tcl_NextHashEntry(&search);
	}
	l->kc_count = 0;
}

//}}}
static void age_cache(struct interp_cx* l) //{{{
{
	Tcl_HashEntry*		he;
	Tcl_HashSearch		search;
	struct kc_entry*	e;

	he = Tcl_FirstHashEntry(&l->kc, &search);
	while (he) {
		ptrdiff_t	idx = (ptrdiff_t)Tcl_GetHashValue(he);

		//if (idx >= KC_ENTRIES) Tcl_Panic("age_cache: idx (%ld) is out of bounds, KC_ENTRIES: %d", idx, KC_ENTRIES);
		//printf("age_cache: kc_count: %d", l->kc_count);
		e = &l->kc_entries[idx];

		if (e->hits < 1) {
			Tcl_DeleteHashEntry(he);
			Tcl_DecrRefCount(e->val);
			Tcl_DecrRefCount(e->val);	// Two references - one for the cache table and one on loan to callers' interim processing
			mark_free(l->freemap, idx);
			e->val = NULL;
		} else {
			e->hits >>= 1;
		}
		he = Tcl_NextHashEntry(&search);
	}
	l->kc_count = 0;
}

//}}}
Tcl_Obj* new_stringobj_dedup(struct interp_cx* l, const char* bytes, int length) //{{{
{
	char				buf[STRING_DEDUP_MAX + 1];
	const char			*keyname;
	int					is_new;
	struct kc_entry*	kce;
	Tcl_Obj*			out;
	Tcl_HashEntry*		entry = NULL;

	if (length == 0) {
		return l->tcl_empty;
	} else if (length < 0) {
		length = strlen(bytes);
	}

	if (length > STRING_DEDUP_MAX || l == NULL)
		return Tcl_NewStringObj(bytes, length);

	if (likely(bytes[length] == 0)) {
		keyname = bytes;
	} else {
		memcpy(buf, bytes, length);
		buf[length] = 0;
		keyname = buf;
	}
	entry = Tcl_CreateHashEntry(&l->kc, keyname, &is_new);

	if (is_new) {
		ptrdiff_t	idx = first_free(l->freemap);

		if (unlikely(idx >= KC_ENTRIES)) {
			// Cache overflow
			Tcl_DeleteHashEntry(entry);
			age_cache(l);
			return Tcl_NewStringObj(bytes, length);
		}

		kce = &l->kc_entries[idx];
		kce->hits = 0;
		out = kce->val = Tcl_NewStringObj(bytes, length);
		Tcl_IncrRefCount(out);	// Two references - one for the cache table and one on loan to callers' interim processing.
		Tcl_IncrRefCount(out);	// Without this, values not referenced elsewhere could reach callers with refCount 1, allowing
								// the value to be mutated in place and corrupt the state of the cache (hash key not matching obj value)

		mark_used(l->freemap, idx);

		Tcl_SetHashValue(entry, (void*)idx);
		l->kc_count++;

		if (unlikely(l->kc_count > (int)(KC_ENTRIES/2.5))) {
			kce->hits++; // Prevent the just-created entry from being pruned
			age_cache(l);
		}
	} else {
		ptrdiff_t	idx = (ptrdiff_t)Tcl_GetHashValue(entry);

		kce = &l->kc_entries[idx];
		out = kce->val;
		if (kce->hits < 255) kce->hits++;
	}

	return out;
}

//}}}

#endif
