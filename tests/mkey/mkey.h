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

#ifndef MKEY_H
#define MKEY_H

#include <algorithm>

struct ibvt_qp_dv : public ibvt_qp_rc {
	ibvt_qp_dv(ibvt_env &e, ibvt_pd &p, ibvt_cq &c) :
		ibvt_qp_rc(e, p, c) {}

	virtual void init_attr(struct ibv_qp_init_attr_ex &attr) {
		ibvt_qp_rc::init_attr(attr);
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
	}

	virtual void init_dv_attr(struct mlx5dv_qp_init_attr &dv_attr) {
		dv_attr.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS;
		dv_attr.send_ops_flags = MLX5DV_QP_EX_WITH_MKEY_CONFIGURE;
	}

	virtual void init() {
		struct ibv_qp_init_attr_ex attr = {};
		struct mlx5dv_qp_init_attr dv_attr = {};

		INIT(pd.init());
		INIT(cq.init());

		init_attr(attr);
		init_dv_attr(dv_attr);
		SET(qp, mlx5dv_create_qp(pd.ctx.ctx, &attr, &dv_attr));
	}

	virtual void wr_id(uint64_t id) {
		ibv_qp_to_qp_ex(qp)->wr_id = id;
	}

	virtual void wr_flags(unsigned int flags) {
		ibv_qp_to_qp_ex(qp)->wr_flags = flags;
	}
};

struct mkey : public ibvt_abstract_mr {
	ibvt_pd &pd;

	mkey(ibvt_env &env, ibvt_pd &pd) :
		ibvt_abstract_mr(env, 0, 0),
		pd(pd) {}

	virtual ~mkey() = default;

	virtual void init() = 0;

	virtual void wr_configure(ibvt_qp &qp) = 0;

	virtual void configure(ibvt_qp &qp) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);

		EXECL(ibv_wr_start(qpx));
		EXEC(wr_configure(qp));
		DO(ibv_wr_complete(qpx));
	}

	virtual void wr_invalidate(ibvt_qp &qp) = 0;

	virtual void invalidate(ibvt_qp &qp) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);

		EXECL(ibv_wr_start(qpx));
		EXEC(wr_invalidate(qp));
		DO(ibv_wr_complete(qpx));
	}

	virtual struct ibv_sge sge(intptr_t start, size_t length) override {
		struct ibv_sge ret = {};

		ret.addr = start;
		ret.length = length;
		ret.lkey = lkey();

		return ret;
	}

	virtual void check() = 0;
};

struct mkey_dv : public mkey {
	const uint16_t max_entries;
	const uint32_t create_flags;
	struct mlx5dv_mkey *mlx5_mkey;

	mkey_dv(ibvt_env &env, ibvt_pd &pd, uint16_t me, uint32_t cf) :
		mkey(env, pd),
		max_entries(me),
		create_flags(cf),
		mlx5_mkey(NULL) {}

	virtual void init() override {
		struct mlx5dv_mkey_init_attr in = {};
		if (mlx5_mkey)
			return;

		in.pd = pd.pd;
		in.max_entries = max_entries;
		in.create_flags = create_flags;
		SET(mlx5_mkey, mlx5dv_create_mkey(&in));
	}

	virtual ~mkey_dv() {
		FREE(mlx5dv_destroy_mkey, mlx5_mkey);
	}

	virtual void wr_invalidate(ibvt_qp &qp) override {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		EXECL(ibv_wr_local_inv(qpx, mlx5_mkey->lkey));
	}

	virtual uint32_t lkey() override {
		return mlx5_mkey->lkey;
	}

	virtual void check() override {
		struct mlx5dv_mkey_err err;
		DO(mlx5dv_mkey_check(mlx5_mkey, &err));
		ASSERT_EQ(MLX5DV_MKEY_NO_ERR, err.err_type);
	}
};

struct mkey_setter {
	virtual ~mkey_setter() = default;
	virtual void init() {};
	virtual void wr_set(ibvt_qp &qp) = 0;
};

struct mkey_valid : public mkey_setter {
	mkey_valid(ibvt_env &env, ibvt_pd &pd) {}

