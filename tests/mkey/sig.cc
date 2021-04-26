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

#if HAVE_DECL_MLX5DV_WR_MKEY_CONFIGURE

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

#define DATA_SIZE 4096

template<typename SrcSigBlock, typename SrcValue,
	 typename DstSigBlock, typename DstValue,
	 uint32_t NumBlocks = 1, 
	 typename Qp = ibvt_qp_dv<>, 
	 typename RdmaOp = rdma_op_read>
struct _mkey_test_sig_block : public mkey_test_base<Qp> {
	static constexpr uint32_t src_block_size = SrcSigBlock::MemDomainType::BlockSizeType::block_size;
	static constexpr uint32_t src_sig_size = SrcSigBlock::MemDomainType::SigType::sig_size;
	static constexpr uint32_t src_ext_block_size = src_block_size + src_sig_size;
	static constexpr uint32_t src_total_size = NumBlocks * src_ext_block_size;
	static constexpr uint32_t dst_block_size = DstSigBlock::MemDomainType::BlockSizeType::block_size;
	static constexpr uint32_t dst_sig_size = DstSigBlock::MemDomainType::SigType::sig_size;
	static constexpr uint32_t dst_ext_block_size = dst_block_size + dst_sig_size;
	static constexpr uint32_t dst_total_size = NumBlocks * dst_ext_block_size;

	struct mkey_dv_new<mkey_access_flags<>,
			   mkey_layout_new_list_fixed_mrs<src_ext_block_size, NumBlocks>,
			   SrcSigBlock> src_mkey;
	struct mkey_dv_new<mkey_access_flags<>,
			   mkey_layout_new_list_fixed_mrs<dst_ext_block_size, NumBlocks>,
			   DstSigBlock> dst_mkey;
	RdmaOp rdma_op;

	_mkey_test_sig_block() :
		src_mkey(*this, this->src_side.pd, NumBlocks, MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
			 MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE),
		dst_mkey(*this, this->dst_side.pd, NumBlocks, MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
			 MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE) {}

	virtual void SetUp() override {
		mkey_test_base<Qp>::SetUp();
		INIT(src_mkey.init());
		INIT(dst_mkey.init());
	}

	bool is_supported() {
		struct mlx5dv_context attr = {};
		attr.comp_mask = MLX5DV_CONTEXT_MASK_SIGNATURE_OFFLOAD;
		mlx5dv_query_device(this->ctx.ctx, &attr);
		return SrcSigBlock::is_supported(attr) && DstSigBlock::is_supported(attr);
	}

	void fill_data() {
		uint8_t src_buf[src_total_size];
		uint8_t *buf = src_buf;

		memset(src_buf, 0xA5, src_total_size);
		for (uint32_t i = 0; i < NumBlocks; ++i) {
			buf += src_block_size;
			if (src_sig_size) {
				SrcValue::sig_to_buf(buf, i);
				buf += src_sig_size;
			}
		}
		src_mkey.layout->set_data(src_buf, src_total_size);
	}

	void corrupt_data(uint32_t offset) {
		uint8_t src_buf[src_total_size];
		uint8_t *buf = src_buf;
		src_mkey.layout->get_data(src_buf, src_total_size);

		for (uint32_t i = 0; i < NumBlocks; ++i) {
			if (offset <= (i+1) * src_total_size) {
				buf += offset;
				*buf = ~(*buf);
				break;
			}
			offset -= src_total_size;
			buf += src_total_size;
		}
		src_mkey.layout->set_data(src_buf, src_total_size);
	}

