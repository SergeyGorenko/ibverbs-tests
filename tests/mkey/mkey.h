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
#include <stdint.h>
#include <endian.h>

struct dif {
	uint16_t guard;
	uint16_t app_tag;
	uint32_t ref_tag;
};

typedef union {
	uint64_t sig; // sig is in Big Endian (Network) mode
	struct dif dif;
} dif_to_sig;

template <uint16_t Guard, uint16_t AppTag, uint32_t RefTag,
	  bool RefRemap = true>
struct t10dif_sig {
	static const uint16_t guard = Guard;
	static const uint16_t app_tag = AppTag;
	static const uint32_t ref_tag = RefTag;

	static void sig_to_buf(uint8_t *buf, uint32_t block_index) {
		dif_to_sig dif;

		dif.dif.guard = htons(guard);
		dif.dif.app_tag = htons(app_tag);

		if (RefRemap) {
			dif.dif.ref_tag = htonl(ref_tag + block_index);
		} else {
			dif.dif.ref_tag = htonl(ref_tag);
		}

		*(uint64_t *)buf = dif.sig;
	}
};

struct sig_none {
	static void sig_to_buf(uint8_t *buf, uint32_t block_index) {}
};

template <uint32_t Sig> struct crc32_sig {
	static const uint32_t sig = Sig;

	static void sig_to_buf(uint8_t *buf, uint32_t block_index) {
		*(uint32_t *)buf = htonl(sig);
	}
};

template <uint64_t Sig> struct crc64_sig {
	static const uint64_t sig = Sig;

	static void sig_to_buf(uint8_t *buf, uint32_t block_index) {
		*(uint64_t *)buf = htobe64(sig);
	}
};

template<uint32_t MaxSendWr = 128, uint32_t MaxSendSge = 16,
	 uint32_t MaxRecvWr = 32, uint32_t MaxRecvSge = 4,
	 uint32_t MaxInlineData = 512, bool Pipelining = false>
struct ibvt_qp_dv : public ibvt_qp_rc {
	ibvt_qp_dv(ibvt_env &e, ibvt_pd &p, ibvt_cq &c) :
		ibvt_qp_rc(e, p, c) {}

	virtual void init_attr(struct ibv_qp_init_attr_ex &attr) override {
		ibvt_qp_rc::init_attr(attr);
		attr.cap.max_send_wr = MaxSendWr;
		attr.cap.max_send_sge = MaxSendSge;
		attr.cap.max_recv_wr = MaxRecvWr;
		attr.cap.max_recv_sge = MaxRecvSge;
		attr.cap.max_inline_data = MaxInlineData;
		attr.comp_mask |= IBV_QP_INIT_ATTR_SEND_OPS_FLAGS;
		attr.send_ops_flags = IBV_QP_EX_WITH_RDMA_WRITE |
				      IBV_QP_EX_WITH_SEND |
				      IBV_QP_EX_WITH_RDMA_READ |
				      IBV_QP_EX_WITH_LOCAL_INV;
	}

	virtual void init_dv_attr(struct mlx5dv_qp_init_attr &dv_attr) {
		dv_attr.comp_mask = MLX5DV_QP_INIT_ATTR_MASK_SEND_OPS_FLAGS;
		dv_attr.send_ops_flags = MLX5DV_QP_EX_WITH_MKEY_CONFIGURE;
		if (Pipelining) {
			dv_attr.comp_mask |=
			    MLX5DV_QP_INIT_ATTR_MASK_QP_CREATE_FLAGS;
			dv_attr.create_flags = MLX5DV_QP_CREATE_SIG_PIPELINING;
		}
	}

	virtual void init() override {
		struct ibv_qp_init_attr_ex attr = {};
		struct mlx5dv_qp_init_attr dv_attr = {};

		INIT(pd.init());
		INIT(cq.init());

		init_attr(attr);
		init_dv_attr(dv_attr);
		SET(qp, mlx5dv_create_qp(pd.ctx.ctx, &attr, &dv_attr));
	}

