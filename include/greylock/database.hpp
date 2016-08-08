#pragma once

#include "greylock/error.hpp"
#include "greylock/id.hpp"
#include "greylock/utils.hpp"

#include <ribosome/expiration.hpp>

#pragma GCC diagnostic push 
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/transaction_db.h>
#pragma GCC diagnostic pop

#include <msgpack.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <set>
#include <vector>

namespace ioremap { namespace greylock {

struct options {
	size_t tokens_shard_size = 100000*40;
	long transaction_expiration = 60000; // 60 seconds
	long transaction_lock_timeout = 60000; // 60 seconds

	int bits_per_key = 10; // bloom filter parameter

	long lru_cache_size = 100 * 1024 * 1024; // 100 MB of uncompressed data cache

	long sync_metadata_timeout = 60000; // 60 seconds

	// mininmum size of the token which will go into separate index,
	// if token size is smaller, it will be combined into 2 indexes
	// with the previous and next tokens.
	// This options greatly speeds up requests with small words (like [to be or not to be]),
	// but heavily increases index size.
	unsigned int ngram_index_size = 0;

	std::string document_prefix;
	std::string token_shard_prefix;
	std::string index_prefix;
	std::string metadata_key;

	options():
		document_prefix("documents."),
		token_shard_prefix("token_shards."),
		index_prefix("index."),
		metadata_key("greylock.meta.key")
	{
	}
};

class metadata {
public:
	metadata() : m_dirty(false), m_seq(0) {}

	bool dirty() const {
		return m_dirty;
	}
	void clear_dirty() {
		m_dirty = false;
	}

	long get_sequence() {
		m_dirty = true;
		return m_seq++;
	}

	enum {
		serialize_version_2 = 2,
	};

	template <typename Stream>
	void msgpack_pack(msgpack::packer<Stream> &o) const {
		o.pack_array(metadata::serialize_version_2);
		o.pack((int)metadata::serialize_version_2);
		o.pack(m_seq.load());
	}

	void msgpack_unpack(msgpack::object o) {
		if (o.type != msgpack::type::ARRAY) {
			std::ostringstream ss;
			ss << "could not unpack metadata, object type is " << o.type <<
				", must be array (" << msgpack::type::ARRAY << ")";
			throw std::runtime_error(ss.str());
		}

		int version;
		long seq;

		msgpack::object *p = o.via.array.ptr;
		p[0].convert(&version);

		if (version != (int)o.via.array.size) {
			std::ostringstream ss;
			ss << "could not unpack document, invalid version: " << version << ", array size: " << o.via.array.size;
			throw std::runtime_error(ss.str());
		}

		switch (version) {
		case metadata::serialize_version_2:
			p[1].convert(&seq);
			m_seq.store(seq);
			break;
		default: {
			std::ostringstream ss;
			ss << "could not unpack metadata, invalid version " << version;
			throw std::runtime_error(ss.str());
		}
		}
	}

private:
	bool m_dirty;
	std::atomic_long m_seq;
};

struct document_for_index {
	id_t indexed_id;
	MSGPACK_DEFINE(indexed_id);

	bool operator<(const document_for_index &other) const {
		return indexed_id < other.indexed_id;
	}
};

struct disk_index {
	typedef document_for_index value_type;
	typedef document_for_index& reference;
	typedef document_for_index* pointer;

	std::vector<document_for_index> ids;

	MSGPACK_DEFINE(ids);
};

struct disk_token {
	std::vector<size_t> shards;
	MSGPACK_DEFINE(shards);

	disk_token() {}
	disk_token(const std::set<size_t> &s): shards(s.begin(), s.end()) {}
	disk_token(const std::vector<size_t> &s): shards(s) {}
};


class disk_index_merge_operator : public rocksdb::MergeOperator {
public:
	virtual const char* Name() const override {
		return "disk_index_merge_operator";
	}

	bool merge_index(const rocksdb::Slice& key, const rocksdb::Slice* old_value,
			const std::deque<std::string>& operand_list,
			std::string* new_value,
			rocksdb::Logger *logger) const {

		struct greylock::disk_index index;
		greylock::error_info err;
		std::set<document_for_index> unique_index;

		if (old_value) {
			err = deserialize(index, old_value->data(), old_value->size());
			if (err) {
				rocksdb::Error(logger, "merge: key: %s, index deserialize failed: %s [%d]",
						key.ToString().c_str(), err.message().c_str(), err.code());
				return false;
			}

			unique_index.insert(index.ids.begin(), index.ids.end());
		}

		for (const auto& value : operand_list) {
			document_for_index did;
			err = deserialize(did, value.data(), value.size());
			if (err) {
				rocksdb::Error(logger, "merge: key: %s, document deserialize failed: %s [%d]",
						key.ToString().c_str(), err.message().c_str(), err.code());
				return false;
			}

			unique_index.emplace(did);
			//printf("full merge: key: %s, indexed_id: %ld\n", key.ToString().c_str(), did.indexed_id);
		}

		index.ids.clear();
		index.ids.insert(index.ids.end(), unique_index.begin(), unique_index.end());
		*new_value = serialize(index);

		return true;
	}

