/**
 * Copyright (C) 2020      Mellanox Technologies Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define __STDC_LIMIT_MACROS
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "env.h"
#include "mkey.h"
#include "mkey_old.h"


#define DATA_SIZE 4096

template<typename Qp, typename RdmaOp, typename Mkey, uint16_t MaxEntries = 1>
struct _mkey_test_basic : public mkey_test_base<Qp> {
	Mkey src_mkey;
	Mkey dst_mkey;
	RdmaOp rdma_op;

	_mkey_test_basic() :
		src_mkey(*this, this->src_side.pd, MaxEntries, MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
			 MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE),
		dst_mkey(*this, this->dst_side.pd, MaxEntries, MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
			 MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE) {}

	virtual void SetUp() override {
		mkey_test_base<Qp>::SetUp();
		EXEC(src_mkey.init());
		EXEC(dst_mkey.init());
	}

	void fill_data() {
		uint8_t src_buf[DATA_SIZE];
		memset(src_buf, 0xA5, DATA_SIZE/2);
		memset(src_buf+DATA_SIZE/2, 0x5A, DATA_SIZE/2);
		src_mkey.layout->set_data(src_buf, DATA_SIZE);
	}

	void check_data() {
		uint8_t src_buf[DATA_SIZE];
		memset(src_buf, 0xA5, DATA_SIZE/2);
		memset(src_buf+DATA_SIZE/2, 0x5A, DATA_SIZE/2);
		uint8_t dst_buf[DATA_SIZE];
		size_t data_len;

		data_len = src_mkey.layout->data_length();
		ASSERT_LE(data_len, (size_t)DATA_SIZE);

		dst_mkey.layout->get_data(dst_buf, data_len);
		ASSERT_EQ(0, memcmp(src_buf, dst_buf, data_len));
	}

	void configure_mkeys() {
		auto &src_side = this->src_side;
		auto &dst_side = this->dst_side;
		dst_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE);
		EXEC(dst_mkey.configure(dst_side.qp));
		EXEC(dst_side.cq.poll());

		src_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE);
		EXEC(src_mkey.configure(src_side.qp));
		EXEC(src_side.cq.poll());
	}

	void check_mkeys() {
		EXEC(src_mkey.check());
		EXEC(dst_mkey.check());
	}

	void execute_rdma() {
		auto &src_side = this->src_side;
		auto &dst_side = this->dst_side;
		EXEC(rdma_op.submit(src_side, src_mkey.sge(),
				    dst_side, dst_mkey.sge()));
		EXEC(rdma_op.complete(src_side, dst_side));
	}
};

template<typename T_Qp, template<typename> typename T_RdmaOp, typename T_Mkey, uint16_t T_MaxEntries = 1>
struct types {
	typedef T_Qp Qp;
	typedef T_RdmaOp<T_Qp> RdmaOp;
	typedef T_Mkey Mkey;
	static constexpr uint64_t MaxEntries = T_MaxEntries;
};

// Basic test suite that should pass for all or almost all mkeys

template<typename T>
using mkey_test_basic = _mkey_test_basic<typename T::Qp, typename T::RdmaOp, typename T::Mkey, T::MaxEntries>;

TYPED_TEST_CASE_P(mkey_test_basic);

TYPED_TEST_P(mkey_test_basic, basic) {
	/* @todo: do we need to check for signature here? */
	CHK_SUT(dv_sig);

	EXEC(fill_data());
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// src_mkey.layout->dump(0, 0, "SRC");
	// dst_mkey.layout->dump(0, 0, "DST");
	EXEC(check_mkeys());
	EXEC(check_data());
}

TYPED_TEST_P(mkey_test_basic, non_signaled) {
	CHK_SUT(dv_sig);
	auto &src_side = this->src_side;
	auto &dst_side = this->dst_side;

	EXEC(fill_data());
	dst_side.qp.wr_flags(IBV_SEND_INLINE);
	EXEC(dst_mkey.configure(dst_side.qp));

	src_side.qp.wr_flags(IBV_SEND_INLINE);
	EXEC(src_mkey.configure(src_side.qp));

	EXEC(execute_rdma());
	EXEC(check_mkeys());
	EXEC(check_data());
}

TYPED_TEST_P(mkey_test_basic, non_inline) {
	CHK_SUT(dv_sig);
	// @todo: remove skip when inline is implemented
	SKIP(1);

	auto &src_side = this->src_side;
	auto &dst_side = this->dst_side;

	EXEC(fill_data());
	dst_side.qp.wr_flags(IBV_SEND_SIGNALED);
	EXEC(dst_mkey.configure(dst_side.qp));
	EXEC(dst_side.cq.poll());

	src_side.qp.wr_flags(IBV_SEND_SIGNALED);
	EXEC(src_mkey.configure(src_side.qp));
	EXEC(src_side.cq.poll());

	EXEC(execute_rdma());
	EXEC(check_mkeys());
	EXEC(check_data());
}

REGISTER_TYPED_TEST_CASE_P(mkey_test_basic, basic, non_signaled, non_inline);

template<typename ...Setters>
using mkey_dv_new_basic = mkey_dv_new<mkey_access_flags<>, Setters...>;