	virtual void wr_set(struct ibvt_qp &qp) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *mqp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);
		mlx5dv_wr_set_mkey_valid(mqp);
	}
};

template<uint32_t AccessFlags = IBV_ACCESS_LOCAL_WRITE |
	 IBV_ACCESS_REMOTE_READ |
	 IBV_ACCESS_REMOTE_WRITE>
struct mkey_access_flags : public mkey_setter {
	uint32_t access_flags;
	/* @todo: add comp_mask attr */

	mkey_access_flags(ibvt_env &env, ibvt_pd &pd, uint32_t access_flags = AccessFlags) :
		access_flags(access_flags) {}

	virtual void wr_set(struct ibvt_qp &qp) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *mqp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);
		mlx5dv_wr_set_mkey_access_flags(mqp, access_flags);
	}
};

struct mkey_layout_new : public mkey_setter {
	virtual ~mkey_layout_new() = default;
	virtual size_t data_length() = 0;
	virtual void set_data(const uint8_t *buf, size_t length) = 0;
	virtual void get_data(uint8_t *buf, size_t length) = 0;
	virtual void dump(size_t offset = 0, size_t length = 0, const char *pfx = "") {}
};

struct mkey_layout_new_list : public mkey_layout_new {
	std::vector<struct ibv_sge> sgl;

	mkey_layout_new_list() :
		sgl() {}

	virtual ~mkey_layout_new_list() = default;

	void init(std::initializer_list<struct ibv_sge> l) {
		sgl = l;
	}

	void init(std::vector<struct ibv_sge> l) {
		sgl = l;
	}

	virtual size_t data_length() override {
		size_t len = 0;

		for (const struct ibv_sge &sge : sgl) {
			len += sge.length;
		}

		return len;
	}

	virtual void wr_set(ibvt_qp &qp) override {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *mqp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);
		struct mlx5dv_mkey_layout_attr layout_attr = {
			.mkey_layout = MLX5DV_MKEY_LAYOUT_LIST,
			.entry_len = 0,
			.repeat_count = 0,
			.num_interleaved = 0,
			.total_byte_count = 0
		};

		mlx5dv_wr_set_mkey_layout(mqp, &layout_attr);
		mlx5dv_wr_set_mkey_entries_list(mqp, 0, sgl.size(), sgl.data());
		mlx5dv_wr_set_mkey_len(mqp, data_length());
	}

	/* @todo: will not work on top of other mkey where addr is zero. */
	virtual void set_data(const uint8_t *buf, size_t length) override {
		for (const auto &sge : sgl) {
			memcpy((void *)sge.addr, buf, std::min((size_t)sge.length, length));
			if (length <= sge.length)
				break;
			length -= sge.length;
			buf += sge.length;
		}
	}

	virtual void get_data(uint8_t *buf, size_t length) override {
		for (const auto &sge : sgl) {
			memcpy(buf, (void *)sge.addr, std::min((size_t)sge.length, length));
			if (length <= sge.length)
				break;
			length -= sge.length;
			buf += sge.length;
		}
	}
};

template<size_t ...Sizes>
struct mkey_layout_new_list_mrs : public mkey_layout_new_list {
	ibvt_env &env;
	ibvt_pd &pd;
	std::vector<struct ibvt_mr> mrs;
	bool initialized;

	mkey_layout_new_list_mrs(ibvt_env &env, ibvt_pd &pd) :
		env(env), pd(pd), initialized(false) {}

	virtual ~mkey_layout_new_list_mrs() = default;

	virtual void init() override {
		if (initialized)
			return;

		initialized = true;
		constexpr std::initializer_list<size_t> sizes = { Sizes... };
		std::vector<struct ibv_sge> sgl;

		for (auto &s: sizes) {
			mrs.emplace_back(env, pd, s);
			auto &mr = mrs.back();
			mr.init();
			mr.fill();
			sgl.push_back(mr.sge());
		}

		mkey_layout_new_list::init(sgl);
	}

	virtual void dump(size_t offset = 0, size_t length = 0, const char *pfx = "") override {
		for (auto &mr : mrs) {
			mr.dump(offset, std::min(length, mr.size), pfx);
			length -= mr.size;
		}
	}
};