	template <typename T>
	std::string dump_iterable(const T &iter) const {
		std::ostringstream ss;
		for (auto it = iter.begin(), end = iter.end(); it != end; ++it) {
			if (it != iter.begin())
				ss << " ";
			ss << *it;
		}
		return ss.str();
	}
	bool merge_token_shards(const rocksdb::Slice& key, const rocksdb::Slice* old_value,
			const std::deque<std::string>& operand_list,
			std::string* new_value,
			rocksdb::Logger *logger) const {

		disk_token dt;
		std::set<size_t> shards;
		greylock::error_info err;

		if (old_value) {
			err = deserialize(dt, old_value->data(), old_value->size());
			if (err) {
				rocksdb::Error(logger, "merge: key: %s, disk_token deserialize failed: %s [%d]",
						key.ToString().c_str(), err.message().c_str(), err.code());
				return false;
			}

			shards.insert(dt.shards.begin(), dt.shards.end());
		}

		for (const auto& value : operand_list) {
			disk_token s;
			err = deserialize(s, value.data(), value.size());
			if (err) {
				rocksdb::Error(logger, "merge: key: %s, disk_token operand deserialize failed: %s [%d]",
						key.ToString().c_str(), err.message().c_str(), err.code());
				return false;
			}

			shards.insert(s.shards.begin(), s.shards.end());
		}

		dt.shards = std::vector<size_t>(shards.begin(), shards.end());
		*new_value = serialize(dt);

		return true;
	}

	virtual bool FullMerge(const rocksdb::Slice& key, const rocksdb::Slice* old_value,
			const std::deque<std::string>& operand_list,
			std::string* new_value,
			rocksdb::Logger *logger) const override {
		if (key.starts_with(rocksdb::Slice("token_shards."))) {
			return merge_token_shards(key, old_value, operand_list, new_value, logger);
		}
		if (key.starts_with(rocksdb::Slice("index."))) {
			return merge_index(key, old_value, operand_list, new_value, logger);
		}

		return false;
	}

	virtual bool PartialMerge(const rocksdb::Slice& key,
			const rocksdb::Slice& left_operand, const rocksdb::Slice& right_operand,
			std::string* new_value,
			rocksdb::Logger* logger) const {
#if 0
		auto dump = [](const rocksdb::Slice &v) {
			std::ostringstream ss;

			msgpack::unpacked msg;
			msgpack::unpack(&msg, v.data(), v.size());

			ss << msg.get();
			return ss.str();
		};

		printf("partial merge: key: %s, left: %s, right: %s\n",
				key.ToString().c_str(), dump(left_operand).c_str(), dump(right_operand).c_str());
#endif
		(void) key;
		(void) left_operand;
		(void) right_operand;
		(void) new_value;
		(void) logger;

		return false;
	}
};

class read_only_database {
public:
	greylock::error_info open(const std::string &path) {
		rocksdb::Options options;
		options.max_open_files = 1000;
		options.merge_operator.reset(new disk_index_merge_operator);

		rocksdb::BlockBasedTableOptions table_options;
		table_options.block_cache = rocksdb::NewLRUCache(100 * 1048576); // 100MB of uncompresseed data cache
		table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, true));
		options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

		rocksdb::DB *db;

		rocksdb::Status s = rocksdb::DB::OpenForReadOnly(options, path, &db);
		if (!s.ok()) {
			return greylock::create_error(-s.code(), "failed to open rocksdb database: '%s', error: %s",
					path.c_str(), s.ToString().c_str());
		}
		m_db.reset(db);

		return greylock::error_info();
	}

	greylock::error_info read(const std::string &key, std::string *ret) {
		auto s = m_db->Get(rocksdb::ReadOptions(), rocksdb::Slice(key), ret);
		if (!s.ok()) {
			return greylock::create_error(-s.code(), "could not read key: %s, error: %s", key.c_str(), s.ToString().c_str());
		}
		return greylock::error_info();
	}

	std::vector<size_t> get_shards(const std::string &key) {
		disk_token dt;

		std::string ser_shards;
		auto err = read(key, &ser_shards);
		if (err) {
			//printf("could not read shards, key: %s, err: %s [%d]\n", key.c_str(), err.message().c_str(), err.code());
			return dt.shards;
		}

		err = deserialize(dt, ser_shards.data(), ser_shards.size());
		if (err)
			return dt.shards;

		return dt.shards;
	}

	const greylock::options &options() const {
		return m_opts;
	}

private:
	std::unique_ptr<rocksdb::DB> m_db;
	greylock::options m_opts;
};