	void check_data() {
		uint8_t dst_buf[dst_total_size];
		uint8_t *buf = dst_buf;
		uint8_t ref_block_buf[dst_block_size];
		uint8_t ref_sig_buf[dst_sig_size];

		VERBS_TRACE("SrcBlockSize %u, SrcSigSize %u, DstBlockSize %u, DstSigSize %u\n",
			    src_block_size, src_sig_size, dst_block_size, dst_sig_size);
		memset(ref_block_buf, 0xA5, dst_block_size);
		dst_mkey.layout->get_data(dst_buf, dst_total_size);
		for (uint32_t i = 0; i < NumBlocks; ++i) {
			ASSERT_EQ(0, memcmp(buf, ref_block_buf, dst_block_size));
			buf += dst_block_size;

			if (dst_sig_size) {
				DstValue::sig_to_buf(ref_sig_buf, i);

				ASSERT_EQ(0, memcmp(buf, ref_sig_buf, dst_sig_size));
				buf += dst_sig_size;
			}
		}
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

	void check_async_event(enum ibv_event_type type)
	{
		struct ibv_async_event event;
		ASSERT_EQ(0, ibv_get_async_event(this->ctx.ctx, &event));
		ibv_ack_async_event(&event);
		ASSERT_EQ(type, event.event_type);
	}
};


template<typename T_SrcSigBlock, typename T_SrcValue,
	 typename T_DstSigBlock, typename T_DstValue,
	 uint32_t T_NumBlocks = 1,
	 typename T_Qp = ibvt_qp_dv<>, 
	 typename T_RdmaOp = rdma_op_read>
struct types {
	typedef T_SrcSigBlock SrcSigBlock;
	typedef T_SrcValue SrcValue;
	typedef T_DstSigBlock DstSigBlock;
	typedef T_DstValue DstValue;
	static constexpr uint64_t NumBlocks = T_NumBlocks;
	typedef T_Qp Qp;
	typedef T_RdmaOp RdmaOp;
};

template<typename T>
using mkey_test_sig_block = _mkey_test_sig_block<typename T::SrcSigBlock, typename T::SrcValue,
						 typename T::DstSigBlock, typename T::DstValue,
						 T::NumBlocks,
						 typename T::Qp,
						 typename T::RdmaOp>;

TYPED_TEST_CASE_P(mkey_test_sig_block);

#define SIG_CHK_SUT() \
	CHK_SUT(dv_sig); \
	if (!this->is_supported()) SKIP(1);

TYPED_TEST_P(mkey_test_sig_block, basic) {
	SIG_CHK_SUT();

	EXEC(fill_data());
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	EXEC(check_mkeys());
	EXEC(check_data());
}

REGISTER_TYPED_TEST_CASE_P(mkey_test_sig_block, basic);

typedef testing::Types<

	// Wire domain
	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>, sig_none,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>, sig_none>,
	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_crc32c, mkey_block_size_512>>, sig_none,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_crc32c, mkey_block_size_512>>, sig_none>,
	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_crc64xp10, mkey_block_size_512>>, sig_none,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_crc64xp10, mkey_block_size_512>>, sig_none>,

	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>>,
	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>>,

	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>, 2>,
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>, 2>,

	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type3_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type3_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a, false>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type3_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type3_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a, false>, 2>,
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type3_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type3_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a, false>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type3_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type3_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a, false>, 2>,

	// BG types of src mem and wire are same, but seeds are different, mem seed is 0x0000 and wire seed is 0xffff
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_type1<mkey_sig_t10dif_crc, 0x0000, 0x5678, 0xf0debc9a>,
						   mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
	      // guard = 0x9ec6 is calculated with seed 0x0000
	      t10dif_sig<0x9ec6,0x5678,0xf0debc9a>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
	      // guard = 0xec7d is calculated with seed 0xffff
	      t10dif_sig<0xec7d,0x5678,0xf0debc9a>, 2>,

	// BG types of src mem and wire are different, and seeds are also different, mem seed is 0x0000 and wire seed is 0xffff
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_type1<mkey_sig_t10dif_crc, 0x0000, 0x5678, 0xf0debc9a>,
						   mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
	      // guard = 0x9ec6 is calculated with seed 0x0000
	      t10dif_sig<0x9ec6,0x5678,0xf0debc9a>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
	      // guard = 0xec7d is calculated with seed 0xffff
	      t10dif_sig<0xec7d,0x5678,0xf0debc9a>, 2>,