struct mkey_layout_new_interleaved : public mkey_layout_new {
	uint16_t repeat_count;
	std::vector<struct mlx5dv_mr_interleaved> interleaved;

	mkey_layout_new_interleaved() :
		repeat_count(0),
		interleaved({}) {}

	virtual ~mkey_layout_new_interleaved() = default;

	void init(uint32_t rc,
		  std::initializer_list<struct mlx5dv_mr_interleaved> &i) {
		repeat_count = rc;
		interleaved = i;
	}

	void init(uint32_t rc,
		  std::vector<struct mlx5dv_mr_interleaved> &i) {
		repeat_count = rc;
		interleaved = i;
	}

	virtual size_t data_length() override {
		size_t len = 0;

		for (const struct mlx5dv_mr_interleaved &i : interleaved) {
			len += i.bytes_count;
		}

		len *= repeat_count;
		return len;
	}

	virtual void wr_set(ibvt_qp &qp) override {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *mqp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);
		struct mlx5dv_mkey_layout_attr layout_attr = {
			.mkey_layout = MLX5DV_MKEY_LAYOUT_LIST_INTERLEAVED,
			.entry_len = 0,
			.repeat_count = repeat_count
		};
		uint64_t total_byte_count = 0;

		for (const struct mlx5dv_mr_interleaved &i : interleaved) {
			total_byte_count += i.bytes_count;
		}
		layout_attr.total_byte_count = total_byte_count;
		layout_attr.num_interleaved = (uint32_t)interleaved.size();

		mlx5dv_wr_set_mkey_layout(mqp, &layout_attr);
		mlx5dv_wr_set_mkey_entries_interleaved(mqp, 0,
						       interleaved.size(),
						       interleaved.data());
		mlx5dv_wr_set_mkey_len(mqp, data_length());
	}

	/* @todo: will not work on top of other mkey where addr is zero. */
	virtual void set_data(const uint8_t *buf, size_t length) override {
		auto tmp_interleaved = interleaved;
		for (uint16_t r = 0; r < repeat_count; ++r) {
			for (auto &i : tmp_interleaved) {
				memcpy((void *)i.addr, buf, std::min((size_t)i.bytes_count, length));
				if (length <= i.bytes_count)
					break;
				length -= i.bytes_count;
				buf += i.bytes_count;
				i.addr += i.bytes_count + i.bytes_skip;
			}
		}
	}

	virtual void get_data(uint8_t *buf, size_t length) override {
		auto tmp_interleaved = interleaved;
		for (uint16_t r = 0; r < repeat_count; ++r) {
			for (auto &i : tmp_interleaved) {
				memcpy(buf, (void *)i.addr, std::min((size_t)i.bytes_count, length));
				if (length <= i.bytes_count)
					break;
				length -= i.bytes_count;
				buf += i.bytes_count;
				i.addr += i.bytes_count + i.bytes_skip;
			}
		}
	}
};

template<uint32_t RepeatCount, uint32_t ...Interleaved>
struct mkey_layout_new_interleaved_mrs : public mkey_layout_new_interleaved {
	ibvt_env &env;
	ibvt_pd &pd;
	std::vector<struct ibvt_mr> mrs;
	bool initialized;

	mkey_layout_new_interleaved_mrs(ibvt_env &env, ibvt_pd &pd) :
		env(env), pd(pd), initialized(false) {}

	virtual ~mkey_layout_new_interleaved_mrs() = default;

	virtual void init() override {
		if (initialized)
			return;

		initialized = true;
		constexpr std::initializer_list<uint32_t> tmp_interleaved = { Interleaved... };
		std::vector<struct mlx5dv_mr_interleaved> mlx5_interleaved;

		static_assert(tmp_interleaved.size() % 2 == 0, "Number of interleaved is not multiple of 2");
		for (auto i = tmp_interleaved.begin(); i != tmp_interleaved.end(); ++i) {
			auto byte_count = *i;
			auto skip_count = *(++i);
			mrs.emplace_back(env, pd, RepeatCount * (byte_count + skip_count));
			struct ibvt_mr &mr = mrs.back();
			mr.init();
			mr.fill();
			mlx5_interleaved.push_back({ (uint64_t)mr.buff, byte_count, skip_count, mr.lkey() });
		}

		mkey_layout_new_interleaved::init(RepeatCount, mlx5_interleaved);
	}

