/*
 * Copyright 2020 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#ifndef _DCN30_RESOURCE_H_
#define _DCN30_RESOURCE_H_

#include "core_types.h"

#define TO_DCN30_RES_POOL(pool)\
	container_of(pool, struct dcn30_resource_pool, base)

struct dc;
struct resource_pool;
struct _vcs_dpi_display_pipe_params_st;

struct dcn30_resource_pool {
	struct resource_pool base;
};
struct resource_pool *dcn30_create_resource_pool(
		const struct dc_init_data *init_data,
		struct dc *dc);

void dcn30_set_mcif_arb_params(
		struct dc *dc,
		struct dc_state *context,
		display_e2e_pipe_params_st *pipes,
		int pipe_cnt);

unsigned int dcn30_calc_max_scaled_time(
		unsigned int time_per_pixel,
		enum mmhubbub_wbif_mode mode,
		unsigned int urgent_watermark);

bool dcn30_validate_bandwidth(struct dc *dc, struct dc_state *context,
		bool fast_validate);
void dcn30_populate_dml_writeback_from_context(
		struct dc *dc, struct resource_context *res_ctx, display_e2e_pipe_params_st *pipes);

int dcn30_populate_dml_pipes_from_context(
	struct dc *dc, struct dc_state *context,
	display_e2e_pipe_params_st *pipes);

bool dcn30_acquire_post_bldn_3dlut(
		struct resource_context *res_ctx,
		const struct resource_pool *pool,
		int mpcc_id,
		struct dc_3dlut **lut,
		struct dc_transfer_func **shaper);

bool dcn30_release_post_bldn_3dlut(
		struct resource_context *res_ctx,
		const struct resource_pool *pool,
		struct dc_3dlut **lut,
		struct dc_transfer_func **shaper);

enum dc_status dcn30_add_stream_to_ctx(
		struct dc *dc,
		struct dc_state *new_ctx,
		struct dc_stream_state *dc_stream);

void dcn30_update_bw_bounding_box(struct dc *dc, struct clk_bw_params *bw_params);

#endif /* _DCN30_RESOURCE_H_ */
