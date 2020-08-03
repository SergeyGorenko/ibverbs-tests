/**
 * Copyright (C) 2017      Mellanox Technologies Ltd. All rights reserved.
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

#if 1
#define BBB 0x10
#else
#define BBB 1
#define BBB 0x100000
#endif

// FIXME: Hardcoded block size
#define SZ (512*BBB)
#define SZD(pi_size) ((512+pi_size)*BBB)

#define SZ_p(n) (512*((n)%BBB+1))
#define SZ_pp(n,from,spare) (from+512*((n)%(BBB-spare-from/512)+1))

struct ibvt_qp_sig : public ibvt_qp_rc {
	ibvt_qp_sig(ibvt_env &e, ibvt_pd &p, ibvt_cq &c) :
		ibvt_qp_rc(e, p, c) {}

	virtual void init() {
		struct ibv_qp_init_attr_ex attr = {};
		struct mlx5dv_qp_init_attr dv_attr = {};

		INIT(pd.init());
		INIT(cq.init());

		ibvt_qp_rc::init_attr(attr);
		// TODO: Check the below numbers.
		attr.cap.max_send_wr = 128;
		attr.cap.max_send_sge = 16;
		attr.cap.max_recv_wr = 32;
		attr.cap.max_recv_sge = 4;
		attr.cap.max_inline_data = 512;
		attr.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
		attr.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE |
				      IBV_QP_EX_WITH_SEND |
				      IBV_QP_EX_WITH_RDMA_READ |
				      IBV_QP_EX_WITH_LOCAL_INV;

		dv_attr.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS;
		dv_attr.send_ops_flags = MLX5DV_QP_EX_WITH_MKEY_CONFIGURE;
		SET(qp, mlx5dv_create_qp(pd.ctx.ctx, &attr, &dv_attr));
	}

	virtual void wr_id(uint64_t id) {
		ibv_qp_to_qp_ex(qp)->wr_id = id;
	}

	virtual void wr_flags(unsigned int flags) {
		ibv_qp_to_qp_ex(qp)->wr_flags = flags;
	}
};

struct mkey_layout {

	virtual ~mkey_layout()  = default;
	virtual size_t data_length() = 0;
	virtual void wr_set(struct mlx5dv_qp_ex *mqp) = 0;
};

struct mkey_layout_list : public mkey_layout {
	std::vector<struct ibv_sge> sgl;
	size_t length;

	mkey_layout_list() :
		length(0) {}

	mkey_layout_list(std::initializer_list<struct ibv_sge> l) :
		sgl(l) {
		length = 0;

		for (const struct ibv_sge &sge : sgl) {
			length += sge.length;
		}
	}

	mkey_layout_list(struct ibv_sge sge) {
		sgl.push_back(sge);

		length = sge.length;
	}

	virtual ~mkey_layout_list()  = default;

	virtual size_t data_length() {
		return length;
	}

	virtual void wr_set(struct mlx5dv_qp_ex *mqp) {
		mlx5dv_wr_mkey_set_layout_list(mqp, sgl.size(), sgl.data());
	}
};

struct mkey_layout_interleaved : public mkey_layout {
	uint32_t repeat_count;
	std::vector<struct mlx5dv_mr_interleaved> interleaved;

	mkey_layout_interleaved(uint32_t rc,
				std::initializer_list<struct mlx5dv_mr_interleaved> i) :
		repeat_count(rc),
		interleaved(i) {}

	virtual ~mkey_layout_interleaved() = default;

	virtual size_t data_length() {
		size_t len = 0;

		for (const struct mlx5dv_mr_interleaved &i : interleaved) {
			len += i.bytes_count;
		}
		len *= repeat_count;

		return len;
	}

	virtual void wr_set(struct mlx5dv_qp_ex *mqp) {
		mlx5dv_wr_mkey_set_layout_interleaved(mqp, repeat_count,
						      interleaved.size(),
						      interleaved.data());
	}
};

struct sig {
	virtual void init_sig_domain(struct mlx5dv_sig_block_domain *d) = 0;
	virtual void init_sig_domain(struct mlx5dv_sig_trans_domain *d) = 0;
};

struct sig_none : public sig {
	virtual void init_sig_domain(struct mlx5dv_sig_block_domain *d) {
		d->sig_type = MLX5DV_SIG_TYPE_NONE;
	}
	virtual void init_sig_domain(struct mlx5dv_sig_trans_domain *d) {
		d->sig_type = MLX5DV_SIG_TYPE_NONE;
	}
};

struct sig_crc : public sig {
	struct mlx5dv_sig_crc crc;

	sig_crc(enum mlx5dv_sig_crc_type t, uint64_t s) {
		crc.type = t;
		if (t == MLX5DV_SIG_CRC_TYPE_CRC64)
			crc.seed.crc64 = s;
		else
			crc.seed.crc32 = s;
	}

	virtual void init_sig_domain(struct mlx5dv_sig_block_domain *d) {
		d->sig_type = MLX5DV_SIG_TYPE_CRC;
		d->sig.crc = &crc;
	}

	virtual void init_sig_domain(struct mlx5dv_sig_trans_domain *d) {
		d->sig_type = MLX5DV_SIG_TYPE_CRC;
		d->sig.crc = &crc;
	}
};

struct sig_crc32 : public sig_crc {
	static size_t pi_size() {
		return 4;
	}

	sig_crc32(uint64_t s = 0xffffffff) :
		sig_crc(MLX5DV_SIG_CRC_TYPE_CRC32, s) {}
};

struct sig_crc32c : public sig_crc {
	static size_t pi_size() {
		return 4;
	}

	sig_crc32c(uint64_t s = 0xffffffff) :
		sig_crc(MLX5DV_SIG_CRC_TYPE_CRC32C, s) {}
};

struct sig_crc64 : public sig_crc {
	static size_t pi_size() {
		return 8;
	}

	sig_crc64(uint64_t s = 0xffffffffffffffff) :
		sig_crc(MLX5DV_SIG_CRC_TYPE_CRC64, s) {}
};

struct sig_t10dif : public sig {

	struct mlx5dv_sig_t10dif dif;

	static size_t pi_size() {
		return 8;
	}

	sig_t10dif(enum mlx5dv_sig_t10dif_bg_type bg_type,
		   uint16_t bg,
		   uint16_t app_tag,
		   uint32_t ref_tag,
		   uint16_t flags) {

		   dif.bg_type = bg_type;
		   dif.bg = bg;
		   dif.app_tag = app_tag;
		   dif.ref_tag = ref_tag;
		   dif.flags = flags;
		   // I'm going to remove apptag_check_mask from the API
		   // because it will not be available in BF-3.
		   // apptag_check_mask is always 0xffff for BF-3.
		   dif.apptag_check_mask = 0xffff;
	}

	virtual void init_sig_domain(struct mlx5dv_sig_block_domain *d) {
		d->sig_type = MLX5DV_SIG_TYPE_T10DIF;
		d->sig.dif = &dif;
	}

	virtual void init_sig_domain(struct mlx5dv_sig_trans_domain *d) {
		// FIXME: T10-DIF is not supported fot the transaction
		// signature. Add FAIL()
	}
};

struct sig_t10dif_crc : public sig_t10dif {

	sig_t10dif_crc(uint16_t bg,
		       uint16_t app_tag,
		       uint32_t ref_tag,
		       uint16_t flags) :
		sig_t10dif(MLX5DV_SIG_T10DIF_CRC, bg, app_tag, ref_tag,
			   flags) {}

};

struct sig_t10dif_csum : public sig_t10dif {

	sig_t10dif_csum(uint16_t bg,
			uint16_t app_tag,
			uint32_t ref_tag,
			uint16_t flags) :
		sig_t10dif(MLX5DV_SIG_T10DIF_CSUM, bg, app_tag, ref_tag,
			   flags) {}

};

// Set default values for the T10-DIF attributes to use them
// in simple basic tests.
struct sig_t10dif_default : public sig_t10dif_crc {
	sig_t10dif_default() :
		sig_t10dif_crc(0x1234, 0x5678, 0x9abcdef0,
			       MLX5DV_SIG_T10DIF_FLAG_REF_REMAP |
			       MLX5DV_SIG_T10DIF_FLAG_APP_ESCAPE |
			       MLX5DV_SIG_T10DIF_FLAG_REF_ESCAPE) {}
};

struct mkey_sig {
	ibvt_env &env;

	mkey_sig(ibvt_env &e) :
		env(e) {}

	virtual void init() = 0;
	virtual ~mkey_sig() = default;
	virtual void wr_set(struct mlx5dv_qp_ex *mqp) = 0;
};

struct mkey_sig_block_domain  {
	ibvt_env &env;
	struct mlx5dv_sig_block_domain domain;
	struct sig &sig;
	unsigned block_size;

	mkey_sig_block_domain(ibvt_env &e, struct sig &s,
			      unsigned bs) :
		env(e),
		sig(s),
		block_size(bs) {}

	virtual void init_domain() {
		sig.init_sig_domain(&domain);

		switch (block_size) {
		case 512:
			domain.block_size = MLX5DV_SIG_BLOCK_SIZE_512;
			break;
		case 520:
			domain.block_size = MLX5DV_SIG_BLOCK_SIZE_520;
			break;
		case 4048:
			domain.block_size = MLX5DV_SIG_BLOCK_SIZE_4048;
			break;
		case 4096:
			domain.block_size = MLX5DV_SIG_BLOCK_SIZE_4096;
			break;
		case 4160:
			domain.block_size = MLX5DV_SIG_BLOCK_SIZE_4160;
			break;
		case 1048576:
			domain.block_size = MLX5DV_SIG_BLOCK_SIZE_1M;
			break;
		default:
			FAIL() << block_size << " is an unsupported block size";
		}
	}

	virtual struct mlx5dv_sig_block_domain *get_sig_domain() {
		return &domain;
	}

	virtual ~mkey_sig_block_domain() = default;
};

struct mkey_sig_block : public mkey_sig {
	struct mlx5dv_sig_block_attr attr;
	struct mkey_sig_block_domain &mkey;
	struct mkey_sig_block_domain &wire;

	mkey_sig_block(ibvt_env &e,
		       struct mkey_sig_block_domain &m,
		       struct mkey_sig_block_domain &w,
		       uint8_t cm = 0) :
		mkey_sig(e),
		mkey(m),
		wire(w) {

		attr.check_mask = cm;
	}

	virtual void init() {
		attr.mkey = mkey.get_sig_domain();
		attr.wire = wire.get_sig_domain();
	}

	virtual ~mkey_sig_block() = default;

	virtual void wr_set(struct mlx5dv_qp_ex *mqp) {
		mlx5dv_wr_mkey_set_sig_block(mqp, &attr);
	}
};

struct mkey_sig_trans_domain {
	ibvt_env &env;
	struct mlx5dv_sig_trans_domain domain;
	struct sig &sig;

	mkey_sig_trans_domain(ibvt_env &e,
			      struct sig &s,
			      uint64_t flags = 0) :
		env(e),
		sig(s) {

		domain.flags = flags;
	}

	virtual void init() {
		sig.init_sig_domain(&domain);
	}

	virtual struct mlx5dv_sig_trans_domain *get_sig_domain() {
		return &domain;
	}

	virtual ~mkey_sig_trans_domain() = default;
};

struct mkey_sig_trans : public mkey_sig {
	struct mlx5dv_sig_trans_attr attr;
	struct mkey_sig_trans_domain &mkey;
	struct mkey_sig_trans_domain &wire;

	mkey_sig_trans(ibvt_env &e,
		       struct mkey_sig_trans_domain &m,
		       struct mkey_sig_trans_domain &w) :
		mkey_sig(e),
		mkey(m),
		wire(w) {}

	virtual void init() {
		attr.mkey = mkey.get_sig_domain();
		attr.wire = wire.get_sig_domain();
	}

	virtual ~mkey_sig_trans() = default;

	virtual void wr_set(struct mlx5dv_qp_ex *mqp) {
		mlx5dv_wr_mkey_set_sig_trans(mqp, &attr);
	}
};

struct mlx5_mkey : public ibvt_abstract_mr {
	ibvt_pd &pd;
	const uint16_t max_entries;
	const uint32_t create_flags;
	struct mlx5dv_mkey *mkey;
	size_t length;

	mlx5_mkey(ibvt_env &env, ibvt_pd &pd,
		  uint16_t me = 1,
		  uint32_t cf = MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
				MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE |
				MLX5DV_MKEY_INIT_ATTR_FLAGS_TRANSACTION_SIGNATURE) :
		ibvt_abstract_mr(env, 0, 0),
		pd(pd),
		max_entries(me),
		create_flags(cf),
		mkey(NULL),
		length(0) {}

	virtual void init() {
		struct mlx5dv_mkey_init_attr in = {};
		if (mkey)
			return;

		in.pd = pd.pd;
		in.max_entries = max_entries;
		in.create_flags = create_flags;
		SET(mkey, mlx5dv_create_mkey(&in));
	}

	virtual ~mlx5_mkey() {
		FREE(mlx5dv_destroy_mkey, mkey);
	}

	virtual struct ibv_sge sge(intptr_t start, size_t length) override {
		struct ibv_sge ret = {};

		ret.addr = start;
		ret.length = length;
		ret.lkey = lkey();

		return ret;
	}

	virtual struct ibv_sge sge() override {
		return sge(0, length);
	}

	virtual void wr_invalidate(ibvt_qp &qp) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		ibv_wr_local_inv(qpx, mkey->lkey);
	}

	virtual void invalidate(ibvt_qp &qp) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);

		ibv_wr_start(qpx);
		wr_invalidate(qp);
		DO(ibv_wr_complete(qpx));
	}

	virtual void wr_configure(ibvt_qp &qp,
				  mkey_layout &layout,
				  mkey_sig &sig,
				  uint32_t access_flags = 0) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *dv_qp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);
		struct mlx5dv_mkey_attr mkey_attr = {
			.access_flags = access_flags
		};

		length = layout.data_length();
		mlx5dv_wr_mkey_configure(dv_qp, mkey, 0);
		mlx5dv_wr_mkey_set_basic_attr(dv_qp, &mkey_attr);
		layout.wr_set(dv_qp);
		sig.wr_set(dv_qp);
	}

	virtual void configure(ibvt_qp &qp,
			       mkey_layout &layout,
			       mkey_sig &sig,
			       uint32_t access_flags = IBV_ACCESS_LOCAL_WRITE |
						       IBV_ACCESS_REMOTE_READ |
						       IBV_ACCESS_REMOTE_WRITE) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);

		ibv_wr_start(qpx);
		wr_configure(qp, layout, sig, access_flags);
		DO(ibv_wr_complete(qpx));
	}

	virtual uint32_t lkey() {
		return mkey->lkey;
	}
};

struct mkey_test_base : public testing::Test, public ibvt_env {
	ibvt_ctx ctx;
	ibvt_pd pd;
	ibvt_cq cq;
	ibvt_qp_sig send_qp;
	ibvt_qp_sig recv_qp;

	mkey_test_base() :
		ctx(*this, NULL),
		pd(*this, ctx),
		cq(*this, ctx),
		send_qp(*this, pd, cq),
		recv_qp(*this, pd, cq) {}

	virtual void SetUp() {
		INIT(ctx.init());
		if (skip)
			return;
		INIT(send_qp.init());
		INIT(recv_qp.init());
		INIT(send_qp.connect(&recv_qp));
		INIT(recv_qp.connect(&send_qp));
	}

	virtual void TearDown() {
		ASSERT_FALSE(HasFailure());
	}
};

TEST_F(mkey_test_base, mkey_test) {
	CHK_SUT(dv_sig);
	ibvt_mr src_mr(*this, pd, SZ);
	ibvt_mr dst_mr(*this, pd, SZD(4));
	mlx5_mkey src_sig_mr(*this, pd);
	mlx5_mkey dst_sig_mr(*this, pd);

	src_mr.init();
	dst_mr.init();
	mkey_layout_list src_data(src_mr.sge());
	mkey_layout_list dst_data(dst_mr.sge());

	sig_none snone;
	sig_crc32 scrc32;
	mkey_sig_block_domain src_mem_domain(*this, *static_cast<sig *>(&snone), 512);
	mkey_sig_block_domain src_wire_domain(*this, *static_cast<sig *>(&scrc32), 512);
	mkey_sig_block_domain dst_mem_domain(*this, *static_cast<sig *>(&scrc32), 512);
	mkey_sig_block_domain dst_wire_domain(*this, *static_cast<sig *>(&scrc32), 512);

	src_mem_domain.init_domain();
	src_wire_domain.init_domain();
	dst_mem_domain.init_domain();
	dst_wire_domain.init_domain();
	
	mkey_sig_block src_sig_block(*this, src_mem_domain, src_wire_domain);

	mkey_sig_block dst_sig_block(*this, dst_mem_domain, dst_wire_domain,
					    MLX5DV_SIG_CHECK_CRC32);

	src_sig_block.init();
	dst_sig_block.init();

	EXECL(src_mr.fill());
	EXECL(dst_mr.fill());
	EXECL(src_sig_mr.init());
	EXECL(dst_sig_mr.init());

	send_qp.wr_flags(IBV_SEND_SIGNALED | IBV_SEND_INLINE);
	EXECL(src_sig_mr.configure(send_qp, src_data, src_sig_block));
	EXECL(dst_sig_mr.configure(send_qp, dst_data, dst_sig_block));
	EXEC(cq.poll());
	send_qp.wr_flags(IBV_SEND_SIGNALED);
	EXEC(send_qp.rdma(src_sig_mr.sge(), dst_sig_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
}

