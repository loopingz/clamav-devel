/*
 *  Copyright (C) 2007-2009 Sourcefire, Inc.
 *
 *  Authors: Tomasz Kojm
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <assert.h>
#include "clamav.h"
#include "memory.h"
#include "others.h"
#include "cltypes.h"
#include "matcher.h"
#include "matcher-bm.h"
#include "filetypes.h"

#include "mpool.h"

#define BM_MIN_LENGTH	3
#define BM_BLOCK_SIZE	3
#define HASH(a,b,c) (211 * a + 37 * b + c)

int cli_bm_addpatt(struct cli_matcher *root, struct cli_bm_patt *pattern, const char *offset)
{
	uint16_t idx, i;
	const unsigned char *pt = pattern->pattern;
	struct cli_bm_patt *prev, *next = NULL;
	int ret;


    if(pattern->length < BM_MIN_LENGTH) {
	cli_errmsg("cli_bm_addpatt: Signature for %s is too short\n", pattern->virname);
	return CL_EMALFDB;
    }

    if((ret = cli_caloff(offset, NULL, NULL, root->type, pattern->offdata, &pattern->offset_min, &pattern->offset_max))) {
	cli_errmsg("cli_bm_addpatt: Can't calculate offset for signature %s\n", pattern->virname);
	return ret;
    }
    if(pattern->offdata[0] != CLI_OFF_ANY) {
	if(pattern->offdata[0] == CLI_OFF_ABSOLUTE)
	    root->bm_absoff_num++;
	else
	    root->bm_reloff_num++;
    }

#if BM_MIN_LENGTH == BM_BLOCK_SIZE
    /* try to load balance bm_suffix (at the cost of bm_shift) */
    for(i = 0; i < pattern->length - BM_BLOCK_SIZE + 1; i++) {
	idx = HASH(pt[i], pt[i + 1], pt[i + 2]);
	if(!root->bm_suffix[idx]) {
	    if(i) {
		pattern->prefix = pattern->pattern;
		pattern->prefix_length = i;
		pattern->pattern = &pattern->pattern[i];
		pattern->length -= i;
		pt = pattern->pattern;
	    }
	    break;
	}
    }
#endif

    for(i = 0; i <= BM_MIN_LENGTH - BM_BLOCK_SIZE; i++) {
	idx = HASH(pt[i], pt[i + 1], pt[i + 2]);
	root->bm_shift[idx] = MIN(root->bm_shift[idx], BM_MIN_LENGTH - BM_BLOCK_SIZE - i);
    }

    prev = next = root->bm_suffix[idx];
    while(next) {
	if(pt[0] >= next->pattern0)
	    break;
	prev = next;
	next = next->next;
    }

    if(next == root->bm_suffix[idx]) {
	pattern->next = root->bm_suffix[idx];
	if(root->bm_suffix[idx])
	    pattern->cnt = root->bm_suffix[idx]->cnt;
	root->bm_suffix[idx] = pattern;
    } else {
	pattern->next = prev->next;
	prev->next = pattern;
    }
    pattern->pattern0 = pattern->pattern[0];
    root->bm_suffix[idx]->cnt++;

    if(root->bm_offmode) {
	root->bm_pattab = (struct cli_bm_patt **) mpool_realloc2(root->mempool, root->bm_pattab, (root->bm_patterns + 1) * sizeof(struct cli_bm_patt *));
	if(!root->bm_pattab) {
	    cli_errmsg("cli_bm_addpatt: Can't allocate memory for root->bm_pattab\n");
	    return CL_EMEM;
	}
	root->bm_pattab[root->bm_patterns] = pattern;
	if(pattern->offdata[0] != CLI_OFF_ABSOLUTE)
	    pattern->offset_min = root->bm_patterns;
    }

    root->bm_patterns++;
    return CL_SUCCESS;
}

int cli_bm_init(struct cli_matcher *root)
{
	uint16_t i, size = HASH(255, 255, 255) + 1;
#ifdef USE_MPOOL
    assert (root->mempool && "mempool must be initialized");
#endif

    if(!(root->bm_shift = (uint8_t *) mpool_calloc(root->mempool, size, sizeof(uint8_t))))
	return CL_EMEM;

    if(!(root->bm_suffix = (struct cli_bm_patt **) mpool_calloc(root->mempool, size, sizeof(struct cli_bm_patt *)))) {
	mpool_free(root->mempool, root->bm_shift);
	return CL_EMEM;
    }

    for(i = 0; i < size; i++)
	root->bm_shift[i] = BM_MIN_LENGTH - BM_BLOCK_SIZE + 1;

    return CL_SUCCESS;
}