	virtual void wr_start() {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp);
		EXECL(ibv_wr_start(qpx));
	}

	virtual void wr_complete(int status = 0) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp);
		ASSERT_EQ(status, ibv_wr_complete(qpx));
	}

	virtual void wr_id(uint64_t id) {
		ibv_qp_to_qp_ex(qp)->wr_id = id;
	}

	virtual void wr_flags(unsigned int flags) {
		ibv_qp_to_qp_ex(qp)->wr_flags = flags;
	}

	virtual void wr_rdma_read(struct ibv_sge local_sge, struct ibv_sge remote_sge) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp);
		ibv_wr_rdma_read(qpx, remote_sge.lkey, remote_sge.addr);
		ibv_wr_set_sge_list(qpx, 1, &local_sge);
	}

	virtual void wr_rdma_write(struct ibv_sge local_sge, struct ibv_sge remote_sge) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp);
		ibv_wr_rdma_write(qpx, remote_sge.lkey, remote_sge.addr);
		ibv_wr_set_sge_list(qpx, 1, &local_sge);
	}

	virtual void wr_send(struct ibv_sge local_sge) {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp);
		ibv_wr_send(qpx);
		ibv_wr_set_sge_list(qpx, 1, &local_sge);
	}

	virtual void cancel_posted_wrs(uint64_t wr_id, int wr_num) {
		struct mlx5dv_qp_ex *dv_qp;
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp);

		dv_qp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);
		int ret = mlx5dv_qp_cancel_posted_send_wrs(dv_qp, wr_id);
		ASSERT_EQ(wr_num, ret);
	}

	virtual void modify_qp_to_rts() {
		struct ibv_qp_attr attr;

		memset(&attr, 0, sizeof(attr));

		attr.qp_state = IBV_QPS_RTS;
		attr.cur_qp_state = IBV_QPS_SQD;
		DO(ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_CUR_STATE));
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
	virtual void check(int err_type) = 0;
	virtual void check(int err_type, uint64_t actual, uint64_t expected,
			   uint64_t offset) = 0;
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

	virtual void check(int err_type) override {
		struct mlx5dv_mkey_err err;
		DO(mlx5dv_mkey_check(mlx5_mkey, &err));
		ASSERT_EQ(err_type, err.err_type);
	}

	virtual void check(int err_type, uint64_t actual, uint64_t expected,
			   uint64_t offset) override {
		struct mlx5dv_mkey_err err;
		struct mlx5dv_sig_err &sig_err = err.err.sig;
		DO(mlx5dv_mkey_check(mlx5_mkey, &err));
		ASSERT_EQ(err_type, err.err_type);
		ASSERT_EQ(actual, sig_err.actual_value);
		ASSERT_EQ(expected, sig_err.expected_value);
		ASSERT_EQ(offset, sig_err.offset);
	}
};

struct mkey_setter {
	virtual ~mkey_setter() = default;
	virtual void init() {};
	virtual void wr_set(ibvt_qp &qp) = 0;
	virtual size_t adjust_length(size_t length) { return length; };
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

		mlx5dv_wr_set_mkey_layout_list(mqp, sgl.size(), sgl.data());
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
		std::vector<struct ibv_sge> sgl;