typedef testing::Types<
	types<ibvt_qp_dv, rdma_op_read, mkey_dv_new_basic<mkey_layout_new_list_mrs<DATA_SIZE>>>,
	types<ibvt_qp_dv, rdma_op_read, mkey_dv_new_basic<mkey_layout_new_list_mrs<DATA_SIZE/4, DATA_SIZE/4, DATA_SIZE/4, DATA_SIZE/4>>>,
	types<ibvt_qp_dv, rdma_op_read, mkey_dv_new_basic<mkey_layout_new_list_mrs<DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8>>, 8>,
	types<ibvt_qp_dv, rdma_op_read, mkey_dv_new_basic<mkey_layout_new_interleaved_mrs<1, DATA_SIZE, 0>>>,
	types<ibvt_qp_dv, rdma_op_read, mkey_dv_new_basic<mkey_layout_new_interleaved_mrs<2, DATA_SIZE/4, 8, 4, 0>>>,
	types<ibvt_qp_dv, rdma_op_read, mkey_dv_new_basic<mkey_layout_new_interleaved_mrs<4, DATA_SIZE/32, 8, 4, 0, DATA_SIZE/32, 8, 4, 0, DATA_SIZE/32, 8, 4, 0, DATA_SIZE/32, 8, 4, 0>>, 9>,
	types<ibvt_qp_dv, rdma_op_read, mkey_dv_old<mkey_layout_old_list_mrs<DATA_SIZE>>>,
	types<ibvt_qp_dv, rdma_op_read, mkey_dv_old<mkey_layout_old_interleaved_mrs<1, DATA_SIZE, 0>>>
	> mkey_test_list_layouts;
INSTANTIATE_TYPED_TEST_CASE_P(layouts, mkey_test_basic, mkey_test_list_layouts);

typedef testing::Types<
	types<ibvt_qp_dv, rdma_op_read, mkey_dv_new_basic<mkey_layout_new_list_mrs<DATA_SIZE>>>,
	types<ibvt_qp_dv, rdma_op_write, mkey_dv_new_basic<mkey_layout_new_list_mrs<DATA_SIZE>>>,
	types<ibvt_qp_dv, rdma_op_send, mkey_dv_new_basic<mkey_layout_new_list_mrs<DATA_SIZE>>>
	> mkey_test_list_ops;
INSTANTIATE_TYPED_TEST_CASE_P(operations, mkey_test_basic, mkey_test_list_ops);

typedef mkey_test_base<ibvt_qp_dv> mkey_test_dv_custom;

TEST_F(mkey_test_dv_custom, basicAttr_badAccessFlags) {
	// Remote read is not allowed from source mkey
	mkey_dv_new<mkey_access_flags<IBV_ACCESS_LOCAL_WRITE>,
		    mkey_layout_new_list_mrs<DATA_SIZE>> src_mkey(*this,
								  this->src_side.pd,
								  1, MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT);
	mkey_dv_new<mkey_access_flags<>,
		    mkey_layout_new_list_mrs<DATA_SIZE>> dst_mkey(*this,
								  this->dst_side.pd,
								  1, MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT);
	rdma_op_read<ibvt_qp_dv> rdma_op;

	EXECL(src_mkey.init());
	EXECL(dst_mkey.init());

	this->dst_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE);
	EXECL(dst_mkey.configure(this->dst_side.qp));
	EXEC(dst_side.cq.poll());

	this->src_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE);
	EXECL(src_mkey.configure(this->src_side.qp));
	EXEC(src_side.cq.poll());

	EXECL(rdma_op.submit(this->src_side, src_mkey.sge(), this->dst_side, dst_mkey.sge()));
	EXECL(rdma_op.complete(this->src_side, this->dst_side, IBV_WC_SUCCESS, IBV_WC_REM_ACCESS_ERR));
}

TEST_F(mkey_test_dv_custom, basicAttr_listLayoutEntriesOverflow) {
	// input SGL exceeds the max entries (1 is aligned to 4)
	mkey_dv_new<mkey_access_flags<>,
		    mkey_layout_new_list_mrs<DATA_SIZE / 8, DATA_SIZE / 8,
					     DATA_SIZE / 8, DATA_SIZE / 8,
					     DATA_SIZE / 8> >
		src_mkey(*this, this->src_side.pd, 1,
			 MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT);

	EXECL(src_mkey.init());
	EXECL(src_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE));
	EXEC(src_side.qp.wr_start());
	EXECL(src_mkey.wr_configure(this->src_side.qp));
	EXEC(src_side.qp.wr_complete(ENOMEM));
}

TEST_F(mkey_test_dv_custom, basicAttr_interleavedLayoutEntriesOverflow) {
	// input SGL exceeds the max entries (1 is aligned to 4)
	mkey_dv_new<mkey_access_flags<>, mkey_valid,
		    mkey_layout_new_interleaved_mrs<4, DATA_SIZE / 32, 1, 4, 0,
						    DATA_SIZE / 32, 2, 4, 0,
						    DATA_SIZE / 32, 3> >
		src_mkey(*this, this->src_side.pd, 1,
			 MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT);

	EXECL(src_mkey.init());
	EXECL(src_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE));
	EXEC(src_side.qp.wr_start());
	EXECL(src_mkey.wr_configure(this->src_side.qp));
	EXEC(src_side.qp.wr_complete(ENOMEM));
}