static int qcompare(const void *a, const void *b)
{
    return *(const uint32_t *)a - *(const uint32_t *)b;
}

int cli_bm_initoff(const struct cli_matcher *root, struct cli_bm_off *data, fmap_t *map)
{
	int ret;
	unsigned int i;
	struct cli_bm_patt *patt;
	struct cli_target_info info;


    if(!root->bm_patterns) {
	data->offtab = data->offset = 0;
	data->cnt = data->pos = 0;
	return CL_SUCCESS;
    }
    memset(&info, 0, sizeof(info));
    info.fsize = map->len;

    data->cnt = data->pos = 0;
    data->offtab = (uint32_t *) cli_malloc(root->bm_patterns * sizeof(uint32_t));
    if(!data->offtab) {
	cli_errmsg("cli_bm_initoff: Can't allocate memory for data->offtab\n");
	return CL_EMEM;
    }
    data->offset = (uint32_t *) cli_malloc(root->bm_patterns * sizeof(uint32_t));
    if(!data->offset) {
	cli_errmsg("cli_bm_initoff: Can't allocate memory for data->offset\n");
	free(data->offtab);
	return CL_EMEM;
    }
    for(i = 0; i < root->bm_patterns; i++) {
	patt = root->bm_pattab[i];
	if(patt->offdata[0] == CLI_OFF_ABSOLUTE) {
	    data->offtab[data->cnt] = patt->offset_min + patt->prefix_length;
	    data->cnt++;
	} else if((ret = cli_caloff(NULL, &info, map, root->type, patt->offdata, &data->offset[patt->offset_min], NULL))) {
	    cli_errmsg("cli_bm_initoff: Can't calculate relative offset in signature for %s\n", patt->virname);
	    if(info.exeinfo.section)
		free(info.exeinfo.section);
	    free(data->offtab);
	    free(data->offset);
	    return ret;
	} else if((data->offset[patt->offset_min] != CLI_OFF_NONE) && (data->offset[patt->offset_min] + patt->length <= info.fsize)) {
	    if(!data->cnt || (data->offset[patt->offset_min] != data->offtab[data->cnt - 1])) {
		data->offtab[data->cnt] = data->offset[patt->offset_min] + patt->prefix_length;
		data->cnt++;
	    }
	}
    }
    if(info.exeinfo.section)
	free(info.exeinfo.section);

    cli_qsort(data->offtab, data->cnt, sizeof(uint32_t), qcompare);
    return CL_SUCCESS;
}

void cli_bm_freeoff(struct cli_bm_off *data)
{
    free(data->offset);
    data->offset = NULL;
    free(data->offtab);
    data->offtab = NULL;
}

void cli_bm_free(struct cli_matcher *root)
{
	struct cli_bm_patt *patt, *prev;
	uint16_t i, size = HASH(255, 255, 255) + 1;


    if(root->bm_shift)
	mpool_free(root->mempool, root->bm_shift);

    if(root->bm_pattab)
	mpool_free(root->mempool, root->bm_pattab);

    if(root->bm_suffix) {
	for(i = 0; i < size; i++) {
	    patt = root->bm_suffix[i];
	    while(patt) {
		prev = patt;
		patt = patt->next;
		if(prev->prefix)
		    mpool_free(root->mempool, prev->prefix);
		else
		    mpool_free(root->mempool, prev->pattern);
		if(prev->virname)
		    mpool_free(root->mempool, prev->virname);
		mpool_free(root->mempool, prev);
	    }
	}
	mpool_free(root->mempool, root->bm_suffix);
    }
}