	virtual void dump(size_t offset = 0, size_t length = 0, const char *pfx = "") override {
		for (auto &mr : mrs) {
			mr.dump(offset, std::min(length, mr.size), pfx);
			length -= mr.size;
		}
	}
};

struct mkey_sig {
	virtual void set_sig(struct mlx5dv_sig_block_domain &domain) = 0;
};

struct mkey_sig_none : public mkey_sig {
	static constexpr uint32_t sig_size = 0;

	virtual void set_sig(struct mlx5dv_sig_block_domain &domain) override {
		domain.sig_type = MLX5DV_SIG_TYPE_NONE;
	}
};


template<enum mlx5dv_sig_crc_type CrcType, uint32_t Seed>
struct mkey_sig_crc32 : public mkey_sig {
	static constexpr uint32_t sig_size = 4;
	struct mlx5dv_sig_crc crc;

	virtual void set_sig(struct mlx5dv_sig_block_domain &domain) override {
		domain.sig_type = MLX5DV_SIG_TYPE_CRC;
		crc.type = CrcType;
		crc.seed.crc32 = Seed;
		domain.sig.crc = &crc;
	}
};

template<enum mlx5dv_sig_crc_type CrcType, uint64_t Seed>
struct mkey_sig_crc64 : public mkey_sig {
	static constexpr uint32_t sig_size = 8;
	struct mlx5dv_sig_crc crc;

	virtual void set_sig(struct mlx5dv_sig_block_domain &domain) override {
		domain.sig_type = MLX5DV_SIG_TYPE_CRC;
		crc.type = CrcType;
		crc.seed.crc64 = Seed;
		domain.sig.crc = &crc;
	}
};

template<enum mlx5dv_sig_block_size Mlx5BlockSize, uint32_t BlockSize>
struct mkey_sig_block_size {
	static const enum mlx5dv_sig_block_size mlx5_block_size = Mlx5BlockSize;
	static const uint32_t block_size = BlockSize;
};

typedef mkey_sig_block_size<MLX5DV_SIG_BLOCK_SIZE_512, 512> mkey_sig_block_size_512;
typedef mkey_sig_block_size<MLX5DV_SIG_BLOCK_SIZE_520, 520> mkey_sig_block_size_520;
typedef mkey_sig_block_size<MLX5DV_SIG_BLOCK_SIZE_4048, 4048> mkey_sig_block_size_4048;
typedef mkey_sig_block_size<MLX5DV_SIG_BLOCK_SIZE_4096, 4096> mkey_sig_block_size_4096;
typedef mkey_sig_block_size<MLX5DV_SIG_BLOCK_SIZE_4160, 4160> mkey_sig_block_size_4160;
typedef mkey_sig_block_size<MLX5DV_SIG_BLOCK_SIZE_1M, 1024*1024> mkey_sig_block_size_1M;

template<typename Sig, typename BlockSize>
struct mkey_sig_block_domain {
	typedef BlockSize BlockSizeType;
	typedef Sig SigType;

	struct mlx5dv_sig_block_domain domain;

	void set_domain(const mlx5dv_sig_block_domain **d) {
		Sig sig;
		sig.set_sig(domain);
		domain.block_size = BlockSize::mlx5_block_size;
		*d = &domain;
	}
};

template<typename MkeyDomain, typename WireDomain, uint8_t CheckMask = 0xFF>
struct mkey_sig_block : public mkey_setter {
	typedef MkeyDomain MkeyDomainType;
	typedef WireDomain WireDomainType;

	mkey_sig_block(ibvt_env &env, ibvt_pd &pd) {}

	virtual void wr_set(ibvt_qp &qp) override {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *mqp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);
		struct mlx5dv_sig_block_attr attr = {};

		MkeyDomain mkey;
		WireDomain wire;
		mkey.set_domain(&attr.mkey);
		wire.set_domain(&attr.wire);
		attr.check_mask = CheckMask;
		mlx5dv_wr_set_mkey_sig_block(mqp, &attr);
	}
};

