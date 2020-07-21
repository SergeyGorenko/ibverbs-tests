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

		dv_attr.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS;
		dv_attr.send_ops_flags = MLX5DV_QP_EX_WITH_MKEY_CONFIGURE;
		SET(qp, mlx5dv_create_qp(pd.ctx.ctx, &attr, &dv_attr));
	}

	virtual void send_2wr(ibv_sge sge, ibv_sge sge2) {
		struct ibv_send_wr wr = {};
		struct ibv_send_wr wr2 = {};
		struct ibv_send_wr *bad_wr = NULL;

		wr.next = &wr2;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		wr._wr_opcode = IBV_WR_SEND;

		wr2.sg_list = &sge2;
		wr2.num_sge = 1;
		wr2.wr_opcode = IBV_WR_SEND;
		wr2.wr_send_flags = IBV_SEND_SIGNALED;

		DO(ibv_post_send(qp, &wr, &bad_wr));
	}

	virtual void send_2wr_m(ibv_sge sge[], int sge_n, ibv_sge sge2) {
		struct ibv_send_wr wr = {};
		struct ibv_send_wr wr2 = {};
		struct ibv_send_wr *bad_wr = NULL;

		wr.next = &wr2;
		wr.sg_list = sge;
		wr.num_sge = sge_n;
		wr._wr_opcode = IBV_WR_SEND;

		wr2.sg_list = &sge2;
		wr2.num_sge = 1;
		wr2.wr_opcode = IBV_WR_SEND;
		wr2.wr_send_flags = IBV_SEND_SIGNALED;

		DO(ibv_post_send(qp, &wr, &bad_wr));
	}
};

struct mkey : public ibvt_abstract_mVr {
	ibvt_pd &pd;
	struct mlx5dv_mkey *mkey;

	mkey(ibvt_env &e, ibvt_pd &p) :
		ibvt_mr(e, p, 0),
		mkey(NULL) {}

	virtual void init() {
		struct mlx5dv_mkey_init_attr in = {};
		if (mr)
			return;

		in.pd = pd.pd;
		in.max_entries = 1;
		in.create_flags = MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
				  MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE |
				  MLX5DV_MKEY_INIT_ATTR_FLAGS_TRANSACTION_SIGNATURE;
		SET(mr, mlx5dv_create_mkey(&in));
	}

	virtual ~ibvt_mr_sig() {
		FREE(mlx5dv_destroy_mkey, mkey);
	}
};

struct mlx5_mkey : public ibvt_abstract_mr {
	ibvt_pd &pd;
	const uint16_t max_entries;
	const uint32_t create_flags;
	struct mlx5dv_mkey *mkey;

	mlx5_mkey(ibvt_env &env, ibvt_pd &pd,
		  uint16_t me = 1,
		  uint32_t cf = MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT |
				MLX5DV_MKEY_INIT_ATTR_FLAGS_BLOCK_SIGNATURE |
				MLX5DV_MKEY_INIT_ATTR_FLAGS_TRANSACTION_SIGNATURE) :
		ibvt_abstract_mr(env, 0, 0),
		pd(pd),
		max_entries(me),
		create_flags(cf) {}

	virtual void init() {
		struct mlx5dv_mkey_init_attr in = {};
		if (mr)
			return;

		in.pd = pd.pd;
		in.max_entries = max_entries;
		in.create_flags = create_flags;
		SET(mr, mlx5dv_create_mkey(&in));
	}

	virtual ~mlx5_mkey() {
		FREE(mlx5dv_destroy_mkey, mkey);
	}

	virtual uint32_t lkey() {
		return mkey->lkey;
	}
};

struct mkey_data_layout {
	size_t size;

	mkey_data_layout() :
		size(0) {}

	virtual ~mkey_data_layout()  = default;


	virtual size_t size() {
		return size;
	}
	virtual void wr_set(struct mlx5dv_qp_ex *mqp) = 0;
};

struct mkey_layout_list : public mkey_layout {
	struct ibv_sge *sgl;
	uint16_t num_sges;

	mkey_layout_list(uint16_t n, struct ibv_sge *s) :
		num_sges(n) {

		SET(sgl, malloc(num_sges * sizeof(ibv_sge)));

		size = 0;
		for (i = 0; i < num_sges; i++) {
			sgl[i] = s[i];
			size += s[i].length;
		}
	}

	virtual ~mkey_layout_list() {
		FREE(free, sgl);
	}

	virtual void wr_set(struct mlx5dv_qp_ex *mqp) {
		mlx5dv_wr_mkey_set_layout_list(mqp, num_sges, sgl);
	}
};

