/*****************************************************************************\
 *  eval_nodes.c - Determine order of nodes for job.
 *****************************************************************************
 *  Copyright (C) SchedMD LLC
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <math.h>

#include "eval_nodes.h"
#include "gres_filter.h"
#include "gres_sched.h"

#include "src/common/xstring.h"

typedef struct node_weight_struct {
	bitstr_t *node_bitmap;	/* bitmap of nodes with this weight */
	uint64_t weight;	/* priority of node for scheduling work on */
} node_weight_type;

/* Find node_weight_type element from list with same weight as node config */
static int _node_weight_find(void *x, void *key)
{
	node_weight_type *nwt = (node_weight_type *) x;
	node_record_t *node_ptr = (node_record_t *) key;
	if (nwt->weight == node_ptr->sched_weight)
		return 1;
	return 0;
}

/* Free node_weight_type element from list */
static void _node_weight_free(void *x)
{
	node_weight_type *nwt = (node_weight_type *) x;
	FREE_NULL_BITMAP(nwt->node_bitmap);
	xfree(nwt);
}

/* Sort list of node_weight_type reords in order of increasing node weight */
static int _node_weight_sort(void *x, void *y)
{
	node_weight_type *nwt1 = *(node_weight_type **) x;
	node_weight_type *nwt2 = *(node_weight_type **) y;
	if (nwt1->weight < nwt2->weight)
		return -1;
	if (nwt1->weight > nwt2->weight)
		return 1;
	return 0;
}

/*
 * Given a bitmap of available nodes, return a list of node_weight_type
 * records in order of increasing "weight" (priority)
 */
static List _build_node_weight_list(bitstr_t *node_bitmap)
{
	List node_list;
	node_record_t *node_ptr;
	node_weight_type *nwt;

	xassert(node_bitmap);
	/* Build list of node_weight_type records, one per node weight */
	node_list = list_create(_node_weight_free);
	for (int i = 0; (node_ptr = next_node_bitmap(node_bitmap, &i)); i++) {
		nwt = list_find_first(node_list, _node_weight_find, node_ptr);
		if (!nwt) {
			nwt = xmalloc(sizeof(node_weight_type));
			nwt->node_bitmap = bit_alloc(node_record_count);
			nwt->weight = node_ptr->sched_weight;
			list_append(node_list, nwt);
		}
		bit_set(nwt->node_bitmap, i);
	}

	/* Sort the list in order of increasing node weight */
	list_sort(node_list, _node_weight_sort);

	return node_list;
}

static int _eval_nodes_block(topology_eval_t *topo_eval)
{
	uint32_t *block_cpu_cnt = NULL;	/* total CPUs on block */
	List *block_gres = NULL;		/* available GRES on block */
	bitstr_t **block_node_bitmap = NULL;	/* nodes on this block */
	bitstr_t **bblock_node_bitmap = NULL;	/* nodes on this base block */
	int *block_node_cnt = NULL;	/* total nodes on block */
	int *nodes_on_bblock = NULL;	/* total nodes on nblock */
	bitstr_t *avail_nodes_bitmap = NULL;	/* nodes on any block */
	bitstr_t *req_nodes_bitmap = NULL;	/* required node bitmap */
	bitstr_t *req2_nodes_bitmap = NULL;	/* required+lowest prio nodes */
	bitstr_t *best_nodes_bitmap = NULL;	/* required+low prio nodes */
	bitstr_t *bblock_bitmap = NULL;
	int *bblock_block_inx = NULL;
	bool *bblock_required = NULL;
	int i, j, rc = SLURM_SUCCESS;
	int best_cpu_cnt, best_node_cnt, req_node_cnt = 0;
	List best_gres = NULL;
	block_record_t *block_ptr;
	List node_weight_list = NULL;
	topo_weight_info_t *nw = NULL;
	ListIterator iter;
	node_record_t *node_ptr;
	int64_t rem_max_cpus;
	int rem_cpus, rem_nodes; /* remaining resources desired */
	int min_rem_nodes;	/* remaining resources desired */
	job_record_t *job_ptr = topo_eval->job_ptr;
	job_details_t *details_ptr = job_ptr->details;
	bool gres_per_job, requested, sufficient = false;
	uint16_t *avail_cpu_per_node = NULL;
	int block_inx = -1;
	uint64_t block_lowest_weight = 0;
	int block_cnt = -1, bblock_per_block;
	int prev_rem_nodes;
	avail_res_t **avail_res_array = topo_eval->avail_res_array;
	uint32_t min_nodes = topo_eval->min_nodes;
	uint32_t req_nodes = topo_eval->req_nodes;

	topo_eval->avail_cpus = 0;

	rem_cpus = details_ptr->min_cpus;
	min_rem_nodes = min_nodes;

	/* Always use min_nodes */
	gres_per_job = gres_sched_init(job_ptr->gres_list_req);
	rem_nodes = MIN(min_nodes, req_nodes);

	rem_max_cpus = eval_nodes_get_rem_max_cpus(details_ptr, rem_nodes);

	bblock_per_block = ((rem_nodes + bblock_node_cnt - 1) /
			    bblock_node_cnt);
	bblock_per_block = ceil(log(bblock_per_block) / log(2)); //block level
	bblock_per_block = bit_ffs_from_bit(block_levels, bblock_per_block);

	/* Validate availability of required nodes */
	if (job_ptr->details->req_node_bitmap) {
		if (!bit_super_set(job_ptr->details->req_node_bitmap,
				   topo_eval->node_map)) {
			info("%pJ requires nodes which are not currently available",
			     job_ptr);
			rc = SLURM_ERROR;
			goto fini;
		}

		if (!bit_super_set(job_ptr->details->req_node_bitmap,
				   blocks_nodes_bitmap)) {
			info("%pJ requires nodes which are not in blocks",
			     job_ptr);
			rc = SLURM_ERROR;
			goto fini;
		}

		req_node_cnt = bit_set_count(job_ptr->details->req_node_bitmap);
		if (req_node_cnt == 0) {
			info("%pJ required node list has no nodes",
			     job_ptr);
			rc = SLURM_ERROR;
			goto fini;
		}
		if (req_node_cnt > topo_eval->max_nodes) {
			info("%pJ requires more nodes than currently available (%u>%u)",
			     job_ptr, req_node_cnt,
			     topo_eval->max_nodes);
			rc = SLURM_ERROR;
			goto fini;
		}
		req_nodes_bitmap = job_ptr->details->req_node_bitmap;
	}

	/*
	 * Add required nodes to job allocation and
	 * build list of node bitmaps, sorted by weight
	 */
	if (!bit_set_count(topo_eval->node_map)) {
		debug("%pJ node_map is empty",
		      job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}
	avail_cpu_per_node = xcalloc(node_record_count, sizeof(uint16_t));
	node_weight_list = list_create(eval_nodes_topo_weight_free);
	for (i = 0;
	     (node_ptr = next_node_bitmap(topo_eval->node_map, &i));
	     i++) {
		topo_weight_info_t nw_static;
		if (req_nodes_bitmap && bit_test(req_nodes_bitmap, i)) {
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			if (topo_eval->avail_cpus == 0) {
				debug2("%pJ insufficient resources on required node",
				       job_ptr);
				rc = SLURM_ERROR;
				goto fini;
			}
			avail_cpu_per_node[i] = topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			rem_cpus -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
		}

		nw_static.weight = node_ptr->sched_weight;
		nw = list_find_first(node_weight_list,
				     eval_nodes_topo_weight_find,
				     &nw_static);
		if (!nw) {	/* New node weight to add */
			nw = xmalloc(sizeof(topo_weight_info_t));
			nw->node_bitmap = bit_alloc(node_record_count);
			nw->weight = node_ptr->sched_weight;
			list_append(node_weight_list, nw);
		}
		bit_set(nw->node_bitmap, i);
		nw->node_cnt++;
	}

	list_sort(node_weight_list, eval_nodes_topo_weight_sort);
	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE)
		(void) list_for_each(node_weight_list,
				     eval_nodes_topo_weight_log, NULL);

	if (bblock_per_block < 0) {
		/* Number of base blocks in block */
		bblock_per_block = block_record_cnt;
		block_cnt = 1;
	} else {
		/* Number of base blocks in block */
		bblock_per_block = pow(2, bblock_per_block);
		block_cnt = (block_record_cnt + bblock_per_block - 1) /
			bblock_per_block;
	}

	log_flag(SELECT_TYPE, "%s: bblock_per_block:%u rem_nodes:%u ",
		 __func__, bblock_per_block, rem_nodes);

	block_cpu_cnt = xcalloc(block_cnt, sizeof(uint32_t));
	block_gres = xcalloc(block_cnt, sizeof(List));
	block_node_bitmap = xcalloc(block_cnt, sizeof(bitstr_t *));
	block_node_cnt = xcalloc(block_cnt, sizeof(int));
	bblock_required = xcalloc(block_record_cnt, sizeof(bool));
	bblock_block_inx = xcalloc(block_record_cnt, sizeof(int));

	for (i = 0, block_ptr = block_record_table; i < block_record_cnt;
	     i++, block_ptr++) {
		int block_inx = i / bblock_per_block;
		if (block_node_bitmap[block_inx])
			bit_or(block_node_bitmap[block_inx],
			       block_ptr->node_bitmap);
		else
			block_node_bitmap[block_inx] =
				bit_copy(block_ptr->node_bitmap);
		bblock_block_inx[i] = block_inx;
	}

	for (i = 0; i < block_cnt; i++) {
		uint32_t block_cpus = 0;
		bit_and(block_node_bitmap[i], topo_eval->node_map);
		block_node_cnt[i] = bit_set_count(block_node_bitmap[i]);
		/*
		 * Count total CPUs of the intersection of node_map and
		 * block_node_bitmap.
		 */
		for (j = 0; (node_ptr = next_node_bitmap(block_node_bitmap[i],
							 &j));
		     j++)
			block_cpus += avail_res_array[j]->avail_cpus;
		block_cpu_cnt[i] = block_cpus;
		if (req_nodes_bitmap &&
		    bit_overlap_any(req_nodes_bitmap, block_node_bitmap[i])) {
			if (block_inx == -1) {
				block_inx = i;
				break;
			}
		}
		if (!eval_nodes_enough_nodes(block_node_cnt[i], rem_nodes,
					     min_nodes, req_nodes) ||
		    (rem_cpus > block_cpu_cnt[i]))
			continue;
		if (!req_nodes_bitmap &&
		    (nw = list_find_first(node_weight_list, eval_nodes_topo_node_find,
					  block_node_bitmap[i]))) {
			if ((block_inx == -1) ||
			    (nw->weight < block_lowest_weight) ||
			    ((nw->weight == block_lowest_weight) &&
			     (block_node_cnt[i] <=
			      block_node_cnt[block_inx]))) {
				block_inx = i;
				block_lowest_weight = nw->weight;
			}
		}
	}

	if (!req_nodes_bitmap) {
		bit_clear_all(topo_eval->node_map);
	}

	if (block_inx == -1) {
		log_flag(SELECT_TYPE, "%pJ unable to find block",
			 job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}

	/* Check that all specificly required nodes are in one block  */
	if (req_nodes_bitmap &&
	    !bit_super_set(req_nodes_bitmap, block_node_bitmap[block_inx])) {
		rc = SLURM_ERROR;
		info("%pJ requires nodes that do not have shared block",
		     job_ptr);
		goto fini;
	}

	if (req_nodes_bitmap) {
		bit_and(topo_eval->node_map, req_nodes_bitmap);
		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
			/* Required nodes completely satisfied the request */
			rc = SLURM_SUCCESS;
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			rc = SLURM_ERROR;
			info("%pJ requires nodes exceed maximum node limit",
			     job_ptr);
			goto fini;
		}

		for (i = 0; i < block_record_cnt; i++) {
			if (block_inx != bblock_block_inx[i])
				continue;
			if (bit_overlap_any(
				    req_nodes_bitmap,
				    block_record_table[i].node_bitmap)) {
				bblock_required[i] = true;
			}
		}

	}

	requested = false;
	best_node_cnt = 0;
	best_cpu_cnt = 0;
	best_nodes_bitmap = bit_alloc(node_record_count);
	iter = list_iterator_create(node_weight_list);
	while (!requested && (nw = list_next(iter))) {
		if (best_node_cnt > 0) {
			/*
			 * All of the lower priority nodes should be included
			 * in the job's allocation. Nodes from the next highest
			 * weight nodes are included only as needed.
			 */
			if (req2_nodes_bitmap)
				bit_or(req2_nodes_bitmap, best_nodes_bitmap);
			else
				req2_nodes_bitmap = bit_copy(best_nodes_bitmap);
		}

		if (!bit_set_count(nw->node_bitmap))
			continue;

		for (i = 0; (node_ptr = next_node_bitmap(nw->node_bitmap, &i));
		     i++) {
			if (req_nodes_bitmap && bit_test(req_nodes_bitmap, i))
				continue;	/* Required node */
			if (!bit_test(block_node_bitmap[block_inx], i))
				continue;
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			if (topo_eval->avail_cpus == 0) {
				bit_clear(nw->node_bitmap, i);
				continue;
			}
			bit_set(best_nodes_bitmap, i);
			avail_cpu_per_node[i] = topo_eval->avail_cpus;
			best_cpu_cnt += topo_eval->avail_cpus;
			best_node_cnt++;
			if (gres_per_job) {
				gres_sched_consec(
					&best_gres, job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list);
			}
		}

		if (!sufficient) {
			sufficient = (best_cpu_cnt >= rem_cpus) &&
				eval_nodes_enough_nodes(
					best_node_cnt, rem_nodes,
					min_nodes, req_nodes);
			if (sufficient && gres_per_job) {
				sufficient = gres_sched_sufficient(
					job_ptr->gres_list_req,
					best_gres);
			}
		}
		requested = ((best_node_cnt >= rem_nodes) &&
			     (best_cpu_cnt >= rem_cpus) &&
			     (!gres_per_job ||
			      gres_sched_sufficient(job_ptr->gres_list_req,
						    best_gres)));

	}
	list_iterator_destroy(iter);

	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		char *gres_str = NULL, *gres_print = "";
		char *node_names;
		if (req_nodes_bitmap) {
			node_names = bitmap2node_name(req_nodes_bitmap);
			info("Required nodes:%s", node_names);
			xfree(node_names);
		}
		node_names = bitmap2node_name(best_nodes_bitmap);
		if (gres_per_job) {
			gres_str = gres_sched_str(best_gres);
			if (gres_str)
				gres_print = gres_str;
		}
		info("Best nodes:%s node_cnt:%d cpu_cnt:%d %s",
		     node_names, best_node_cnt, best_cpu_cnt, gres_print);
		xfree(node_names);
		xfree(gres_str);
	}
	if (!sufficient) {
		log_flag(SELECT_TYPE, "insufficient resources currently available for %pJ",
			 job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}

	/*
	 * Add lowest weight nodes. Treat similar to required nodes for the job.
	 * Job will still need to add some higher weight nodes later.
	 */
	if (req2_nodes_bitmap) {
		for (i = 0;
		     (next_node_bitmap(req2_nodes_bitmap, &i) &&
		      (topo_eval->max_nodes > 0));
		     i++) {
			topo_eval->avail_cpus = avail_cpu_per_node[i];
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			rem_cpus -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
		}

		bit_or(topo_eval->node_map, req2_nodes_bitmap);

		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    (!gres_per_job || gres_sched_test(job_ptr->gres_list_req,
						      job_ptr->job_id))) {
			/* Required nodes completely satisfied the request */
			error("Scheduling anomaly for %pJ",
			      job_ptr);
			rc = SLURM_SUCCESS;
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			rc = SLURM_ERROR;
			debug("%pJ reached maximum node limit",
			      job_ptr);
			goto fini;
		}
		for (i = 0; i < block_record_cnt; i++) {
			if (block_inx != bblock_block_inx[i])
				continue;
			if (bblock_required[i])
				continue;
			if (bit_overlap_any(
				    req2_nodes_bitmap,
				    block_record_table[i].node_bitmap)) {
				bblock_required[i] = true;
			}
		}
	}

	/* Add additional resources for already required base block */
	if (req_nodes_bitmap || req2_nodes_bitmap) {
		for (i = 0; i < block_record_cnt; i++) {
			if (!bblock_required[i])
				continue;
			if (!bblock_bitmap)
				bblock_bitmap = bit_copy(
					block_record_table[i].node_bitmap);
			else
				bit_copybits(bblock_bitmap,
					     block_record_table[i].node_bitmap);

			bit_and(bblock_bitmap, block_node_bitmap[block_inx]);
			bit_and(bblock_bitmap, best_nodes_bitmap);
			bit_and_not(bblock_bitmap, topo_eval->node_map);

			for (j = 0; next_node_bitmap(bblock_bitmap, &j); j++) {
				if (!avail_cpu_per_node[j])
					continue;
				topo_eval->avail_cpus = avail_cpu_per_node[j];
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
				if (gres_per_job) {
					gres_sched_add(
						job_ptr->gres_list_req,
						avail_res_array[j]->
						sock_gres_list,
						&topo_eval->avail_cpus);
				}
				rem_nodes--;
				min_rem_nodes--;
				topo_eval->max_nodes--;
				rem_cpus -= topo_eval->avail_cpus;
				rem_max_cpus -= topo_eval->avail_cpus;
				bit_set(topo_eval->node_map, j);
				if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
				    (!gres_per_job ||
				     gres_sched_test(job_ptr->gres_list_req,
						     job_ptr->job_id))) {
					rc = SLURM_SUCCESS;
					goto fini;
				}
			}
		}
	}

	nodes_on_bblock = xcalloc(block_record_cnt, sizeof(int));
	bblock_node_bitmap = xcalloc(block_record_cnt, sizeof(bitstr_t *));
	for (i = 0; i < block_record_cnt; i++) {
		if (block_inx != bblock_block_inx[i])
			continue;
		if (bblock_required[i])
			continue;
		bblock_node_bitmap[i] =
			bit_copy(block_record_table[i].node_bitmap);
		bit_and(bblock_node_bitmap[i], block_node_bitmap[block_inx]);
		bit_and(bblock_node_bitmap[i], best_nodes_bitmap);
		nodes_on_bblock[i] = bit_set_count(bblock_node_bitmap[i]);
	}

	prev_rem_nodes = rem_nodes + 1;
	while (1) {
		int best_bblock_inx = -1;
		bool best_fit, fit;
		bitstr_t *best_bblock_bitmap = NULL;
		if (prev_rem_nodes == rem_nodes)
			break; 	/* Stalled */
		prev_rem_nodes = rem_nodes;
		for (i = 0; i < block_record_cnt; i++) {
			if (block_inx != bblock_block_inx[i])
				continue;
			if (bblock_required[i])
				continue;
			fit = (nodes_on_bblock[i] >= rem_nodes);

			if (best_bblock_inx == -1 ||
			    (fit && !best_fit) ||
			    (!fit && !best_fit &&
			     (nodes_on_bblock[i] >
			      nodes_on_bblock[best_bblock_inx])) ||
			    (fit && (nodes_on_bblock[i] <=
				     nodes_on_bblock[best_bblock_inx]))) {
				best_bblock_inx = i;
				best_fit = fit;
			}
		}
		log_flag(SELECT_TYPE, "%s: rem_nodes:%d  best_bblock_inx:%d",
			 __func__, rem_nodes, best_bblock_inx);
		if (best_bblock_inx == -1)
			break;

		best_bblock_bitmap = bblock_node_bitmap[best_bblock_inx];
		bit_and_not(best_bblock_bitmap, topo_eval->node_map);
		bblock_required[best_bblock_inx] = true;
		/*
		 * NOTE: Ideally we would add nodes in order of resource
		 * availability rather than in order of bitmap position, but
		 * that would add even more complexity and overhead.
		 */
		for (i = 0; next_node_bitmap(best_bblock_bitmap, &i) &&
			     (topo_eval->max_nodes > 0); i++) {
			if (!avail_cpu_per_node[i])
				continue;
			topo_eval->avail_cpus = avail_cpu_per_node[i];
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus,
					       min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			rem_cpus -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			bit_set(topo_eval->node_map, i);
			if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
			    (!gres_per_job ||
			     gres_sched_test(job_ptr->gres_list_req,
					     job_ptr->job_id))) {
				rc = SLURM_SUCCESS;
				goto fini;
			}
		}
	}

	if ((min_rem_nodes <= 0) && (rem_cpus <= 0) &&
	    (!gres_per_job ||
	     gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id))) {
		rc = SLURM_SUCCESS;
		goto fini;
	}
	rc = SLURM_ERROR;