	// Mkey domain
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain_none>, crc32_sig<0x699ACA21>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32c, mkey_block_size_512>,
			     mkey_sig_block_domain_none>, crc32_sig<0x4207E6B4>>,
	// @todo: check crc64 signature
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain_none>, crc32_sig<0x699ACA21>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc64xp10, mkey_block_size_512>,
			     mkey_sig_block_domain_none>, crc64_sig<0x8C8ADB450CCE85AA>>
	> mkey_test_list_sig_types;
INSTANTIATE_TYPED_TEST_CASE_P(sig_types, mkey_test_sig_block, mkey_test_list_sig_types);

typedef testing::Types<
	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      1, ibvt_qp_dv<>, rdma_op_read>,
	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      1, ibvt_qp_dv<>, rdma_op_write>,

	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      1, ibvt_qp_dv<>, rdma_op_read>,
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      1, ibvt_qp_dv<>, rdma_op_write>,

	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      2, ibvt_qp_dv<>, rdma_op_read>,
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	      2, ibvt_qp_dv<>, rdma_op_write>,

	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      1, ibvt_qp_dv<>, rdma_op_read>,
	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      1, ibvt_qp_dv<>, rdma_op_write>,
	types<mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      mkey_sig_block<mkey_sig_block_domain_none,
			     mkey_sig_block_domain<mkey_sig_t10dif_csum_type1_default, mkey_block_size_512>>,
			     t10dif_sig<0,0,0>,
	      1, ibvt_qp_dv<>, rdma_op_send>,

	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
			     crc32_sig<0x699ACA21>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
			     crc32_sig<0x699ACA21>,
	      1, ibvt_qp_dv<>, rdma_op_read>,
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
			     crc32_sig<0x699ACA21>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
			     crc32_sig<0x699ACA21>,
	      1, ibvt_qp_dv<>, rdma_op_write>,
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
			     crc32_sig<0x699ACA21>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
			     crc32_sig<0x699ACA21>,
	      1, ibvt_qp_dv<128,16,32,4,512>, rdma_op_write>,
	types<mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
			     crc32_sig<0x699ACA21>,
	      mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
			     mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
			     crc32_sig<0x699ACA21>,
	      1, ibvt_qp_dv<>, rdma_op_send>

	> mkey_test_list_ops;
INSTANTIATE_TYPED_TEST_CASE_P(ops, mkey_test_sig_block, mkey_test_list_ops);

template<typename T>
using mkey_test_sig_block_stress_test = _mkey_test_sig_block<
	mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
		     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
	t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>,
		     mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default, mkey_block_size_512>>,
	t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
	T::NumSgl, typename T::Qp, typename T::RdmaOp>;

TYPED_TEST_CASE_P(mkey_test_sig_block_stress_test);

TYPED_TEST_P(mkey_test_sig_block_stress_test, basic) {
	SIG_CHK_SUT();

	for (int i = 0; i < 10000; i++) {
		EXEC(fill_data());
		EXEC(configure_mkeys());
		EXEC(execute_rdma());
	}
}

REGISTER_TYPED_TEST_CASE_P(mkey_test_sig_block_stress_test, basic);
template<uint32_t T_NumSgl, typename T_Qp, typename T_RdmaOp = rdma_op_read>
struct sig_stress_test_types {
	static constexpr uint32_t NumSgl = T_NumSgl;
	typedef T_Qp Qp;
	typedef T_RdmaOp RdmaOp;
};
typedef testing::Types<
	sig_stress_test_types<1, ibvt_qp_dv<16,16,16,16>, rdma_op_write>
	> mkey_test_stress_test_ops;
INSTANTIATE_TYPED_TEST_CASE_P(stress_test_ops, mkey_test_sig_block_stress_test, mkey_test_stress_test_ops);


template<typename T>
using mkey_test_sig_block_fence = _mkey_test_sig_block<
	mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
		       mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
	crc32_sig<0x699ACA21>,
	mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
		       mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>>,
	crc32_sig<0x699ACA21>,
	1,
	ibvt_qp_dv<>,
	typename T::RdmaOp>;

TYPED_TEST_CASE_P(mkey_test_sig_block_fence);

TYPED_TEST_P(mkey_test_sig_block_fence, basic) {
	SIG_CHK_SUT();

	EXEC(fill_data());

	this->dst_side.qp.wr_flags(IBV_SEND_INLINE);
	EXECL(this->dst_mkey.configure(this->dst_side.qp));

	this->src_side.qp.wr_flags(IBV_SEND_INLINE);
	EXEC(src_side.qp.wr_start());
	EXECL(this->src_mkey.wr_configure(this->src_side.qp));

	EXECL(this->rdma_op.wr_submit(this->src_side, this->src_mkey.sge(), this->dst_side, this->dst_mkey.sge()));

	EXECL(this->src_side.qp.wr_complete());

	EXECL(this->rdma_op.complete(this->src_side, this->dst_side, IBV_WC_SUCCESS, IBV_WC_SUCCESS));
}

REGISTER_TYPED_TEST_CASE_P(mkey_test_sig_block_fence, basic);
template<typename T_RdmaOp>
struct sig_fence_test_types {
	typedef T_RdmaOp RdmaOp;
};
typedef testing::Types<
	sig_fence_test_types<rdma_op_write>,
	sig_fence_test_types<rdma_op_send>
	> mkey_test_fence_ops;
INSTANTIATE_TYPED_TEST_CASE_P(fence_ops, mkey_test_sig_block_fence, mkey_test_fence_ops);

typedef mkey_test_base<ibvt_qp_dv<>> mkey_test_sig_custom;

TEST_F(mkey_test_sig_custom, noBlockSigAttr) {
	// @todo: add caps check
	//SIG_CHK_SUT();

	// Mkey is created without block signature support
	mkey_dv_new<mkey_access_flags<>,
		    mkey_layout_new_list_mrs<DATA_SIZE>,
		    mkey_sig_block_none>
		src_mkey(*this, this->src_side.pd, 1, MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT);

	EXECL(src_mkey.init());

	EXEC(src_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE));
	EXEC(src_side.qp.wr_start());
	EXECL(src_mkey.wr_configure(this->src_side.qp));
	EXEC(src_side.qp.wr_complete(EINVAL));
}

