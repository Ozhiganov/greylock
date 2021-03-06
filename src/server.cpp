#include "greylock/database.hpp"
#include "greylock/error.hpp"
#include "greylock/json.hpp"
#include "greylock/jsonvalue.hpp"
#include "greylock/intersection.hpp"
#include "greylock/types.hpp"
#include "greylock/utils.hpp"

#include <unistd.h>
#include <signal.h>

#include <thevoid/server.hpp>
#include <thevoid/stream.hpp>

#include <ribosome/html.hpp>
#include <ribosome/split.hpp>
#include <ribosome/timer.hpp>

#include <swarm/logger.hpp>

#include <msgpack.hpp>

#include <functional>
#include <string>
#include <thread>

#define ILOG(level, a...) BH_LOG(logger(), level, ##a)
#define ILOG_ERROR(a...) ILOG(SWARM_LOG_ERROR, ##a)
#define ILOG_WARNING(a...) ILOG(SWARM_LOG_WARNING, ##a)
#define ILOG_INFO(a...) ILOG(SWARM_LOG_INFO, ##a)
#define ILOG_NOTICE(a...) ILOG(SWARM_LOG_NOTICE, ##a)
#define ILOG_DEBUG(a...) ILOG(SWARM_LOG_DEBUG, ##a)

using namespace ioremap;

template <typename Server>
struct simple_request_stream_error : public thevoid::simple_request_stream<Server> {
	void send_error(int status, int error, const char *fmt, ...) {
		va_list args;
		va_start(args, fmt);

		char buffer[1024];
		int sz = vsnprintf(buffer, sizeof(buffer), fmt, args);

		BH_LOG(this->server()->logger(), SWARM_LOG_ERROR, "%s: %d", buffer, error);

		greylock::JsonValue val;
		rapidjson::Value ev(rapidjson::kObjectType);


		rapidjson::Value esv(buffer, sz, val.GetAllocator());
		ev.AddMember("message", esv, val.GetAllocator());
		ev.AddMember("code", error, val.GetAllocator());
		val.AddMember("error", ev, val.GetAllocator());

		va_end(args);

		std::string data = val.ToString();

		thevoid::http_response http_reply;
		http_reply.set_code(status);
		http_reply.headers().set_content_length(data.size());
		http_reply.headers().set_content_type("text/json");

		this->send_reply(std::move(http_reply), std::move(data));
	}
};

class http_server : public thevoid::server<http_server>
{
public:
	virtual ~http_server() {
	}

	virtual bool initialize(const rapidjson::Value &config) {
		if (!rocksdb_init(config))
			return false;

		on<on_ping>(
			options::exact_match("/ping"),
			options::methods("GET")
		);

		on<on_compact>(
			options::exact_match("/compact"),
			options::methods("POST", "PUT")
		);

		on<on_index>(
			options::exact_match("/index"),
			options::methods("POST", "PUT")
		);

		on<on_search>(
			options::exact_match("/search"),
			options::methods("POST", "PUT")
		);

		return true;
	}

	struct on_ping : public simple_request_stream_error<http_server> {
		virtual void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
			(void) buffer;
			(void) req;