fini:
	FREE_NULL_LIST(best_gres);
	FREE_NULL_LIST(node_weight_list);
	FREE_NULL_BITMAP(avail_nodes_bitmap);
	FREE_NULL_BITMAP(req2_nodes_bitmap);
	FREE_NULL_BITMAP(best_nodes_bitmap);
	FREE_NULL_BITMAP(bblock_bitmap);
	xfree(avail_cpu_per_node);
	xfree(block_cpu_cnt);
	xfree(block_gres);
	xfree(bblock_block_inx);
	if (block_node_bitmap) {
		for (i = 0; i < block_cnt; i++)
			FREE_NULL_BITMAP(block_node_bitmap[i]);
		xfree(block_node_bitmap);
	}
	if (bblock_node_bitmap) {
		for (i = 0; i < block_record_cnt; i++)
			FREE_NULL_BITMAP(bblock_node_bitmap[i]);
		xfree(bblock_node_bitmap);
	}
	xfree(block_node_cnt);
	xfree(nodes_on_bblock);
	xfree(bblock_required);
	return rc;
}

/*
 * A variation of _eval_nodes() to select resources using busy nodes first.
 */
static int _eval_nodes_busy(topology_eval_t *topo_eval)
{
	int i, i_start, i_end, error_code = SLURM_ERROR;
	int idle_test;
	int rem_cpus, rem_nodes; /* remaining resources desired */
	int min_rem_nodes;	/* remaining resources desired */
	int total_cpus = 0;	/* #CPUs allocated to job */
	int64_t rem_max_cpus;
	job_record_t *job_ptr = topo_eval->job_ptr;
	job_details_t *details_ptr = job_ptr->details;
	bitstr_t *req_map = details_ptr->req_node_bitmap;
	bitstr_t *orig_node_map = bit_copy(topo_eval->node_map);
	avail_res_t **avail_res_array = topo_eval->avail_res_array;
	uint32_t min_nodes = topo_eval->min_nodes;
	uint32_t req_nodes = topo_eval->req_nodes;
	bool all_done = false, gres_per_job;
	node_record_t *node_ptr;
	List node_weight_list = NULL;
	node_weight_type *nwt;
	ListIterator iter;

	topo_eval->avail_cpus = 0;

	rem_cpus = details_ptr->min_cpus;
	min_rem_nodes = min_nodes;
	if ((details_ptr->num_tasks != NO_VAL) &&
	    (details_ptr->num_tasks != 0))
		topo_eval->max_nodes = MIN(topo_eval->max_nodes,
					   details_ptr->num_tasks);
	if ((gres_per_job = gres_sched_init(job_ptr->gres_list_req)))
		rem_nodes = MIN(min_nodes, req_nodes);
	else
		rem_nodes = MAX(min_nodes, req_nodes);
	rem_max_cpus = eval_nodes_get_rem_max_cpus(details_ptr, rem_nodes);

	i_start = bit_ffs(topo_eval->node_map);
	if (i_start >= 0)
		i_end = bit_fls(topo_eval->node_map);
	else
		i_end = i_start - 1;
	if (req_map) {
		for (i = i_start; i <= i_end; i++) {
			if (!bit_test(req_map, i)) {
				bit_clear(topo_eval->node_map, i);
				continue;
			}
			node_ptr = node_record_table_ptr[i];
			if (!avail_res_array[i] ||
			    !avail_res_array[i]->avail_cpus) {
				debug("%pJ required node %s lacks available resources",
				      job_ptr, node_ptr->name);
				goto fini;
			}
			if (topo_eval->max_nodes <= 0) {
				log_flag(SELECT_TYPE, "%pJ requires nodes exceed maximum node limit",
					 job_ptr);
				goto fini;
			}
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus,
					       min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->
					sock_gres_list, &topo_eval->avail_cpus);
			}
			if (topo_eval->avail_cpus <= 0) {
				debug("%pJ required node %s lacks available resources",
				      job_ptr, node_ptr->name);
				goto fini;
			}
			total_cpus += topo_eval->avail_cpus;
			rem_cpus   -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			/* leaving bitmap set, decr max limit */
			topo_eval->max_nodes--;
		}
		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
			error_code = SLURM_SUCCESS;
			bit_and(topo_eval->node_map, req_map);
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			error_code = SLURM_ERROR;
			goto fini;
		}
		bit_and_not(orig_node_map, topo_eval->node_map);
	} else {
		bit_clear_all(topo_eval->node_map);
	}

	/* Compute CPUs already allocated to required nodes */
	if ((details_ptr->max_cpus != NO_VAL) &&
	    (total_cpus > details_ptr->max_cpus)) {
		info("%pJ can't use required nodes due to max CPU limit",
		     job_ptr);
		goto fini;
	}

	/*
	 * Start by using nodes that already have a job running.
	 * Then try to use idle nodes.
	 */
	if (topo_eval->max_nodes == 0)
		all_done = true;
	node_weight_list = _build_node_weight_list(orig_node_map);
	iter = list_iterator_create(node_weight_list);
	while (!all_done && (nwt = (node_weight_type *) list_next(iter))) {
		for (idle_test = 0; idle_test < 2; idle_test++) {
			for (i = i_start; i <= i_end; i++) {
				if (!avail_res_array[i] ||
				    !avail_res_array[i]->avail_cpus)
					continue;
				/* Node not available or already selected */
				if (!bit_test(nwt->node_bitmap, i) ||
				    bit_test(topo_eval->node_map, i))
					continue;
				if (((idle_test == 0) &&
				     bit_test(idle_node_bitmap, i)) ||
				    ((idle_test == 1) &&
				     !bit_test(idle_node_bitmap, i)))
					continue;
				eval_nodes_select_cores(topo_eval, i,
							min_rem_nodes);
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
				if (gres_per_job) {
					gres_sched_add(
						job_ptr->gres_list_req,
						avail_res_array[i]->
						sock_gres_list,
						&topo_eval->avail_cpus);
				}
				if (topo_eval->avail_cpus == 0)
					continue;
				total_cpus += topo_eval->avail_cpus;
				rem_cpus -= topo_eval->avail_cpus;
				rem_max_cpus -= topo_eval->avail_cpus;
				rem_nodes--;
				min_rem_nodes--;
				topo_eval->max_nodes--;
				bit_set(topo_eval->node_map, i);
				if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
				    gres_sched_test(job_ptr->gres_list_req,
						    job_ptr->job_id)) {
					error_code = SLURM_SUCCESS;
					all_done = true;
					break;
				}
				if (topo_eval->max_nodes == 0) {
					all_done = true;
					break;
				}
			}
		}
	}
	list_iterator_destroy(iter);

	if (error_code == SLURM_SUCCESS) {
		/* Already succeeded */
	} else if ((rem_cpus > 0) || (min_rem_nodes > 0) ||
		   !gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
		bit_clear_all(topo_eval->node_map);
		error_code = SLURM_ERROR;
	} else {
		error_code = SLURM_SUCCESS;
	}

fini:	FREE_NULL_LIST(node_weight_list);
	FREE_NULL_BITMAP(orig_node_map);
	return error_code;
}