typedef mkey_test_base<ibvt_qp_dv<2,16,32,4,512>> mkey_test_sig_max_send_wr;
TEST_F(mkey_test_sig_max_send_wr, maxSendWrTooSmall) {
	//SIG_CHK_SUT();

	mkey_dv_new<
	    mkey_access_flags<>,
	    mkey_layout_new_list_mrs<DATA_SIZE>,
	    mkey_sig_block<mkey_sig_block_domain<mkey_sig_crc32ieee,
						 mkey_block_size_512>,
			   mkey_sig_block_domain<mkey_sig_crc32ieee,
						 mkey_block_size_512> > >
	src_mkey(*this, this->src_side.pd, 1,
		 MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
		     MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE);

	EXECL(src_mkey.init());

	this->src_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE);
	EXEC(src_side.qp.wr_start());
	EXECL(src_mkey.wr_configure(this->src_side.qp));
	EXEC(src_side.qp.wr_complete(ENOMEM));
}

typedef mkey_test_base<ibvt_qp_dv<128,2,32,4,512>> mkey_test_sig_max_send_sge;
TEST_F(mkey_test_sig_max_send_sge, maxSendSgeTooSmall) {
	//SIG_CHK_SUT();

	mkey_dv_new<
	    mkey_access_flags<>,
	    mkey_layout_new_list_mrs<DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8>,
	    mkey_sig_block<mkey_sig_block_domain_none,
			   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
						 mkey_block_size_512> > >
	src_mkey(*this, this->src_side.pd, 1,
		 MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
		     MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE);

	EXECL(src_mkey.init());

	this->src_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE);
	EXEC(src_side.qp.wr_start());
	EXECL(src_mkey.wr_configure(this->src_side.qp));
	EXEC(src_side.qp.wr_complete(ENOMEM));
}


typedef mkey_test_base<ibvt_qp_dv<128,16,32,4,64>> mkey_test_sig_max_inline_data;
TEST_F(mkey_test_sig_max_inline_data, maxInlineDataTooSmall) {
	//SIG_CHK_SUT();

	mkey_dv_new<
	    mkey_access_flags<>,
	    mkey_layout_new_list_mrs<DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8, DATA_SIZE/8>,
	    mkey_sig_block<mkey_sig_block_domain_none,
			   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
						 mkey_block_size_512> > >
	src_mkey(*this, this->src_side.pd, 1,
		 MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
		     MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE);

	EXECL(src_mkey.init());

	this->src_side.qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE);
	EXEC(src_side.qp.wr_start());
	EXECL(src_mkey.wr_configure(this->src_side.qp));
	EXEC(src_side.qp.wr_complete(ENOMEM));
}