			this->send_reply(thevoid::http_response::ok);
		}
	};

	struct on_compact : public simple_request_stream_error<http_server> {
		virtual void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
			(void) req;
			(void) buffer;

			server()->db_docs().compact();
			server()->db_indexes().compact();
			this->send_reply(thevoid::http_response::ok);
		}
	};

	struct on_search : public simple_request_stream_error<http_server> {
		bool check_negation(const std::vector<greylock::token> &tokens, const std::vector<std::string> &content) {
			for (const auto &t: tokens) {
				for (const auto &word: content) {
					if (t.name == word) {
						return true;
					}
				}
			}

			return false;
		}

		bool check_exact(const std::vector<greylock::token> &tokens, const std::vector<std::string> &content) {
			auto check_token_positions = [] (const greylock::token &token,
					const std::vector<std::string> &content, size_t content_offset) -> bool {
				for (size_t pos: token.positions) {
					size_t offset = content_offset + pos;
					if (offset >= content.size()) {
						return false;
					}

					if (token.name != content[offset]) {
						return false;
					}
				}

				return true;
			};

			for (size_t content_offset = 0; content_offset < content.size(); ++content_offset) {
				bool match = true;

				for (const auto &token: tokens) {
					match = check_token_positions(token, content, content_offset);
					if (!match)
						break;
				}

				if (match)
					return true;
			}

			return false;
		}

		std::vector<std::string> split_content(const std::string &content) {
			std::vector<std::string> ret;

			ribosome::html_parser html;
			html.feed_text(content);

			ribosome::split spl;
			for (auto &t: html.tokens()) {
				ribosome::lstring lt = ribosome::lconvert::from_utf8(t);
				auto lower_request = ribosome::lconvert::to_lower(lt);

				auto all_words = spl.convert_split_words(lower_request, ".:,");
				for (auto &word: all_words) {
					ret.emplace_back(ribosome::lconvert::to_string(word));
				}
			}

			return ret;
		}

		// returns true if record has to be accepted, false - if record must be dropped
		bool check_result(const greylock::intersection_query &iq, greylock::single_doc_result &sd) {
			const greylock::document &doc = sd.doc;

			for (const auto &ent: iq.se) {
				for (const auto &attr: ent.idx.exact) {
					bool match;

					if (attr.name.find("title") != std::string::npos) {
						match = check_exact(attr.tokens, split_content(doc.ctx.title));
					} else {
						match = check_exact(attr.tokens, split_content(doc.ctx.content));
					}

					if (!match)
						return false;
				}
			}

			return true;
		}

		virtual void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
			(void) req;

			ribosome::timer search_tm;

			// this is needed to put ending zero-byte, otherwise rapidjson parser will explode
			std::string data(const_cast<char *>(boost::asio::buffer_cast<const char*>(buffer)),
					boost::asio::buffer_size(buffer));

			rapidjson::Document doc;
			doc.Parse<0>(data.c_str());

			if (doc.HasParseError()) {
				send_error(swarm::http_response::bad_request, -EINVAL,
						"search: could not parse document: %s, error offset: %d",
						doc.GetParseError(), doc.GetErrorOffset());
				return;
			}
			if (!doc.IsObject()) {
				send_error(swarm::http_response::bad_request, -EINVAL, "search: document must be object");
				return;
			}

			greylock::intersection_query iq;

			const auto &paging = greylock::get_object(doc, "paging");
			if (paging.IsObject()) {
				iq.next_document_id = greylock::id_t(greylock::get_string(paging, "next_document_id"));
				iq.max_number = greylock::get_int64(paging, "max_number", LONG_MAX);
			}

			long sec_start = 0, sec_end = LONG_MAX;
			const auto &time = greylock::get_object(doc, "time");
			if (time.IsObject()) {
				sec_start = greylock::get_int64(time, "start", sec_start);
				sec_end = greylock::get_int64(time, "end", sec_end);
			}
			iq.range_start.set_timestamp(sec_start, 0);
			iq.range_end.set_timestamp(sec_end, 0);


			std::vector<greylock::mailbox_query> se;
			const auto &request = greylock::get_object(doc, "request");
			if (!request.IsObject()) {
				send_error(swarm::http_response::bad_request, -EINVAL, "search: document must contain 'request' object");
				return;
			}

			for (auto it = request.MemberBegin(), jse_end = request.MemberEnd(); it != jse_end; ++it) {
				if (!it->value.IsObject()) {
					send_error(swarm::http_response::bad_request, -EINVAL,
							"search: mailbox query '%s' must contain object",
								it->name.GetString());
					return;
				}

				greylock::mailbox_query q(server()->db_indexes().options(), it->value);
				if (q.parse_error) {
					send_error(swarm::http_response::bad_request, q.parse_error.code(),
							"search: could not parse mailbox query: %s",
								q.parse_error.message().c_str());
					return;
				}

				q.mbox.assign(it->name.GetString(), it->name.GetStringLength());

				iq.se.emplace_back(std::move(q));
			}

			greylock::search_result result;
			greylock::intersector<greylock::database> inter(server()->db_docs(), server()->db_indexes());
			result = inter.intersect(iq, std::bind(&on_search::check_result, this, std::ref(iq), std::placeholders::_1));

			send_search_result(result);

			ILOG_INFO("search: query: %s, next_document_id: %s -> %s, indexes: %ld/%ld, completed: %d, duration: %d ms",
					iq.to_string().c_str(),
					iq.next_document_id.to_string().c_str(), result.next_document_id.to_string().c_str(),
					result.docs.size(), iq.max_number,
					result.completed, search_tm.elapsed());
		}

		void pack_string_array(rapidjson::Value &parent, rapidjson::Document::AllocatorType &allocator,
				const char *name, const std::vector<std::string> &data) {
			rapidjson::Value arr(rapidjson::kArrayType);
			for (const auto &s: data) {
				rapidjson::Value v(s.c_str(), s.size(), allocator);
				arr.PushBack(v, allocator);
			}

			parent.AddMember(name, arr, allocator);
		}

		template <typename T>
		void pack_simple_array(rapidjson::Value &parent, rapidjson::Document::AllocatorType &allocator,
				const char *name, const std::vector<T> &data) {
			rapidjson::Value arr(rapidjson::kArrayType);
			for (const auto &s: data) {
				arr.PushBack(s, allocator);
			}

			parent.AddMember(name, arr, allocator);
		}

		void send_search_result(const greylock::search_result &result) {
			greylock::JsonValue ret;
			auto &allocator = ret.GetAllocator();

			rapidjson::Value ids(rapidjson::kArrayType);
			for (auto it = result.docs.begin(), end = result.docs.end(); it != end; ++it) {
				rapidjson::Value key(rapidjson::kObjectType);

				const greylock::document &doc = it->doc;

				rapidjson::Value idv(doc.id.c_str(), doc.id.size(), allocator);
				key.AddMember("id", idv, allocator);

				std::string id_str = doc.indexed_id.to_string();
				rapidjson::Value indv(id_str.c_str(), id_str.size(), allocator);
				key.AddMember("indexed_id", indv, allocator);

				rapidjson::Value av(doc.author.c_str(), doc.author.size(), allocator);
				key.AddMember("author", av, allocator);

				rapidjson::Value cv(rapidjson::kObjectType);

				rapidjson::Value csv(doc.ctx.content.c_str(), doc.ctx.content.size(), allocator);
				cv.AddMember("content", csv, allocator);

				rapidjson::Value tsv(doc.ctx.title.c_str(), doc.ctx.title.size(), allocator);
				cv.AddMember("title", tsv, allocator);

				pack_string_array(cv, allocator, "links", doc.ctx.links);
				pack_string_array(cv, allocator, "images", doc.ctx.images);
				key.AddMember("content", cv, allocator);

				key.AddMember("relevance", it->relevance, allocator);

				long tsec, tnsec;
				doc.indexed_id.get_timestamp(&tsec, &tnsec);
				rapidjson::Value ts(rapidjson::kObjectType);
				ts.AddMember("tsec", tsec, allocator);
				ts.AddMember("tnsec", tnsec, allocator);
				key.AddMember("timestamp", ts, allocator);

				ids.PushBack(key, allocator);
			}

			ret.AddMember("ids", ids, allocator);
			ret.AddMember("completed", result.completed, allocator);

			std::string next_id_str = result.next_document_id.to_string();
			rapidjson::Value nidv(next_id_str.c_str(), next_id_str.size(), allocator);
			ret.AddMember("next_document_id", nidv, allocator);

			std::string data = ret.ToString();

			thevoid::http_response reply;
			reply.set_code(swarm::http_response::ok);
			reply.headers().set_content_type("text/json; charset=utf-8");
			reply.headers().set_content_length(data.size());

			this->send_reply(std::move(reply), std::move(data));
		}
	};

	struct on_index : public simple_request_stream_error<http_server> {
		greylock::error_info process_one_document(greylock::document &doc) {
			doc.generate_token_keys(server()->db_indexes().options());

			rocksdb::WriteBatch docs_batch, indexes_batch;

			std::string doc_serialized = serialize(doc);
			rocksdb::Slice doc_value(doc_serialized);

			greylock::document_for_index did;
			did.indexed_id = doc.indexed_id;
			std::string sdid = serialize(did);

			size_t indexes = 0;
			for (const auto &attr: doc.idx.attributes) {
				for (const auto &t: attr.tokens) {
					indexes_batch.Merge(rocksdb::Slice(t.key), rocksdb::Slice(sdid));

					greylock::disk_token dt(t.shards);
					std::string dts = serialize(dt);

					indexes_batch.Merge(rocksdb::Slice(t.shard_key), rocksdb::Slice(dts));

					indexes++;
				}
			}

			// we must have a copy, since otherwise batch will cache stall pointer to rvalue
			std::string dkey = doc.indexed_id.to_string();
			docs_batch.Put(server()->db_docs().cfhandle(greylock::options::documents_column), rocksdb::Slice(dkey), doc_value);

			std::string doc_indexed_id_serialized = serialize(doc.indexed_id);
			docs_batch.Put(server()->db_docs().cfhandle(greylock::options::document_ids_column),
					rocksdb::Slice(doc.id), rocksdb::Slice(doc_indexed_id_serialized));


			auto err = server()->db_docs().write(&docs_batch);
			if (err) {
				return greylock::create_error(err.code(), "could not write docs batch, mbox: %s, id: %s, error: %s",
					doc.mbox.c_str(), doc.id.c_str(), err.message().c_str());
			}

			err = server()->db_indexes().write(&indexes_batch);
			if (err) {
				return greylock::create_error(err.code(), "could not write indexes batch, mbox: %s, id: %s, error: %s",
					doc.mbox.c_str(), doc.id.c_str(), err.message().c_str());
			}

			ILOG_INFO("index: successfully indexed document: mbox: %s, id: %s, "
					"indexed_id: %s, indexes: %ld, serialized_doc_size: %ld",
					doc.mbox.c_str(), doc.id.c_str(),
					doc.indexed_id.to_string().c_str(), indexes, doc_value.size());
			return greylock::error_info();
		}

		template <typename T>
		std::vector<T> get_numeric_vector(const rapidjson::Value &data, const char *name) {
			std::vector<T> ret;
			const auto &arr = greylock::get_array(data, name);
			if (!arr.IsArray())
				return ret;

			for (auto it = arr.Begin(), end = arr.End(); it != end; it++) {
				if (it->IsNumber())
					ret.push_back((T)it->GetDouble());
			}

			return ret;
		}

		std::vector<std::string> get_string_vector(const rapidjson::Value &ctx, const char *name) {
			std::vector<std::string> ret;

			const auto &a = greylock::get_array(ctx, name);
			if (!a.IsArray())
				return ret;

			for (auto it = a.Begin(), end = a.End(); it != end; ++it) {
				if (it->IsString())
					ret.push_back(std::string(it->GetString(), it->GetStringLength()));
			}

			return ret;
		}
		greylock::error_info parse_content(const rapidjson::Value &ctx, greylock::document &doc) {
			doc.ctx.content = greylock::get_string(ctx, "content", "");
			doc.ctx.title = greylock::get_string(ctx, "title", "");
			doc.ctx.links = get_string_vector(ctx, "links");
			doc.ctx.images = get_string_vector(ctx, "images");

			return greylock::error_info();
		}

		greylock::error_info parse_docs(const std::string &mbox, const rapidjson::Value &docs) {
			greylock::error_info err = greylock::create_error(-ENOENT,
					"parse_docs: mbox: %s: could not parse document, there are no valid index entries", mbox.c_str());

			for (auto it = docs.Begin(), id_end = docs.End(); it != id_end; ++it) {
				if (!it->IsObject()) {
					return greylock::create_error(-EINVAL, "docs entries must be objects");
				}

				const char *id = greylock::get_string(*it, "id");
				const char *author = greylock::get_string(*it, "author");
				if (!id) {
					return greylock::create_error(-EINVAL, "id must be string");
				}

				struct timespec ts;
				clock_gettime(CLOCK_REALTIME, &ts);

				long tsec, tnsec;
				const rapidjson::Value &timestamp = greylock::get_object(*it, "timestamp");
				if (timestamp.IsObject()) {
					tsec = greylock::get_int64(timestamp, "tsec", ts.tv_sec);
					tnsec = greylock::get_int64(timestamp, "tnsec", ts.tv_nsec);
				} else {
					tsec = ts.tv_sec;
					tnsec = ts.tv_nsec;
				}


				greylock::document doc;
				doc.mbox = mbox;
				doc.assign_id(id, std::hash<std::string>{}(id), tsec, tnsec);

				if (author) {
					doc.author.assign(author);
				}

				const rapidjson::Value &ctx = greylock::get_object(*it, "content");
				if (ctx.IsObject()) {
					err = parse_content(ctx, doc);
					if (err)
						return err;
				}

				const rapidjson::Value &idxs = greylock::get_object(*it, "index");
				if (!idxs.IsObject()) {
					return greylock::create_error(-EINVAL, "docs/index must be array");
				}

				doc.idx = greylock::indexes::get_indexes(server()->db_indexes().options(), idxs);

				err = process_one_document(doc);
				if (err)
					return err;
			}

			return err;
		}

		virtual void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
			(void) req;
			ribosome::timer index_tm;

			// this is needed to put ending zero-byte, otherwise rapidjson parser will explode
			std::string data(const_cast<char *>(boost::asio::buffer_cast<const char*>(buffer)),
					boost::asio::buffer_size(buffer));

			rapidjson::Document doc;
			doc.Parse<0>(data.c_str());

			if (doc.HasParseError()) {
				send_error(swarm::http_response::bad_request, -EINVAL,
						"index: could not parse document: %s, error offset: %d",
						doc.GetParseError(), doc.GetErrorOffset());
				return;
			}

			if (!doc.IsObject()) {
				send_error(swarm::http_response::bad_request, -EINVAL, "index: document must be object, its type: %d",
						doc.GetType());
				return;
			}

			const char *mbox = greylock::get_string(doc, "mailbox");
			if (!mbox) {
				send_error(swarm::http_response::bad_request, -ENOENT, "index: 'mailbox' must be a string");
				this->send_reply(swarm::http_response::bad_request);
				return;
			}

			const rapidjson::Value &docs = greylock::get_array(doc, "docs");
			if (!docs.IsArray()) {
				send_error(swarm::http_response::bad_request, -ENOENT, "index: mailbox: %s, 'docs' must be array", mbox);
				return;
			}

			greylock::error_info err = parse_docs(mbox, docs);
			if (err) {
				send_error(swarm::http_response::bad_request, err.code(),
						"index: mailbox: %s, keys: %d: insertion error: %s",
					mbox, docs.Size(), err.message());
				return;
			}

			ILOG_INFO("index: mailbox: %s, keys: %d: insertion completed, index duration: %d ms",
					mbox, docs.Size(), index_tm.elapsed());
			this->send_reply(thevoid::http_response::ok);
		}
	};

	greylock::database &db_docs() {
		return m_db_docs;
	}
	greylock::database &db_indexes() {
		return m_db_indexes;
	}