static int _eval_nodes_consec(topology_eval_t *topo_eval)
{
	int i, j, error_code = SLURM_ERROR;
	int *consec_cpus;	/* how many CPUs we can add from this
				 * consecutive set of nodes */
	List *consec_gres;	/* how many GRES we can add from this
				 * consecutive set of nodes */
	int *consec_nodes;	/* how many nodes we can add from this
				 * consecutive set of nodes */
	int *consec_start;	/* where this consecutive set starts (index) */
	int *consec_end;	/* where this consecutive set ends (index) */
	int *consec_req;	/* are nodes from this set required
				 * (in req_bitmap) */
	uint64_t *consec_weight; /* node scheduling weight */
	node_record_t *node_ptr = NULL;
	int consec_index, consec_size, sufficient;
	int rem_cpus, rem_nodes; /* remaining resources desired */
	int min_rem_nodes;	/* remaining resources desired */
	int best_fit_nodes, best_fit_cpus, best_fit_req;
	int best_fit_sufficient, best_fit_index = 0;
	bool new_best;
	uint64_t best_weight = 0;
	int64_t rem_max_cpus;
	int total_cpus = 0;	/* #CPUs allocated to job */
	bool gres_per_job, required_node;
	avail_res_t **avail_res_array = topo_eval->avail_res_array;
	job_record_t *job_ptr = topo_eval->job_ptr;
	job_details_t *details_ptr = job_ptr->details;
	bitstr_t *req_map = details_ptr->req_node_bitmap;
	uint32_t min_nodes = topo_eval->min_nodes;
	uint32_t req_nodes = topo_eval->req_nodes;
	uint16_t *avail_cpu_per_node = NULL;

	topo_eval->avail_cpus = 0;

	/* make allocation for 50 sets of consecutive nodes, expand as needed */
	consec_size = 50;
	consec_cpus   = xcalloc(consec_size, sizeof(int));
	consec_nodes  = xcalloc(consec_size, sizeof(int));
	consec_start  = xcalloc(consec_size, sizeof(int));
	consec_end    = xcalloc(consec_size, sizeof(int));
	consec_req    = xcalloc(consec_size, sizeof(int));
	consec_weight = xcalloc(consec_size, sizeof(uint64_t));

	/* Build table with information about sets of consecutive nodes */
	consec_index = 0;
	consec_req[consec_index] = -1;	/* no required nodes here by default */
	consec_weight[consec_index] = NO_VAL64;

	avail_cpu_per_node = xcalloc(node_record_count, sizeof(uint16_t));
	rem_cpus = details_ptr->min_cpus;
	min_rem_nodes = min_nodes;
	if ((gres_per_job = gres_sched_init(job_ptr->gres_list_req))) {
		rem_nodes = MIN(min_nodes, req_nodes);
		consec_gres = xcalloc(consec_size, sizeof(List));
	} else
		rem_nodes = MAX(min_nodes, req_nodes);
	rem_max_cpus = eval_nodes_get_rem_max_cpus(details_ptr, rem_nodes);

	/*
	 * If there are required nodes, first determine the resources they
	 * provide, then select additional resources as needed in next loop
	 */
	if (req_map) {
		int count = 0;
		uint16_t *arbitrary_tpn = job_ptr->details->arbitrary_tpn;
		for (i = 0;
		     ((node_ptr = next_node_bitmap(req_map, &i)) &&
		      (topo_eval->max_nodes > 0));
		     i++) {
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			if (arbitrary_tpn) {
				int req_cpus = arbitrary_tpn[count++];
				if ((details_ptr->cpus_per_task != NO_VAL16) &&
				    (details_ptr->cpus_per_task != 0))
					req_cpus *= details_ptr->cpus_per_task;

				req_cpus = MAX(req_cpus,
					       (int) details_ptr->pn_min_cpus);
				req_cpus = MAX(req_cpus,
					       details_ptr->min_gres_cpu);

				if (topo_eval->avail_cpus < req_cpus) {
					debug("%pJ required node %s needed %d cpus but only has %d",
					      job_ptr, node_ptr->name, req_cpus,
					      topo_eval->avail_cpus);
					goto fini;
				}
				topo_eval->avail_cpus = req_cpus;

				avail_res_array[i]->avail_cpus =
					topo_eval->avail_cpus;
				avail_res_array[i]->avail_res_cnt =
					avail_res_array[i]->avail_cpus +
					avail_res_array[i]->avail_gpus;

			} else
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			if (topo_eval->avail_cpus == 0) {
				debug("%pJ required node %s lacks available resources",
				      job_ptr, node_ptr->name);
				goto fini;
			}
			avail_cpu_per_node[i] = topo_eval->avail_cpus;
			total_cpus += topo_eval->avail_cpus;
			rem_cpus -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
		}

		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
			error_code = SLURM_SUCCESS;
			bit_and(topo_eval->node_map, req_map);
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			error_code = SLURM_ERROR;
			goto fini;
		}
	}

	for (i = 0; next_node(&i); i++) { /* For each node */
		if ((consec_index + 1) >= consec_size) {
			consec_size *= 2;
			xrecalloc(consec_cpus, consec_size, sizeof(int));
			xrecalloc(consec_nodes, consec_size, sizeof(int));
			xrecalloc(consec_start, consec_size, sizeof(int));
			xrecalloc(consec_end, consec_size, sizeof(int));
			xrecalloc(consec_req, consec_size, sizeof(int));
			xrecalloc(consec_weight, consec_size, sizeof(uint64_t));
			if (gres_per_job) {
				xrecalloc(consec_gres,
					  consec_size, sizeof(List));
			}
		}
		if (req_map)
			required_node = bit_test(req_map, i);
		else
			required_node = false;
		if (!bit_test(topo_eval->node_map, i)) {
			node_ptr = NULL;    /* Use as flag, avoid second test */
		} else if (required_node) {
			node_ptr = node_record_table_ptr[i];
		} else {
			node_ptr = node_record_table_ptr[i];
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			if (topo_eval->avail_cpus == 0) {
				bit_clear(topo_eval->node_map, i);
				node_ptr = NULL;
			}
			avail_cpu_per_node[i] = topo_eval->avail_cpus;
		}
		/*
		 * If job requested contiguous nodes,
		 * do not worry about matching node weights
		 */
		if (node_ptr &&
		    !details_ptr->contiguous &&
		    (consec_weight[consec_index] != NO_VAL64) && /* Init value*/
		    (node_ptr->sched_weight != consec_weight[consec_index])) {
			/* End last consecutive set, setup start of next set */
			if (consec_nodes[consec_index] == 0) {
				/* Only required nodes, re-use consec record */
				consec_req[consec_index] = -1;
			} else {
				/* End last set, setup for start of next set */
				consec_end[consec_index]   = i - 1;
				consec_req[++consec_index] = -1;
			}
		}
		if (node_ptr) {
			if (consec_nodes[consec_index] == 0)
				consec_start[consec_index] = i;
			if (required_node) {
				/*
				 * Required node, resources counters updated
				 * in above loop, leave bitmap set
				 */
				if (consec_req[consec_index] == -1) {
					/* first required node in set */
					consec_req[consec_index] = i;
				}
				continue;
			}

			/* node not selected (yet) */
			bit_clear(topo_eval->node_map, i);
			consec_cpus[consec_index] += topo_eval->avail_cpus;
			consec_nodes[consec_index]++;
			if (gres_per_job) {
				gres_sched_consec(
					&consec_gres[consec_index],
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list);
			}
			consec_weight[consec_index] = node_ptr->sched_weight;
		} else if (consec_nodes[consec_index] == 0) {
			/* Only required nodes, re-use consec record */
			consec_req[consec_index] = -1;
			consec_weight[consec_index] = NO_VAL64;
		} else {
			/* End last set, setup for start of next set */
			consec_end[consec_index]   = i - 1;
			consec_req[++consec_index] = -1;
			consec_weight[consec_index] = NO_VAL64;
		}
	}
	if (consec_nodes[consec_index] != 0)
		consec_end[consec_index++] = i - 1;

	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		if (consec_index == 0) {
			info("consec_index is zero");
		}
		for (i = 0; i < consec_index; i++) {
			char *gres_str = NULL, *gres_print = "";
			bitstr_t *host_bitmap;
			char *host_list;
			if (gres_per_job) {
				gres_str = gres_sched_str(consec_gres[i]);
				if (gres_str) {
					xstrcat(gres_str, " ");
					gres_print = gres_str;
				}
			}

			host_bitmap = bit_alloc(node_record_count);
			bit_nset(host_bitmap, consec_start[i], consec_end[i]);
			host_list = bitmap2node_name(host_bitmap);
			info("set:%d consec CPUs:%d nodes:%d:%s %sbegin:%d end:%d required:%d weight:%"PRIu64,
			     i, consec_cpus[i], consec_nodes[i],
			     host_list, gres_print, consec_start[i],
			     consec_end[i], consec_req[i], consec_weight[i]);
			FREE_NULL_BITMAP(host_bitmap);
			xfree(gres_str);
			xfree(host_list);
		}
	}

	/* Compute CPUs already allocated to required nodes */
	if ((details_ptr->max_cpus != NO_VAL) &&
	    (total_cpus > details_ptr->max_cpus)) {
		info("%pJ can't use required nodes due to max CPU limit",
		     job_ptr);
		goto fini;
	}

	/*
	 * accumulate nodes from these sets of consecutive nodes until
	 * sufficient resources have been accumulated
	 */
	while (consec_index && (topo_eval->max_nodes > 0)) {
		best_fit_cpus = best_fit_nodes = best_fit_sufficient = 0;
		best_fit_req = -1;	/* first required node, -1 if none */
		for (i = 0; i < consec_index; i++) {
			if (consec_nodes[i] == 0)
				continue;	/* no usable nodes here */

			if (details_ptr->contiguous &&
			    details_ptr->req_node_bitmap &&
			    (consec_req[i] == -1))
				continue;  /* not required nodes */
			sufficient = (consec_cpus[i] >= rem_cpus) &&
				     eval_nodes_enough_nodes(
					     consec_nodes[i], rem_nodes,
					     min_nodes, req_nodes);
			if (sufficient && gres_per_job) {
				sufficient = gres_sched_sufficient(
					job_ptr->gres_list_req, consec_gres[i]);
			}

			/*
			 * if first possibility OR
			 * contains required nodes OR
			 * lowest node weight
			 */
			if ((best_fit_nodes == 0) ||
			    ((best_fit_req == -1) && (consec_req[i] != -1)) ||
			    (consec_weight[i] < best_weight))
				new_best = true;
			else
				new_best = false;
			/*
			 * If equal node weight
			 * first set large enough for request OR
			 * tightest fit (less resource/CPU waste) OR
			 * nothing yet large enough, but this is biggest
			 */
			if (!new_best && (consec_weight[i] == best_weight) &&
			    ((sufficient && (best_fit_sufficient == 0)) ||
			     (sufficient && (consec_cpus[i] < best_fit_cpus)) ||
			     (!sufficient &&
			      (consec_cpus[i] > best_fit_cpus))))
				new_best = true;
			/*
			 * if first continuous node set large enough
			 */
			if (!new_best && !best_fit_sufficient &&
			    details_ptr->contiguous && sufficient)
				new_best = true;
			if (new_best) {
				best_fit_cpus = consec_cpus[i];
				best_fit_nodes = consec_nodes[i];
				best_fit_index = i;
				best_fit_req = consec_req[i];
				best_fit_sufficient = sufficient;
				best_weight = consec_weight[i];
			}

			if (details_ptr->contiguous &&
			    details_ptr->req_node_bitmap) {
				/*
				 * Must wait for all required nodes to be
				 * in a single consecutive block
				 */
				int j, other_blocks = 0;
				for (j = (i+1); j < consec_index; j++) {
					if (consec_req[j] != -1) {
						other_blocks = 1;
						break;
					}
				}
				if (other_blocks) {
					best_fit_nodes = 0;
					break;
				}
			}
		}
		if (best_fit_nodes == 0)
			break;

		if (details_ptr->contiguous && !best_fit_sufficient)
			break;	/* no hole large enough */
		if (best_fit_req != -1) {
			/*
			 * This collection of nodes includes required ones
			 * select nodes from this set, first working up
			 * then down from the required nodes
			 */
			for (i = best_fit_req;
			     i <= consec_end[best_fit_index]; i++) {
				if ((topo_eval->max_nodes == 0) ||
				    ((rem_nodes <= 0) && (rem_cpus <= 0) &&
				     (!gres_per_job ||
				      gres_sched_test(job_ptr->gres_list_req,
						      job_ptr->job_id))))
					break;
				if (bit_test(topo_eval->node_map, i)) {
					/* required node already in set */
					continue;
				}
				if (avail_cpu_per_node[i] == 0)
					continue;
				topo_eval->avail_cpus = avail_cpu_per_node[i];

				/*
				 * This could result in 0, but if the user
				 * requested nodes here we will still give
				 * them and then the step layout will sort
				 * things out.
				 */
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
				if (gres_per_job) {
					gres_sched_add(
						job_ptr->gres_list_req,
						avail_res_array[i]->
						sock_gres_list,
						&topo_eval->avail_cpus);
				}
				total_cpus += topo_eval->avail_cpus;
				bit_set(topo_eval->node_map, i);
				rem_nodes--;
				min_rem_nodes--;
				topo_eval->max_nodes--;
				rem_cpus -= topo_eval->avail_cpus;
				rem_max_cpus -= topo_eval->avail_cpus;
			}
			for (i = (best_fit_req - 1);
			     i >= consec_start[best_fit_index]; i--) {
				if ((topo_eval->max_nodes == 0) ||
				    ((rem_nodes <= 0) && (rem_cpus <= 0) &&
				     (!gres_per_job ||
				      gres_sched_test(job_ptr->gres_list_req,
						      job_ptr->job_id))))
					break;
				if (bit_test(topo_eval->node_map, i))
					continue;
				if (avail_cpu_per_node[i] == 0)
					continue;
				topo_eval->avail_cpus = avail_cpu_per_node[i];

				/*
				 * This could result in 0, but if the user
				 * requested nodes here we will still give
				 * them and then the step layout will sort
				 * things out.
				 */
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
				if (gres_per_job) {
					gres_sched_add(
						job_ptr->gres_list_req,
						avail_res_array[i]->
						sock_gres_list,
						&topo_eval->avail_cpus);
				}
				total_cpus += topo_eval->avail_cpus;
				rem_cpus -= topo_eval->avail_cpus;
				rem_max_cpus -= topo_eval->avail_cpus;
				bit_set(topo_eval->node_map, i);
				rem_nodes--;
				min_rem_nodes--;
				topo_eval->max_nodes--;
			}
		} else {
			/* No required nodes, try best fit single node */
			int best_fit = -1, best_size = 0;
			int first = consec_start[best_fit_index];
			int last  = consec_end[best_fit_index];
			if (rem_nodes <= 1) {
				for (i = first, j = 0; i <= last; i++, j++) {
					if (bit_test(topo_eval->node_map, i) ||
					    !avail_res_array[i])
						continue;
					if (avail_cpu_per_node[i] < rem_cpus)
						continue;
					if (gres_per_job &&
					    !gres_sched_sufficient(
						    job_ptr->gres_list_req,
						    avail_res_array[i]->
						    sock_gres_list)) {
						continue;
					}
					if ((best_fit == -1) ||
					    (avail_cpu_per_node[i] <best_size)){
						best_fit = i;
						best_size =
							avail_cpu_per_node[i];
						if (best_size == rem_cpus)
							break;
					}
				}
				/*
				 * If we found a single node to use,
				 * clear CPU counts for all other nodes
				 */
				if (best_fit != -1) {
					for (i = first; i <= last; i++) {
						if (i == best_fit)
							continue;
						avail_cpu_per_node[i] = 0;
					}
				}
			}

			for (i = first, j = 0; i <= last; i++, j++) {
				if ((topo_eval->max_nodes == 0) ||
				    ((rem_nodes <= 0) && (rem_cpus <= 0) &&
				     (!gres_per_job ||
				      gres_sched_test(job_ptr->gres_list_req,
						      job_ptr->job_id))))
					break;
				if (bit_test(topo_eval->node_map, i) ||
				    !avail_res_array[i])
					continue;

				topo_eval->avail_cpus = avail_cpu_per_node[i];
				if (topo_eval->avail_cpus <= 0)
					continue;

				if ((topo_eval->max_nodes == 1) &&
				    (topo_eval->avail_cpus < rem_cpus)) {
					/*
					 * Job can only take one more node and
					 * this one has insufficient CPU
					 */
					continue;
				}

				/*
				 * This could result in 0, but if the user
				 * requested nodes here we will still give
				 * them and then the step layout will sort
				 * things out.
				 */
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
				if (gres_per_job) {
					gres_sched_add(
						job_ptr->gres_list_req,
						avail_res_array[i]->
						sock_gres_list,
						&topo_eval->avail_cpus);
				}
				total_cpus += topo_eval->avail_cpus;
				rem_cpus -= topo_eval->avail_cpus;
				rem_max_cpus -= topo_eval->avail_cpus;
				bit_set(topo_eval->node_map, i);
				rem_nodes--;
				min_rem_nodes--;
				topo_eval->max_nodes--;
			}
		}

		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
			error_code = SLURM_SUCCESS;
			break;
		}
		consec_cpus[best_fit_index] = 0;
		consec_nodes[best_fit_index] = 0;
	}

	if (error_code && (rem_cpus <= 0) &&
	    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id) &&
	    eval_nodes_enough_nodes(0, rem_nodes, min_nodes, req_nodes))
		error_code = SLURM_SUCCESS;

fini:	xfree(avail_cpu_per_node);
	xfree(consec_cpus);
	xfree(consec_nodes);
	xfree(consec_start);
	xfree(consec_end);
	xfree(consec_req);
	xfree(consec_weight);
	if (gres_per_job) {
		for (i = 0; i < consec_size; i++)
			FREE_NULL_LIST(consec_gres[i]);
		xfree(consec_gres);
	}

	return error_code;
}