typedef _mkey_test_sig_block<
    mkey_sig_block<
	mkey_sig_block_domain<mkey_sig_t10dif_type1<mkey_sig_t10dif_crc, 0xffff,
						    0xfff0, 0x0000000f>,
			      mkey_block_size_512>,
	mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
			      mkey_block_size_512> >,
    // guard = 0x0000 is an incorerrect value, CRC16(data) is expected
    // app_tag = 0xffff is a magic number to checking of guard and ref_tag
    // ref_tag = 0x00000000 is an incorrect values, 0x0x0000000f is expected
    t10dif_sig<0x0000,0xffff,0x00000000>,
    mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512>,
		   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512> >,
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>, 2, ibvt_qp_dv<>,
    rdma_op_write> mkey_test_t10dif_type1;

TEST_F(mkey_test_t10dif_type1, skipCheckRefTag) {

	EXEC(fill_data());
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	this->src_mkey.check(MLX5DV_MKEY_NO_ERR);
}
typedef _mkey_test_sig_block<
    mkey_sig_block<
	mkey_sig_block_domain<mkey_sig_t10dif_type3<mkey_sig_t10dif_crc, 0xffff,
						    0xffff, 0x0000000f>,
			      mkey_block_size_512>,
	mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
			      mkey_block_size_512> >,
    // guard = 0x0000 is an incorerrect value, CRC16(data) is expected
    // app_tag = 0xffff and ref_tag = 0xffffffff are magic numbes to skip
    //      cheking of guard
    t10dif_sig<0x0000,0xffff,0xffffffff, false>,
    mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512>,
		   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512> >,
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>, 2, ibvt_qp_dv<>,
    rdma_op_write> mkey_test_t10dif_type3;

TEST_F(mkey_test_t10dif_type3, skipCheckRefTag) {

	EXEC(fill_data());
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	this->src_mkey.check(MLX5DV_MKEY_NO_ERR);
}

typedef _mkey_test_sig_block<
    mkey_sig_block<
	mkey_sig_block_domain<mkey_sig_t10dif_type1<mkey_sig_t10dif_crc, 0xffff,
						    0x5678, 0xf0debc9a>,
			      mkey_block_size_512>,
	mkey_sig_block_domain<mkey_sig_t10dif_type1<mkey_sig_t10dif_crc, 0xffff,
						    0x1234, 0xf0debc9a>,
					 mkey_block_size_512>, 
		   MLX5DV_SIG_CHECK_T10DIF_APPTAG_BYTE0>,
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
    mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_type1<mkey_sig_t10dif_crc, 0xffff,
						    0x5678, 0xf0debc9a>,
			      mkey_block_size_512>,
	mkey_sig_block_domain<mkey_sig_t10dif_type1<mkey_sig_t10dif_crc, 0xffff,
						    0x1234, 0xf0debc9a>,
					 mkey_block_size_512> >,
    //APP Tag 0x5678 is regenerated
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>, 1, ibvt_qp_dv<>,
    rdma_op_write> mkey_test_different_app_tag_byte0_rdma_write;

TEST_F(mkey_test_different_app_tag_byte0_rdma_write, corruptByte1) {

	EXEC(fill_data());
	// Byte1 of App Tag is corrupted
	EXEC(corrupt_data(512 + 2));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// Mask MLX5DV_SIG_CHECK_T10DIF_APPTAG_BYTE0 only checks error
	// in byte 0, so no error will be detected for byte1 corruption
	this->src_mkey.check(MLX5DV_MKEY_NO_ERR);
}

TEST_F(mkey_test_different_app_tag_byte0_rdma_write, corruptByte1ReGenSig) {

	EXEC(fill_data());
	// Byte1 of App Tag is corrupted
	EXEC(corrupt_data(512 + 2));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// because APP Tag setting is different between src and dst,
	// APP Tag is regenerated on the dst, and no error on the dst
	this->dst_mkey.check(MLX5DV_MKEY_NO_ERR);
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	// APP Tag 0x5678 is corrupted to 0xA978, which is not copied to
	// the destination because APP Tag is regenerated due to APP Tag
	// is different in memory domain and wire domain
	EXEC(check_data());
}

