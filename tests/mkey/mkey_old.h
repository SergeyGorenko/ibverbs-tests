#ifndef MKEY_OLD_H
#define MKEY_OLD_H

struct mkey_layout_old {
	virtual ~mkey_layout_old() = default;
	virtual void init() {};
	virtual void wr_set(ibvt_qp &qp, mkey_dv &mkey, uint32_t access_flags) = 0;
	virtual size_t data_length() = 0;
	virtual void set_data(const uint8_t *buf, size_t length) = 0;
	virtual void get_data(uint8_t *buf, size_t length) = 0;
	virtual void dump(size_t offset = 0, size_t length = 0, const char *pfx = "") {}
};

struct mkey_layout_old_none : public mkey_layout_old {
	mkey_layout_old_none(ibvt_env &env, ibvt_pd &pd) {}
	virtual void wr_set(ibvt_qp &qp, mkey_dv &mkey, uint32_t access_flags) {}
	virtual size_t data_length() { return 0; }
	virtual void set_data(const uint8_t *buf, size_t length) {}
	virtual void get_data(uint8_t *buf, size_t length) {}
};

struct mkey_layout_old_list : public mkey_layout_old {
	std::vector<struct ibv_sge> sgl;

	mkey_layout_old_list() :
		sgl() {}

	virtual ~mkey_layout_old_list() = default;

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

	virtual void wr_set(ibvt_qp &qp, mkey_dv &mkey, uint32_t access_flags) override {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *mqp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);

		mlx5dv_wr_mr_list(mqp,
				  mkey.mlx5_mkey,
				  access_flags,
				  sgl.size(),
				  sgl.data());
	}

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
struct mkey_layout_old_list_mrs : public mkey_layout_old_list {
	ibvt_env &env;
	ibvt_pd &pd;
	std::vector<struct ibvt_mr> mrs;
	bool initialized;

	mkey_layout_old_list_mrs(ibvt_env &env, ibvt_pd &pd) :
		env(env), pd(pd), initialized(false) {}

	virtual ~mkey_layout_old_list_mrs() = default;

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

		mkey_layout_old_list::init(sgl);
	}

	virtual void dump(size_t offset = 0, size_t length = 0, const char *pfx = "") override {
		for (auto &mr : mrs) {
			mr.dump(offset, std::min(length, mr.size), pfx);
			length -= mr.size;
		}
	}
};

struct mkey_layout_old_interleaved : public mkey_layout_old {
	uint16_t repeat_count;
	std::vector<struct mlx5dv_mr_interleaved> interleaved;

	mkey_layout_old_interleaved() :
		repeat_count(0),
		interleaved() {}

	virtual ~mkey_layout_old_interleaved() = default;

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

	virtual void wr_set(ibvt_qp &qp, mkey_dv &mkey, uint32_t access_flags) override {
		struct ibv_qp_ex *qpx = ibv_qp_to_qp_ex(qp.qp);
		struct mlx5dv_qp_ex *mqp = mlx5dv_qp_ex_from_ibv_qp_ex(qpx);

		mlx5dv_wr_mr_interleaved(mqp,
					 mkey.mlx5_mkey,
					 access_flags,
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
struct mkey_layout_old_interleaved_mrs : public mkey_layout_old_interleaved {
	ibvt_env &env;
	ibvt_pd &pd;
	std::vector<struct ibvt_mr> mrs;
	bool initialized;

	mkey_layout_old_interleaved_mrs(ibvt_env &env, ibvt_pd &pd) :
		env(env), pd(pd), initialized(false) {}

	virtual ~mkey_layout_old_interleaved_mrs() = default;

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

		mkey_layout_old_interleaved::init(RepeatCount, mlx5_interleaved);
	}

	virtual void dump(size_t offset = 0, size_t length = 0, const char *pfx = "") override {
		for (auto &mr : mrs) {
			mr.dump(offset, std::min(length, mr.size), pfx);
			length -= mr.size;
		}
	}
};

template<typename Layout = mkey_layout_old_none,
	 uint32_t AccessFlags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE>
struct mkey_dv_old : public mkey_dv {
	/* @todo: do we need to store layout? It is required only in configure.
	 * May be pass it to configure or define in subclass
	 */
	struct mkey_layout_old *preset_layout;
	struct mkey_layout_old *layout;
	uint32_t access_flags;
	bool initialized;

	mkey_dv_old(ibvt_env &env, ibvt_pd &pd, uint16_t me, uint32_t cf) :
		mkey_dv(env, pd, me, cf),
		access_flags(AccessFlags),
		initialized(false) {
		preset_layout = new Layout(env, pd);
		layout = preset_layout;
	}

	virtual ~mkey_dv_old() {
		if (preset_layout) delete preset_layout;
	};

	virtual void init() override {
		if (initialized)
			return;

		initialized = true;
		mkey_dv::init();
		if (layout) layout->init();
	}

	void set_layout(struct mkey_layout_old *layout) {
		this->layout = layout;
	}

	void set_access_flags(uint32_t access_flags) {
		this->access_flags = access_flags;
	}

	virtual void wr_configure(ibvt_qp &qp) override {
		EXEC(layout->wr_set(qp, *this, access_flags));
	}

	virtual struct ibv_sge sge() override {
		size_t length = layout ? layout->data_length() : 0;
		return mkey_dv::sge(0, length);
	}

	virtual void dump(size_t offset = 0, size_t length = 0, const char *pfx = "") override {
		if (layout) layout->dump(offset, length, pfx);
	}
};

#endif /* MKEY_OLD_H */