/*
 * Allocate resources to the job on one leaf switch if possible,
 * otherwise distribute the job allocation over many leaf switches.
 */
static int _eval_nodes_dfly(topology_eval_t *topo_eval)
{
	List      *switch_gres = NULL;		/* available GRES on switch */
	bitstr_t **switch_node_bitmap = NULL;	/* nodes on this switch */
	int       *switch_node_cnt = NULL;	/* total nodes on switch */
	int       *switch_required = NULL;	/* set if has required node */
	bitstr_t  *avail_nodes_bitmap = NULL;	/* nodes on any switch */
	bitstr_t  *req_nodes_bitmap   = NULL;	/* required node bitmap */
	bitstr_t  *req2_nodes_bitmap  = NULL;	/* required+lowest prio nodes */
	bitstr_t  *best_nodes_bitmap  = NULL;	/* required+low prio nodes */
	int i, j, rc = SLURM_SUCCESS;
	int best_cpu_cnt = 0, best_node_cnt = 0, req_node_cnt = 0;
	List best_gres = NULL;
	switch_record_t *switch_ptr;
	List node_weight_list = NULL;
	topo_weight_info_t *nw = NULL;
	ListIterator iter;
	node_record_t *node_ptr;
	int64_t rem_max_cpus;
	int rem_cpus, rem_nodes; /* remaining resources desired */
	int min_rem_nodes;	/* remaining resources desired */
	job_record_t *job_ptr = topo_eval->job_ptr;
	job_details_t *details_ptr = job_ptr->details;
	bool gres_per_job, sufficient = false;
	uint16_t *avail_cpu_per_node = NULL;
	time_t time_waiting = 0;
	int leaf_switch_count = 0;
	int top_switch_inx = -1;
	int prev_rem_nodes;
	avail_res_t **avail_res_array = topo_eval->avail_res_array;
	uint32_t min_nodes = topo_eval->min_nodes;
	uint32_t req_nodes = topo_eval->req_nodes;

	topo_eval->avail_cpus = 0;

	if (job_ptr->req_switch > 1) {
		/* Maximum leaf switch count >1 probably makes no sense */
		info("Resetting %pJ leaf switch count from %u to 0",
		     job_ptr, job_ptr->req_switch);
		job_ptr->req_switch = 0;
	}
	if (job_ptr->req_switch) {
		time_t     time_now;
		time_now = time(NULL);
		if (job_ptr->wait4switch_start == 0)
			job_ptr->wait4switch_start = time_now;
		time_waiting = time_now - job_ptr->wait4switch_start;
	}

	rem_cpus = details_ptr->min_cpus;
	min_rem_nodes = min_nodes;
	if ((gres_per_job = gres_sched_init(job_ptr->gres_list_req)))
		rem_nodes = MIN(min_nodes, req_nodes);
	else
		rem_nodes = MAX(min_nodes, req_nodes);
	rem_max_cpus = eval_nodes_get_rem_max_cpus(details_ptr, rem_nodes);

	/* Validate availability of required nodes */
	if (job_ptr->details->req_node_bitmap) {
		if (!bit_super_set(job_ptr->details->req_node_bitmap,
				   topo_eval->node_map)) {
			info("%pJ requires nodes which are not currently available",
			      job_ptr);
			rc = SLURM_ERROR;
			goto fini;
		}

		req_node_cnt = bit_set_count(job_ptr->details->req_node_bitmap);
		if (req_node_cnt == 0) {
			info("%pJ required node list has no nodes",
			      job_ptr);
			rc = SLURM_ERROR;
			goto fini;
		}
		if (req_node_cnt > topo_eval->max_nodes) {
			info("%pJ requires more nodes than currently available (%u>%u)",
			      job_ptr, req_node_cnt,
			      topo_eval->max_nodes);
			rc = SLURM_ERROR;
			goto fini;
		}
		req_nodes_bitmap = bit_copy(job_ptr->details->req_node_bitmap);
	}

	/*
	 * Add required nodes to job allocation and
	 * build list of node bitmaps, sorted by weight
	 */
	if (!bit_set_count(topo_eval->node_map)) {
		debug("%pJ node_map is empty",
		      job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}
	avail_cpu_per_node = xcalloc(node_record_count, sizeof(uint16_t));
	node_weight_list = list_create(eval_nodes_topo_weight_free);
	for (i = 0; (node_ptr = next_node_bitmap(topo_eval->node_map, &i)); i++) {
		topo_weight_info_t nw_static;
		if (req_nodes_bitmap && bit_test(req_nodes_bitmap, i)) {
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			eval_nodes_cpus_to_use(
				topo_eval, i, rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			if (topo_eval->avail_cpus == 0) {
				log_flag(SELECT_TYPE, "%pJ insufficient resources on required node",
				       job_ptr);
				rc = SLURM_ERROR;
				goto fini;
			}
			avail_cpu_per_node[i] = topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			rem_cpus   -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
		}

		nw_static.weight = node_ptr->sched_weight;
		nw = list_find_first(node_weight_list,
				     eval_nodes_topo_weight_find,
				     &nw_static);
		if (!nw) {	/* New node weight to add */
			nw = xmalloc(sizeof(topo_weight_info_t));
			nw->node_bitmap = bit_alloc(node_record_count);
			nw->weight = node_ptr->sched_weight;
			list_append(node_weight_list, nw);
		}
		bit_set(nw->node_bitmap, i);
		nw->node_cnt++;
	}

	if (req_nodes_bitmap) {
		bit_and(topo_eval->node_map, req_nodes_bitmap);
		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
			/* Required nodes completely satisfied the request */
			rc = SLURM_SUCCESS;
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			rc = SLURM_ERROR;
			log_flag(SELECT_TYPE, "%pJ requires nodes exceed maximum node limit",
				 job_ptr);
			goto fini;
		}
	} else {
		bit_clear_all(topo_eval->node_map);
	}

	list_sort(node_weight_list, eval_nodes_topo_weight_sort);
	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE)
		(void) list_for_each(node_weight_list,
				     eval_nodes_topo_weight_log, NULL);

	/*
	 * Identify the highest level switch to be used.
	 * Note that nodes can be on multiple non-overlapping switches.
	 */
	switch_gres        = xcalloc(switch_record_cnt, sizeof(List));
	switch_node_bitmap = xcalloc(switch_record_cnt, sizeof(bitstr_t *));
	switch_node_cnt    = xcalloc(switch_record_cnt, sizeof(int));
	switch_required    = xcalloc(switch_record_cnt, sizeof(int));

	if (!req_nodes_bitmap)
		nw = list_peek(node_weight_list);
	for (i = 0, switch_ptr = switch_record_table; i < switch_record_cnt;
	     i++, switch_ptr++) {
		switch_node_bitmap[i] = bit_copy(switch_ptr->node_bitmap);
		if (req_nodes_bitmap &&
		    bit_overlap_any(req_nodes_bitmap, switch_node_bitmap[i])) {
			switch_required[i] = 1;
			if (switch_record_table[i].level == 0) {
				leaf_switch_count++;
			}
			if ((top_switch_inx == -1) ||
			    (switch_record_table[i].level >
			     switch_record_table[top_switch_inx].level)) {
				top_switch_inx = i;
			}
		}
		if (!req_nodes_bitmap &&
		    (list_find_first(node_weight_list,
				     eval_nodes_topo_node_find,
				    switch_node_bitmap[i]))) {
			if ((top_switch_inx == -1) ||
			    (switch_record_table[i].level >
			     switch_record_table[top_switch_inx].level)) {
				top_switch_inx = i;
			}
		}
	}

	/*
	 * Top switch is highest level switch containing all required nodes
	 * OR all nodes of the lowest scheduling weight
	 * OR -1 of can not identify top-level switch
	 */
	if (top_switch_inx == -1) {
		error("%pJ unable to identify top level switch",
		       job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}

	/* Check that all specificly required nodes are on shared network */
	if (req_nodes_bitmap &&
	    !bit_super_set(req_nodes_bitmap,
			   switch_node_bitmap[top_switch_inx])) {
		rc = SLURM_ERROR;
		info("%pJ requires nodes that do not have shared network",
		     job_ptr);
		goto fini;
	}

	/*
	 * Remove nodes from consideration that can not be reached from this
	 * top level switch
	 */
	for (i = 0; i < switch_record_cnt; i++) {
		if (top_switch_inx != i) {
			  bit_and(switch_node_bitmap[i],
				  switch_node_bitmap[top_switch_inx]);
		}
	}

	/*
	 * Identify the best set of nodes (i.e. nodes with the lowest weight,
	 * in addition to the required nodes) that can be used to satisfy the
	 * job request. All nodes must be on a common top-level switch. The
	 * logic here adds groups of nodes, all with the same weight, so we
	 * usually identify more nodes than required to satisfy the request.
	 * Later logic selects from those nodes to get the best topology.
	 */
	best_nodes_bitmap = bit_alloc(node_record_count);
	iter = list_iterator_create(node_weight_list);
	while (!sufficient && (nw = list_next(iter))) {
		if (best_node_cnt > 0) {
			/*
			 * All of the lower priority nodes should be included
			 * in the job's allocation. Nodes from the next highest
			 * weight nodes are included only as needed.
			 */
			if (req2_nodes_bitmap)
				bit_or(req2_nodes_bitmap, best_nodes_bitmap);
			else
				req2_nodes_bitmap = bit_copy(best_nodes_bitmap);
		}
		for (i = 0; next_node_bitmap(nw->node_bitmap, &i); i++) {
			if (avail_cpu_per_node[i])
				continue;	/* Required node */
			if (!bit_test(switch_node_bitmap[top_switch_inx], i))
				continue;
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			if (topo_eval->avail_cpus == 0) {
				bit_clear(nw->node_bitmap, i);
				continue;
			}
			bit_set(best_nodes_bitmap, i);
			avail_cpu_per_node[i] = topo_eval->avail_cpus;
			best_cpu_cnt += topo_eval->avail_cpus;
			best_node_cnt++;
			if (gres_per_job) {
				gres_sched_consec(
					&best_gres, job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list);
			}
		}

		sufficient = (best_cpu_cnt >= rem_cpus) &&
			     eval_nodes_enough_nodes(best_node_cnt, rem_nodes,
						     min_nodes, req_nodes);
		if (sufficient && gres_per_job) {
			sufficient = gres_sched_sufficient(
				job_ptr->gres_list_req, best_gres);
		}
	}
	list_iterator_destroy(iter);

	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		char *gres_str = NULL, *gres_print = "";
		char *node_names;
		if (req_nodes_bitmap) {
			node_names = bitmap2node_name(req_nodes_bitmap);
			info("Required nodes:%s", node_names);
			xfree(node_names);
		}
		node_names = bitmap2node_name(best_nodes_bitmap);
		if (gres_per_job) {
			gres_str = gres_sched_str(best_gres);
			if (gres_str)
				gres_print = gres_str;
		}
		info("Best nodes:%s node_cnt:%d cpu_cnt:%d %s",
		     node_names, best_node_cnt, best_cpu_cnt, gres_print);
		xfree(node_names);
		xfree(gres_str);
	}
	if (!sufficient) {
		log_flag(SELECT_TYPE, "insufficient resources currently available for %pJ",
		      job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}

	/*
	 * Add lowest weight nodes. Treat similar to required nodes for the job.
	 * Job will still need to add some higher weight nodes later.
	 */
	if (req2_nodes_bitmap) {
		for (i = 0;
		     next_node_bitmap(req2_nodes_bitmap, &i) && (topo_eval->max_nodes > 0);
		     i++) {
			topo_eval->avail_cpus = avail_cpu_per_node[i];
			eval_nodes_cpus_to_use(
				topo_eval, i, rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			rem_cpus   -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
		}

		for (i = 0, switch_ptr = switch_record_table;
		     i < switch_record_cnt; i++, switch_ptr++) {
			if (switch_required[i])
				continue;
			if (bit_overlap_any(req2_nodes_bitmap,
					    switch_node_bitmap[i])) {
				switch_required[i] = 1;
				if (switch_record_table[i].level == 0) {
					leaf_switch_count++;
				}
			}
		}
		bit_or(topo_eval->node_map, req2_nodes_bitmap);
		if (topo_eval->max_nodes <= 0) {
			rc = SLURM_ERROR;
			log_flag(SELECT_TYPE, "%pJ reached maximum node limit",
				 job_ptr);
			goto fini;
		}
		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    (!gres_per_job || gres_sched_test(job_ptr->gres_list_req,
						      job_ptr->job_id))) {
			/* Required nodes completely satisfied the request */
			error("Scheduling anomaly for %pJ",
			      job_ptr);
			rc = SLURM_SUCCESS;
			goto fini;
		}
	}

	/*
	 * Construct a set of switch array entries.
	 * Use the same indexes as switch_record_table in slurmctld.
	 */
	bit_or(best_nodes_bitmap, topo_eval->node_map);
	avail_nodes_bitmap = bit_alloc(node_record_count);
	for (i = 0, switch_ptr = switch_record_table; i < switch_record_cnt;
	     i++, switch_ptr++) {
		bit_and(switch_node_bitmap[i], best_nodes_bitmap);
		bit_or(avail_nodes_bitmap, switch_node_bitmap[i]);
		switch_node_cnt[i] = bit_set_count(switch_node_bitmap[i]);
	}

	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		for (i = 0; i < switch_record_cnt; i++) {
			char *node_names = NULL;
			if (switch_node_cnt[i]) {
				node_names =
					bitmap2node_name(switch_node_bitmap[i]);
			}
			info("switch=%s level=%d nodes=%u:%s required:%u speed:%u",
			     switch_record_table[i].name,
			     switch_record_table[i].level,
			     switch_node_cnt[i], node_names,
			     switch_required[i],
			     switch_record_table[i].link_speed);
			xfree(node_names);
		}
	}

	if (req_nodes_bitmap &&
	    (!bit_super_set(req_nodes_bitmap, avail_nodes_bitmap))) {
		info("%pJ requires nodes not available on any switch",
		     job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}

	/*
	 * If no resources have yet been  selected,
	 * then pick one leaf switch with the most available nodes.
	 */
	if (leaf_switch_count == 0) {
		int best_switch_inx = -1;
		for (i = 0; i < switch_record_cnt; i++) {
			if (switch_record_table[i].level != 0)
				continue;
			if ((best_switch_inx == -1) ||
			    (switch_node_cnt[i] >
			     switch_node_cnt[best_switch_inx]))
				best_switch_inx = i;
		}
		if (best_switch_inx != -1) {
			leaf_switch_count = 1;
			switch_required[best_switch_inx] = 1;
		}
	}

	/*
	 * All required resources currently on one leaf switch. Determine if
	 * the entire job request can be satisfied using just that one switch.
	 */
	if (leaf_switch_count == 1) {
		best_cpu_cnt = 0;
		best_node_cnt = 0;
		FREE_NULL_LIST(best_gres);
		for (i = 0; i < switch_record_cnt; i++) {
			if (!switch_required[i] || !switch_node_bitmap[i] ||
			    (switch_record_table[i].level != 0))
				continue;
			for (j = 0; next_node_bitmap(switch_node_bitmap[i], &j);
			     j++) {
				if (bit_test(topo_eval->node_map, j) ||
				    !avail_cpu_per_node[j])
					continue;
				topo_eval->avail_cpus = avail_cpu_per_node[j];
				best_cpu_cnt += topo_eval->avail_cpus;
				best_node_cnt++;
				if (gres_per_job) {
					gres_sched_consec(
						&best_gres,
						job_ptr->gres_list_req,
						avail_res_array[j]->
						sock_gres_list);
				}
			}
			break;
		}
		sufficient = (best_cpu_cnt >= rem_cpus) &&
			     eval_nodes_enough_nodes(best_node_cnt, rem_nodes,
						     min_nodes, req_nodes);
		if (sufficient && gres_per_job) {
			sufficient = gres_sched_sufficient(
				job_ptr->gres_list_req, best_gres);
		}
		if (sufficient && (i < switch_record_cnt)) {
			/* Complete request using this one leaf switch */
			for (j = 0; next_node_bitmap(switch_node_bitmap[i], &j);
			     j++) {
				if (bit_test(topo_eval->node_map, j) ||
				    !avail_cpu_per_node[j])
					continue;
				topo_eval->avail_cpus = avail_cpu_per_node[j];
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
				if (gres_per_job) {
					gres_sched_add(
						job_ptr->gres_list_req,
						avail_res_array[j]->
						sock_gres_list,
						&topo_eval->avail_cpus);
				}
				rem_nodes--;
				min_rem_nodes--;
				topo_eval->max_nodes--;
				rem_cpus   -= topo_eval->avail_cpus;
				rem_max_cpus -= topo_eval->avail_cpus;
				bit_set(topo_eval->node_map, j);
				if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
				    (!gres_per_job ||
				     gres_sched_test(job_ptr->gres_list_req,
						     job_ptr->job_id))) {
					rc = SLURM_SUCCESS;
					goto fini;
				}
				if (topo_eval->max_nodes <= 0) {
					rc = SLURM_ERROR;
					log_flag(SELECT_TYPE, "%pJ reached maximum node limit",
						 job_ptr);
					goto fini;
				}
			}
		}
	}

	/*
	 * Add additional resources as required from additional leaf switches
	 * on a round-robin basis
	 */
	prev_rem_nodes = rem_nodes + 1;
	while (1) {
		if (prev_rem_nodes == rem_nodes)
			break;	/* Stalled */
		prev_rem_nodes = rem_nodes;
		for (i = 0; i < switch_record_cnt; i++) {
			if (!switch_node_bitmap[i] ||
			    (switch_record_table[i].level != 0))
				continue;
			for (j = 0; next_node_bitmap(switch_node_bitmap[i], &j);
			     j++) {
				if (bit_test(topo_eval->node_map, j) ||
				    !avail_cpu_per_node[j])
					continue;
				topo_eval->avail_cpus = avail_cpu_per_node[j];
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
				if (gres_per_job) {
					gres_sched_add(
						job_ptr->gres_list_req,
						avail_res_array[j]->
						sock_gres_list,
						&topo_eval->avail_cpus);
				}
				rem_nodes--;
				min_rem_nodes--;
				topo_eval->max_nodes--;
				rem_cpus   -= topo_eval->avail_cpus;
				rem_max_cpus -= topo_eval->avail_cpus;
				bit_set(topo_eval->node_map, j);
				if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
				    (!gres_per_job ||
				     gres_sched_test(job_ptr->gres_list_req,
						     job_ptr->job_id))) {
					rc = SLURM_SUCCESS;
					goto fini;
				}
				if (topo_eval->max_nodes <= 0) {
					rc = SLURM_ERROR;
					log_flag(SELECT_TYPE, "%pJ reached maximum node limit",
						 job_ptr);
					goto fini;
				}
				break;	/* Move to next switch */
			}
		}
	}
	if ((min_rem_nodes <= 0) && (rem_cpus <= 0) &&
	    (!gres_per_job ||
	     gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id))) {
		rc = SLURM_SUCCESS;
		goto fini;
	}
	rc = SLURM_ERROR;

