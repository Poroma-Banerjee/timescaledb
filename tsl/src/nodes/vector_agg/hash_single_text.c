/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

/*
 * Implementation of column hashing for a single text column.
 */

#include <postgres.h>

#include <common/hashfn.h>

#include "bytes_view.h"
#include "compression/arrow_c_data_interface.h"
#include "grouping_policy_hash.h"
#include "nodes/decompress_chunk/compressed_batch.h"
#include "nodes/vector_agg/exec.h"

#include "import/umash.h"

struct hash_table_key
{
	uint32 hash;
	uint64 rest;
} __attribute__((packed));

#define UMASH
#define HASH_TABLE_KEY_TYPE struct hash_table_key
#define KEY_HASH(X) (X.hash)
#define KEY_EQUAL(a, b) (a.hash == b.hash && a.rest == b.rest)

static BytesView
get_bytes_view(CompressedColumnValues *column_values, int arrow_row)
{
	const uint32 start = ((uint32 *) column_values->buffers[1])[arrow_row];
	const int32 value_bytes = ((uint32 *) column_values->buffers[1])[arrow_row + 1] - start;
	Assert(value_bytes >= 0);

	return (BytesView){ .len = value_bytes, .data = &((uint8 *) column_values->buffers[2])[start] };
}

static pg_attribute_always_inline void
single_text_get_key(HashingConfig config, int row, void *restrict output_key_ptr,
					void *restrict hash_table_key_ptr, bool *restrict valid)
{
	Assert(config.policy->num_grouping_columns == 1);

	BytesView *restrict output_key = (BytesView *) output_key_ptr;
	HASH_TABLE_KEY_TYPE *restrict hash_table_key = (HASH_TABLE_KEY_TYPE *) hash_table_key_ptr;

	if (unlikely(config.single_key.decompression_type == DT_Scalar))
	{
		/* Already stored. */
		output_key->len = VARSIZE_ANY_EXHDR(*config.single_key.output_value);
		output_key->data = (const uint8 *) VARDATA_ANY(*config.single_key.output_value);
		*valid = !*config.single_key.output_isnull;
	}
	else if (config.single_key.decompression_type == DT_ArrowText)
	{
		*output_key = get_bytes_view(&config.single_key, row);
		*valid = arrow_row_is_valid(config.single_key.buffers[0], row);
	}
	else if (config.single_key.decompression_type == DT_ArrowTextDict)
	{
		const int16 index = ((int16 *) config.single_key.buffers[3])[row];
		*output_key = get_bytes_view(&config.single_key, index);
		*valid = arrow_row_is_valid(config.single_key.buffers[0], row);
	}
	else
	{
		pg_unreachable();
	}

	DEBUG_PRINT("%p consider key row %d key index %d is %d bytes: ",
				policy,
				row,
				policy->last_used_key_index + 1,
				output_key->len);
	for (size_t i = 0; i < output_key->len; i++)
	{
		DEBUG_PRINT("%.2x.", output_key->data[i]);
	}
	DEBUG_PRINT("\n");

	struct umash_fp fp = umash_fprint(config.policy->umash_params,
									  /* seed = */ -1ull,
									  output_key->data,
									  output_key->len);
	hash_table_key->hash = fp.hash[0] & (~(uint32) 0);
	hash_table_key->rest = fp.hash[1];
}

static pg_attribute_always_inline HASH_TABLE_KEY_TYPE
single_text_store_output_key(GroupingPolicyHash *restrict policy, uint32 new_key_index,
							 BytesView output_key, HASH_TABLE_KEY_TYPE hash_table_key)
{
	const int total_bytes = output_key.len + VARHDRSZ;
	text *restrict stored =
		(text *) MemoryContextAlloc(policy->strategy.key_body_mctx, total_bytes);
	SET_VARSIZE(stored, total_bytes);
	memcpy(VARDATA(stored), output_key.data, output_key.len);
	output_key.data = (uint8 *) VARDATA(stored);
	policy->strategy.output_keys[new_key_index] = PointerGetDatum(stored);
	return hash_table_key;
}

/*
 * We use the standard single-key key output functions.
 */
#define EXPLAIN_NAME "single text"
#define KEY_VARIANT single_text
#define OUTPUT_KEY_TYPE BytesView

#include "hash_single_output_key_helper.c"

/*
 * We use a special batch preparation function to sometimes hash the dictionary-
 * encoded column using the dictionary.
 */

#define USE_DICT_HASHING

static pg_attribute_always_inline void single_text_dispatch_for_config(HashingConfig config,
																	   int start_row, int end_row);