// Some helper types
typedef mkey_sig_crc32<MLX5DV_SIG_CRC_TYPE_CRC32, 0xFFFFFFFF> mkey_sig_crc32ieee;
typedef mkey_sig_crc32<MLX5DV_SIG_CRC_TYPE_CRC32C, 0xFFFFFFFF> mkey_sig_crc32c;
typedef mkey_sig_crc64<MLX5DV_SIG_CRC_TYPE_CRC64, 0xFFFFFFFFFFFFFFFF> mkey_sig_crc64xp10;
typedef mkey_sig_block_domain<mkey_sig_none, mkey_sig_block_size_512> mkey_sig_block_domain_none;
typedef mkey_sig_block<mkey_sig_block_domain_none, mkey_sig_block_domain_none> mkey_sig_block_none;

template<typename ...Setters>
struct mkey_dv_new : public mkey_dv {
	struct mkey_layout_new *layout;
	std::vector<struct mkey_setter *> preset_setters;
	std::vector<struct mkey_setter *> setters;
	bool initialized;

	mkey_dv_new(ibvt_env &env, ibvt_pd &pd, uint16_t me, uint32_t cf) :
		mkey_dv(env, pd, me, cf),
		layout(NULL),
		initialized(false) {
		create_setters(new Setters(env, pd)...);
	}

	virtual ~mkey_dv_new() {
		for (auto s : preset_setters) {
			delete s;
		}
	}

	virtual void init() override {
		if (initialized)
			return;

		initialized = true;
		mkey_dv::init();
		if (layout) layout->init();
	}

	void create_setters() {}

	template<typename Setter, typename ...Rest>
	void create_setters(Setter *setter, Rest * ...rest) {
		preset_setters.push_back(setter);
		add_setter(setter);
		if (std::is_base_of<struct mkey_layout_new, Setter>::value) {
			layout = dynamic_cast<mkey_layout_new *>(setter);
		}
		create_setters(rest...);
	}

	void set_layout(struct mkey_layout_new *layout) {
		this->layout = layout;
		add_setter(layout);
	}

	void add_setter(struct mkey_setter *setter) {
		setters.push_back(setter);
	}

	virtual void wr_configure(ibvt_qp &qp) override {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *mqp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);

		EXECL(mlx5dv_wr_mkey_configure(mqp, mlx5_mkey, 0));
		for (auto s : setters) {
			EXECL(s->wr_set(qp));
		}
	}

	virtual struct ibv_sge sge() override {
		size_t length = layout ? layout->data_length() : 0;
		return mkey_dv::sge(0, length);
	}

	virtual void dump(size_t offset = 0, size_t length = 0, const char *pfx = "") override {
		if (layout) layout->dump(offset, length, pfx);
	}
};

template<typename QP>
struct mkey_test_side : public ibvt_obj {
	ibvt_pd pd;
	ibvt_cq cq;
	QP qp;

	mkey_test_side(ibvt_env &env, ibvt_ctx &ctx) :
		ibvt_obj(env),
		pd(env, ctx),
		cq(env, ctx),
		qp(env, pd, cq) {}

	virtual void init() {
		INIT(qp.init());
	}

	virtual void connect(struct mkey_test_side &remote) {
		qp.connect(&remote.qp);
	}
};

template<typename QP>
struct rdma_op {
	virtual void submit(mkey_test_side<QP> &src_side,
			    ibv_sge src_sge,
			    mkey_test_side<QP> &dst_side,
			    ibv_sge dst_sge) = 0;

	virtual void complete(mkey_test_side<QP> &src_side,
			      mkey_test_side<QP> &dst_side,
			      enum ibv_wc_status src_status = IBV_WC_SUCCESS,
			      enum ibv_wc_status dst_status = IBV_WC_SUCCESS) = 0;

	void check_completion(mkey_test_side<QP> &side, enum ibv_wc_status status = IBV_WC_SUCCESS) {
		ibvt_wc wc(side.cq);
		side.cq.do_poll(wc);
		ASSERT_EQ(status, wc().status);
	}
};