		for (auto &s: { Sizes... }) {
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

template <typename T, T V, size_t Index>
struct deindex_v {
	static constexpr T value = V;
};

template <typename T,
	  T V,
	  size_t N,
	  template<T...> typename TT,
	  typename Indices = std::make_index_sequence<N> >
struct repeat_v;

template <typename T,
	  T V,
	  size_t N,
	  template<T...> typename TT,
	  size_t... Indices>
struct repeat_v<T, V, N, TT, std::index_sequence<Indices...>> {
	using type = TT<deindex_v<T, V, Indices>::value...>;
};

template <size_t Size, size_t Count>
using mkey_layout_new_list_fixed_mrs = typename repeat_v<size_t, Size, Count, mkey_layout_new_list_mrs>::type;

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

		mlx5dv_wr_set_mkey_layout_interleaved(mqp,
						      repeat_count,
						      interleaved.size(),
						      interleaved.data());
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
		std::initializer_list<uint32_t> tmp_interleaved = { Interleaved... };
		std::vector<struct mlx5dv_mr_interleaved> mlx5_interleaved;

		static_assert(sizeof...(Interleaved) % 2 == 0, "Number of interleaved is not multiple of 2");
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

template<enum mlx5dv_sig_t10dif_bg_type BgType, enum mlx5dv_sig_t10dif_bg_caps BgTypeCaps>
struct mkey_sig_t10dif_type {
	static const enum mlx5dv_sig_t10dif_bg_type mlx5_t10dif_type = BgType;
	static const enum mlx5dv_sig_t10dif_bg_caps mlx5_t10dif_caps = BgTypeCaps;
};

typedef mkey_sig_t10dif_type<MLX5DV_SIG_T10DIF_CRC, MLX5DV_SIG_T10DIF_BG_CAP_CRC> mkey_sig_t10dif_crc;
typedef mkey_sig_t10dif_type<MLX5DV_SIG_T10DIF_CSUM, MLX5DV_SIG_T10DIF_BG_CAP_CSUM> mkey_sig_t10dif_csum;

template<typename BgType, uint16_t Bg, uint16_t AppTag, uint32_t RefTag>
struct mkey_sig_t10dif_type1 {
	static constexpr uint32_t sig_size = 8;
	struct mlx5dv_sig_t10dif dif;

	void set_sig(struct mlx5dv_sig_block_domain &domain) {
		domain.sig_type = MLX5DV_SIG_TYPE_T10DIF;
		dif.bg_type = BgType::mlx5_t10dif_type;
		dif.bg = Bg;
		dif.app_tag = AppTag;
		dif.ref_tag = RefTag;
		dif.flags = MLX5DV_SIG_T10DIF_FLAG_REF_REMAP |
			    MLX5DV_SIG_T10DIF_FLAG_APP_ESCAPE;
		domain.sig.dif = &dif;
		domain.comp_mask = 0;
	}

	static bool is_supported(struct mlx5dv_context &attr) {
		return attr.sig_caps.t10dif_bg & BgType::mlx5_t10dif_caps &&
		       attr.sig_caps.block_prot & MLX5DV_SIG_PROT_CAP_T10DIF;
	}
};

template<typename BgType, uint16_t Bg, uint16_t AppTag, uint32_t RefTag>
struct mkey_sig_t10dif_type3 {
	static constexpr uint32_t sig_size = 8;
	struct mlx5dv_sig_t10dif dif;

	void set_sig(struct mlx5dv_sig_block_domain &domain) {
		domain.sig_type = MLX5DV_SIG_TYPE_T10DIF;
		dif.bg_type = BgType::mlx5_t10dif_type;
		dif.bg = Bg;
		dif.app_tag = AppTag;
		dif.ref_tag = RefTag;
		dif.flags = MLX5DV_SIG_T10DIF_FLAG_APP_ESCAPE |
			    MLX5DV_SIG_T10DIF_FLAG_APP_REF_ESCAPE;
		domain.sig.dif = &dif;
		domain.comp_mask = 0;
	}

	static bool is_supported(struct mlx5dv_context &attr) {
		return attr.sig_caps.t10dif_bg & BgType::mlx5_t10dif_caps &&
		       attr.sig_caps.block_prot & MLX5DV_SIG_PROT_CAP_T10DIF;
	}
};

template<enum mlx5dv_sig_crc_type CrcType, enum mlx5dv_sig_crc_type_caps CrcTypeCaps>
struct mkey_sig_crc_type {
	static const enum mlx5dv_sig_crc_type mlx5_crc_type = CrcType;
	static const enum mlx5dv_sig_crc_type_caps mlx5_crc_type_caps = CrcTypeCaps;
};

typedef mkey_sig_crc_type<MLX5DV_SIG_CRC_TYPE_CRC32, MLX5DV_SIG_CRC_TYPE_CAP_CRC32> mkey_sig_crc_type_crc32;
typedef mkey_sig_crc_type<MLX5DV_SIG_CRC_TYPE_CRC32C, MLX5DV_SIG_CRC_TYPE_CAP_CRC32C> mkey_sig_crc_type_crc32c;
typedef mkey_sig_crc_type<MLX5DV_SIG_CRC_TYPE_CRC64, MLX5DV_SIG_CRC_TYPE_CAP_CRC64> mkey_sig_crc_type_crc64;

template<typename CrcType, uint32_t Seed>
struct mkey_sig_crc32 {
	static constexpr uint32_t sig_size = 4;
	struct mlx5dv_sig_crc crc;

	void set_sig(struct mlx5dv_sig_block_domain &domain) {
		domain.sig_type = MLX5DV_SIG_TYPE_CRC;
		crc.type = CrcType::mlx5_crc_type;
		crc.seed = Seed;
		domain.sig.crc = &crc;
		domain.comp_mask = 0;
	}

	static bool is_supported(struct mlx5dv_context &attr) {
		return attr.sig_caps.crc_type & CrcType::mlx5_crc_type_caps &&
		       attr.sig_caps.block_prot & MLX5DV_SIG_PROT_CAP_CRC;
	}
};

template<typename CrcType, uint64_t Seed>
struct mkey_sig_crc64 {
	static constexpr uint32_t sig_size = 8;
	struct mlx5dv_sig_crc crc;

	void set_sig(struct mlx5dv_sig_block_domain &domain) {
		domain.sig_type = MLX5DV_SIG_TYPE_CRC;
		crc.type = CrcType::mlx5_crc_type;
		crc.seed = Seed;
		domain.sig.crc = &crc;
		domain.comp_mask = 0;
	}

	static bool is_supported(struct mlx5dv_context &attr) {
		return attr.sig_caps.crc_type & CrcType::mlx5_crc_type_caps &&
		       attr.sig_caps.block_prot & MLX5DV_SIG_PROT_CAP_CRC;
	}
};

template<enum mlx5dv_block_size Mlx5BlockSize,
	 enum mlx5dv_block_size_caps Mlx5BlockSizeCaps,
	 uint32_t BlockSize>
struct mkey_block_size {
	static const enum mlx5dv_block_size mlx5_block_size = Mlx5BlockSize;
	static const enum mlx5dv_block_size_caps mlx5_block_size_caps = Mlx5BlockSizeCaps;
	static const uint32_t block_size = BlockSize;
};

typedef mkey_block_size<MLX5DV_BLOCK_SIZE_512, MLX5DV_BLOCK_SIZE_CAP_512, 512> mkey_block_size_512;
typedef mkey_block_size<MLX5DV_BLOCK_SIZE_520, MLX5DV_BLOCK_SIZE_CAP_520, 520> mkey_block_size_520;
typedef mkey_block_size<MLX5DV_BLOCK_SIZE_4048, MLX5DV_BLOCK_SIZE_CAP_4048, 4048> mkey_block_size_4048;
typedef mkey_block_size<MLX5DV_BLOCK_SIZE_4096, MLX5DV_BLOCK_SIZE_CAP_4096, 4096> mkey_block_size_4096;
typedef mkey_block_size<MLX5DV_BLOCK_SIZE_4160, MLX5DV_BLOCK_SIZE_CAP_4160, 4160> mkey_block_size_4160;

template<typename Sig, typename BlockSize>
struct mkey_sig_block_domain {
	typedef BlockSize BlockSizeType;
	typedef Sig SigType;

	struct mlx5dv_sig_block_domain domain;
	Sig sig;

	void set_domain(const mlx5dv_sig_block_domain **d) {
		sig.set_sig(domain);
		domain.block_size = BlockSize::mlx5_block_size;
		*d = &domain;
	}

	static bool is_supported(struct mlx5dv_context &attr) {
		return attr.sig_caps.block_size & BlockSizeType::mlx5_block_size_caps &&
			SigType::is_supported(attr);
	}
};

struct mkey_sig_block_domain_none {
	typedef mkey_block_size_512 BlockSizeType;
	typedef struct mkey_sig_none {
		static constexpr uint32_t sig_size = 0;
	} SigType;

	void set_domain(const mlx5dv_sig_block_domain **d) {
		*d = NULL;
	}

	static bool is_supported(struct mlx5dv_context &attr) {
		return true;
	}
};

#define MLX5DV_SIG_CHECK_T10DIF_APPTAG_BYTE1 0x20
#define MLX5DV_SIG_CHECK_T10DIF_APPTAG_BYTE0 0x10

template<typename MemDomain, typename WireDomain, uint8_t CheckMask = 0xFF>
struct mkey_sig_block : public mkey_setter {
	typedef MemDomain MemDomainType;
	typedef WireDomain WireDomainType;

	mkey_sig_block(ibvt_env &env, ibvt_pd &pd) {}

	virtual void wr_set(ibvt_qp &qp) override {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *mqp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);
		struct mlx5dv_sig_block_attr attr = {};

		MemDomain mem;
		WireDomain wire;
		mem.set_domain(&attr.mem);
		wire.set_domain(&attr.wire);
		attr.check_mask = CheckMask;
		mlx5dv_wr_set_mkey_sig_block(mqp, &attr);
	}

	virtual size_t adjust_length(size_t length) {
		size_t mem_num_blocks = length / (MemDomainType::BlockSizeType::block_size + MemDomainType::SigType::sig_size);
		size_t data_length = length - mem_num_blocks * MemDomainType::SigType::sig_size;
		size_t wire_num_blocks = data_length / WireDomainType::BlockSizeType::block_size;
		size_t wire_length = data_length + wire_num_blocks * WireDomainType::SigType::sig_size;
		return wire_length;
	}

	static bool is_supported(struct mlx5dv_context &attr) {
		return attr.comp_mask & MLX5DV_CONTEXT_MASK_SIGNATURE_OFFLOAD &&
			MemDomainType::is_supported(attr) &&
			WireDomainType::is_supported(attr);
	}
};

// Some helper types
typedef mkey_sig_crc32<mkey_sig_crc_type_crc32, 0xFFFFFFFF> mkey_sig_crc32ieee;
typedef mkey_sig_crc32<mkey_sig_crc_type_crc32c, 0xFFFFFFFF> mkey_sig_crc32c;
typedef mkey_sig_crc64<mkey_sig_crc_type_crc64, 0xFFFFFFFFFFFFFFFF> mkey_sig_crc64xp10;
typedef mkey_sig_t10dif_type1<mkey_sig_t10dif_crc, 0xffff, 0x5678, 0xf0debc9a> mkey_sig_t10dif_crc_type1_default;
typedef mkey_sig_t10dif_type3<mkey_sig_t10dif_crc, 0xffff, 0x5678, 0xf0debc9a> mkey_sig_t10dif_crc_type3_default;

typedef mkey_sig_t10dif_type1<mkey_sig_t10dif_csum, 0xffff, 0x5678, 0xf0debc9a> mkey_sig_t10dif_csum_type1_default;
typedef mkey_sig_t10dif_type3<mkey_sig_t10dif_csum, 0xffff, 0x5678, 0xf0debc9a> mkey_sig_t10dif_csum_type3_default;

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
		struct mlx5dv_mkey_conf_attr attr = {};