private:
	greylock::database m_db_docs, m_db_indexes;

	bool rocksdb_init(const rapidjson::Value &config) {
		const auto &rdbconf = greylock::get_object(config, "rocksdb.docs");
		if (!rdbconf.IsObject()) {
			ILOG_ERROR("there is no 'rocksdb.docs' object in config");
			return false;
		}

		const auto &riconf = greylock::get_object(config, "rocksdb.indexes");
		if (!riconf.IsObject()) {
			ILOG_ERROR("there is no 'rocksdb.indexes' object in config");
			return false;
		}

		if (!rocksdb_config_parse(rdbconf, &m_db_docs))
			return false;

		if (!rocksdb_config_parse(riconf, &m_db_indexes))
			return false;

		return true;
	}

	bool rocksdb_config_parse(const rapidjson::Value &config, greylock::database *db) {
		const char *path = greylock::get_string(config, "path");
		if (!path) {
			ILOG_ERROR("there is no 'path' string in rocksdb config");
			return false;
		}
		bool ro = greylock::get_bool(config, "read_only", false);
		bool bulk = greylock::get_bool(config, "bulk_upload", false);

		auto err = db->open(path, ro, bulk);
		if (err) {
			ILOG_ERROR("could not open database: %s [%d]", err.message().c_str(), err.code());
			return false;
		}

		return true;
	}
};

int main(int argc, char **argv)
{
	ioremap::ribosome::set_locale("en_US.UTF8");

	ioremap::thevoid::register_signal_handler(SIGINT, ioremap::thevoid::handle_stop_signal);
	ioremap::thevoid::register_signal_handler(SIGTERM, ioremap::thevoid::handle_stop_signal);
	ioremap::thevoid::register_signal_handler(SIGHUP, ioremap::thevoid::handle_reload_signal);
	ioremap::thevoid::register_signal_handler(SIGUSR1, ioremap::thevoid::handle_ignore_signal);
	ioremap::thevoid::register_signal_handler(SIGUSR2, ioremap::thevoid::handle_ignore_signal);

	ioremap::thevoid::run_signal_thread();

	auto server = ioremap::thevoid::create_server<http_server>();
	int err = server->run(argc, argv);

	ioremap::thevoid::stop_signal_thread();

	return err;
}