TEST_F(mkey_test_different_app_tag_byte0_rdma_write, corruptByte0) {

	EXEC(fill_data());
	// Byte0 of App Tag is corrupted
	EXEC(corrupt_data(512 + 3));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// The src side detects the corruption of APP TAG in byte0
	this->src_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_APPTAG, 0x5687, 0x5678,
			     src_block_size + src_sig_size - 1);
}

TEST_F(mkey_test_different_app_tag_byte0_rdma_write, corruptByte0ReGenSig) {

	EXEC(fill_data());
	// Byte1 of App Tag is corrupted
	EXEC(corrupt_data(512 + 3));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// because APP Tag setting is different between src and dst,
	// APP Tag is regenerated on the dst, and no error on the dst
	this->dst_mkey.check(MLX5DV_MKEY_NO_ERR);
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	// APP Tag 0x5678 is corrupted to 0xA978, which is not copied to
	// the destination because APP Tag is regenerated due to APP Tag
	// is different in memory domain and wire domain
	EXEC(check_data());
}

typedef _mkey_test_sig_block<
    mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512>,
		   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512>,
		   MLX5DV_SIG_CHECK_T10DIF_APPTAG_BYTE0>,
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
    mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512>,
		   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512> >,
    t10dif_sig<0xec7d,0xA978,0xf0debc9a>, 1, ibvt_qp_dv<>,
    rdma_op_write> mkey_test_same_app_tag_byte0_rdma_read;

TEST_F(mkey_test_same_app_tag_byte0_rdma_read, corruptByte1) {

	EXEC(fill_data());
	// Byte1 of App Tag is corrupted
	EXEC(corrupt_data(512 + 2));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// Mask MLX5DV_SIG_CHECK_T10DIF_APPTAG_BYTE0 only checks error
	// in byte 0, so no error will be found for byte1 corruption
	this->src_mkey.check(MLX5DV_MKEY_NO_ERR);
}

TEST_F(mkey_test_same_app_tag_byte0_rdma_read, corruptByte1CopySig) {

	EXEC(fill_data());
	// Byte1 of App Tag is corrupted
	EXEC(corrupt_data(512 + 2));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// the dst received corrupted data, it detected the error
	this->dst_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_APPTAG, 0xA978, 0x5678,
			     dst_block_size + dst_sig_size - 1);
	// APP Tag 0x5678 is corrupted to 0xA978, which is copied to
	// the destination, so check_data can detect the corruption
	EXEC(check_data());
}

TEST_F(mkey_test_same_app_tag_byte0_rdma_read, corruptByte0) {

	EXEC(fill_data());
	// Byte0 of App Tag is corrupted
	EXEC(corrupt_data(512 + 3));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// the src side corrupted data, sig error was detected
	this->src_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_APPTAG, 0x5687, 0x5678,
			     src_block_size + src_sig_size - 1);
}

TEST_F(mkey_test_same_app_tag_byte0_rdma_read, corruptByte0CopySig) {

	EXEC(fill_data());
	// Byte0 of App Tag is corrupted
	EXEC(corrupt_data(512 + 3));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// The dst side detects the corruption of APP TAG in byte0
	this->dst_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_APPTAG, 0x5687, 0x5678,
			     dst_block_size + dst_sig_size - 1);
}

typedef _mkey_test_sig_block<
    mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512>,
		   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512> >,
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
    mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512>,
		   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512> >,
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>, 1, ibvt_qp_dv<>,
    rdma_op_write> mkey_test_sig_corrupt;

TEST_F(mkey_test_sig_corrupt, guardError) {

	EXEC(fill_data());
	EXEC(corrupt_data(0));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	this->src_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_GUARD, 0xec7d, 0x9916,
			     src_block_size + src_sig_size - 1);
}

TEST_F(mkey_test_sig_corrupt, appTagError) {

	EXEC(fill_data());
	EXEC(corrupt_data(512 + 2));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	this->src_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_APPTAG, 0xa978, 0x5678,
			     src_block_size + src_sig_size - 1);
}

TEST_F(mkey_test_sig_corrupt, refTagError) {

	EXEC(fill_data());
	EXEC(corrupt_data(512 + 4));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	this->src_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_REFTAG, 0x0fdebc9a,
			     0xf0debc9a, src_block_size + src_sig_size - 1);
}