		EXECL(mlx5dv_wr_mkey_configure(mqp, mlx5_mkey, setters.size(), &attr));
		for (auto s : setters) {
			EXECL(s->wr_set(qp));
		}
	}

	virtual struct ibv_sge sge() override {
		size_t length = layout ? layout->data_length() : 0;
		for (auto s : setters) {
			length = s->adjust_length(length);
		}
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

	void trigger_poll() {
		struct ibv_cq_ex *cq_ex = cq.cq2();
		struct ibv_poll_cq_attr attr = {};

		ASSERT_EQ(ENOENT, ibv_start_poll(cq_ex, &attr));
	}
};

struct rdma_op {
	template<typename QP>
	void check_completion(mkey_test_side<QP> &side, enum ibv_wc_status status = IBV_WC_SUCCESS) {
		ibvt_wc wc(side.cq);
		side.cq.do_poll(wc);
		ASSERT_EQ(status, wc().status);
	}

	template<typename QP>
	void check_completion(mkey_test_side<QP> &side,
				     enum ibv_wc_opcode opcode,
				     enum ibv_wc_status status = IBV_WC_SUCCESS) {
		ibvt_wc wc(side.cq);
		side.cq.do_poll(wc);
		ASSERT_EQ(status, wc().status);
		ASSERT_EQ(opcode, wc().opcode);
	}
};