fini:
	if ((job_ptr->req_switch > 0) && (rc == SLURM_SUCCESS) &&
	    switch_node_bitmap) {
		/* req_switch == 1 here; enforced at the top of the function. */
		leaf_switch_count = 0;

		/* count up leaf switches */
		for (i = 0, switch_ptr = switch_record_table;
		     i < switch_record_cnt; i++, switch_ptr++) {
			if (switch_record_table[i].level != 0)
				continue;
			if (bit_overlap_any(switch_node_bitmap[i], topo_eval->node_map))
				leaf_switch_count++;
		}
		if (time_waiting >= job_ptr->wait4switch) {
			job_ptr->best_switch = true;
			debug3("%pJ waited %ld sec for switches use=%d",
				job_ptr, time_waiting, leaf_switch_count);
		} else if (leaf_switch_count > job_ptr->req_switch) {
			/*
			 * Allocation is for more than requested number of
			 * switches.
			 */
			job_ptr->best_switch = false;
			debug3("%pJ waited %ld sec for switches=%u found=%d wait %u",
				job_ptr, time_waiting, job_ptr->req_switch,
				leaf_switch_count, job_ptr->wait4switch);
		} else {
			job_ptr->best_switch = true;
		}
	}

	FREE_NULL_LIST(best_gres);
	FREE_NULL_LIST(node_weight_list);
	FREE_NULL_BITMAP(avail_nodes_bitmap);
	FREE_NULL_BITMAP(req_nodes_bitmap);
	FREE_NULL_BITMAP(req2_nodes_bitmap);
	FREE_NULL_BITMAP(best_nodes_bitmap);
	xfree(avail_cpu_per_node);
	xfree(switch_gres);
	if (switch_node_bitmap) {
		for (i = 0; i < switch_record_cnt; i++)
			FREE_NULL_BITMAP(switch_node_bitmap[i]);
		xfree(switch_node_bitmap);
	}
	xfree(switch_node_cnt);
	xfree(switch_required);
	return rc;
}

static int _eval_nodes_lln(topology_eval_t *topo_eval)
{
	int i, i_start, i_end, error_code = SLURM_ERROR;
	int rem_cpus, rem_nodes; /* remaining resources desired */
	int min_rem_nodes;	/* remaining resources desired */
	int total_cpus = 0;	/* #CPUs allocated to job */
	int64_t rem_max_cpus;
	job_record_t *job_ptr = topo_eval->job_ptr;
	job_details_t *details_ptr = job_ptr->details;
	bitstr_t *req_map = details_ptr->req_node_bitmap;
	bitstr_t *orig_node_map = bit_copy(topo_eval->node_map);
	bool all_done = false, gres_per_job;
	node_record_t *node_ptr;
	List node_weight_list = NULL;
	node_weight_type *nwt;
	ListIterator iter;
	avail_res_t **avail_res_array = topo_eval->avail_res_array;
	uint32_t min_nodes = topo_eval->min_nodes;
	uint32_t req_nodes = topo_eval->req_nodes;

	topo_eval->avail_cpus = 0;

	rem_cpus = details_ptr->min_cpus;
	min_rem_nodes = min_nodes;
	if ((details_ptr->num_tasks != NO_VAL) &&
	    (details_ptr->num_tasks != 0))
		topo_eval->max_nodes = MIN(topo_eval->max_nodes,
					   details_ptr->num_tasks);
	if ((gres_per_job = gres_sched_init(job_ptr->gres_list_req)))
		rem_nodes = MIN(min_nodes, req_nodes);
	else
		rem_nodes = MAX(min_nodes, req_nodes);
	rem_max_cpus = eval_nodes_get_rem_max_cpus(details_ptr, rem_nodes);

	i_start = bit_ffs(topo_eval->node_map);
	if (i_start >= 0)
		i_end = bit_fls(topo_eval->node_map);
	else
		i_end = i_start - 1;
	if (req_map) {
		for (i = i_start; i <= i_end; i++) {
			if (!bit_test(req_map, i)) {
				bit_clear(topo_eval->node_map, i);
				continue;
			}
			node_ptr = node_record_table_ptr[i];
			if (!avail_res_array[i] ||
			    !avail_res_array[i]->avail_cpus) {
				debug("%pJ required node %s lacks available resources",
				      job_ptr, node_ptr->name);
				goto fini;
			}
			if (topo_eval->max_nodes <= 0) {
				log_flag(SELECT_TYPE, "%pJ requires nodes exceed maximum node limit",
					 job_ptr);
				goto fini;
			}
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->
					sock_gres_list, &topo_eval->avail_cpus);
			}
			if (topo_eval->avail_cpus <= 0) {
				debug("%pJ required node %s not available",
				      job_ptr, node_ptr->name);
				goto fini;
			}
			total_cpus += topo_eval->avail_cpus;
			rem_cpus   -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			/* leaving bitmap set, decr max limit */
			topo_eval->max_nodes--;
		}
		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
			error_code = SLURM_SUCCESS;
			bit_and(topo_eval->node_map, req_map);
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			error_code = SLURM_ERROR;
			goto fini;
		}
		bit_and_not(orig_node_map, topo_eval->node_map);
	} else {
		bit_clear_all(topo_eval->node_map);
	}

	/* Compute CPUs already allocated to required nodes */
	if ((details_ptr->max_cpus != NO_VAL) &&
	    (total_cpus > details_ptr->max_cpus)) {
		info("%pJ can't use required nodes due to max CPU limit",
		     job_ptr);
		goto fini;
	}

	/*
	 * Accumulate nodes from those with highest available CPU count.
	 * Logic is optimized for small node/CPU count allocations.
	 * For larger allocation, use list_sort().
	 */
	if (topo_eval->max_nodes == 0)
		all_done = true;
	node_weight_list = _build_node_weight_list(orig_node_map);
	iter = list_iterator_create(node_weight_list);
	while (!all_done && (nwt = (node_weight_type *) list_next(iter))) {
		int last_max_cpu_cnt = -1;
		while (!all_done) {
			int max_cpu_idx = -1;
			uint16_t max_cpu_avail_cpus = 0;
			for (i = i_start; i <= i_end; i++) {
				/* Node not available or already selected */
				if (!bit_test(nwt->node_bitmap, i) ||
				    bit_test(topo_eval->node_map, i))
					continue;
				eval_nodes_select_cores(topo_eval, i,
							min_rem_nodes);
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
				if (topo_eval->avail_cpus == 0)
					continue;
				/*
				 * Find the "least-loaded" node at the current
				 * node-weight level. This is defined as the
				 * node with the greatest ratio of available to
				 * total cpus. (But shift the divisors around
				 * to avoid any floating-point math.)
				 */
				if ((max_cpu_idx == -1) ||
				    ((avail_res_array[max_cpu_idx]->max_cpus *
				      node_record_table_ptr[i]->cpus) <
				     (avail_res_array[i]->max_cpus *
				      node_record_table_ptr[max_cpu_idx]->
				      cpus))) {
					max_cpu_idx = i;
					max_cpu_avail_cpus =
						topo_eval->avail_cpus;
					if (avail_res_array[max_cpu_idx]->
					    max_cpus == last_max_cpu_cnt)
						break;
				}
			}
			if ((max_cpu_idx == -1) ||
			    (max_cpu_avail_cpus == 0)) {
				/* No more usable nodes left, get next weight */
				break;
			}
			i = max_cpu_idx;
			topo_eval->avail_cpus = max_cpu_avail_cpus;
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			last_max_cpu_cnt = avail_res_array[i]->max_cpus;
			total_cpus += topo_eval->avail_cpus;
			rem_cpus -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			bit_set(topo_eval->node_map, i);
			if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
			    gres_sched_test(job_ptr->gres_list_req,
					    job_ptr->job_id)) {
				error_code = SLURM_SUCCESS;
				all_done = true;
				break;
			}
			if (topo_eval->max_nodes == 0) {
				all_done = true;
				break;
			}
		}
	}
	list_iterator_destroy(iter);

	if (error_code == SLURM_SUCCESS) {
		/* Already succeeded */
	} else if ((rem_cpus > 0) || (min_rem_nodes > 0) ||
		   !gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
		bit_clear_all(topo_eval->node_map);
		error_code = SLURM_ERROR;
	} else {
		error_code = SLURM_SUCCESS;
	}

fini:	FREE_NULL_LIST(node_weight_list);
	FREE_NULL_BITMAP(orig_node_map);
	return error_code;
}

/*
 * A variation of _eval_nodes() to select resources at the end of the node
 * list to reduce fragmentation
 */
static int _eval_nodes_serial(topology_eval_t *topo_eval)
{
	int i, i_start, i_end, error_code = SLURM_ERROR;
	int rem_cpus, rem_nodes; /* remaining resources desired */
	int min_rem_nodes;	/* remaining resources desired */
	int total_cpus = 0;	/* #CPUs allocated to job */
	int64_t rem_max_cpus;
	job_record_t *job_ptr = topo_eval->job_ptr;
	job_details_t *details_ptr = job_ptr->details;
	bitstr_t *req_map = details_ptr->req_node_bitmap;
	bitstr_t *orig_node_map = bit_copy(topo_eval->node_map);
	avail_res_t **avail_res_array = topo_eval->avail_res_array;
	uint32_t min_nodes = topo_eval->min_nodes;
	uint32_t req_nodes = topo_eval->req_nodes;
	bool all_done = false, gres_per_job;
	node_record_t *node_ptr;
	List node_weight_list = NULL;
	node_weight_type *nwt;
	ListIterator iter;

	topo_eval->avail_cpus = 0;

	rem_cpus = details_ptr->min_cpus;
	min_rem_nodes = min_nodes;
	if ((details_ptr->num_tasks != NO_VAL) &&
	    (details_ptr->num_tasks != 0))
		topo_eval->max_nodes = MIN(topo_eval->max_nodes,
					   details_ptr->num_tasks);
	if ((gres_per_job = gres_sched_init(job_ptr->gres_list_req)))
		rem_nodes = MIN(min_nodes, req_nodes);
	else
		rem_nodes = MAX(min_nodes, req_nodes);
	rem_max_cpus = eval_nodes_get_rem_max_cpus(details_ptr, rem_nodes);

	i_start = bit_ffs(topo_eval->node_map);
	if (i_start >= 0)
		i_end = bit_fls(topo_eval->node_map);
	else
		i_end = i_start - 1;
	if (req_map) {
		for (i = i_start; i <= i_end; i++) {
			if (!bit_test(req_map, i)) {
				bit_clear(topo_eval->node_map, i);
				continue;
			}
			node_ptr = node_record_table_ptr[i];
			if (!avail_res_array[i] ||
			    !avail_res_array[i]->avail_cpus) {
				debug("%pJ required node %s lacks available resources",
				      job_ptr, node_ptr->name);
				goto fini;
			}
			if (topo_eval->max_nodes <= 0) {
				log_flag(SELECT_TYPE, "%pJ requires nodes exceed maximum node limit",
					 job_ptr);
				goto fini;
			}
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->
					sock_gres_list, &topo_eval->avail_cpus);
			}
			if (topo_eval->avail_cpus <= 0) {
				debug("%pJ required node %s lacks available resources",
				      job_ptr, node_ptr->name);
				goto fini;
			}
			total_cpus += topo_eval->avail_cpus;
			rem_cpus   -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			/* leaving bitmap set, decr max limit */
			topo_eval->max_nodes--;
		}
		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
			error_code = SLURM_SUCCESS;
			bit_and(topo_eval->node_map, req_map);
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			error_code = SLURM_ERROR;
			goto fini;
		}
		bit_and_not(orig_node_map, topo_eval->node_map);
	} else {
		bit_clear_all(topo_eval->node_map);
	}

	/* Compute CPUs already allocated to required nodes */
	if ((details_ptr->max_cpus != NO_VAL) &&
	    (total_cpus > details_ptr->max_cpus)) {
		info("%pJ can't use required nodes due to max CPU limit",
		     job_ptr);
		goto fini;
	}

	if (topo_eval->max_nodes == 0)
		all_done = true;
	node_weight_list = _build_node_weight_list(orig_node_map);
	iter = list_iterator_create(node_weight_list);
	while (!all_done && (nwt = (node_weight_type *) list_next(iter))) {
		for (i = i_end;
		     ((i >= i_start) && (topo_eval->max_nodes > 0));
		     i--) {
			if (!avail_res_array[i] ||
			    !avail_res_array[i]->avail_cpus)
				continue;
			/* Node not available or already selected */
			if (!bit_test(nwt->node_bitmap, i) ||
			    bit_test(topo_eval->node_map, i))
				continue;
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus, min_rem_nodes);
			if (topo_eval->avail_cpus == 0)
				continue;
			total_cpus += topo_eval->avail_cpus;
			rem_cpus -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			bit_set(topo_eval->node_map, i);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
			    gres_sched_test(job_ptr->gres_list_req,
					    job_ptr->job_id)) {
				error_code = SLURM_SUCCESS;
				all_done = true;
				break;
			}
			if (topo_eval->max_nodes == 0) {
				all_done = true;
				break;
			}
		}
	}
	list_iterator_destroy(iter);

	if (error_code == SLURM_SUCCESS) {
		/* Already succeeded */
	} else if ((rem_cpus > 0) || (min_rem_nodes > 0) ||
		   !gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
		bit_clear_all(topo_eval->node_map);
		error_code = SLURM_ERROR;
	} else {
		error_code = SLURM_SUCCESS;
	}