struct mkey_layout_interleaved : public mkey_layout {
	uint32_t repeat_count;
	uint16_t num_interleaved;
	struct mlx5dv_mr_interleaved *data;	

	mkey_layout_interleaved(uint32_t rc, uint16_t ni,
				struct mlx5dv_mr_interleaved *d) :
		repeat_count(rc),
		num_interleaved(ni) {

		SET(data, malloc(num_interleaved * sizeof(mlx5dv_mr_interleaved)));

		size = 0;
		for (unsigned i = 0; i < num_interleaved; i++) {
			data[i] = d[i];
			size += d[i].bytes_count;
		}

		size *= repeat_count;
	}

	virtual ~mkey_layout_interleaved() {
		FREE(free, data);
	}

	virtual void wr_set(struct mlx5dv_qp_ex *mqp) {
		mlx5dv_wr_mkey_set_layout_interleaved(mqp, repeat_count,
						      num_interleaved, data);
	}
};

struct sig {
	virtual void init_sig_domain(struct mlx5dv_sig_block_domain *d) = 0;
	virtual void init_sig_domain(struct mlx5dv_sig_block_trans *t) = 0;
};

struct sig_none : public sig {
	virtual void init_sig_domain(struct mlx5dv_sig_block_domain *d) {
		t->sig_type = MLX5DV_SIG_TYPE_NONE;
	}
	virtual void init_sig_domain(struct mlx5dv_sig_block_trans *t) {
		t->sig_type = MLX5DV_SIG_TYPE_NONE;
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

	virtual void init_sig_domain(struct mlx5dv_sig_block_trans *t) {
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

	virtual void init_sig_domain(struct mlx5dv_sig_block_trans *t) {
		d->sig_type = MLX5DV_SIG_TYPE_T10DIF;
		d->sig.dif = &dif;
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
}

struct mkey_sig {
	virtual ~mkey_sig() = default;

	virtual void wr_set_sig(struct mlx5dv_qp_ex *mqp) = 0;
};

struct mkey_sig_block_domain {
	struct mlx5dv_sig_block_domain domain;
	std::unique_ptr<struct sig> sig;

	mkey_sig_block_domain(std::unique_ptr<struct sig> s,
			      unsigned block_size) :
		sig(s) {
			sig->init_sig_domain(&domain);

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
}

struct mkey_sig_block : public mkey_sig {
	struct mlx5dv_sig_block_attr attr;
	std::unique_ptr<struct mkey_sig_block_domain> mkey;
	std::unique_ptr<struct mkey_sig_block_domain> wire;

	mkey_sig_block(std::unique_ptr<struct mkey_sig_block_domain> m,
		       std::unique_ptr<struct mkey_sig_block_domain> w,
		       uint8_t cm = 0) :
		mkey(m),
		wire(w) {

		attr.check_mask = cm;
		attr.mkey = mkey->get_sig_domain();
		attr.wire = wire->get_sig_domain();
	}

	virtual ~mkey_sig_block() = default;

	virtual void wr_set_sig(struct mlx5dv_qp_ex *mqp) {
		mlx5dv_wr_mkey_set_sig_block(mqp, &attr);
	}
};

struct mkey_sig_trans_domain {
	struct mlx5dv_sig_trans_domain domain;
	std::unique_ptr<struct sig> sig;

	mkey_sig_trans_domain(std::unique_ptr<struct sig> s,
			      uint64_t flags) :
		sig(s) {
			sig->init_sig_domain(&domain);
			domain.flags = flags;
		}

	virtual struct mlx5dv_sig_trans_domain *get_sig_domain() {
		return &domain;
	}

	virtual ~mkey_sig_trans_domain() = default;
}

struct mkey_sig_trans : public mkey_sig {
	struct mlx5dv_sig_trans_attr attr;
	std::unique_ptr<struct mkey_sig_trans_domain> mkey;
	std::unique_ptr<struct mkey_sig_trans_domain> wire;

	mkey_sig_block(std::unique_ptr<struct mkey_sig_block_domain> m,
		       std::unique_ptr<struct mkey_sig_block_domain> w) :
		mkey(m),
		wire(w) {

		attr.mkey = mkey->get_sig_domain();
		attr.wire = wire->get_sig_domain();
	}

	virtual ~mkey_sig_trans() = default;

	virtual void wr_set_sig(struct mlx5dv_qp_ex *mqp) {
		mlx5dv_wr_mkey_set_sig_trans(mqp, &attr);
	}
};

#if 0
template <typename QP, typename SD>
struct sig_test_base : public testing::Test, public SD, public ibvt_env {
	ibvt_ctx ctx;
	ibvt_pd pd;
	ibvt_cq cq;
	QP send_qp;
	QP recv_qp;
	ibvt_mr src_mr;
	ibvt_mr src2_mr;
	ibvt_mr mid_mr;
	ibvt_mr mid2_mr;
	ibvt_mr mid_mr_x2;
	ibvt_mr dst_mr;
	ibvt_mr dst_mr_x2;
	ibvt_mr_sig insert_mr;
	ibvt_mr_sig insert2_mr;
	ibvt_mr_sig check_mr;
	ibvt_mr_sig strip_mr;
	ibvt_mr_sig strip_mr_x2;
	ibvt_mw mw;

	sig_test_base() :
		ctx(*this, NULL),
		pd(*this, ctx),
		cq(*this, ctx),
		send_qp(*this, pd, cq),
		recv_qp(*this, pd, cq),
		src_mr(*this, pd, SZ),
		src2_mr(*this, pd, SZ),
		mid_mr(*this, pd, SZD(this->pi_size())),
		mid2_mr(*this, pd, SZD(this->pi_size())),
		mid_mr_x2(*this, pd, SZD(this->pi_size()) * 2),
		dst_mr(*this, pd, SZ),
		dst_mr_x2(*this, pd, SZ * 2),
		insert_mr(*this, pd),
		insert2_mr(*this, pd),
		check_mr(*this, pd),
		strip_mr(*this, pd),
		strip_mr_x2(*this, pd),
		mw(mid_mr, 0, SZD(this->pi_size()), send_qp)
	{ }

	virtual void config_wr(ibvt_mr &sig_mr, struct ibv_sge data,
			    struct ibv_exp_sig_domain mem,
			    struct ibv_exp_sig_domain wire) {
		struct __cfg_wr {
			ibv_send_wr wr;
			ibv_exp_sig_attrs sig;
			ibv_sge data;
		} *wr = (__cfg_wr *)calloc(1, sizeof(*wr));

		wr->sig.check_mask = this->check_mask();
		wr->sig.mem = mem;
		wr->sig.wire = wire;

		wr->data = data;

		wr->wr.exp_opcode = IBV_EXP_WR_REG_SIG_MR;
		wr->wr.exp_send_flags = IBV_EXP_SEND_SOLICITED;
		wr->wr.ext_op.sig_handover.sig_attrs = &wr->sig;
		wr->wr.ext_op.sig_handover.sig_mr = sig_mr.mr;
		wr->wr.ext_op.sig_handover.access_flags =
			IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_READ |
			IBV_ACCESS_REMOTE_WRITE;
		wr->wr.ext_op.sig_handover.prot = NULL;

		wr->wr.num_sge = 1;
		wr->wr.sg_list = &wr->data;

		add_wr(&wr->wr);
	}

	virtual void config(ibvt_mr &sig_mr, struct ibv_sge data,
			    struct ibv_exp_sig_domain mem,
			    struct ibv_exp_sig_domain wire) {
		config_wr(sig_mr, data, mem, wire);
		EXEC(send_qp.post_all_wr());
	}

	virtual void linv_wr(ibvt_mr &sig_mr, int sign = 0) {
		struct ibv_send_wr *wr = (ibv_send_wr *)calloc(1, sizeof(*wr));

		wr->exp_opcode = IBV_EXP_WR_LOCAL_INV;
		wr->exp_send_flags = IBV_EXP_SEND_SOLICITED | IBV_EXP_SEND_FENCE;
		if (sign)
			wr->exp_send_flags |= IBV_EXP_SEND_SIGNALED;
		wr->ex.invalidate_rkey = sig_mr.mr->rkey;

		add_wr(wr);
	}

	virtual void linv(ibvt_mr &sig_mr) {
		linv_wr(sig_mr);
		EXEC(send_qp.post_all_wr());
	}

	void mr_status(ibvt_mr &mr, int expected) {
		struct ibv_exp_mr_status status;

		DO(ibv_exp_check_mr_status(mr.mr, IBV_EXP_MR_CHECK_SIG_STATUS,
					   &status));
		VERBS_INFO("SEGERR %d %x %x %lx\n",
			   status.sig_err.err_type,
			   status.sig_err.expected,
			   status.sig_err.actual,
			   status.sig_err.sig_err_offset);
		ASSERT_EQ(expected, status.fail_status);
	}

	void ae() {
		struct ibv_async_event event;

		DO(ibv_get_async_event(this->ctx.ctx, &event));
		ibv_ack_async_event(&event);
	}

	virtual void SetUp() {
		INIT(ctx.init());
		if (skip)
			return;
		INIT(send_qp.init());
		INIT(recv_qp.init());
		INIT(send_qp.connect(&recv_qp));
		INIT(recv_qp.connect(&send_qp));
		INIT(src_mr.fill());
		INIT(src2_mr.fill());
		INIT(mid_mr.init());
		INIT(mid2_mr.init());
		INIT(mid_mr_x2.init());
		INIT(dst_mr.init());
		INIT(dst_mr_x2.init());
		INIT(insert_mr.init());
		INIT(insert2_mr.init());
		INIT(check_mr.init());
		INIT(strip_mr.init());
		INIT(strip_mr_x2.init());
		INIT(cq.arm());
		INIT(mw.init());
	}

	virtual void TearDown() {
		ASSERT_FALSE(HasFailure());
	}
};

template <typename SD>
struct sig_test : public sig_test_base<ibvt_qp_sig, SD> {};
typedef testing::Types<
#if HAVE_DECL_IBV_EXP_SIG_TYPE_CRC32
	sig_crc32,
#endif
	sig_t10dif
> sig_domain_types;
TYPED_TEST_CASE(sig_test, sig_domain_types);

typedef sig_test<sig_t10dif> sig_test_t10dif;
typedef sig_test_base<ibvt_qp_sig_pipeline, sig_t10dif> sig_test_pipeline;

TYPED_TEST(sig_test, c0) {
	CHK_SUT(sig_handover);
	EXEC(config(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
	EXEC(config(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	EXEC(send_qp.rdma(this->mid_mr.sge(), this->insert_mr.sge(0,SZD(this->pi_size())), IBV_WR_RDMA_READ));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->dst_mr.sge(), this->strip_mr.sge(0,SZ), IBV_WR_RDMA_READ));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 0));
	EXEC(dst_mr.check());
}

TYPED_TEST(sig_test, c1) {
	CHK_SUT(sig_handover);
	EXEC(config(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
	EXEC(config(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	EXEC(send_qp.rdma(this->insert_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->strip_mr.sge(0,SZ), this->dst_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 0));
	EXEC(dst_mr.check());
}

TYPED_TEST(sig_test, c2) {
	CHK_SUT(sig_handover);
	EXEC(config(this->insert_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	EXEC(config(this->strip_mr, this->dst_mr.sge(), this->nosig(), this->sig()));
	EXEC(send_qp.rdma(this->insert_mr.sge(0,SZ), this->src_mr.sge(), IBV_WR_RDMA_READ));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->strip_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_READ));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 0));
	EXEC(dst_mr.check());
}

TYPED_TEST(sig_test, c3) {
	CHK_SUT(sig_handover);
	EXEC(config(this->insert_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	EXEC(config(this->strip_mr, this->dst_mr.sge(), this->nosig(), this->sig()));
	EXEC(send_qp.rdma(this->src_mr.sge(), this->insert_mr.sge(0,SZ), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->mid_mr.sge(), this->strip_mr.sge(0,SZD(this->pi_size())), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 0));
	EXEC(dst_mr.check());
}

TYPED_TEST(sig_test, c4) {
	CHK_SUT(sig_handover);
	EXEC(config(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
	EXEC(config(this->check_mr, this->mid_mr.sge(), this->sig(), this->sig()));
	EXEC(config(this->strip_mr, this->mid2_mr.sge(), this->sig(), this->nosig()));
	EXEC(send_qp.rdma(this->insert_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->check_mr.sge(0,SZD(this->pi_size())), this->mid2_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->strip_mr.sge(0,SZ), this->dst_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 0));
	EXEC(dst_mr.check());
}

TYPED_TEST(sig_test, r0) {
	CHK_SUT(sig_handover);
	EXEC(config(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
	EXEC(config(this->check_mr, this->mid_mr.sge(), this->sig(), this->sig()));
	struct ibv_exp_sig_domain sd = this->sig();
	sd.sig.dif.ref_remap = 0;
	EXEC(config(this->strip_mr, this->mid2_mr.sge(), sd, this->nosig()));

	EXEC(send_qp.rdma(this->insert_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->check_mr.sge(0,SZD(this->pi_size())), this->mid2_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());

	for (long i = 0; i < 10; i++) {
		EXEC(send_qp.rdma(this->strip_mr.sge(0,SZ), this->dst_mr.sge(), IBV_WR_RDMA_WRITE));
		EXEC(cq.poll());
		EXEC(mr_status(this->strip_mr, 0));
		EXEC(dst_mr.check());
	}
}

TYPED_TEST(sig_test, r1) {
	CHK_SUT(sig_handover);
	EXEC(config(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
	EXEC(config(this->check_mr, this->mid_mr.sge(), this->sig(), this->sig()));

	EXEC(send_qp.rdma(this->insert_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->check_mr.sge(0,SZD(this->pi_size())), this->mid2_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());

	for (long i = 0; i < 100; i++) {
		EXEC(config(this->strip_mr, this->mid2_mr.sge(), this->sig(), this->nosig()));
		EXEC(send_qp.rdma(this->strip_mr.sge(0,SZ), this->dst_mr.sge(), IBV_WR_RDMA_WRITE));
		EXEC(cq.poll());
		EXEC(mr_status(this->strip_mr, 0));
		EXEC(dst_mr.check());
		EXEC(linv(this->strip_mr));
	}
}

TYPED_TEST(sig_test, r2) {
	CHK_SUT(sig_handover);
	EXEC(config(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
	EXEC(config(this->check_mr, this->mid_mr.sge(), this->sig(), this->sig()));

	EXEC(send_qp.rdma(this->insert_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->check_mr.sge(0,SZD(this->pi_size())), this->mid2_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());

	for (long i = 0; i < 100; i++) {
		EXEC(config(this->strip_mr, this->mid2_mr.sge(), this->sig(), this->nosig()));
		EXEC(send_qp.rdma(this->strip_mr.sge(0,SZ_p(i)), this->dst_mr.sge(), IBV_WR_RDMA_WRITE));
		EXEC(cq.poll());
		EXEC(mr_status(this->strip_mr, 0));
		EXEC(dst_mr.check(0,0,1,SZ_p(i)));
		EXEC(linv(this->strip_mr));
	}
}

TYPED_TEST(sig_test, r3) {
	CHK_SUT(sig_handover);
	EXEC(config(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
	EXEC(config(this->check_mr, this->mid_mr.sge(), this->sig(), this->sig()));

	EXEC(send_qp.rdma(this->insert_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->check_mr.sge(0,SZD(this->pi_size())), this->mid2_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());

	for (long i = 0; i < 100; i++) {
		int j = SZ_pp(i,0,3);
		int k = SZ_pp(i,j,2);
		int l = SZ_pp(i,k,1);
		EXEC(config(this->strip_mr, this->mid2_mr.sge(), this->sig(), this->nosig()));
		EXEC(config(this->strip_mr_x2, this->mid2_mr.sge(), this->sig(), this->nosig()));
		EXEC(send_qp.rdma(this->strip_mr.sge(0,j), this->dst_mr.sge(), IBV_WR_RDMA_WRITE, (enum ibv_send_flags)0));
		EXEC(send_qp.rdma(this->strip_mr_x2.sge(j,k-j), this->dst_mr.sge(j,0), IBV_WR_RDMA_WRITE, (enum ibv_send_flags)0));
		EXEC(send_qp.rdma(this->strip_mr.sge(k,l-k), this->dst_mr.sge(k,0), IBV_WR_RDMA_WRITE, (enum ibv_send_flags)0));
		EXEC(send_qp.rdma(this->strip_mr_x2.sge(l,SZ-l), this->dst_mr.sge(l,0), IBV_WR_RDMA_WRITE));
		EXEC(cq.poll());
		EXEC(mr_status(this->strip_mr, 0));
		EXEC(mr_status(this->strip_mr_x2, 0));
		EXEC(dst_mr.check(0,0,1,SZ));
		EXEC(linv(this->strip_mr));
		EXEC(linv(this->strip_mr_x2));
	}
}

TYPED_TEST(sig_test, r4) {
	CHK_SUT(sig_handover);
	struct ibv_exp_sig_domain sd = this->sig();
	enum ibv_send_flags nof = (enum ibv_send_flags)0;

	for (long i = 0; i < 100; i++) {
		int j = SZ_pp(i,0,3);
		int k = SZ_pp(i,j,2);
		int l = SZ_pp(i,k,1);
		sd.sig.dif.ref_tag = 0x28f5b5d * i;

		EXEC(config(this->insert_mr, this->src_mr.sge(), this->nosig(), sd));
		EXEC(send_qp.rdma(this->insert_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_WRITE));
		EXEC(cq.poll());

		EXEC(config(this->strip_mr, this->mw.sge(), sd, this->nosig()));
		EXEC(config(this->strip_mr_x2, this->mw.sge(), sd, this->nosig()));
		EXEC(send_qp.rdma(this->strip_mr.sge(0,j), this->dst_mr.sge(), IBV_WR_RDMA_WRITE, nof));
		EXEC(send_qp.rdma(this->strip_mr_x2.sge(j,k-j), this->dst_mr.sge(j,0), IBV_WR_RDMA_WRITE, nof));
		EXEC(send_qp.rdma(this->strip_mr.sge(k,l-k), this->dst_mr.sge(k,0), IBV_WR_RDMA_WRITE, nof));
		EXEC(send_qp.rdma(this->strip_mr_x2.sge(l,SZ-l), this->dst_mr.sge(l,0), IBV_WR_RDMA_WRITE));
		EXEC(cq.poll());
		EXEC(mr_status(this->insert_mr, 0));
		EXEC(mr_status(this->strip_mr, 0));
		EXEC(mr_status(this->strip_mr_x2, 0));
		EXEC(dst_mr.check(0,0,1,SZ));

		EXEC(linv(this->insert_mr));
		EXEC(linv(this->strip_mr));
		EXEC(linv(this->strip_mr_x2));
	}
}

TYPED_TEST(sig_test, c6) {
	CHK_SUT(sig_handover);
	struct ibv_exp_sig_domain sd = this->sig();
	sd.sig.dif.ref_remap = 0;
	EXEC(config(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
	EXEC(config(this->insert2_mr, this->src2_mr.sge(), this->nosig(), this->sig()));
	EXEC(config(this->strip_mr_x2, this->mid_mr_x2.sge(), sd, this->nosig()));
	EXEC(send_qp.rdma2(this->insert_mr.sge(0,SZD(this->pi_size())),
			   this->insert2_mr.sge(0,SZD(this->pi_size())),
			   this->mid_mr_x2.sge(),
			   IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(send_qp.rdma(this->strip_mr_x2.sge(0,SZ * 2),
			  this->dst_mr_x2.sge(),
			  IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr_x2, 0));
	EXEC(dst_mr_x2.check());
}

TYPED_TEST(sig_test, e0) {
	CHK_SUT(sig_handover);
	EXEC(config(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	EXEC(send_qp.rdma(this->dst_mr.sge(), this->strip_mr.sge(0,SZ), IBV_WR_RDMA_READ));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 1));
}

TYPED_TEST(sig_test, e1) {
	CHK_SUT(sig_handover);
	EXEC(config(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	EXEC(send_qp.rdma(this->strip_mr.sge(0,SZ), this->dst_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 1));
}

TYPED_TEST(sig_test, e2) {
	CHK_SUT(sig_handover);
	EXEC(config(this->strip_mr, this->dst_mr.sge(), this->nosig(), this->sig()));
	EXEC(send_qp.rdma(this->strip_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_READ));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 1));
}

TYPED_TEST(sig_test, e3) {
	CHK_SUT(sig_handover);
	EXEC(config(this->strip_mr, this->dst_mr.sge(), this->nosig(), this->sig()));
	EXEC(send_qp.rdma(this->mid_mr.sge(), this->strip_mr.sge(0,SZD(this->pi_size())), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 1));
}

TYPED_TEST(sig_test, e4) {
	CHK_SUT(sig_handover);
	EXEC(config(this->check_mr, this->mid_mr.sge(), this->sig(), this->sig()));
	EXEC(send_qp.rdma(this->check_mr.sge(0,SZD(this->pi_size())), this->mid2_mr.sge(), IBV_WR_RDMA_WRITE));
	EXEC(cq.poll());
	EXEC(mr_status(this->check_mr, 1));
}

TEST_F(sig_test_pipeline, p0) {
	CHK_SUT(sig_handover);
	EXEC(config(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	EXEC(recv_qp.recv(this->dst_mr.sge()));
	EXEC(recv_qp.recv(this->dst_mr.sge()));
	EXEC(send_qp.send_2wr(this->strip_mr.sge(0,SZ), this->src_mr.sge(0,SZ)));
	cq.poll();
	EXEC(ae());
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 1));
}

TEST_F(sig_test_pipeline, p1) {
	CHK_SUT(sig_handover);
	EXEC(config(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	EXEC(recv_qp.recv(this->dst_mr.sge()));
	EXEC(recv_qp.recv(this->dst_mr.sge()));
	EXEC(send_qp.send_2wr(this->strip_mr.sge(0,SZ), this->src_mr.sge()));
	cq.poll();
	EXEC(mr_status(this->strip_mr, 1));
}

TEST_F(sig_test_pipeline, p2) {
	CHK_SUT(sig_handover);
	EXEC(config(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	EXEC(recv_qp.recv(this->dst_mr.sge()));
	EXEC(recv_qp.recv(this->dst_mr.sge()));
	struct ibv_sge sge[4];
	for (int i=0; i<4; i++)
		sge[i] = this->strip_mr.sge(0 + i * SZ / 4, SZ/4);
	EXEC(send_qp.send_2wr_m(sge, 4, this->src_mr.sge(0,SZ)));
	cq.poll();
	EXEC(ae());
	EXEC(cq.poll());
	EXEC(mr_status(this->strip_mr, 1));
}


TEST_F(sig_test_pipeline, p3) {
	CHK_SUT(sig_handover);
	for (long j = 0; j < 100; j++) {
		EXEC(config(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
		EXEC(recv_qp.recv(this->dst_mr.sge()));
		EXEC(recv_qp.recv(this->dst_mr.sge()));
		struct ibv_sge sge[4];
		for (int i=0; i<4; i++)
			sge[i] = this->strip_mr.sge(0 + i * SZ / 4, SZ/4);
		EXEC(send_qp.send_2wr_m(sge, 4, this->src_mr.sge(0,SZ)));
		cq.poll();
		EXEC(ae());
		EXEC(cq.poll());
		EXEC(mr_status(this->strip_mr, 1));
		EXEC(linv(this->strip_mr));
	}
}

TEST_F(sig_test_pipeline, p4) {
	CHK_SUT(sig_handover);
	EXEC(config(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
	for (long i = 0; i < 100; i++) {
		EXEC(recv_qp.recv(this->dst_mr.sge()));
		EXEC(recv_qp.recv(this->dst_mr.sge()));
		EXEC(send_qp.send_2wr(this->strip_mr.sge(0,SZ), this->src_mr.sge()));
	}
}

#ifndef SIG_MR_REUSE
#define N 150
#define M 15000

TEST_F(sig_test_t10dif, b0) {
	CHK_SUT(sig_handover);

	for (long j = 0; j < M; j++) {
		for (long i = 0; i < N; i++) {
			EXEC(config_wr(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
			EXEC(config_wr(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
			EXEC(send_qp.rdma_wr(this->mid_mr.sge(), this->insert_mr.sge(0,SZD(this->pi_size())), IBV_WR_RDMA_READ, 0));
			EXEC(send_qp.rdma_wr(this->dst_mr.sge(), this->strip_mr.sge(0,SZ), IBV_WR_RDMA_READ));
			EXEC(linv_wr(this->insert_mr));
			EXEC(linv_wr(this->strip_mr));
		}
		EXEC(send_qp.post_all_wr());
		for (long i = 0; i < N; i++)
			EXEC(cq.poll());
		EXEC(dst_mr.check());
	}
}

TEST_F(sig_test_t10dif, b1) {
	CHK_SUT(sig_handover);

	for (long j = 0; j < M; j++) {
		for (long i = 0; i < N; i++) {
			EXEC(config_wr(this->insert_mr, this->src_mr.sge(), this->nosig(), this->sig()));
			EXEC(config_wr(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
			EXEC(send_qp.rdma_wr(this->insert_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_WRITE, 0));
			EXEC(send_qp.rdma_wr(this->strip_mr.sge(0,SZ), this->dst_mr.sge(), IBV_WR_RDMA_WRITE));
			EXEC(linv_wr(this->insert_mr));
			EXEC(linv_wr(this->strip_mr));
		}
		EXEC(send_qp.post_all_wr());
		for (long i = 0; i < N; i++)
			EXEC(cq.poll());

		EXEC(dst_mr.check());
	}
}

TEST_F(sig_test_t10dif, b2) {
	CHK_SUT(sig_handover);

	for (long j = 0; j < M; j++) {
		for (long i = 0; i < N; i++) {
			EXEC(config_wr(this->insert_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
			EXEC(config_wr(this->strip_mr, this->dst_mr.sge(), this->nosig(), this->sig()));
			EXEC(send_qp.rdma_wr(this->insert_mr.sge(0,SZ), this->src_mr.sge(), IBV_WR_RDMA_READ, 0));
			EXEC(send_qp.rdma_wr(this->strip_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_READ));
			EXEC(linv_wr(this->insert_mr));
			EXEC(linv_wr(this->strip_mr));
		}
		EXEC(send_qp.post_all_wr());
		for (long i = 0; i < N; i++)
			EXEC(cq.poll());

		EXEC(dst_mr.check());
	}
}

TEST_F(sig_test_t10dif, b3) {
	CHK_SUT(sig_handover);

	for (long j = 0; j < M; j++) {
		for (long i = 0; i < N; i++) {
			EXEC(config_wr(this->insert_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
			EXEC(config_wr(this->strip_mr, this->dst_mr.sge(), this->nosig(), this->sig()));
			EXEC(send_qp.rdma_wr(this->src_mr.sge(), this->insert_mr.sge(0,SZ), IBV_WR_RDMA_WRITE, 0));
			EXEC(send_qp.rdma_wr(this->mid_mr.sge(), this->strip_mr.sge(0,SZD(this->pi_size())), IBV_WR_RDMA_WRITE));
			EXEC(linv_wr(this->insert_mr));
			EXEC(linv_wr(this->strip_mr));
		}
		EXEC(send_qp.post_all_wr());
		for (long i = 0; i < N; i++)
			EXEC(cq.poll());

		EXEC(dst_mr.check());
	}
}

TEST_F(sig_test_t10dif, b4) {
	CHK_SUT(sig_handover);

	for (long j = 0; j < M; j++) {
		for (long i = 0; i < N; i++) {
			EXEC(config_wr(this->insert_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
			EXEC(config_wr(this->strip_mr, this->dst_mr.sge(), this->nosig(), this->sig()));
			if ((i ^ j) & 1) {
				EXEC(send_qp.rdma_wr(this->src_mr.sge(), this->insert_mr.sge(0,SZ), IBV_WR_RDMA_WRITE, 0));
				EXEC(send_qp.rdma_wr(this->strip_mr.sge(0,SZD(this->pi_size())), this->mid_mr.sge(), IBV_WR_RDMA_READ));
			} else {
				EXEC(send_qp.rdma_wr(this->insert_mr.sge(0,SZ), this->src_mr.sge(), IBV_WR_RDMA_READ, 0));
				EXEC(send_qp.rdma_wr(this->mid_mr.sge(), this->strip_mr.sge(0,SZD(this->pi_size())), IBV_WR_RDMA_WRITE));
			}
			EXEC(linv_wr(this->insert_mr));
			EXEC(linv_wr(this->strip_mr));
		}
		EXEC(send_qp.post_all_wr());
		for (long i = 0; i < N; i++)
			EXEC(cq.poll());

		EXEC(dst_mr.check());
	}
}

TEST_F(sig_test_t10dif, b5) {
	CHK_SUT(sig_handover);

	for (long j = 0; j < M; j++) {
		for (long i = 0; i < N; i++) {
			int offset = 0; //i & 1 ? 512 : 0;
			EXEC(config_wr(this->insert_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
			EXEC(config_wr(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
			EXEC(send_qp.rdma_wr(this->insert_mr.sge(offset,SZ), this->src_mr.sge(), IBV_WR_RDMA_READ, 0));
			EXEC(send_qp.rdma_wr(this->dst_mr.sge(offset,SZ), this->strip_mr.sge(), IBV_WR_RDMA_READ, 0));
			EXEC(linv_wr(this->insert_mr));
			EXEC(linv_wr(this->strip_mr, 1));
		}
		EXEC(send_qp.post_all_wr());
		for (long i = 0; i < N; i++)
			EXEC(cq.poll());

		EXEC(dst_mr.check());
	}
}

TEST_F(sig_test_t10dif, b5o) {
	CHK_SUT(sig_handover);

	for (long j = 0; j < M; j++) {
		for (long i = 0; i < N; i++) {
			int offset = i & 1 ? 512 : 0;
			EXEC(config_wr(this->insert_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
			EXEC(config_wr(this->strip_mr, this->mid_mr.sge(), this->sig(), this->nosig()));
			EXEC(send_qp.rdma_wr(this->insert_mr.sge(offset,SZ), this->src_mr.sge(), IBV_WR_RDMA_READ, 0));
			EXEC(send_qp.rdma_wr(this->dst_mr.sge(offset,SZ), this->strip_mr.sge(), IBV_WR_RDMA_READ, 0));
			EXEC(linv_wr(this->insert_mr));
			EXEC(linv_wr(this->strip_mr, 1));
		}
		EXEC(send_qp.post_all_wr());
		for (long i = 0; i < N; i++)
			EXEC(cq.poll());

		EXEC(dst_mr.check());
	}
}
#endif
#endif