struct rdma_op_write : rdma_op {
	template<typename QP>
	void wr_submit(mkey_test_side<QP> &src_side,
			       ibv_sge src_sge,
			       mkey_test_side<QP> &dst_side,
			       ibv_sge dst_sge) {
		src_side.qp.wr_flags(IBV_SEND_SIGNALED);
		src_side.qp.wr_rdma_write(src_sge, dst_sge);
	}

	template<typename QP>
	void submit(mkey_test_side<QP> &src_side,
			    ibv_sge src_sge,
			    mkey_test_side<QP> &dst_side,
			    ibv_sge dst_sge) {
		src_side.qp.wr_start();
		wr_submit(src_side, src_sge, dst_side, dst_sge);
		src_side.qp.wr_complete();
	}

	template<typename QP>
	void complete(mkey_test_side<QP> &src_side,
			      mkey_test_side<QP> &dst_side,
			      enum ibv_wc_status src_status = IBV_WC_SUCCESS,
			      enum ibv_wc_status dst_status = IBV_WC_SUCCESS) {
		this->check_completion(src_side, src_status);
		dst_side.trigger_poll();
	}
};

struct rdma_op_read : rdma_op {
	template<typename QP>
	void wr_submit(mkey_test_side<QP> &src_side,
			       ibv_sge src_sge,
			       mkey_test_side<QP> &dst_side,
			       ibv_sge dst_sge) {
		dst_side.qp.wr_flags(IBV_SEND_SIGNALED);
		dst_side.qp.wr_rdma_read(dst_sge, src_sge);
	}