fini:	FREE_NULL_LIST(node_weight_list);
	FREE_NULL_BITMAP(orig_node_map);
	return error_code;

}

/*
 * A variation of _eval_nodes() to select resources using as many nodes as
 * possible.
 */
static int _eval_nodes_spread(topology_eval_t *topo_eval)
{
	int i, i_start, i_end, error_code = SLURM_ERROR;
	int rem_cpus, rem_nodes; /* remaining resources desired */
	int min_rem_nodes;	/* remaining resources desired */
	int total_cpus = 0;	/* #CPUs allocated to job */
	int64_t rem_max_cpus;
	avail_res_t **avail_res_array = topo_eval->avail_res_array;
	job_record_t *job_ptr = topo_eval->job_ptr;
	job_details_t *details_ptr = job_ptr->details;
	bitstr_t *req_map = details_ptr->req_node_bitmap;
	bitstr_t *orig_node_map = bit_copy(topo_eval->node_map);
	uint32_t min_nodes = topo_eval->min_nodes;
	uint32_t req_nodes = topo_eval->req_nodes;
	bool all_done = false, gres_per_job;
	node_record_t *node_ptr;
	List node_weight_list = NULL;
	node_weight_type *nwt;
	ListIterator iter;

	topo_eval->avail_cpus = 0;

	rem_cpus = details_ptr->min_cpus;
	min_rem_nodes = min_nodes;
	if ((details_ptr->num_tasks != NO_VAL) &&
	    (details_ptr->num_tasks != 0))
		topo_eval->max_nodes = MIN(topo_eval->max_nodes,
					   details_ptr->num_tasks);
	if ((gres_per_job = gres_sched_init(job_ptr->gres_list_req)))
		rem_nodes = MIN(min_nodes, req_nodes);
	else
		rem_nodes = MAX(min_nodes, req_nodes);
	rem_max_cpus = eval_nodes_get_rem_max_cpus(details_ptr, rem_nodes);

	i_start = bit_ffs(topo_eval->node_map);
	if (i_start >= 0)
		i_end = bit_fls(topo_eval->node_map);
	else
		i_end = i_start - 1;
	if (req_map) {
		for (i = i_start; i <= i_end; i++) {
			if (!bit_test(req_map, i)) {
				bit_clear(topo_eval->node_map, i);
				continue;
			}
			node_ptr = node_record_table_ptr[i];
			if (!avail_res_array[i] ||
			    !avail_res_array[i]->avail_cpus) {
				debug("%pJ required node %s lacks available resources",
				      job_ptr, node_ptr->name);
				goto fini;
			}
			if (topo_eval->max_nodes <= 0) {
				log_flag(SELECT_TYPE, "%pJ requires nodes exceed maximum node limit",
					 job_ptr);
				goto fini;
			}
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]-> sock_gres_list,
					&topo_eval->avail_cpus);
			}
			if (topo_eval->avail_cpus <= 0) {
				debug("%pJ required node %s lacks available resources",
				      job_ptr, node_ptr->name);
				goto fini;
			}
			total_cpus += topo_eval->avail_cpus;
			rem_cpus   -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			/* leaving bitmap set, decr max limit */
			topo_eval->max_nodes--;
		}
		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
			error_code = SLURM_SUCCESS;
			bit_and(topo_eval->node_map, req_map);
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			error_code = SLURM_ERROR;
			goto fini;
		}
		bit_and_not(orig_node_map, topo_eval->node_map);
	} else {
		bit_clear_all(topo_eval->node_map);
	}

	/* Compute CPUs already allocated to required nodes */
	if ((details_ptr->max_cpus != NO_VAL) &&
	    (total_cpus > details_ptr->max_cpus)) {
		info("%pJ can't use required nodes due to max CPU limit",
		     job_ptr);
		goto fini;
	}

	if (topo_eval->max_nodes == 0)
		all_done = true;
	node_weight_list = _build_node_weight_list(orig_node_map);
	iter = list_iterator_create(node_weight_list);
	while (!all_done && (nwt = (node_weight_type *) list_next(iter))) {
		for (i = i_start; i <= i_end; i++) {
			if (!avail_res_array[i] ||
			    !avail_res_array[i]->avail_cpus)
				continue;
			/* Node not available or already selected */
			if (!bit_test(nwt->node_bitmap, i) ||
			    bit_test(topo_eval->node_map, i))
				continue;
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			if (topo_eval->avail_cpus == 0)
				continue;
			total_cpus += topo_eval->avail_cpus;
			rem_cpus -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			bit_set(topo_eval->node_map, i);
			if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
			    gres_sched_test(job_ptr->gres_list_req,
					    job_ptr->job_id)) {
				error_code = SLURM_SUCCESS;
				all_done = true;
				break;
			}
			if (topo_eval->max_nodes == 0) {
				all_done = true;
				break;
			}
		}
	}
	list_iterator_destroy(iter);

	if (error_code == SLURM_SUCCESS) {
		/* Already succeeded */
	} else if ((rem_cpus > 0) || (min_rem_nodes > 0) ||
		   !gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
		bit_clear_all(topo_eval->node_map);
		error_code = SLURM_ERROR;
	} else {
		error_code = SLURM_SUCCESS;
	}

fini:	FREE_NULL_LIST(node_weight_list);
	FREE_NULL_BITMAP(orig_node_map);
	return error_code;
}

static void _topo_add_dist(uint32_t *dist, int inx)
{
	int i;
	for (i = 0; i < switch_record_cnt; i++) {
		if (switch_record_table[inx].switches_dist[i] == INFINITE ||
		    dist[i] == INFINITE) {
			dist[i] = INFINITE;
		} else {
			dist[i] += switch_record_table[inx].switches_dist[i];
		}
	}
}

static int _topo_compare_switches(int i, int j, int rem_nodes,
				  int *switch_node_cnt, int rem_cpus,
				  uint32_t *switch_cpu_cnt)
{
	while (1) {
		bool i_fit = ((switch_node_cnt[i] >= rem_nodes) &&
			      (switch_cpu_cnt[i] >= rem_cpus));
		bool j_fit = ((switch_node_cnt[j] >= rem_nodes) &&
			      (switch_cpu_cnt[j] >= rem_cpus));
		if (i_fit && j_fit) {
			if (switch_node_cnt[i] < switch_node_cnt[j])
				return 1;
			if (switch_node_cnt[i] > switch_node_cnt[j])
				return -1;
			break;
		} else if (i_fit) {
			return 1;
		} else if (j_fit) {
			return -1;
		}

		if (((switch_record_table[i].parent != i) ||
		     (switch_record_table[j].parent != j)) &&
		    (switch_record_table[i].parent !=
		     switch_record_table[j].parent)) {
			i = switch_record_table[i].parent;
			j = switch_record_table[j].parent;
			continue;
		}

		break;
	}

	if (switch_node_cnt[i] > switch_node_cnt[j])
		return 1;
	if (switch_node_cnt[i] < switch_node_cnt[j])
		return -1;
	if (switch_record_table[i].level < switch_record_table[j].level)
		return 1;
	if (switch_record_table[i].level > switch_record_table[j].level)
		return -1;
	return 0;

}

static void _topo_choose_best_switch(uint32_t *dist, int *switch_node_cnt,
				     int rem_nodes, uint32_t *switch_cpu_cnt,
				     int rem_cpus, int i, int *best_switch)
{
	int tcs = 0;

	if (*best_switch == -1 || dist[i] == INFINITE || !switch_node_cnt[i]) {
		/*
		 * If first possibility
		 */
		if (switch_node_cnt[i] && dist[i] < INFINITE)
			*best_switch = i;
		return;
	}

	tcs = _topo_compare_switches(i, *best_switch, rem_nodes,
				     switch_node_cnt, rem_cpus, switch_cpu_cnt);
	if (((dist[i] < dist[*best_switch]) && (tcs >= 0)) ||
	    ((dist[i] == dist[*best_switch]) && (tcs > 0))) {
		/*
		 * If closer and fit request OR
		 * same distance and tightest fit (less resource waste)
		 */
		*best_switch = i;
	}
}

/* Allocate resources to job using a minimal leaf switch count */
static int _eval_nodes_topo(topology_eval_t *topo_eval)
{
	uint32_t *switch_cpu_cnt = NULL;	/* total CPUs on switch */
	bitstr_t **switch_node_bitmap = NULL;	/* nodes on this switch */
	bitstr_t **start_switch_node_bitmap = NULL;
	int       *switch_node_cnt = NULL;	/* total nodes on switch */
	int       *switch_required = NULL;	/* set if has required node */
	int *req_switch_required = NULL;
	bitstr_t  *avail_nodes_bitmap = NULL;	/* nodes on any switch */
	bitstr_t  *req_nodes_bitmap   = NULL;	/* required node bitmap */
	bitstr_t  *req2_nodes_bitmap  = NULL;	/* required+lowest prio nodes */
	bitstr_t  *best_nodes_bitmap  = NULL;	/* required+low prio nodes */
	bitstr_t *start_node_map = NULL;
	int i, j, rc = SLURM_SUCCESS;
	int best_cpu_cnt, best_node_cnt, req_node_cnt = 0;
	List best_gres = NULL;
	switch_record_t *switch_ptr;
	List node_weight_list = NULL;
	topo_weight_info_t *nw = NULL;
	ListIterator iter;
	node_record_t *node_ptr;
	int64_t rem_max_cpus, start_rem_max_cpus;
	int rem_cpus, start_rem_cpus, rem_nodes; /* remaining resources desired */
	int min_rem_nodes;	/* remaining resources desired */
	job_record_t *job_ptr = topo_eval->job_ptr;
	job_details_t *details_ptr = job_ptr->details;
	bool gres_per_job, requested, sufficient = false;
	uint16_t *avail_cpu_per_node = NULL;
	uint32_t *switches_dist= NULL;
	time_t time_waiting = 0;
	int top_switch_inx = -1;
	uint64_t top_switch_lowest_weight = 0;
	int prev_rem_nodes;
	avail_res_t **avail_res_array = topo_eval->avail_res_array;
	uint32_t min_nodes = topo_eval->min_nodes;
	uint32_t req_nodes = topo_eval->req_nodes;
	uint32_t org_max_nodes = topo_eval->max_nodes;

	topo_eval->avail_cpus = 0;

	if (job_ptr->req_switch) {
		time_t     time_now;
		time_now = time(NULL);
		if (job_ptr->wait4switch_start == 0)
			job_ptr->wait4switch_start = time_now;
		time_waiting = time_now - job_ptr->wait4switch_start;
	}

	rem_cpus = details_ptr->min_cpus;
	min_rem_nodes = min_nodes;
	if ((gres_per_job = gres_sched_init(job_ptr->gres_list_req)))
		rem_nodes = MIN(min_nodes, req_nodes);
	else
		rem_nodes = MAX(min_nodes, req_nodes);

	rem_max_cpus = eval_nodes_get_rem_max_cpus(details_ptr, rem_nodes);

	/* Validate availability of required nodes */
	if (job_ptr->details->req_node_bitmap) {
		if (!bit_super_set(job_ptr->details->req_node_bitmap,
				   topo_eval->node_map)) {
			info("%pJ requires nodes which are not currently available",
			      job_ptr);
			rc = SLURM_ERROR;
			goto fini;
		}

		req_node_cnt = bit_set_count(job_ptr->details->req_node_bitmap);
		if (req_node_cnt == 0) {
			info("%pJ required node list has no nodes",
			      job_ptr);
			rc = SLURM_ERROR;
			goto fini;
		}
		if (req_node_cnt > topo_eval->max_nodes) {
			info("%pJ requires more nodes than currently available (%u>%u)",
			      job_ptr, req_node_cnt,
			      topo_eval->max_nodes);
			rc = SLURM_ERROR;
			goto fini;
		}
		req_nodes_bitmap = job_ptr->details->req_node_bitmap;
	}

	/*
	 * Add required nodes to job allocation and
	 * build list of node bitmaps, sorted by weight
	 */
	if (!bit_set_count(topo_eval->node_map)) {
		debug("%pJ node_map is empty",
		      job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}
	avail_cpu_per_node = xcalloc(node_record_count, sizeof(uint16_t));
	node_weight_list = list_create(eval_nodes_topo_weight_free);
	for (i = 0; (node_ptr = next_node_bitmap(topo_eval->node_map, &i)); i++) {
		topo_weight_info_t nw_static;
		if (req_nodes_bitmap && bit_test(req_nodes_bitmap, i)) {
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			if (topo_eval->avail_cpus == 0) {
				debug2("%pJ insufficient resources on required node",
				       job_ptr);
				rc = SLURM_ERROR;
				goto fini;
			}
			avail_cpu_per_node[i] = topo_eval->avail_cpus;
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			rem_cpus   -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
		}

		nw_static.weight = node_ptr->sched_weight;
		nw = list_find_first(node_weight_list,
				     eval_nodes_topo_weight_find,
				     &nw_static);
		if (!nw) {	/* New node weight to add */
			nw = xmalloc(sizeof(topo_weight_info_t));
			nw->node_bitmap = bit_alloc(node_record_count);
			nw->weight = node_ptr->sched_weight;
			list_append(node_weight_list, nw);
		}
		bit_set(nw->node_bitmap, i);
		nw->node_cnt++;
	}

	list_sort(node_weight_list, eval_nodes_topo_weight_sort);
	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE)
		(void) list_for_each(node_weight_list,
				     eval_nodes_topo_weight_log, NULL);

	/*
	 * Identify the highest level switch to be used.
	 * Note that nodes can be on multiple non-overlapping switches.
	 */
	switch_cpu_cnt = xcalloc(switch_record_cnt, sizeof(uint32_t));
	switch_node_bitmap = xcalloc(switch_record_cnt, sizeof(bitstr_t *));
	start_switch_node_bitmap = xcalloc(switch_record_cnt, sizeof(bitstr_t *));
	switch_node_cnt    = xcalloc(switch_record_cnt, sizeof(int));
	switch_required    = xcalloc(switch_record_cnt, sizeof(int));
	req_switch_required = xcalloc(switch_record_cnt, sizeof(int));

	for (i = 0, switch_ptr = switch_record_table; i < switch_record_cnt;
	     i++, switch_ptr++) {
		uint32_t switch_cpus = 0;
		switch_node_bitmap[i] = bit_copy(switch_ptr->node_bitmap);
		bit_and(switch_node_bitmap[i], topo_eval->node_map);
		switch_node_cnt[i] = bit_set_count(switch_node_bitmap[i]);
		/*
		 * Count total CPUs of the intersection of node_map and
		 * switch_node_bitmap.
		 */
		for (j = 0; (node_ptr = next_node_bitmap(switch_node_bitmap[i],
							 &j));
		     j++)
			switch_cpus += avail_res_array[j]->avail_cpus;
		switch_cpu_cnt[i] = switch_cpus;
		if (req_nodes_bitmap &&
		    bit_overlap_any(req_nodes_bitmap, switch_node_bitmap[i])) {
			switch_required[i] = 1;
			if ((top_switch_inx == -1) ||
			    (switch_record_table[i].level >
			     switch_record_table[top_switch_inx].level)) {
				top_switch_inx = i;
			}
		}
		if (!eval_nodes_enough_nodes(switch_node_cnt[i], rem_nodes,
					     min_nodes, req_nodes) ||
		    (rem_cpus > switch_cpu_cnt[i]))
			continue;
		if (!req_nodes_bitmap &&
		    (nw = list_find_first(node_weight_list,
					  eval_nodes_topo_node_find,
					  switch_node_bitmap[i]))) {
			if ((top_switch_inx == -1) ||
			    ((switch_record_table[i].level >=
			      switch_record_table[top_switch_inx].level) &&
			     (nw->weight <= top_switch_lowest_weight))) {
				top_switch_inx = i;
				top_switch_lowest_weight = nw->weight;
			}
		}
	}

	if (!req_nodes_bitmap) {
		bit_clear_all(topo_eval->node_map);
	}

	/*
	 * Top switch is highest level switch containing all required nodes
	 * OR all nodes of the lowest scheduling weight
	 * OR -1 if can not identify top-level switch, which may be due to a
	 * disjoint topology and available nodes living on different switches.
	 */
	if (top_switch_inx == -1) {
		log_flag(SELECT_TYPE, "%pJ unable to identify top level switch",
			 job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}

	/* Check that all specificly required nodes are on shared network */
	if (req_nodes_bitmap &&
	    !bit_super_set(req_nodes_bitmap,
			   switch_node_bitmap[top_switch_inx])) {
		rc = SLURM_ERROR;
		info("%pJ requires nodes that do not have shared network",
		     job_ptr);
		goto fini;
	}

	/*
	 * Remove nodes from consideration that can not be reached from this
	 * top level switch.
	 */
	for (i = 0; i < switch_record_cnt; i++) {
		if (top_switch_inx != i) {
			  bit_and(switch_node_bitmap[i],
				  switch_node_bitmap[top_switch_inx]);
		}
	}

	start_rem_cpus = rem_cpus;
	start_rem_max_cpus = rem_max_cpus;
	if (req_nodes_bitmap) {
		bit_and(topo_eval->node_map, req_nodes_bitmap);
		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id)) {
			/* Required nodes completely satisfied the request */
			rc = SLURM_SUCCESS;
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			rc = SLURM_ERROR;
			log_flag(SELECT_TYPE, "%pJ requires nodes exceed maximum node limit",
				 job_ptr);
			goto fini;
		}
	}

	start_node_map = bit_copy(topo_eval->node_map);
	memcpy(req_switch_required, switch_required,
	       switch_record_cnt * sizeof(int));
	for (i = 0; i < switch_record_cnt; i++)
		start_switch_node_bitmap[i] = bit_copy(switch_node_bitmap[i]);