class database {
public:
	~database() {
		m_expiration_timer.stop();

		sync_metadata(NULL);
	}

	const greylock::options &options() const {
		return m_opts;
	}
	greylock::metadata &metadata() {
		return m_meta;
	}

	void compact() {
		struct rocksdb::CompactRangeOptions opts;
		opts.change_level = true;
		opts.target_level = 0;
		m_db->CompactRange(opts, NULL, NULL);
	}

	greylock::error_info sync_metadata(rocksdb::WriteBatch *batch) {
		if (!m_meta.dirty())
			return greylock::error_info();

		std::string meta_serialized = serialize(m_meta);

		rocksdb::Status s;
		if (batch) {
			batch->Put(rocksdb::Slice(m_opts.metadata_key), rocksdb::Slice(meta_serialized));
		} else {
			s = m_db->Put(rocksdb::WriteOptions(), rocksdb::Slice(m_opts.metadata_key), rocksdb::Slice(meta_serialized));
		}

		if (!s.ok()) {
			return greylock::create_error(-s.code(), "could not write metadata key: %s, error: %s",
					m_opts.metadata_key.c_str(), s.ToString().c_str());
		}

		m_meta.clear_dirty();
		return greylock::error_info();
	}

	greylock::error_info open(const std::string &path) {
		rocksdb::Options dbo;
		dbo.max_open_files = 1000;
		//dbo.disableDataSync = true;

		dbo.compression = rocksdb::kLZ4HCCompression;

		dbo.create_if_missing = true;
		dbo.create_missing_column_families = true;

		dbo.merge_operator.reset(new disk_index_merge_operator);

		//dbo.prefix_extractor.reset(NewFixedPrefixTransform(3));
		//dbo.memtable_prefix_bloom_bits = 100000000;
		//dbo.memtable_prefix_bloom_probes = 6;

		rocksdb::BlockBasedTableOptions table_options;
		table_options.block_cache = rocksdb::NewLRUCache(m_opts.lru_cache_size); // 100MB of uncompresseed data cache
		table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(m_opts.bits_per_key, true));
		dbo.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

		rocksdb::DB *db;

		rocksdb::Status s = rocksdb::DB::Open(dbo, path, &db);
		if (!s.ok()) {
			return greylock::create_error(-s.code(), "failed to open rocksdb database: '%s', error: %s",
					path.c_str(), s.ToString().c_str());
		}
		m_db.reset(db);

		std::string meta;
		s = m_db->Get(rocksdb::ReadOptions(), rocksdb::Slice(m_opts.metadata_key), &meta);
		if (!s.ok() && !s.IsNotFound()) {
			return greylock::create_error(-s.code(), "could not read key: %s, error: %s",
					m_opts.metadata_key.c_str(), s.ToString().c_str());
		}

		if (s.ok()) {
			auto err = deserialize(m_meta, meta.data(), meta.size());
			if (err)
				return greylock::create_error(err.code(), "metadata deserialization failed, key: %s, error: %s",
					m_opts.metadata_key.c_str(), err.message().c_str());
		}

		if (m_opts.sync_metadata_timeout > 0) {
			sync_metadata_callback();
		}

		return greylock::error_info(); 
	}

	std::vector<size_t> get_shards(const std::string &key) {
		disk_token dt;

		std::string ser_shards;
		auto err = read(key, &ser_shards);
		if (err)
			return dt.shards;

		err = deserialize(dt, ser_shards.data(), ser_shards.size());
		if (err)
			return dt.shards;

		return dt.shards;
	}

	greylock::error_info read(const std::string &key, std::string *ret) {
		auto s = m_db->Get(rocksdb::ReadOptions(), rocksdb::Slice(key), ret);
		if (!s.ok()) {
			return greylock::create_error(-s.code(), "could not read key: %s, error: %s", key.c_str(), s.ToString().c_str());
		}
		return greylock::error_info();
	}

	greylock::error_info write(rocksdb::WriteBatch *batch) {
		auto wo = rocksdb::WriteOptions();

		auto s = m_db->Write(wo, batch);
		if (!s.ok()) {
			return greylock::create_error(-s.code(), "could not write batch: %s", s.ToString().c_str());
		}

		return greylock::error_info();
	}

private:
	std::unique_ptr<rocksdb::DB> m_db;
	greylock::options m_opts;
	greylock::metadata m_meta;

	ribosome::expiration m_expiration_timer;

	void sync_metadata_callback() {
		sync_metadata(NULL);

		auto expires_at = std::chrono::system_clock::now() + std::chrono::milliseconds(m_opts.sync_metadata_timeout);
		m_expiration_timer.insert(expires_at, std::bind(&database::sync_metadata_callback, this));
	}
};

}} // namespace ioremap::greylock