// refTag is changed to a non-defualt value (e.g., f0debc9a)
typedef _mkey_test_sig_block<
    mkey_sig_block<
	mkey_sig_block_domain<mkey_sig_t10dif_type1<mkey_sig_t10dif_crc, 0xffff,
						    0x5678, 0xff000000>,
			      mkey_block_size_512>,
	mkey_sig_block_domain<
	    mkey_sig_t10dif_crc_type1_default,
	    mkey_block_size_512> >,
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
    mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512>,
		   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512> >,
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>, 1, ibvt_qp_dv<>,
    rdma_op_write> mkey_test_sig_incorrect_ref_tag;
TEST_F(mkey_test_sig_incorrect_ref_tag, refTagError) {

	EXEC(fill_data());
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	this->src_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_REFTAG);
}

typedef _mkey_test_sig_block<
    mkey_sig_block<
	mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
	mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512> >,
    crc32_sig<0x699ACA21>,
    mkey_sig_block<
	mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512>,
	mkey_sig_block_domain<mkey_sig_crc32ieee, mkey_block_size_512> >,
    crc32_sig<0x699ACA21>, 1, ibvt_qp_dv<>,
    rdma_op_write> mkey_test_crc_sig_corrupt;

TEST_F(mkey_test_crc_sig_corrupt, corruptData) {

	EXEC(fill_data());
	EXEC(corrupt_data(0));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	this->src_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_GUARD, 0x699ACA21,
			     0xE33CB35A, src_block_size + src_sig_size - 1);
}

TEST_F(mkey_test_crc_sig_corrupt, corruptSig) {

	EXEC(fill_data());
	EXEC(corrupt_data(512));
	EXEC(configure_mkeys());
	EXEC(execute_rdma());
	// this->src_mkey.layout->dump(0, 0, "SRC");
	// this->dst_mkey.layout->dump(0, 0, "DST");
	this->src_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_GUARD, 0x969ACA21, 0x699ACA21,
			     src_block_size + src_sig_size - 1);
}

typedef _mkey_test_sig_block<
    mkey_sig_block<mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512>,
		   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512> >,
    t10dif_sig<0xec7d,0x5678,0xf0debc9a>,
    mkey_sig_block<mkey_sig_block_domain_none,
		   mkey_sig_block_domain<mkey_sig_t10dif_crc_type1_default,
					 mkey_block_size_512> >,
    sig_none, 1, ibvt_qp_dv<128,16,32,4,512,true>,
    rdma_op_write> mkey_test_sig_pipelining;

TEST_F(mkey_test_sig_pipelining, pipeliningBasicFlow) {
	SIG_CHK_SUT();

	const size_t SEND_SIZE = 64;
	ibvt_mr send_mr(env, this->src_side.pd, SEND_SIZE); // for send data
	ibvt_mr recv_mr(env, this->dst_side.pd, SEND_SIZE);
	rdma_op_send send_op;

	send_mr.init();
	send_mr.fill();
	recv_mr.init();
	recv_mr.fill();

	EXEC(fill_data());
	EXEC(corrupt_data(0));
	EXEC(configure_mkeys());

	EXEC(src_side.qp.wr_start());
	EXEC(src_side.qp.wr_id(1));
	EXEC(rdma_op.wr_submit(src_side, src_mkey.sge(), dst_side, dst_mkey.sge()));
	EXEC(src_side.qp.wr_id(2));
	EXECL(send_op.wr_submit(src_side, send_mr.sge(), dst_side, recv_mr.sge()));
	EXEC(src_side.qp.wr_complete());
	EXEC(rdma_op.complete(src_side, dst_side));
	
	//SQ_DRAINED event happen due to data corruption in rdma write
	EXEC(check_async_event(IBV_EVENT_SQ_DRAINED));
	EXEC(src_mkey.check(MLX5DV_MKEY_SIG_BLOCK_BAD_GUARD, 0xec7d, 0x9916,
			     src_block_size + src_sig_size - 1));
	EXEC(src_side.qp.cancel_posted_wrs(2, 1));
	EXEC(src_side.qp.modify_qp_to_rts());
	EXEC(rdma_op.check_completion(this->src_side, IBV_WC_SEND));
}

#endif /* HAVE_DECL_MLX5DV_WR_MKEY_CONFIGURE */