try_again:
	/*
	 * Identify the best set of nodes (i.e. nodes with the lowest weight,
	 * in addition to the required nodes) that can be used to satisfy the
	 * job request. All nodes must be on a common top-level switch. The
	 * logic here adds groups of nodes, all with the same weight, so we
	 * usually identify more nodes than required to satisfy the request.
	 * Later logic selects from those nodes to get the best topology.
	 */
	requested = false;
	best_node_cnt = 0;
	best_cpu_cnt = 0;
	best_nodes_bitmap = bit_alloc(node_record_count);
	iter = list_iterator_create(node_weight_list);
	while (!requested && (nw = list_next(iter))) {
		if (best_node_cnt > 0) {
			/*
			 * All of the lower priority nodes should be included
			 * in the job's allocation. Nodes from the next highest
			 * weight nodes are included only as needed.
			 */
			if (req2_nodes_bitmap)
				bit_or(req2_nodes_bitmap, best_nodes_bitmap);
			else
				req2_nodes_bitmap = bit_copy(best_nodes_bitmap);
		}

		if (!bit_set_count(nw->node_bitmap))
			continue;

		for (i = 0; (node_ptr = next_node_bitmap(nw->node_bitmap, &i));
		     i++) {
			if (req_nodes_bitmap && bit_test(req_nodes_bitmap, i))
				continue;	/* Required node */
			if (!bit_test(switch_node_bitmap[top_switch_inx], i))
				continue;
			eval_nodes_select_cores(topo_eval, i, min_rem_nodes);
			if (topo_eval->avail_cpus == 0) {
				bit_clear(nw->node_bitmap, i);
				continue;
			}
			bit_set(best_nodes_bitmap, i);
			avail_cpu_per_node[i] = topo_eval->avail_cpus;
			best_cpu_cnt += topo_eval->avail_cpus;
			best_node_cnt++;
			if (gres_per_job) {
				gres_sched_consec(
					&best_gres, job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list);
			}
		}

		if (!sufficient) {
			sufficient = (best_cpu_cnt >= rem_cpus) &&
				     eval_nodes_enough_nodes(
					     best_node_cnt, rem_nodes,
					     min_nodes, req_nodes);
			if (sufficient && gres_per_job) {
				sufficient = gres_sched_sufficient(
						job_ptr->gres_list_req,
						best_gres);
			}
		}
		requested = ((best_node_cnt >= rem_nodes) &&
			     (best_cpu_cnt >= rem_cpus) &&
			     (!gres_per_job ||
			      gres_sched_sufficient(job_ptr->gres_list_req,
						    best_gres)));
	}
	list_iterator_destroy(iter);

	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		char *gres_str = NULL, *gres_print = "";
		char *node_names;
		if (req_nodes_bitmap) {
			node_names = bitmap2node_name(req_nodes_bitmap);
			info("Required nodes:%s", node_names);
			xfree(node_names);
		}
		node_names = bitmap2node_name(best_nodes_bitmap);
		if (gres_per_job) {
			gres_str = gres_sched_str(best_gres);
			if (gres_str)
				gres_print = gres_str;
		}
		info("Best nodes:%s node_cnt:%d cpu_cnt:%d %s",
		     node_names, best_node_cnt, best_cpu_cnt, gres_print);
		xfree(node_names);
		xfree(gres_str);
	}
	if (!sufficient) {
		log_flag(SELECT_TYPE, "insufficient resources currently available for %pJ",
		      job_ptr);
		rc = SLURM_ERROR;
		goto fini;
	}

	/*
	 * Add lowest weight nodes. Treat similar to required nodes for the job.
	 * Job will still need to add some higher weight nodes later.
	 */
	if (req2_nodes_bitmap) {
		for (i = 0;
		     next_node_bitmap(req2_nodes_bitmap, &i) && (topo_eval->max_nodes > 0);
		     i++) {
			topo_eval->avail_cpus = avail_cpu_per_node[i];
			eval_nodes_cpus_to_use(topo_eval, i,
					       rem_max_cpus, min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			rem_cpus   -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
		}

		for (i = 0, switch_ptr = switch_record_table;
		     i < switch_record_cnt; i++, switch_ptr++) {
			if (switch_required[i])
				continue;
			if (bit_overlap_any(req2_nodes_bitmap,
					    switch_node_bitmap[i])) {
				switch_required[i] = 1;
			}
		}
		bit_or(topo_eval->node_map, req2_nodes_bitmap);

		if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
		    (!gres_per_job || gres_sched_test(job_ptr->gres_list_req,
						      job_ptr->job_id))) {
			/* Required nodes completely satisfied the request */
			error("Scheduling anomaly for %pJ",
			      job_ptr);
			rc = SLURM_SUCCESS;
			goto fini;
		}
		if (topo_eval->max_nodes <= 0) {
			rc = SLURM_ERROR;
			log_flag(SELECT_TYPE, "%pJ reached maximum node limit",
				 job_ptr);
			goto fini;
		}
	}

	/*
	 * Construct a set of switch array entries.
	 * Use the same indexes as switch_record_table in slurmctld.
	 */
	bit_or(best_nodes_bitmap, topo_eval->node_map);
	avail_nodes_bitmap = bit_alloc(node_record_count);
	for (i = 0, switch_ptr = switch_record_table; i < switch_record_cnt;
	     i++, switch_ptr++) {
		bit_and(switch_node_bitmap[i], best_nodes_bitmap);
		bit_or(avail_nodes_bitmap, switch_node_bitmap[i]);
		switch_node_cnt[i] = bit_set_count(switch_node_bitmap[i]);
	}

	if (slurm_conf.debug_flags & DEBUG_FLAG_SELECT_TYPE) {
		for (i = 0; i < switch_record_cnt; i++) {
			char *node_names = NULL;
			if (switch_node_cnt[i]) {
				node_names =
					bitmap2node_name(switch_node_bitmap[i]);
			}
			info("switch=%s level=%d nodes=%u:%s required:%u speed:%u",
			     switch_record_table[i].name,
			     switch_record_table[i].level,
			     switch_node_cnt[i], node_names,
			     switch_required[i],
			     switch_record_table[i].link_speed);
			xfree(node_names);
		}
	}

	/* Add additional resources for already required leaf switches */
	if (req_nodes_bitmap || req2_nodes_bitmap) {
		for (i = 0; i < switch_record_cnt; i++) {
			if (!switch_required[i] || !switch_node_bitmap[i] ||
			    (switch_record_table[i].level != 0))
				continue;
			for (j = 0; next_node_bitmap(switch_node_bitmap[i], &j);
			     j++) {
				if (bit_test(topo_eval->node_map, j) ||
				    !avail_cpu_per_node[j])
					continue;
				topo_eval->avail_cpus = avail_cpu_per_node[j];
				eval_nodes_cpus_to_use(topo_eval, i,
						       rem_max_cpus,
						       min_rem_nodes);
				if (gres_per_job) {
					gres_sched_add(
						job_ptr->gres_list_req,
						avail_res_array[j]->
						sock_gres_list,
						&topo_eval->avail_cpus);
				}
				rem_nodes--;
				min_rem_nodes--;
				topo_eval->max_nodes--;
				rem_cpus   -= topo_eval->avail_cpus;
				rem_max_cpus -= topo_eval->avail_cpus;
				bit_set(topo_eval->node_map, j);
				if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
				    (!gres_per_job ||
				     gres_sched_test(job_ptr->gres_list_req,
						     job_ptr->job_id))) {
					rc = SLURM_SUCCESS;
					goto fini;
				}
			}
		}
	}

	switches_dist = xcalloc(switch_record_cnt, sizeof(uint32_t));

	for (i = 0; i < switch_record_cnt; i++) {
		if (switch_required[i])
			_topo_add_dist(switches_dist, i);
	}
	/* Add additional resources as required from additional leaf switches */
	prev_rem_nodes = rem_nodes + 1;
	while (1) {
		int best_switch_inx = -1;
		if (prev_rem_nodes == rem_nodes)
			break; 	/* Stalled */
		prev_rem_nodes = rem_nodes;

		for (i = 0; i < switch_record_cnt; i++) {
			if (switch_required[i] || !switch_node_bitmap[i] ||
			    (switch_record_table[i].level != 0))
				continue;
			_topo_choose_best_switch(switches_dist, switch_node_cnt,
						 rem_nodes, switch_cpu_cnt,
						 rem_cpus, i, &best_switch_inx);

		}
		if (best_switch_inx == -1)
			break;

		_topo_add_dist(switches_dist, best_switch_inx);
		/*
		 * NOTE: Ideally we would add nodes in order of resource
		 * availability rather than in order of bitmap position, but
		 * that would add even more complexity and overhead.
		 */
		for (i = 0;
		     next_node_bitmap(
			     switch_node_bitmap[best_switch_inx], &i) &&
		     (topo_eval->max_nodes > 0);
		     i++) {
			if (bit_test(topo_eval->node_map, i) || !avail_cpu_per_node[i])
				continue;
			topo_eval->avail_cpus = avail_cpu_per_node[i];
			eval_nodes_cpus_to_use(topo_eval, i, rem_max_cpus,
					       min_rem_nodes);
			if (gres_per_job) {
				gres_sched_add(
					job_ptr->gres_list_req,
					avail_res_array[i]->sock_gres_list,
					&topo_eval->avail_cpus);
			}
			rem_nodes--;
			min_rem_nodes--;
			topo_eval->max_nodes--;
			rem_cpus   -= topo_eval->avail_cpus;
			rem_max_cpus -= topo_eval->avail_cpus;
			bit_set(topo_eval->node_map, i);
			if ((rem_nodes <= 0) && (rem_cpus <= 0) &&
			    (!gres_per_job ||
			     gres_sched_test(job_ptr->gres_list_req,
					     job_ptr->job_id))) {
				rc = SLURM_SUCCESS;
				goto fini;
			}
		}
		switch_node_cnt[best_switch_inx] = 0;	/* Used all */
	}
	if ((min_rem_nodes <= 0) && (rem_cpus <= 0) &&
	    (!gres_per_job ||
	     gres_sched_test(job_ptr->gres_list_req, job_ptr->job_id))) {
		rc = SLURM_SUCCESS;
		goto fini;
	}
	rc = SLURM_ERROR;