	template<typename QP>
	void submit(mkey_test_side<QP> &src_side,
			    ibv_sge src_sge,
			    mkey_test_side<QP> &dst_side,
			    ibv_sge dst_sge) {
		dst_side.qp.wr_start();
		wr_submit(src_side, src_sge, dst_side, dst_sge);
		dst_side.qp.wr_complete();
	}

	template<typename QP>
	void complete(mkey_test_side<QP> &src_side,
			      mkey_test_side<QP> &dst_side,
			      enum ibv_wc_status src_status = IBV_WC_SUCCESS,
			      enum ibv_wc_status dst_status = IBV_WC_SUCCESS) {
		this->check_completion(dst_side, dst_status);
		src_side.trigger_poll();
	}
};

struct rdma_op_send : rdma_op {
	template<typename QP>
	void wr_submit(mkey_test_side<QP> &src_side,
			       ibv_sge src_sge,
			       mkey_test_side<QP> &dst_side,
			       ibv_sge dst_sge) {
		// @todo: chaining for recv part is not implemented
		dst_side.qp.recv(dst_sge);
		src_side.qp.wr_flags(IBV_SEND_SIGNALED);
		src_side.qp.wr_send(src_sge);
	}

	template<typename QP>
	void submit(mkey_test_side<QP> &src_side,
			    ibv_sge src_sge,
			    mkey_test_side<QP> &dst_side,
			    ibv_sge dst_sge) {
		dst_side.qp.recv(dst_sge);
		src_side.qp.wr_start();
		wr_submit(src_side, src_sge, dst_side, dst_sge);
		src_side.qp.wr_complete();
	}

	template<typename QP>
	void complete(mkey_test_side<QP> &src_side,
			      mkey_test_side<QP> &dst_side,
			      enum ibv_wc_status src_status = IBV_WC_SUCCESS,
			      enum ibv_wc_status dst_status = IBV_WC_SUCCESS) {
		this->check_completion(src_side, src_status);
		this->check_completion(dst_side, dst_status);
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