static void
single_text_prepare_for_batch(GroupingPolicyHash *policy, DecompressBatchState *batch_state)
{
	/*
	 * Allocate the key storage.
	 */
	single_text_alloc_output_keys(policy, batch_state);

	/*
	 * Determine whether we're going to use the dictionary for hashing.
	 */
	policy->use_key_index_for_dict = false;

	Assert(policy->num_grouping_columns == 1);

	HashingConfig config = build_hashing_config(policy, batch_state);

	if (config.single_key.decompression_type != DT_ArrowTextDict)
	{
		return;
	}

	const int dict_rows = config.single_key.arrow->dictionary->length;
	if ((size_t) dict_rows >
		arrow_num_valid(batch_state->vector_qual_result, batch_state->total_batch_rows))
	{
		return;
	}
	/*
	 * Remember which
	 * aggregation states have already existed, and which we have to
	 * initialize. State index zero is invalid.
	 */
	const uint32 first_initialized_key_index = policy->last_used_key_index;

	/*
	 * Initialize the array for storing the aggregate state offsets corresponding
	 * to a given batch row. We don't need the offsets for the previous batch
	 * that are currently stored there, so we don't need to use repalloc.
	 */
	if ((size_t) dict_rows > policy->num_key_index_for_dict)
	{
		if (policy->key_index_for_dict != NULL)
		{
			pfree(policy->key_index_for_dict);
		}
		policy->num_key_index_for_dict = dict_rows;
		policy->key_index_for_dict =
			palloc(sizeof(policy->key_index_for_dict[0]) * policy->num_key_index_for_dict);
	}

	/*
	 * We shouldn't add the dictionary entries that are not used by any mathching
	 * rows. Translate the batch filter bitmap to dictionary rows.
	 */
	const int batch_rows = batch_state->total_batch_rows;
	const uint64 *row_filter = batch_state->vector_qual_result;
	if (batch_state->vector_qual_result != NULL)
	{
		uint64 *restrict dict_filter = policy->tmp_filter;
		const size_t dict_words = (dict_rows + 63) / 64;
		memset(dict_filter, 0, sizeof(*dict_filter) * dict_words);

		bool *restrict tmp = (bool *) policy->key_index_for_dict;
		Assert(sizeof(*tmp) <= sizeof(*policy->key_index_for_dict));
		memset(tmp, 0, sizeof(*tmp) * dict_rows);

		int outer;
		for (outer = 0; outer < batch_rows / 64; outer++)
		{
#define INNER_LOOP(INNER_MAX)                                                                      \
	const uint64 word = row_filter[outer];                                                         \
	for (int inner = 0; inner < INNER_MAX; inner++)                                                \
	{                                                                                              \
		const int16 index = ((int16 *) config.single_key.buffers[3])[outer * 64 + inner];          \
		tmp[index] = tmp[index] || (word & (1ull << inner));                                       \
	}

			INNER_LOOP(64)
		}

		if (batch_rows % 64)
		{
			INNER_LOOP(batch_rows % 64)
		}
#undef INNER_LOOP

		for (outer = 0; outer < dict_rows / 64; outer++)
		{
#define INNER_LOOP(INNER_MAX)                                                                      \
	uint64 word = 0;                                                                               \
	for (int inner = 0; inner < INNER_MAX; inner++)                                                \
	{                                                                                              \
		word |= (tmp[outer * 64 + inner] ? 1ull : 0ull) << inner;                                  \
	}                                                                                              \
	dict_filter[outer] = word;

			INNER_LOOP(64)
		}
		if (dict_rows % 64)
		{
			INNER_LOOP(dict_rows % 64)
		}
#undef INNER_LOOP

		config.batch_filter = dict_filter;
	}
	else
	{
		config.batch_filter = NULL;
	}

	/*
	 * The dictionary contains no null entries, so we will be adding the null
	 * key separately. Determine if we have any null key that also passes the
	 * batch filter.
	 */
	bool have_null_key = false;
	if (batch_state->vector_qual_result != NULL)
	{
		if (config.single_key.arrow->null_count > 0)
		{
			Assert(config.single_key.buffers[0] != NULL);
			const size_t batch_words = (batch_rows + 63) / 64;
			for (size_t i = 0; i < batch_words; i++)
			{
				have_null_key =
					have_null_key ||
					(row_filter[i] & (~((uint64 *) config.single_key.buffers[0])[i])) != 0;
			}
		}
	}
	else
	{
		if (config.single_key.arrow->null_count > 0)
		{
			Assert(config.single_key.buffers[0] != NULL);
			have_null_key = true;
		}
	}

	/*
	 * Build key indexes for the dictionary entries as for normal non-nullable
	 * text values.
	 */
	Assert(config.single_key.decompression_type = DT_ArrowTextDict);
	config.single_key.decompression_type = DT_ArrowText;
	config.single_key.buffers[0] = NULL;

	Assert((size_t) dict_rows <= policy->num_key_index_for_dict);
	config.result_key_indexes = policy->key_index_for_dict;
	memset(policy->key_index_for_dict, 0, sizeof(*policy->key_index_for_dict) * dict_rows);

	single_text_dispatch_for_config(config, 0, dict_rows);

	/*
	 * The dictionary doesn't store nulls, so add the null key separately if we
	 * have one.
	 *
	 * FIXME doesn't respect nulls last/first in GroupAggregate. Add a test.
	 */
	if (have_null_key && policy->null_key_index == 0)
	{
		policy->null_key_index = ++policy->last_used_key_index;
		policy->strategy.output_keys[policy->null_key_index] = PointerGetDatum(NULL);
	}

	policy->use_key_index_for_dict = true;

	/*
	 * Initialize the new keys if we added any.
	 */
	if (policy->last_used_key_index > first_initialized_key_index)
	{
		const uint64 new_aggstate_rows = policy->num_agg_state_rows * 2 + 1;
		const int num_fns = policy->num_agg_defs;
		for (int i = 0; i < num_fns; i++)
		{
			const VectorAggDef *agg_def = &policy->agg_defs[i];
			if (policy->last_used_key_index >= policy->num_agg_state_rows)
			{
				policy->per_agg_states[i] = repalloc(policy->per_agg_states[i],
													 new_aggstate_rows * agg_def->func.state_bytes);
			}

			/*
			 * Initialize the aggregate function states for the newly added keys.
			 */
			void *first_uninitialized_state =
				agg_def->func.state_bytes * (first_initialized_key_index + 1) +
				(char *) policy->per_agg_states[i];
			agg_def->func.agg_init(first_uninitialized_state,
								   policy->last_used_key_index - first_initialized_key_index);
		}

		/*
		 * Record the newly allocated number of rows in case we had to reallocate.
		 */
		if (policy->last_used_key_index >= policy->num_agg_state_rows)
		{
			Assert(new_aggstate_rows > policy->num_agg_state_rows);
			policy->num_agg_state_rows = new_aggstate_rows;
		}
	}

	DEBUG_PRINT("computed the dict offsets\n");
}