template<typename QP>
struct rdma_op_write : public rdma_op<QP> {
	virtual void submit(mkey_test_side<QP> &src_side,
			    ibv_sge src_sge,
			    mkey_test_side<QP> &dst_side,
			    ibv_sge dst_sge) override {
		src_side.qp.wr_flags(IBV_SEND_SIGNALED);
		src_side.qp.rdma(src_sge, dst_sge, IBV_WR_RDMA_WRITE);
	}

	virtual void complete(mkey_test_side<QP> &src_side,
			      mkey_test_side<QP> &dst_side,
			      enum ibv_wc_status src_status = IBV_WC_SUCCESS,
			      enum ibv_wc_status dst_status = IBV_WC_SUCCESS) override {
		this->check_completion(src_side, src_status);
	}
};

template<typename QP>
struct rdma_op_read : public rdma_op<QP> {
	virtual void submit(mkey_test_side<QP> &src_side,
			    ibv_sge src_sge,
			    mkey_test_side<QP> &dst_side,
			    ibv_sge dst_sge) override {
		dst_side.qp.wr_flags(IBV_SEND_SIGNALED);
		dst_side.qp.rdma(dst_sge, src_sge, IBV_WR_RDMA_READ);
	}

	virtual void complete(mkey_test_side<QP> &src_side,
			      mkey_test_side<QP> &dst_side,
			      enum ibv_wc_status src_status = IBV_WC_SUCCESS,
			      enum ibv_wc_status dst_status = IBV_WC_SUCCESS) override {
		this->check_completion(dst_side, dst_status);
	}
};

template<typename QP>
struct rdma_op_send : public rdma_op<QP> {
	virtual void submit(mkey_test_side<QP> &src_side,
			    ibv_sge src_sge,
			    mkey_test_side<QP> &dst_side,
			    ibv_sge dst_sge) override {
		dst_side.qp.recv(dst_sge);
		src_side.qp.post_send(src_sge, IBV_WR_SEND);
	}

	virtual void complete(mkey_test_side<QP> &src_side,
			      mkey_test_side<QP> &dst_side,
			      enum ibv_wc_status src_status = IBV_WC_SUCCESS,
			      enum ibv_wc_status dst_status = IBV_WC_SUCCESS) override {
		this->check_completion(src_side, src_status);
		this->check_completion(dst_side, dst_status);
	}

};

template<typename QP>
struct rdma_op_all : public rdma_op<QP> {
	virtual void submit(mkey_test_side<QP> &src_side,
			    ibv_sge src_sge,
			    mkey_test_side<QP> &dst_side,
			    ibv_sge dst_sge) override {
		struct rdma_op_read<QP> read;
		struct rdma_op_write<QP> write;
		struct rdma_op_send<QP> send;
		read.execute(src_side, src_sge, dst_side, dst_sge);
		write.execute(src_side, src_sge, dst_side, dst_sge);
		send.execute(src_side, src_sge, dst_side, dst_sge);
	}

	virtual void complete(mkey_test_side<QP> &src_side,
			      mkey_test_side<QP> &dst_side,
			      enum ibv_wc_status src_status = IBV_WC_SUCCESS,
			      enum ibv_wc_status dst_status = IBV_WC_SUCCESS) override {
		struct rdma_op_read<QP> read;
		struct rdma_op_write<QP> write;
		struct rdma_op_send<QP> send;
		read.complete(src_side, dst_side, src_status, dst_status);
		write.complete(src_side, dst_side, src_status, dst_status);
		send.complete(src_side, dst_side, src_status, dst_status);
	}
};

template<typename Qp>
struct mkey_test_base : public testing::Test, public ibvt_env {
	ibvt_ctx ctx;
	struct mkey_test_side<Qp> src_side;
	struct mkey_test_side<Qp> dst_side;

	mkey_test_base() :
		ctx(*this, NULL),
		src_side(*this, ctx),
		dst_side(*this, ctx) {}

	virtual void SetUp() override {
		INIT(ctx.init());
		if (skip)
			return;
		INIT(src_side.init());
		INIT(dst_side.init());
		INIT(src_side.connect(dst_side));
		INIT(dst_side.connect(src_side));
	}

	virtual void TearDown() override {
		ASSERT_FALSE(HasFailure());
	}
};

#endif /* MKEY_H */