fini:
	if (job_ptr->req_switch > 0 && rc == SLURM_SUCCESS) {
		int leaf_switch_count = 0;

		/* Count up leaf switches. */
		for (i = 0, switch_ptr = switch_record_table;
		     i < switch_record_cnt; i++, switch_ptr++) {
			if (switch_record_table[i].level != 0)
				continue;
			if (bit_overlap_any(switch_node_bitmap[i], topo_eval->node_map))
				leaf_switch_count++;
		}
		if (time_waiting >= job_ptr->wait4switch) {
			job_ptr->best_switch = true;
			debug3("%pJ waited %ld sec for switches use=%d",
				job_ptr, time_waiting, leaf_switch_count);
		} else if (leaf_switch_count > job_ptr->req_switch) {
			/*
			 * Allocation is for more than requested number of
			 * switches.
			 */
			if ((req_nodes > min_nodes) && best_nodes_bitmap) {
				/* TRUE only for !gres_per_job */
				req_nodes--;
				rem_nodes = req_nodes;
				rem_nodes -= req_node_cnt;
				min_rem_nodes = min_nodes;
				min_rem_nodes -= req_node_cnt;
				topo_eval->max_nodes = org_max_nodes;
				topo_eval->max_nodes -= req_node_cnt;
				rem_cpus = start_rem_cpus;
				rem_max_cpus = start_rem_max_cpus;
				xfree(switches_dist);
				bit_copybits(topo_eval->node_map, start_node_map);
				memcpy(switch_required, req_switch_required,
				       switch_record_cnt * sizeof(int));
				memset(avail_cpu_per_node, 0,
				       node_record_count * sizeof(uint16_t));
				for (i = 0; i < switch_record_cnt; i++)
					bit_copybits(
						switch_node_bitmap[i],
						start_switch_node_bitmap[i]);
				FREE_NULL_BITMAP(avail_nodes_bitmap);
				FREE_NULL_BITMAP(req2_nodes_bitmap);
				FREE_NULL_BITMAP(best_nodes_bitmap);
				FREE_NULL_LIST(best_gres);
				log_flag(SELECT_TYPE, "%pJ goto try_again req_nodes %d",
					 job_ptr, req_nodes);
				goto try_again;
			}
			job_ptr->best_switch = false;
			debug3("%pJ waited %ld sec for switches=%u found=%d wait %u",
				job_ptr, time_waiting, job_ptr->req_switch,
				leaf_switch_count, job_ptr->wait4switch);
		} else {
			job_ptr->best_switch = true;
		}
	}

	FREE_NULL_LIST(best_gres);
	FREE_NULL_LIST(node_weight_list);
	FREE_NULL_BITMAP(avail_nodes_bitmap);
	FREE_NULL_BITMAP(req2_nodes_bitmap);
	FREE_NULL_BITMAP(best_nodes_bitmap);
	FREE_NULL_BITMAP(start_node_map);
	xfree(avail_cpu_per_node);
	xfree(switch_cpu_cnt);
	if (switch_node_bitmap) {
		for (i = 0; i < switch_record_cnt; i++)
			FREE_NULL_BITMAP(switch_node_bitmap[i]);
		xfree(switch_node_bitmap);
	}
	if (start_switch_node_bitmap) {
		for (i = 0; i < switch_record_cnt; i++)
			FREE_NULL_BITMAP(start_switch_node_bitmap[i]);
		xfree(start_switch_node_bitmap);
	}
	xfree(switch_node_cnt);
	xfree(switch_required);
	xfree(req_switch_required);
	xfree(switches_dist);
	return rc;
}

extern int eval_nodes(topology_eval_t *topo_eval)
{
	job_details_t *details_ptr = topo_eval->job_ptr->details;
	static bool pack_serial_at_end = false;
	static bool have_dragonfly = false;
	static bool topo_optional = false;

	static bool set = false;

	if (!set) {
		if (xstrcasestr(slurm_conf.sched_params, "pack_serial_at_end"))
			pack_serial_at_end = true;
		else
			pack_serial_at_end = false;
		if (xstrcasestr(slurm_conf.topology_param, "dragonfly"))
			have_dragonfly = true;
		if (xstrcasestr(slurm_conf.topology_param, "TopoOptional"))
			topo_optional = true;
		set = true;
	}

	xassert(topo_eval->node_map);
	if (bit_set_count(topo_eval->node_map) < topo_eval->min_nodes)
		return SLURM_ERROR;

	if ((details_ptr->req_node_bitmap) &&
	    (!bit_super_set(details_ptr->req_node_bitmap, topo_eval->node_map)))
		return SLURM_ERROR;

	if (blocks_nodes_bitmap &&
	    bit_overlap_any(blocks_nodes_bitmap, topo_eval->node_map))
		return _eval_nodes_block(topo_eval);

	if (topo_eval->job_ptr->bit_flags & SPREAD_JOB) {
		/* Spread the job out over many nodes */
		return _eval_nodes_spread(topo_eval);
	}

	if (topo_eval->prefer_alloc_nodes && !details_ptr->contiguous) {
		/*
		 * Select resource on busy nodes first in order to leave
		 * idle resources free for as long as possible so that longer
		 * running jobs can get more easily started by the backfill
		 * scheduler plugin
		 */
		return _eval_nodes_busy(topo_eval);
	}


	if ((topo_eval->cr_type & CR_LLN) ||
	    (topo_eval->job_ptr->part_ptr &&
	     (topo_eval->job_ptr->part_ptr->flags & PART_FLAG_LLN))) {
		/* Select resource on the Least Loaded Node */
		return _eval_nodes_lln(topo_eval);
	}

	if (pack_serial_at_end &&
	    (details_ptr->min_cpus == 1) && (topo_eval->req_nodes == 1)) {
		/*
		 * Put serial jobs at the end of the available node list
		 * rather than using a best-fit algorithm, which fragments
		 * resources.
		 */
		return _eval_nodes_serial(topo_eval);
	}

	if (switch_record_cnt && switch_record_table &&
	    !details_ptr->contiguous &&
	    ((topo_optional == false) || topo_eval->job_ptr->req_switch)) {
		/* Perform optimized resource selection based upon topology */
		if (have_dragonfly) {
			return _eval_nodes_dfly(topo_eval);
		} else {
			return _eval_nodes_topo(topo_eval);
		}
	}

	return _eval_nodes_consec(topo_eval);
}

extern void eval_nodes_cpus_to_use(topology_eval_t *topo_eval, int node_inx,
				   int64_t rem_max_cpus, int rem_nodes)
{
	job_record_t *job_ptr = topo_eval->job_ptr;
	job_details_t *details_ptr = job_ptr->details;
	avail_res_t *avail_res = topo_eval->avail_res_array[node_inx];
	int resv_cpus;	/* CPUs to be allocated on other nodes */

	if (details_ptr->whole_node == 1)	/* Use all resources on node */
		return;

	resv_cpus = MAX((rem_nodes - 1), 0);
	resv_cpus *= job_mgr_determine_cpus_per_core(details_ptr, node_inx);
	if (topo_eval->cr_type & CR_SOCKET)
		resv_cpus *= node_record_table_ptr[node_inx]->cores;
	rem_max_cpus -= resv_cpus;
	if (topo_eval->avail_cpus > rem_max_cpus) {
		topo_eval->avail_cpus = MAX(rem_max_cpus,
					    (int)details_ptr->pn_min_cpus);
		if (avail_res->gres_min_cpus)
			topo_eval->avail_cpus =
				MAX(topo_eval->avail_cpus,
				    avail_res->gres_min_cpus);
		else
			topo_eval->avail_cpus =
				MAX(topo_eval->avail_cpus,
				    details_ptr->min_gres_cpu);
		/* Round up CPU count to CPU in allocation unit (e.g. core) */
		avail_res->avail_cpus = topo_eval->avail_cpus;
	}
	avail_res->avail_res_cnt = avail_res->avail_cpus +
				   avail_res->avail_gpus;
}

extern void eval_nodes_select_cores(topology_eval_t *topo_eval,
				    int node_inx, int rem_nodes)
{
	bitstr_t **avail_core = topo_eval->avail_core;
	uint16_t *avail_cpus = &topo_eval->avail_cpus;
	avail_res_t **avail_res_array = topo_eval->avail_res_array;
	uint16_t cr_type = topo_eval->cr_type;
	bool enforce_binding = topo_eval->enforce_binding;
	bool first_pass = topo_eval->first_pass;
	job_record_t *job_ptr = topo_eval->job_ptr;
	gres_mc_data_t *mc_ptr = topo_eval->mc_ptr;

	uint32_t min_tasks_this_node = 0, max_tasks_this_node = 0;
	uint32_t min_cores_this_node = 0;
	job_details_t *details_ptr = job_ptr->details;
	node_record_t *node_ptr = node_record_table_ptr[node_inx];

	xassert(mc_ptr->cpus_per_task);

	rem_nodes = MIN(rem_nodes, 1);	/* If range of node counts */
	if (mc_ptr->ntasks_per_node) {
		min_tasks_this_node = mc_ptr->ntasks_per_node;
		max_tasks_this_node = mc_ptr->ntasks_per_node;
	} else if (mc_ptr->ntasks_per_board) {
		min_tasks_this_node = mc_ptr->ntasks_per_board;
		max_tasks_this_node = mc_ptr->ntasks_per_board *
				      node_ptr->boards;
	} else if (mc_ptr->ntasks_per_socket) {
		min_tasks_this_node = mc_ptr->ntasks_per_socket;
		max_tasks_this_node = mc_ptr->ntasks_per_socket *
				      node_ptr->tot_sockets;
	} else if (mc_ptr->ntasks_per_core) {
		min_tasks_this_node = mc_ptr->ntasks_per_core;
		max_tasks_this_node = mc_ptr->ntasks_per_core *
				      (node_ptr->tot_cores -
				       node_ptr->core_spec_cnt);
	} else if (details_ptr && details_ptr->ntasks_per_tres &&
		   (details_ptr->ntasks_per_tres != NO_VAL16)) {
		/* Node ranges not allowed with --ntasks-per-gpu */
		if ((details_ptr->min_nodes != NO_VAL) &&
		    (details_ptr->min_nodes != 0) &&
		    (details_ptr->min_nodes == details_ptr->max_nodes)) {
			min_tasks_this_node = details_ptr->num_tasks /
				details_ptr->min_nodes;
			max_tasks_this_node = min_tasks_this_node;
		} else {
			min_tasks_this_node = details_ptr->ntasks_per_tres;
			max_tasks_this_node = details_ptr->num_tasks;
		}
	} else if (details_ptr && (details_ptr->max_nodes == 1)) {
		if ((details_ptr->num_tasks == NO_VAL) ||
		    (details_ptr->num_tasks == 0)) {
			min_tasks_this_node = 1;
			max_tasks_this_node = NO_VAL;
		} else {
			min_tasks_this_node = details_ptr->num_tasks;
			max_tasks_this_node = details_ptr->num_tasks;
		}
	} else if (details_ptr &&
		   ((details_ptr->num_tasks == 1) ||
		    ((details_ptr->num_tasks == details_ptr->min_nodes) &&
		     (details_ptr->num_tasks == details_ptr->max_nodes)))) {
		min_tasks_this_node = 1;
		max_tasks_this_node = 1;
	} else {
		min_tasks_this_node = 1;
		max_tasks_this_node = NO_VAL;
	}
	/* Determine how many tasks can be started on this node */
	if ((!details_ptr || !details_ptr->overcommit)) {
		int alloc_tasks = avail_res_array[node_inx]->avail_cpus /
			      mc_ptr->cpus_per_task;
		if (alloc_tasks < min_tasks_this_node)
			max_tasks_this_node = 0;
		else if ((max_tasks_this_node == NO_VAL) ||
			 (alloc_tasks < max_tasks_this_node))
			max_tasks_this_node = alloc_tasks;
	}

	*avail_cpus = avail_res_array[node_inx]->avail_cpus;
	/*
	 * _allocate_sc() filters available cpus and cores if the job does
	 * not request gres. If the job requests gres, _allocate_sc() defers
	 * filtering cpus and cores so that gres_select_filter_sock_core() can
	 * do it.
	 */
	if (job_ptr->gres_list_req) {
		gres_filter_sock_core(
			job_ptr,
			mc_ptr,
			avail_res_array[node_inx]->sock_gres_list,
			avail_res_array[node_inx]->sock_cnt,
			node_ptr->cores, node_ptr->tpc, avail_cpus,
			&min_tasks_this_node, &max_tasks_this_node,
			&min_cores_this_node,
			rem_nodes, enforce_binding, first_pass,
			avail_core[node_inx],
			node_record_table_ptr[node_inx]->name,
			cr_type);
	}
	if (max_tasks_this_node == 0) {
		*avail_cpus = 0;
	} else if ((slurm_conf.select_type_param & CR_ONE_TASK_PER_CORE) &&
		   ((mc_ptr->ntasks_per_core == INFINITE16) ||
		    (mc_ptr->ntasks_per_core == 0)) &&
		   details_ptr && (details_ptr->min_gres_cpu == 0)) {
		*avail_cpus = bit_set_count(avail_core[node_inx]);
	}
	avail_res_array[node_inx]->gres_min_cpus =
		job_mgr_determine_cpus_per_core(job_ptr->details, node_inx) *
		min_cores_this_node;
	avail_res_array[node_inx]->gres_max_tasks = max_tasks_this_node;
}

extern int64_t eval_nodes_get_rem_max_cpus(
	job_details_t *details_ptr, int rem_nodes)
{
	int64_t rem_max_cpus = details_ptr->min_cpus;

	if (details_ptr->max_cpus != NO_VAL)
		rem_max_cpus = details_ptr->max_cpus;
	if (details_ptr->min_gres_cpu)
		rem_max_cpus = MAX(rem_max_cpus,
				   details_ptr->min_gres_cpu * rem_nodes);
	if (details_ptr->min_job_gres_cpu)
		rem_max_cpus = MAX(rem_max_cpus, details_ptr->min_job_gres_cpu);

	return rem_max_cpus;

}

extern int eval_nodes_topo_weight_find(void *x, void *key)
{
	topo_weight_info_t *nw = (topo_weight_info_t *) x;
	topo_weight_info_t *nw_key = (topo_weight_info_t *) key;
	if (nw->weight == nw_key->weight)
		return 1;
	return 0;
}

extern int eval_nodes_topo_node_find(void *x, void *key)
{
	topo_weight_info_t *nw = (topo_weight_info_t *) x;
	bitstr_t *nw_key = (bitstr_t *) key;
	if (bit_overlap_any(nw->node_bitmap, nw_key))
		return 1;
	return 0;
}

extern void eval_nodes_topo_weight_free(void *x)
{
	topo_weight_info_t *nw = (topo_weight_info_t *) x;
	FREE_NULL_BITMAP(nw->node_bitmap);
	xfree(nw);
}

extern int eval_nodes_topo_weight_log(void *x, void *arg)
{
	topo_weight_info_t *nw = (topo_weight_info_t *) x;
	char *node_names = bitmap2node_name(nw->node_bitmap);
	info("Topo:%s weight:%"PRIu64, node_names, nw->weight);
	xfree(node_names);
	return 0;
}

extern int eval_nodes_topo_weight_sort(void *x, void *y)
{
	topo_weight_info_t *nwt1 = *(topo_weight_info_t **) x;
	topo_weight_info_t *nwt2 = *(topo_weight_info_t **) y;
	if (nwt1->weight < nwt2->weight)
		return -1;
	if (nwt1->weight > nwt2->weight)
		return 1;
	return 0;
}

extern bool eval_nodes_enough_nodes(int avail_nodes, int rem_nodes,
				    uint32_t min_nodes, uint32_t req_nodes)
{
	int needed_nodes;

	if (req_nodes > min_nodes)
		needed_nodes = rem_nodes + min_nodes - req_nodes;
	else
		needed_nodes = rem_nodes;

	return (avail_nodes >= needed_nodes);
}