static pg_attribute_always_inline void
single_text_offsets_translate_impl(HashingConfig config, int start_row, int end_row)
{
	GroupingPolicyHash *policy = config.policy;
	Assert(policy->use_key_index_for_dict);

	uint32 *restrict indexes_for_rows = config.result_key_indexes;
	uint32 *restrict indexes_for_dict = policy->key_index_for_dict;

	for (int row = start_row; row < end_row; row++)
	{
		const bool row_valid = arrow_row_is_valid(config.single_key.buffers[0], row);
		const int16 dict_index = ((int16 *) config.single_key.buffers[3])[row];

		if (row_valid)
		{
			indexes_for_rows[row] = indexes_for_dict[dict_index];
		}
		else
		{
			indexes_for_rows[row] = policy->null_key_index;
		}

		Assert(indexes_for_rows[row] != 0 || !arrow_row_is_valid(config.batch_filter, row));
	}
}

#define APPLY_FOR_VALIDITY(X, NAME, COND)                                                          \
	X(NAME##_notnull, (COND) && (config.single_key.buffers[0] == NULL))                            \
	X(NAME##_nullable, (COND) && (config.single_key.buffers[0] != NULL))

#define APPLY_FOR_SPECIALIZATIONS(X) APPLY_FOR_VALIDITY(X, single_text_offsets_translate, true)

#define DEFINE(NAME, CONDITION)                                                                    \
	static pg_noinline void NAME(HashingConfig config, int start_row, int end_row)                 \
	{                                                                                              \
		if (!(CONDITION))                                                                          \
		{                                                                                          \
			pg_unreachable();                                                                      \
		}                                                                                          \
                                                                                                   \
		single_text_offsets_translate_impl(config, start_row, end_row);                            \
	}

APPLY_FOR_SPECIALIZATIONS(DEFINE)

#undef DEFINE

static void
single_text_offsets_translate(HashingConfig config, int start_row, int end_row)
{
#define DISPATCH(NAME, CONDITION)                                                                  \
	if (CONDITION)                                                                                 \
	{                                                                                              \
		NAME(config, start_row, end_row);                                                          \
	}                                                                                              \
	else

	APPLY_FOR_SPECIALIZATIONS(DISPATCH) { pg_unreachable(); }
#undef DISPATCH
}

#undef APPLY_FOR_SPECIALIZATIONS
#undef APPLY_FOR_VALIDITY
#undef APPLY_FOR_BATCH_FILTER

#include "hash_table_functions_impl.c"
