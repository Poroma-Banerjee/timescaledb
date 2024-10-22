/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#pragma once

#include <postgres.h>

#include <nodes/execnodes.h>

#include "function/functions.h"
#include "grouping_policy.h"

typedef struct
{
	VectorAggFunctions func;
	int input_offset;
	int output_offset;
	List *filter_clauses;
	uint64 *filter_result;
} VectorAggDef;

typedef struct
{
	int input_offset;
	int output_offset;

	Oid typid;
	int16 value_bytes;
	bool by_value;
} GroupingColumn;

typedef struct
{
	CustomScanState custom;

	List *agg_defs;

	List *output_grouping_columns;

	/*
	 * We can't call the underlying scan after it has ended, or it will be
	 * restarted. This is the behavior of Postgres heap scans. So we have to
	 * track whether it has ended to avoid this.
	 */
	bool input_ended;

	GroupingPolicy *grouping;
} VectorAggState;

extern Node *vector_agg_state_create(CustomScan *cscan);