int cli_bm_scanbuff(const unsigned char *buffer, uint32_t length, const char **virname, const struct cli_matcher *root, uint32_t offset, fmap_t *map, struct cli_bm_off *offdata)
{
	uint32_t i, j, off, off_min, off_max;
	uint8_t found, pchain, shift;
	uint16_t idx, idxchk;
	struct cli_bm_patt *p;
	const unsigned char *bp, *pt;
	unsigned char prefix;
        struct cli_target_info info;
        int ret;

    if(!root || !root->bm_shift)
	return CL_CLEAN;

    if(length < BM_MIN_LENGTH)
	return CL_CLEAN;

    memset(&info, 0, sizeof(info));
    i = BM_MIN_LENGTH - BM_BLOCK_SIZE;
    if(offdata) {
	if(offdata->pos == offdata->cnt)
	    return CL_CLEAN;
	i += offdata->offtab[offdata->pos] - offset;
    }
    for(; i < length - BM_BLOCK_SIZE + 1; ) {
	idx = HASH(buffer[i], buffer[i + 1], buffer[i + 2]);
	shift = root->bm_shift[idx];

	if(shift == 0) {
	    prefix = buffer[i - BM_MIN_LENGTH + BM_BLOCK_SIZE];
	    p = root->bm_suffix[idx];
	    if(p && p->cnt == 1 && p->pattern0 != prefix) {
		if(offdata) {
		    off = offset + i - BM_MIN_LENGTH + BM_BLOCK_SIZE;
		    for(; offdata->pos < offdata->cnt && off >= offdata->offtab[offdata->pos]; offdata->pos++);
		    if(offdata->pos == offdata->cnt || off >= offdata->offtab[offdata->pos])
			return CL_CLEAN;
		    i += offdata->offtab[offdata->pos] - off;
		} else {
		    i++;
		}
		continue;
	    }
	    pchain = 0;
	    while(p) {
		if(p->pattern0 != prefix) {
		    if(pchain)
			break;
		    p = p->next;
		    continue;
		} else pchain = 1;

		off = i - BM_MIN_LENGTH + BM_BLOCK_SIZE;
		bp = buffer + off;

		if((off + p->length > length) || (p->prefix_length > off)) {
		    p = p->next;
		    continue;
		}

		if(offdata) {
		    if(p->offdata[0] == CLI_OFF_ABSOLUTE) {
			if(p->offset_min != offset + off - p->prefix_length) {
			    p = p->next;
			    continue;
			}
		    } else if((offdata->offset[p->offset_min] == CLI_OFF_NONE) || (offdata->offset[p->offset_min] != offset + off - p->prefix_length)) {
			p = p->next;
			continue;
		    }
		}

		idxchk = MIN(p->length, length - off) - 1;
		if(idxchk) {
		    if((bp[idxchk] != p->pattern[idxchk]) ||  (bp[idxchk / 2] != p->pattern[idxchk / 2])) {
			p = p->next;
			continue;
		    }
		}

		if(p->prefix_length) {
		    off -= p->prefix_length;
		    bp -= p->prefix_length;
		    pt = p->prefix;
		} else {
		    pt = p->pattern;
		}

		found = 1;
		for(j = 0; j < p->length + p->prefix_length && off < length; j++, off++) {
		    if(bp[j] != pt[j]) {
			found = 0;
			break;
		    }
		}

		if(found && p->length + p->prefix_length == j) {
		    if(!offdata && (p->offset_min != CLI_OFF_ANY)) {
			if(p->offdata[0] != CLI_OFF_ABSOLUTE) {
			    ret = cli_caloff(NULL, &info, map, root->type, p->offdata, &off_min, &off_max);
			    if(ret != CL_SUCCESS) {
				cli_errmsg("cli_bm_scanbuff: Can't calculate relative offset in signature for %s\n", p->virname);
				if(info.exeinfo.section)
				    free(info.exeinfo.section);
				return ret;
			    }
			} else {
			    off_min = p->offset_min;
			    off_max = p->offset_max;
			}
			off = offset + i - p->prefix_length - BM_MIN_LENGTH + BM_BLOCK_SIZE;
			if(off_max < off || off_min > off) {
			    p = p->next;
			    continue;
			}
		    }
		    if(virname)
			*virname = p->virname;
		    if(info.exeinfo.section)
			free(info.exeinfo.section);
		    return CL_VIRUS;
		}
		p = p->next;
	    }
	    shift = 1;
	}

	if(offdata) {
	    off = offset + i - BM_MIN_LENGTH + BM_BLOCK_SIZE;
	    for(; offdata->pos < offdata->cnt && off >= offdata->offtab[offdata->pos]; offdata->pos++);
	    if(offdata->pos == offdata->cnt || off >= offdata->offtab[offdata->pos])
		return CL_CLEAN;
	    i += offdata->offtab[offdata->pos] - off;
	} else {
	    i += shift;
	}

    }

    if(info.exeinfo.section)
	free(info.exeinfo.section);
    return CL_CLEAN;
}
