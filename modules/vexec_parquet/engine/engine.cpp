// SPDX-License-Identifier: Apache-2.0
//
// engine.cpp
//	  vexec_parquet's engine: Arrow C++'s filesystems -- the local one and
//	  S3's -- its Parquet reader and writer, behind engine.h's C calls.
//
// Every call blocks the asynchronous signals while it runs (SignalBlock),
// catches every C++ exception at its own frame, and reports through
// vpq_error(): see engine.h.  The process's state -- the filesystems, the
// footers read, S3's client -- is used from the backend's thread alone.

#include "engine.h"

#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <arrow/extension/json.h>
#include <arrow/extension/uuid.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/filesystem/s3fs.h>
#include <arrow/io/file.h>
#include <arrow/io/interfaces.h>
#include <arrow/io/memory.h>
#include <arrow/util/bit_util.h>
#include <arrow/util/config.h>
#include <arrow/util/key_value_metadata.h>
#include <arrow/util/parallel.h>
#include <arrow/util/thread_pool.h>
#include <arrow/util/uri.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/arrow/writer.h>
#include <parquet/column_reader.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/level_conversion.h>
#include <parquet/metadata.h>
#include <parquet/page_index.h>
#include <parquet/properties.h>
#include <parquet/schema.h>
#include <parquet/statistics.h>
#include <parquet/geospatial/statistics.h>

namespace fs = arrow::fs;

namespace
{

thread_local std::string last_error;

int
fail(const std::string &message)
{
	last_error = message;
	return VPQ_ERROR;
}

int
fail(const arrow::Status &status)
{
	return fail(status.ToString());
}

// The process's asynchronous signals blocked for a call's length: a thread
// Arrow or the AWS SDK starts inside it inherits the mask (engine.h).
class SignalBlock
{
  public:
	SignalBlock()
	{
		sigset_t	all;

		sigfillset(&all);
		sigdelset(&all, SIGSEGV);
		sigdelset(&all, SIGBUS);
		sigdelset(&all, SIGFPE);
		sigdelset(&all, SIGILL);
		sigdelset(&all, SIGTRAP);
		sigdelset(&all, SIGABRT);
		pthread_sigmask(SIG_BLOCK, &all, &old_);
	}
	~SignalBlock() { pthread_sigmask(SIG_SETMASK, &old_, nullptr); }

  private:
	sigset_t	old_;
};

#define ENGINE_TRY \
	SignalBlock signal_block_; \
	try

#define ENGINE_CATCH \
	catch (const std::bad_alloc &) \
	{ \
		return fail("out of memory in Arrow"); \
	} \
	catch (const std::exception &e) \
	{ \
		return fail(e.what()); \
	} \
	catch (...) \
	{ \
		return fail("an unknown C++ exception"); \
	}

// OT4 (pg_vector_executor.md §3.41): the C code's hook, which puts the backend under a wait event while
// storage blocks it -- a file's read, a wait for the point reads' threads -- called on the process's own
// thread alone, the backend's, whose thread id is its process id: never from a thread of Arrow's or ours.
vpq_wait_hook wait_hook = nullptr;

bool
on_backend_thread()
{
	static thread_local int is = -1;

	if (is < 0)
		is = gettid() == getpid() ? 1 : 0;
	return is == 1;
}

// A wait's length on the backend's thread, its outermost scope's alone
class WaitScope
{
  public:
	explicit WaitScope(int what)
	{
		if (wait_hook == nullptr || !on_backend_thread())
			return;
		counted_ = true;
		if (depth_++ == 0)
			wait_hook(what);
	}
	~WaitScope()
	{
		if (counted_ && --depth_ == 0)
			wait_hook(VPQ_WAIT_NONE);
	}

  private:
	bool		counted_ = false;
	static thread_local int depth_;
};

thread_local int WaitScope::depth_ = 0;

// A file whose reads -- and its size, which S3 asks for -- the backend waits on under VPQ_WAIT_READ; the rest
// is the file's own, as are the asynchronous reads, which Arrow's threads make
class WaitedFile : public arrow::io::RandomAccessFile
{
  public:
	explicit WaitedFile(std::shared_ptr<arrow::io::RandomAccessFile> file) : f_(std::move(file)) {}

	using arrow::io::RandomAccessFile::ReadAsync;
	using arrow::io::RandomAccessFile::ReadManyAsync;

	arrow::Status Close() override { return f_->Close(); }
	arrow::Future<> CloseAsync() override { return f_->CloseAsync(); }
	arrow::Status Abort() override { return f_->Abort(); }
	arrow::Result<int64_t> Tell() const override { return f_->Tell(); }
	bool closed() const override { return f_->closed(); }
	arrow::Status Seek(int64_t position) override { return f_->Seek(position); }
	const arrow::io::IOContext &io_context() const override { return f_->io_context(); }
	arrow::Result<std::string_view> Peek(int64_t nbytes) override { return f_->Peek(nbytes); }
	bool supports_zero_copy() const override { return f_->supports_zero_copy(); }
	arrow::Result<std::shared_ptr<const arrow::KeyValueMetadata>> ReadMetadata() override
	{
		return f_->ReadMetadata();
	}
	arrow::Future<std::shared_ptr<const arrow::KeyValueMetadata>> ReadMetadataAsync(
		const arrow::io::IOContext &ctx) override
	{
		return f_->ReadMetadataAsync(ctx);
	}
	arrow::Result<int64_t> Read(int64_t nbytes, void *out) override
	{
		WaitScope	w(VPQ_WAIT_READ);

		return f_->Read(nbytes, out);
	}
	arrow::Result<std::shared_ptr<arrow::Buffer>> Read(int64_t nbytes) override
	{
		WaitScope	w(VPQ_WAIT_READ);

		return f_->Read(nbytes);
	}
	arrow::Result<int64_t> GetSize() override
	{
		WaitScope	w(VPQ_WAIT_READ);

		return f_->GetSize();
	}
	arrow::Result<int64_t> ReadAt(int64_t position, int64_t nbytes, bool allow_short_read, void *out) override
	{
		WaitScope	w(VPQ_WAIT_READ);

		return f_->ReadAt(position, nbytes, allow_short_read, out);
	}
	arrow::Result<int64_t> ReadAt(int64_t position, int64_t nbytes, void *out) override
	{
		WaitScope	w(VPQ_WAIT_READ);

		return f_->ReadAt(position, nbytes, out);
	}
	arrow::Result<std::shared_ptr<arrow::Buffer>> ReadAt(int64_t position, int64_t nbytes,
														 bool allow_short_read) override
	{
		WaitScope	w(VPQ_WAIT_READ);

		return f_->ReadAt(position, nbytes, allow_short_read);
	}
	arrow::Result<std::shared_ptr<arrow::Buffer>> ReadAt(int64_t position, int64_t nbytes) override
	{
		WaitScope	w(VPQ_WAIT_READ);

		return f_->ReadAt(position, nbytes);
	}
	arrow::Future<std::shared_ptr<arrow::Buffer>> ReadAsync(const arrow::io::IOContext &ctx, int64_t position,
															int64_t nbytes, bool allow_short_read) override
	{
		return f_->ReadAsync(ctx, position, nbytes, allow_short_read);
	}
	arrow::Future<std::shared_ptr<arrow::Buffer>> ReadAsync(const arrow::io::IOContext &ctx, int64_t position,
															int64_t nbytes) override
	{
		return f_->ReadAsync(ctx, position, nbytes);
	}
	std::vector<arrow::Future<std::shared_ptr<arrow::Buffer>>> ReadManyAsync(
		const arrow::io::IOContext &ctx, const std::vector<arrow::io::ReadRange> &ranges) override
	{
		return f_->ReadManyAsync(ctx, ranges);
	}
	arrow::Status WillNeed(const std::vector<arrow::io::ReadRange> &ranges) override { return f_->WillNeed(ranges); }

  private:
	std::shared_ptr<arrow::io::RandomAccessFile> f_;
};

// A file opened for reading, its reads waited on as such
arrow::Result<std::shared_ptr<arrow::io::RandomAccessFile>>
open_input(const std::shared_ptr<fs::FileSystem> &filesystem, const fs::FileInfo &info)
{
	ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::io::RandomAccessFile> file, filesystem->OpenInputFile(info));
	return std::make_shared<WaitedFile>(std::move(file));
}

// Arrow's Status, Result: the call fails with its message
#define CHECK_OK(expr) \
	do { \
		arrow::Status st_ = (expr); \
		if (!st_.ok()) \
			return fail(st_); \
	} while (0)

#define VPQ_CONCAT_(a, b) a##b
#define VPQ_CONCAT(a, b) VPQ_CONCAT_(a, b)
#define ASSIGN_OR_FAIL(lhs, rexpr) \
	auto VPQ_CONCAT(result_, __LINE__) = (rexpr); \
	if (!VPQ_CONCAT(result_, __LINE__).ok()) \
		return fail(VPQ_CONCAT(result_, __LINE__).status()); \
	lhs = std::move(VPQ_CONCAT(result_, __LINE__)).ValueUnsafe()

//
// PQ8 (pg_vector_executor.md §3.37): Arrow's buffers from a pool of the
// engine's own.  Arrow's system pool reallocates by a new block, a copy and
// a free -- never realloc(), which keeps no 64-byte alignment -- and glibc
// maps a large block afresh each time: a column chunk's strings, grown by
// doubling as Parquet's decoder appends to them and shrunk to fit as the
// builder finishes, were copied at every step into pages faulted in anew,
// 47% of ClickBench's Q20 at 100M rows on the CPU.  Here a block of
// big_bytes or more is mapped, grown and shrunk by mremap() -- its pages
// moved, not copied -- and a freed one kept for the process's next reads,
// within keep_limit bytes (vexec_parquet.read_memory_mb), all given back by
// release() as the transaction ends; a smaller one is glibc's, as before.
// A block is told by its address, not by the size a call names.
//
class ReadPool : public arrow::MemoryPool
{
public:
	static constexpr int64_t big_bytes = 256 * 1024;

	arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t **out) override
	{
		if (size < big_bytes || alignment > page)
			return system()->Allocate(size, alignment, out);

		std::lock_guard<std::mutex> g(mu);

		ARROW_RETURN_NOT_OK(take(size, out));
		used += size;
		peak = std::max(peak, used);
		total += size;
		allocations++;
		return arrow::Status::OK();
	}

	arrow::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment, uint8_t **ptr) override
	{
		std::unique_lock<std::mutex> g(mu);
		auto		it = blocks.find(*ptr);

		if (it == blocks.end())
		{
			g.unlock();
			if (new_size < big_bytes || alignment > page)
				return system()->Reallocate(old_size, new_size, alignment, ptr);

			uint8_t    *out;

			/* a small block grown past big_bytes: into a block of the pool's */
			ARROW_RETURN_NOT_OK(Allocate(new_size, alignment, &out));
			memcpy(out, *ptr, (size_t) std::min(old_size, new_size));
			system()->Free(*ptr, old_size, alignment);
			*ptr = out;
			return arrow::Status::OK();
		}
		if (new_size > it->second)
		{
			int64_t		cap = round_up(new_size);
			void	   *q = mremap(*ptr, (size_t) it->second, (size_t) cap, MREMAP_MAYMOVE);

			if (q == MAP_FAILED)
				return arrow::Status::OutOfMemory("mremap() of ", cap, " bytes failed: ", strerror(errno));
			blocks.erase(it);
			*ptr = static_cast<uint8_t *>(q);
			blocks[*ptr] = cap;
			moved++;
		}
		/* shrunk, or grown within its pages: in place, its pages kept for its next use */
		used += new_size - old_size;
		peak = std::max(peak, used);
		return arrow::Status::OK();
	}

	void Free(uint8_t *buffer, int64_t size, int64_t alignment) override
	{
		std::unique_lock<std::mutex> g(mu);
		auto		it = blocks.find(buffer);

		if (it == blocks.end())
		{
			g.unlock();
			system()->Free(buffer, size, alignment);
			return;
		}

		int64_t		cap = it->second;

		blocks.erase(it);
		used -= size;
		if (kept_bytes + cap <= keep_limit)
		{
			kept.emplace(cap, buffer);
			kept_bytes += cap;
			return;
		}
		munmap(buffer, (size_t) cap);
	}

	void ReleaseUnused() override
	{
		release();
	}

	int64_t bytes_allocated() const override
	{
		std::lock_guard<std::mutex> g(mu);

		return used;
	}

	int64_t max_memory() const override
	{
		std::lock_guard<std::mutex> g(mu);

		return peak;
	}

	int64_t total_bytes_allocated() const override
	{
		std::lock_guard<std::mutex> g(mu);

		return total;
	}

	int64_t num_allocations() const override
	{
		std::lock_guard<std::mutex> g(mu);

		return allocations;
	}

	std::string backend_name() const override
	{
		return "vexec_parquet";
	}

	void set_keep(int64_t bytes)
	{
		std::lock_guard<std::mutex> g(mu);

		keep_limit = std::max<int64_t>(bytes, 0);
		while (kept_bytes > keep_limit && !kept.empty())
			drop(std::prev(kept.end()));
	}

	// every kept block given back to the system
	void release()
	{
		std::lock_guard<std::mutex> g(mu);

		while (!kept.empty())
			drop(kept.begin());
	}

	void stats(int64_t *mapped_, int64_t *reused_, int64_t *moved_, int64_t *kept_)
	{
		std::lock_guard<std::mutex> g(mu);

		*mapped_ = mapped;
		*reused_ = reused;
		*moved_ = moved;
		*kept_ = kept_bytes;
	}

	void reset_stats()
	{
		std::lock_guard<std::mutex> g(mu);

		mapped = reused = moved = 0;
	}

private:
	static constexpr int64_t page = 4096;

	static int64_t round_up(int64_t n)
	{
		return (n + page - 1) & ~(page - 1);
	}

	static arrow::MemoryPool *system()
	{
		return arrow::system_memory_pool();
	}

	// a block of size bytes at least: the least kept one that holds them, else the largest kept one grown, else
	// one mapped afresh
	arrow::Status take(int64_t size, uint8_t **out)
	{
		auto		it = kept.lower_bound(size);

		if (it == kept.end() && !kept.empty())
			it = std::prev(kept.end());
		if (it != kept.end())
		{
			int64_t		cap = it->first;
			uint8_t    *p = it->second;

			kept.erase(it);
			kept_bytes -= cap;
			if (cap < size)
			{
				void	   *q = mremap(p, (size_t) cap, (size_t) round_up(size), MREMAP_MAYMOVE);

				if (q == MAP_FAILED)
				{
					munmap(p, (size_t) cap);
					return arrow::Status::OutOfMemory("mremap() of ", round_up(size), " bytes failed: ",
													  strerror(errno));
				}
				p = static_cast<uint8_t *>(q);
				cap = round_up(size);
				moved++;
			}
			blocks[p] = cap;
			reused++;
			*out = p;
			return arrow::Status::OK();
		}

		int64_t		cap = round_up(size);
		void	   *p = mmap(nullptr, (size_t) cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p == MAP_FAILED)
			return arrow::Status::OutOfMemory("mmap() of ", cap, " bytes failed: ", strerror(errno));
		blocks[static_cast<uint8_t *>(p)] = cap;
		mapped++;
		*out = static_cast<uint8_t *>(p);
		return arrow::Status::OK();
	}

	void drop(std::multimap<int64_t, uint8_t *>::iterator it)
	{
		munmap(it->second, (size_t) it->first);
		kept_bytes -= it->first;
		kept.erase(it);
	}

	mutable std::mutex mu;
	std::unordered_map<uint8_t *, int64_t> blocks;	// the pool's blocks in use, by address: their mapped bytes
	std::multimap<int64_t, uint8_t *> kept;	// freed ones kept, by their mapped bytes
	int64_t		kept_bytes = 0;
	int64_t		keep_limit = (int64_t) 256 << 20;
	int64_t		used = 0;
	int64_t		peak = 0;
	int64_t		total = 0;
	int64_t		allocations = 0;
	int64_t		mapped = 0;
	int64_t		reused = 0;
	int64_t		moved = 0;
};

// The process's pool, never destroyed: a process exits without giving its memory back block by block (PQ10)
ReadPool *
read_pool()
{
	static ReadPool *p = new ReadPool();

	return p;
}

arrow::MemoryPool *
pool()
{
	return read_pool();
}

struct Config
{
	std::string endpoint;
	std::string region;
	std::string access_key;
	std::string secret_key;
	std::string session_token;
	std::string scheme;
	bool		virtual_addressing = false;
	bool		anonymous = false;
	double		connect_timeout = -1;
	double		request_timeout = -1;
	bool		prefetch = true;
	int			io_threads = 8;

	bool operator==(const Config &o) const
	{
		return endpoint == o.endpoint && region == o.region && access_key == o.access_key &&
			secret_key == o.secret_key && session_token == o.session_token && scheme == o.scheme &&
			virtual_addressing == o.virtual_addressing && anonymous == o.anonymous &&
			connect_timeout == o.connect_timeout && request_timeout == o.request_timeout;
	}
};

Config		config;
bool		configured = false;
int			io_threads_set = 0;
bool		s3_initialized = false;
std::map<std::string, std::shared_ptr<fs::FileSystem>> s3_filesystems;	// by settings
std::shared_ptr<fs::FileSystem> local_fs;

// A location resolved: its filesystem and the filesystem's path of it
struct Located
{
	std::shared_ptr<fs::FileSystem> fs;
	std::string path;			// absolute for the local one, bucket/key for S3
	bool		s3 = false;
	std::string query;			// an s3:// URI's own settings, kept on the paths listed
};

std::string
s3_location(const std::string &path, const std::string &query)
{
	return "s3://" + path + (query.empty() ? "" : "?" + query);
}

bool
starts_with(const std::string &s, const char *prefix)
{
	return s.compare(0, strlen(prefix), prefix) == 0;
}

std::string
env_or_empty(const char *name)
{
	const char *v = getenv(name);

	return v ? std::string(v) : std::string();
}

arrow::Status
ensure_s3()
{
	if (!s3_initialized)
	{
		fs::S3GlobalOptions g = fs::S3GlobalOptions::Defaults();

		g.install_sigpipe_handler = false;
		ARROW_RETURN_NOT_OK(fs::InitializeS3(g));
		s3_initialized = true;
	}
	return arrow::Status::OK();
}

// An s3:// URI's filesystem: the GUCs' settings, under the URI's query's own
arrow::Result<Located>
locate_s3(const std::string &location)
{
	arrow::util::Uri uri;
	Located		out;
	std::string endpoint = config.endpoint;
	std::string scheme = config.scheme;
	std::string region = config.region;
	bool		region_given = !region.empty();

	ARROW_RETURN_NOT_OK(uri.Parse(location));
	if (!uri.username().empty() || !uri.password().empty())
		return arrow::Status::Invalid("an s3:// location takes no credentials: set vexec_parquet.s3_access_key_id and vexec_parquet.s3_secret_access_key");
	std::string bucket = uri.host();
	std::string path = uri.path();

	if (bucket.empty())
		return arrow::Status::Invalid("an s3:// location names no bucket: ", location);
	path = path.empty() ? bucket : bucket + path;
	while (path.size() > 1 && path.back() == '/')
		path.pop_back();
	out.path = path;
	out.s3 = true;
	out.query = uri.query_string();

	ARROW_ASSIGN_OR_RAISE(auto items, uri.query_items());
	for (const auto &kv : items)
	{
		if (kv.first == "region")
		{
			region = kv.second;
			region_given = true;
		}
		else if (kv.first == "endpoint_override")
			endpoint = kv.second;
		else if (kv.first == "scheme")
			scheme = kv.second;
		else
			return arrow::Status::Invalid("an s3:// location's query takes region, endpoint_override and scheme, not \"", kv.first, "\"");
	}
	if (endpoint.empty())
		endpoint = env_or_empty("AWS_ENDPOINT_URL_S3");
	if (endpoint.empty())
		endpoint = env_or_empty("AWS_ENDPOINT_URL");
	if (starts_with(endpoint, "http://"))
	{
		scheme = "http";
		endpoint = endpoint.substr(7);
	}
	else if (starts_with(endpoint, "https://"))
	{
		scheme = "https";
		endpoint = endpoint.substr(8);
	}
	if (!region_given)
	{
		region = env_or_empty("AWS_REGION");
		if (region.empty())
			region = env_or_empty("AWS_DEFAULT_REGION");
		region_given = !region.empty();
	}

	ARROW_RETURN_NOT_OK(ensure_s3());

	std::string key = endpoint + "|" + scheme + "|" + (region_given ? region : "@" + bucket) + "|" +
		config.access_key + "|" + std::to_string(std::hash<std::string>{}(config.secret_key)) + "|" +
		config.session_token + "|" + (config.anonymous ? "a" : "") + (config.virtual_addressing ? "v" : "") +
		"|" + std::to_string(config.connect_timeout) + "|" + std::to_string(config.request_timeout);
	auto it = s3_filesystems.find(key);

	if (it != s3_filesystems.end())
	{
		out.fs = it->second;
		return out;
	}

	fs::S3Options o;

	if (config.anonymous)
		o.ConfigureAnonymousCredentials();
	else if (!config.access_key.empty())
		o.ConfigureAccessKey(config.access_key, config.secret_key, config.session_token);
	else
		o.ConfigureDefaultCredentials();
	if (!endpoint.empty())
		o.endpoint_override = endpoint;
	if (!scheme.empty())
		o.scheme = scheme;
	if (region_given)
		o.region = region;
	else if (!endpoint.empty())
		o.region = "us-east-1";
	else
	{
		ARROW_ASSIGN_OR_RAISE(o.region, fs::ResolveS3BucketRegion(bucket));
	}
	o.force_virtual_addressing = config.virtual_addressing;
	if (config.connect_timeout > 0)
		o.connect_timeout = config.connect_timeout;
	if (config.request_timeout > 0)
		o.request_timeout = config.request_timeout;
	ARROW_ASSIGN_OR_RAISE(auto s3fs, fs::S3FileSystem::Make(o));
	out.fs = s3fs;
	s3_filesystems[key] = s3fs;
	return out;
}

arrow::Result<Located>
locate(const std::string &location)
{
	if (starts_with(location, "s3://"))
		return locate_s3(location);

	Located		out;
	std::string path = location;

	if (starts_with(path, "file://"))
		path = path.substr(7);
	if (path.empty() || path[0] != '/')
		return arrow::Status::Invalid("a location is an absolute path, a file:// URI or an s3:// URI: \"", location, "\"");
	if (!local_fs)
		local_fs = std::make_shared<fs::LocalFileSystem>();
	out.fs = local_fs;
	out.path = path;
	return out;
}

// A path of a filesystem in its location's form
std::string
location_of(const Located &base, const std::string &path)
{
	return base.s3 ? s3_location(path, base.query) : path;
}

int64_t
mtime_ns(const fs::FileInfo &info)
{
	if (info.mtime() == fs::kNoTime)
		return -1;
	return std::chrono::duration_cast<std::chrono::nanoseconds>(info.mtime().time_since_epoch()).count();
}

// '*' and '?' within a name, [...] a class, "**" across directories
bool
glob_match(const char *p, const char *s)
{
	while (*p)
	{
		if (p[0] == '*' && p[1] == '*')
		{
			p += 2;
			if (*p == '/')
				p++;
			for (const char *t = s;; t++)
			{
				if (glob_match(p, t))
					return true;
				if (*t == '\0')
					return false;
			}
		}
		if (*p == '*')
		{
			p++;
			for (const char *t = s;; t++)
			{
				if (glob_match(p, t))
					return true;
				if (*t == '\0' || *t == '/')
					return false;
			}
		}
		if (*s == '\0')
			return false;
		if (*p == '?')
		{
			if (*s == '/')
				return false;
		}
		else if (*p == '[')
		{
			const char *q = p + 1;
			bool		negate = false;
			bool		matched = false;

			if (*q == '!' || *q == '^')
			{
				negate = true;
				q++;
			}
			while (*q && *q != ']')
			{
				if (q[1] == '-' && q[2] && q[2] != ']')
				{
					if ((unsigned char) *s >= (unsigned char) q[0] && (unsigned char) *s <= (unsigned char) q[2])
						matched = true;
					q += 3;
				}
				else
				{
					if (*q == *s)
						matched = true;
					q++;
				}
			}
			if (*q != ']')
				return false;	/* an unclosed class names nothing */
			if (matched == negate || *s == '/')
				return false;
			p = q;
		}
		else if (*p != *s)
			return false;
		p++;
		s++;
	}
	return *s == '\0';
}

bool
has_glob(const std::string &s)
{
	return s.find_first_of("*?[") != std::string::npos;
}

// A location less an s3:// URI's query -- whose '?' starts it, never a
// wildcard -- and the query with its '?', or nothing
std::pair<std::string, std::string>
split_query(const std::string &location)
{
	size_t		q;

	if (!starts_with(location, "s3://") || (q = location.find('?')) == std::string::npos)
		return {location, ""};
	return {location.substr(0, q), location.substr(q)};
}

std::string
base_name(const std::string &path)
{
	size_t		slash = path.rfind('/');

	return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool
is_parquet_name(const std::string &path)
{
	std::string name = base_name(path);
	std::string lower = name;

	if (name.empty() || name[0] == '.' || name[0] == '_')
		return false;
	std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
	return lower.size() > 8 && lower.compare(lower.size() - 8, 8, ".parquet") == 0;
}

vpq_entry
make_entry(const std::string &location, const fs::FileInfo &info)
{
	vpq_entry	e;

	e.path = strdup(location.c_str());
	e.size = info.size();
	e.mtime_ns = mtime_ns(info);
	return e;
}

int
fill_entries(const std::vector<std::pair<std::string, fs::FileInfo>> &found, vpq_entry **entries,
			 int64_t *n)
{
	vpq_entry  *out = (vpq_entry *) calloc(std::max<size_t>(found.size(), 1), sizeof(vpq_entry));

	if (out == nullptr)
		return fail("out of memory listing files");
	for (size_t i = 0; i < found.size(); i++)
	{
		out[i] = make_entry(found[i].first, found[i].second);
		if (out[i].path == nullptr)
		{
			vpq_list_free(out, (int64_t) i);
			return fail("out of memory listing files");
		}
	}
	*entries = out;
	*n = (int64_t) found.size();
	return VPQ_OK;
}

//
// Footers, read once in the process by a file's path, size and modification
// time, kept while they fit in footer_cache_bytes
//
struct Footer
{
	std::shared_ptr<parquet::FileMetaData> md;
	std::shared_ptr<arrow::Schema> schema;
	std::vector<int> leaf_of;	// per top-level field: its leaf column, or -1 when nested
	std::vector<int> field_ids;
	std::vector<vpq_type> types;
	size_t		bytes = 0;
	std::vector<int8_t> page_versions;	// [rg * columns + leaf]: a chunk's first data page's version, 0 unread
	int64_t		facts_chunks_at = -1;	// PQ7: where the chunks' records begin in the facts it makes
	// PQ16 (pg_vector_executor.md §3.39): GeoParquet's key "geo" ("" none), and per top-level field its
	// geospatial logical type (engine.h's VPQ_GEO_*) and the CRS the type writes, a "projjson:<key>" the key's value
	std::string geo;
	std::vector<uint8_t> geo_kind;
	std::vector<std::string> geo_crs;
	bool		has_geo = false;	// either of them
	// PQ19 (§3.40): per top-level field its first leaf and leaves, and a nested one's flat children
	std::vector<int> leaf_first;
	std::vector<int> leaf_count;
	std::vector<std::vector<vpq_type>> children;
	std::vector<std::vector<uint8_t>> child_nullable;
	bool		has_nested = false;
};

const size_t footer_cache_bytes = 256u << 20;
std::list<std::pair<std::string, std::shared_ptr<Footer>>> footer_lru;
std::unordered_map<std::string, std::list<std::pair<std::string, std::shared_ptr<Footer>>>::iterator> footer_index;
size_t		footer_cached = 0;

std::string
footer_key(const std::string &path, int64_t size, int64_t mtime)
{
	return path + "\x1f" + std::to_string(size) + "\x1f" + std::to_string(mtime);
}

std::shared_ptr<Footer>
footer_find(const std::string &key)
{
	auto		it = footer_index.find(key);

	if (it == footer_index.end())
		return nullptr;
	footer_lru.splice(footer_lru.begin(), footer_lru, it->second);
	return it->second->second;
}

void
footer_keep(const std::string &key, const std::shared_ptr<Footer> &f)
{
	auto		old = footer_index.find(key);

	if (old != footer_index.end())
	{
		footer_cached -= old->second->second->bytes;
		footer_lru.erase(old->second);
		footer_index.erase(old);
	}
	if (f->bytes > footer_cache_bytes / 4)
		return;
	footer_lru.emplace_front(key, f);
	footer_index[key] = footer_lru.begin();
	footer_cached += f->bytes;
	while (footer_cached > footer_cache_bytes && !footer_lru.empty())
	{
		auto	   &last = footer_lru.back();

		footer_cached -= last.second->bytes;
		footer_index.erase(last.first);
		footer_lru.pop_back();
	}
}

//
// Facts (engine.h, pg_vector_executor.md §3.34's PQ1 and PQ4): what a scan
// asks of a footer, as bytes of the engine's own form, so that a handle made
// of them reads no footer until Arrow's reader is opened over it --
//
//	uint32 magic "VPQF", uint16 form (1), uint16 flags (engine.h's
//		VPQ_FACTS_*), int64 rows, int32 row groups, int32 columns, int32
//		leaves, uint8 every chunk's codec where no chunk is kept, 3 pad,
//		uint32 created_by's bytes and them
//	int64 rows of each row group
//	per top-level column: uint16 its name's bytes and them; int32 field id;
//		uint8 nullable; int32 leaf, -1 nested; its vpq_type: int32 id, unit,
//		precision, scale, byte_width, value_id and index_width, uint8 has_tz
//		and uuid, uint8 its name's bytes and them
//	VPQ_FACTS_GEO, set where the footer has GeoParquet's key "geo" or a
//		column of a geospatial logical type (PQ16): uint32 the key's bytes and
//		them; per top-level column uint8 its type (VPQ_GEO_*), uint32 its
//		CRS's bytes and them; per row group and column of a geospatial type,
//		GEO_BOUNDS_BYTES: uint8 its GeospatialStatistics' X and Y bounds
//		valid, 7 pad, double xmin, xmax, ymin, ymax
//	VPQ_FACTS_NESTED, set where the footer has a nested column of flat
//		children (PQ19): per top-level column uint8 its children, and each
//		child's vpq_type as a column's is kept, then uint8 nullable
//	VPQ_FACTS_CHUNKS: per row group and leaf, CHUNK_BYTES: uint8 codec -- its
//		bit CHUNK_TEXT_CHECKED set where a reader found the chunk's text
//		UTF-8 whole since (PQ14) -- int8 first data page's version (0
//		unread), uint16 its bytes before compression per 256 compressed, at
//		most 65,535 (W38; 0 unsaid), uint32 its statistics' place past the
//		chunks plus one (0 none), int64 the first data page's offset, int64
//		compressed bytes
//	VPQ_FACTS_STATS: those statistics -- uint8 1 (a NULL count alone) or 2
//		(and the least and greatest), uint8 least exact, uint8 greatest exact,
//		uint8 pad, int64 NULLs (-1 unknown), and for 2 the two values, each a
//		uint8 tag: 0 and int64 i; 3 and double d; 1 and int64 i, uint64
//		i128_lo, int64 i128_hi, double d; 2 and int32 length and the bytes
//
// -- every integer native.  Kept in the process by the footers' key, while
// they fit in facts_cache_bytes, with the versions of first data pages read
// since.
//
const uint32_t FACTS_MAGIC = 0x46515056;	// "VPQF"
const uint16_t FACTS_FORM = 1;
const size_t CHUNK_BYTES = 24;
const uint8_t CHUNK_TEXT_CHECKED = 0x80;	// PQ14: a chunk's codec's byte, its text UTF-8 whole
const size_t GEO_BOUNDS_BYTES = 40;

struct FactsColumn
{
	std::string name;
	int			field_id = -1;
	bool		nullable = true;
	int			leaf = -1;
	vpq_type	type;
};

struct Facts
{
	std::string blob;			// its bytes: chunks and statistics are read from them
	int			flags = 0;
	int64_t		num_rows = 0;
	int			nrg = 0;
	int			nleaves = 0;
	uint8_t		codec = VPQ_CODEC_OTHER;
	std::string created_by;
	std::vector<int64_t> rg_rows;
	std::vector<FactsColumn> cols;
	size_t		chunks_at = 0;
	size_t		stats_at = 0;
	std::vector<int8_t> versions;	// [rg * leaves + leaf]: read since, 0 unread
	size_t		bytes = 0;
	// VPQ_FACTS_GEO: as the Footer's, and per column its place among those of a geospatial type, -1 none
	std::string geo;
	std::vector<uint8_t> geo_kind;
	std::vector<std::string> geo_crs;
	std::vector<int> geo_slot;
	int			ngeo = 0;
	size_t		geo_bounds_at = 0;
	// VPQ_FACTS_NESTED: as the Footer's
	std::vector<std::vector<vpq_type>> children;
	std::vector<std::vector<uint8_t>> child_nullable;
};

const size_t facts_cache_bytes = 64u << 20;
std::list<std::pair<std::string, std::shared_ptr<Facts>>> facts_lru;
std::unordered_map<std::string, std::list<std::pair<std::string, std::shared_ptr<Facts>>>::iterator> facts_index;
size_t		facts_cached = 0;

std::shared_ptr<Facts>
facts_find(const std::string &key)
{
	auto		it = facts_index.find(key);

	if (it == facts_index.end())
		return nullptr;
	facts_lru.splice(facts_lru.begin(), facts_lru, it->second);
	return it->second->second;
}

void
facts_keep(const std::string &key, const std::shared_ptr<Facts> &f)
{
	auto		old = facts_index.find(key);

	if (old != facts_index.end())
	{
		facts_cached -= old->second->second->bytes;
		facts_lru.erase(old->second);
		facts_index.erase(old);
	}
	if (f->bytes > facts_cache_bytes / 4)
		return;
	facts_lru.emplace_front(key, f);
	facts_index[key] = facts_lru.begin();
	facts_cached += f->bytes;
	while (facts_cached > facts_cache_bytes && !facts_lru.empty())
	{
		auto	   &last = facts_lru.back();

		facts_cached -= last.second->bytes;
		facts_index.erase(last.first);
		facts_lru.pop_back();
	}
}

// The process's footers read and handles made of facts, for the tests (engine.h)
vpq_engine_stats counters;

// A reader of facts' bytes: false once it would pass their end
struct FactsReader
{
	const char *p;
	const char *end;

	template <typename T> bool take(T *out)
	{
		if ((size_t) (end - p) < sizeof(T))
			return false;
		memcpy(out, p, sizeof(T));
		p += sizeof(T);
		return true;
	}
	bool take_string(size_t n, std::string *out)
	{
		if ((size_t) (end - p) < n)
			return false;
		out->assign(p, n);
		p += n;
		return true;
	}
	bool skip(size_t n)
	{
		if ((size_t) (end - p) < n)
			return false;
		p += n;
		return true;
	}
};

template <typename T> void
put(std::string *out, T v)
{
	out->append(reinterpret_cast<const char *>(&v), sizeof(T));
}

// PQ19: a vpq_type as the facts keep a column's -- int32 id, unit, precision, scale, byte_width, value_id and
// index_width, uint8 has_tz and uuid, uint8 its name's bytes and them -- for a nested column's children
void
put_type(std::string *out, const vpq_type &t)
{
	std::string tname(t.name);

	put<int32_t>(out, t.id);
	put<int32_t>(out, t.unit);
	put<int32_t>(out, t.precision);
	put<int32_t>(out, t.scale);
	put<int32_t>(out, t.byte_width);
	put<int32_t>(out, t.value_id);
	put<int32_t>(out, t.index_width);
	put<uint8_t>(out, t.has_tz ? 1 : 0);
	put<uint8_t>(out, t.uuid ? 1 : 0);
	put<uint8_t>(out, (uint8_t) std::min<size_t>(tname.size(), 63));
	out->append(tname, 0, std::min<size_t>(tname.size(), 63));
}

bool
take_type(FactsReader &r, vpq_type *t)
{
	int32_t		v[7];
	uint8_t		has_tz;
	uint8_t		uuid;
	uint8_t		tlen;
	std::string tname;

	memset(t, 0, sizeof(*t));
	for (int i = 0; i < 7; i++)
		if (!r.take(&v[i]))
			return false;
	if (!r.take(&has_tz) || !r.take(&uuid) || !r.take(&tlen) || !r.take_string(tlen, &tname))
		return false;
	t->id = v[0];
	t->unit = v[1];
	t->precision = v[2];
	t->scale = v[3];
	t->byte_width = v[4];
	t->value_id = v[5];
	t->index_width = v[6];
	t->has_tz = has_tz != 0;
	t->uuid = uuid != 0;
	snprintf(t->name, sizeof(t->name), "%s", tname.c_str());
	return true;
}

// Facts' bytes parsed -- their header, row groups and columns; the chunks and
// statistics read where asked -- or null where they are not of this form
std::shared_ptr<Facts>
parse_facts(const char *data, size_t len)
{
	auto		f = std::make_shared<Facts>();
	uint32_t	magic;
	uint16_t	form;
	uint16_t	flags;
	int32_t		nrg;
	int32_t		ncols;
	int32_t		nleaves;
	uint8_t		codec;
	uint32_t	cblen;

	f->blob.assign(data, len);

	FactsReader r{f->blob.data(), f->blob.data() + f->blob.size()};

	if (!r.take(&magic) || magic != FACTS_MAGIC || !r.take(&form) || form != FACTS_FORM ||
		!r.take(&flags) || !r.take(&f->num_rows) || !r.take(&nrg) || !r.take(&ncols) ||
		!r.take(&nleaves) || !r.take(&codec) || !r.skip(3) || !r.take(&cblen) ||
		nrg < 0 || ncols < 0 || nleaves < 0 || !r.take_string(cblen, &f->created_by))
		return nullptr;
	f->flags = flags;
	f->nrg = nrg;
	f->nleaves = nleaves;
	f->codec = codec;
	f->rg_rows.resize(nrg);
	for (int g = 0; g < nrg; g++)
		if (!r.take(&f->rg_rows[g]))
			return nullptr;
	f->cols.resize(ncols);
	for (int c = 0; c < ncols; c++)
	{
		FactsColumn &col = f->cols[c];
		uint16_t	nlen;
		uint8_t		nullable;
		int32_t		v[7];
		uint8_t		has_tz;
		uint8_t		uuid;
		uint8_t		tlen;
		std::string tname;

		memset(&col.type, 0, sizeof(col.type));
		if (!r.take(&nlen) || !r.take_string(nlen, &col.name) || !r.take(&col.field_id) ||
			!r.take(&nullable) || !r.take(&col.leaf))
			return nullptr;
		for (int i = 0; i < 7; i++)
			if (!r.take(&v[i]))
				return nullptr;
		if (!r.take(&has_tz) || !r.take(&uuid) || !r.take(&tlen) || !r.take_string(tlen, &tname))
			return nullptr;
		if (col.leaf >= nleaves)
			return nullptr;
		col.nullable = nullable != 0;
		col.type.id = v[0];
		col.type.unit = v[1];
		col.type.precision = v[2];
		col.type.scale = v[3];
		col.type.byte_width = v[4];
		col.type.value_id = v[5];
		col.type.index_width = v[6];
		col.type.has_tz = has_tz != 0;
		col.type.uuid = uuid != 0;
		snprintf(col.type.name, sizeof(col.type.name), "%s", tname.c_str());
	}
	if (flags & VPQ_FACTS_GEO)
	{
		uint32_t	glen;

		if (!r.take(&glen) || !r.take_string(glen, &f->geo))
			return nullptr;
		f->geo_kind.resize(ncols);
		f->geo_crs.resize(ncols);
		f->geo_slot.assign(ncols, -1);
		for (int c = 0; c < ncols; c++)
		{
			uint32_t	clen;

			if (!r.take(&f->geo_kind[c]) || !r.take(&clen) || !r.take_string(clen, &f->geo_crs[c]))
				return nullptr;
			if (f->geo_kind[c] != VPQ_GEO_NONE)
				f->geo_slot[c] = f->ngeo++;
		}
		f->geo_bounds_at = (size_t) (r.p - f->blob.data());
		if (!r.skip(GEO_BOUNDS_BYTES * (size_t) nrg * (size_t) f->ngeo))
			return nullptr;
	}
	if (flags & VPQ_FACTS_NESTED)
	{
		f->children.resize(ncols);
		f->child_nullable.resize(ncols);
		for (int c = 0; c < ncols; c++)
		{
			uint8_t		n;

			if (!r.take(&n) || n > 2)
				return nullptr;
			for (int k = 0; k < n; k++)
			{
				vpq_type	t;
				uint8_t		nullable;

				if (!take_type(r, &t) || !r.take(&nullable))
					return nullptr;
				f->children[c].push_back(t);
				f->child_nullable[c].push_back(nullable);
			}
		}
	}
	f->chunks_at = (size_t) (r.p - f->blob.data());
	if (flags & VPQ_FACTS_CHUNKS)
	{
		if (!r.skip(CHUNK_BYTES * (size_t) nrg * (size_t) nleaves))
			return nullptr;
		f->stats_at = (size_t) (r.p - f->blob.data());
	}
	f->bytes = f->blob.size() + f->cols.size() * sizeof(FactsColumn) + sizeof(Facts);
	return f;
}

// A chunk of facts: its codec, its first data page's version, its statistics' place, its first page and bytes
struct FactsChunk
{
	uint8_t		codec;
	bool		text_checked;	// PQ14
	int8_t		version;
	uint16_t	expand;			// W38: bytes before compression per 256 compressed
	uint32_t	stats;
	int64_t		data_page_offset;
	int64_t		bytes;
};

bool
facts_chunk(const Facts &f, int rg, int leaf, FactsChunk *out)
{
	if (!(f.flags & VPQ_FACTS_CHUNKS) || rg < 0 || rg >= f.nrg || leaf < 0 || leaf >= f.nleaves)
		return false;

	const char *p = f.blob.data() + f.chunks_at + CHUNK_BYTES * ((size_t) rg * f.nleaves + leaf);

	memcpy(&out->codec, p, 1);
	out->text_checked = (out->codec & CHUNK_TEXT_CHECKED) != 0;
	out->codec &= (uint8_t) ~CHUNK_TEXT_CHECKED;
	memcpy(&out->version, p + 1, 1);
	memcpy(&out->expand, p + 2, 2);
	memcpy(&out->stats, p + 4, 4);
	memcpy(&out->data_page_offset, p + 8, 8);
	memcpy(&out->bytes, p + 16, 8);
	return true;
}

int
codec_id(arrow::Compression::type c)
{
	switch (c)
	{
		case arrow::Compression::SNAPPY:
			return VPQ_CODEC_SNAPPY;
		case arrow::Compression::LZ4:
		case arrow::Compression::LZ4_FRAME:
		case arrow::Compression::LZ4_HADOOP:
			return VPQ_CODEC_LZ4;
		default:
			return VPQ_CODEC_OTHER;
	}
}

void
describe(const std::shared_ptr<arrow::DataType> &t, vpq_type *out)
{
	memset(out, 0, sizeof(*out));
	snprintf(out->name, sizeof(out->name), "%s", t->ToString().c_str());
	switch (t->id())
	{
		case arrow::Type::NA:
			out->id = VPQ_T_NULL;
			break;
		case arrow::Type::BOOL:
			out->id = VPQ_T_BOOL;
			break;
		case arrow::Type::INT8:
			out->id = VPQ_T_INT8;
			break;
		case arrow::Type::INT16:
			out->id = VPQ_T_INT16;
			break;
		case arrow::Type::INT32:
			out->id = VPQ_T_INT32;
			break;
		case arrow::Type::INT64:
			out->id = VPQ_T_INT64;
			break;
		case arrow::Type::UINT8:
			out->id = VPQ_T_UINT8;
			break;
		case arrow::Type::UINT16:
			out->id = VPQ_T_UINT16;
			break;
		case arrow::Type::UINT32:
			out->id = VPQ_T_UINT32;
			break;
		case arrow::Type::UINT64:
			out->id = VPQ_T_UINT64;
			break;
		case arrow::Type::HALF_FLOAT:
			out->id = VPQ_T_FLOAT16;
			break;
		case arrow::Type::FLOAT:
			out->id = VPQ_T_FLOAT32;
			break;
		case arrow::Type::DOUBLE:
			out->id = VPQ_T_FLOAT64;
			break;
		case arrow::Type::DATE32:
			out->id = VPQ_T_DATE32;
			break;
		case arrow::Type::DATE64:
			out->id = VPQ_T_DATE64;
			break;
		case arrow::Type::TIME32:
			out->id = VPQ_T_TIME32;
			out->unit = static_cast<const arrow::Time32Type &>(*t).unit() == arrow::TimeUnit::SECOND ?
				VPQ_UNIT_S : VPQ_UNIT_MS;
			break;
		case arrow::Type::TIME64:
			out->id = VPQ_T_TIME64;
			out->unit = static_cast<const arrow::Time64Type &>(*t).unit() == arrow::TimeUnit::MICRO ?
				VPQ_UNIT_US : VPQ_UNIT_NS;
			break;
		case arrow::Type::TIMESTAMP:
			{
				const auto &ts = static_cast<const arrow::TimestampType &>(*t);

				out->id = VPQ_T_TIMESTAMP;
				switch (ts.unit())
				{
					case arrow::TimeUnit::SECOND:
						out->unit = VPQ_UNIT_S;
						break;
					case arrow::TimeUnit::MILLI:
						out->unit = VPQ_UNIT_MS;
						break;
					case arrow::TimeUnit::MICRO:
						out->unit = VPQ_UNIT_US;
						break;
					case arrow::TimeUnit::NANO:
						out->unit = VPQ_UNIT_NS;
						break;
				}
				out->has_tz = !ts.timezone().empty();
			}
			break;
		case arrow::Type::DECIMAL32:
		case arrow::Type::DECIMAL64:
		case arrow::Type::DECIMAL128:
		case arrow::Type::DECIMAL256:
			{
				const auto &d = static_cast<const arrow::DecimalType &>(*t);

				out->id = t->id() == arrow::Type::DECIMAL32 ? VPQ_T_DECIMAL32 :
					t->id() == arrow::Type::DECIMAL64 ? VPQ_T_DECIMAL64 :
					t->id() == arrow::Type::DECIMAL128 ? VPQ_T_DECIMAL128 : VPQ_T_DECIMAL256;
				out->precision = d.precision();
				out->scale = d.scale();
				out->byte_width = d.byte_width();
			}
			break;
		case arrow::Type::STRING:
			out->id = VPQ_T_STRING;
			break;
		case arrow::Type::LARGE_STRING:
			out->id = VPQ_T_LARGE_STRING;
			break;
		case arrow::Type::STRING_VIEW:
			out->id = VPQ_T_STRING_VIEW;
			break;
		case arrow::Type::BINARY:
			out->id = VPQ_T_BINARY;
			break;
		case arrow::Type::LARGE_BINARY:
			out->id = VPQ_T_LARGE_BINARY;
			break;
		case arrow::Type::BINARY_VIEW:
			out->id = VPQ_T_BINARY_VIEW;
			break;
		case arrow::Type::FIXED_SIZE_BINARY:
			out->id = VPQ_T_FIXED_BINARY;
			out->byte_width = static_cast<const arrow::FixedSizeBinaryType &>(*t).byte_width();
			break;
		case arrow::Type::DICTIONARY:
			{
				const auto &d = static_cast<const arrow::DictionaryType &>(*t);
				vpq_type	values;

				describe(d.value_type(), &values);
				out->id = VPQ_T_DICTIONARY;
				out->value_id = values.id;
				out->index_width = d.index_type()->byte_width();
			}
			break;
		case arrow::Type::EXTENSION:
			{
				const auto &e = static_cast<const arrow::ExtensionType &>(*t);
				char		name[64];

				snprintf(name, sizeof(name), "%s", out->name);
				describe(e.storage_type(), out);
				snprintf(out->name, sizeof(out->name), "%s", name);
				if (e.extension_name() == "arrow.uuid")
					out->uuid = true;
			}
			break;
		case arrow::Type::LIST:
			out->id = VPQ_T_LIST;
			break;
		case arrow::Type::LARGE_LIST:
			out->id = VPQ_T_LARGE_LIST;
			break;
		case arrow::Type::MAP:
			out->id = VPQ_T_MAP;
			break;
		default:
			out->id = VPQ_T_OTHER;
			break;
	}
}

// PQ19: a nested type's children, each flat -- a list's elements, a map's keys and values -- false for any other shape
bool
nested_children(const std::shared_ptr<arrow::DataType> &t, std::vector<vpq_type> *kids, std::vector<uint8_t> *nullable)
{
	std::vector<std::shared_ptr<arrow::Field>> fields;

	switch (t->id())
	{
		case arrow::Type::LIST:
		case arrow::Type::LARGE_LIST:
			fields.push_back(static_cast<const arrow::BaseListType &>(*t).value_field());
			break;
		case arrow::Type::MAP:
			{
				const auto &m = static_cast<const arrow::MapType &>(*t);

				fields.push_back(m.key_field());
				fields.push_back(m.item_field());
			}
			break;
		default:
			return false;
	}
	for (const auto &f : fields)
	{
		vpq_type	k;

		describe(f->type(), &k);
		if (k.id == VPQ_T_OTHER || k.id == VPQ_T_LIST || k.id == VPQ_T_LARGE_LIST || k.id == VPQ_T_MAP)
			return false;
		kids->push_back(k);
		nullable->push_back(f->nullable() ? 1 : 0);
	}
	return true;
}

int
count_leaves(const parquet::schema::Node *node)
{
	if (node->is_primitive())
		return 1;

	const auto *g = static_cast<const parquet::schema::GroupNode *>(node);
	int			n = 0;

	for (int i = 0; i < g->field_count(); i++)
		n += count_leaves(g->field(i).get());
	return n;
}

parquet::ArrowReaderProperties
reader_properties()
{
	parquet::ArrowReaderProperties p(false);

	p.set_coerce_int96_timestamp_unit(arrow::TimeUnit::MICRO);
	return p;
}

// The footer's schema as Arrow's reader makes it, each top-level field's leaf, and its geospatial facts (PQ16)
arrow::Status
describe_footer(Footer &f)
{
	const parquet::SchemaDescriptor *schema = f.md->schema();
	const auto *root = schema->group_node();
	const auto &kv = f.md->key_value_metadata();
	int			leaf = 0;

	ARROW_RETURN_NOT_OK(parquet::arrow::FromParquetSchema(schema, reader_properties(),
														  f.md->key_value_metadata(), &f.schema));
	if (f.schema->num_fields() != root->field_count())
		return arrow::Status::Invalid("the file's schema has ", root->field_count(),
									  " columns where Arrow's reader makes ", f.schema->num_fields());
	if (kv)
	{
		auto		geo = kv->Get("geo");

		if (geo.ok())
			f.geo = *geo;
	}
	f.has_geo = !f.geo.empty();
	for (int i = 0; i < root->field_count(); i++)
	{
		const auto *node = root->field(i).get();
		vpq_type	t;
		uint8_t		geo_kind = VPQ_GEO_NONE;
		std::string crs;

		describe(f.schema->field(i)->type(), &t);
		if (node->is_primitive())
		{
			const auto *prim = static_cast<const parquet::schema::PrimitiveNode *>(node);
			const auto &lt = prim->logical_type();

			f.leaf_of.push_back(leaf);
			if (lt && lt->is_UUID())
				t.uuid = true;
			if (lt && lt->is_geometry())
			{
				geo_kind = VPQ_GEO_GEOMETRY;
				crs = static_cast<const parquet::GeometryLogicalType &>(*lt).crs();
			}
			else if (lt && lt->is_geography())
			{
				geo_kind = VPQ_GEO_GEOGRAPHY;
				crs = static_cast<const parquet::GeographyLogicalType &>(*lt).crs();
			}
			/* Parquet's "projjson:<key>": the CRS is the footer's key's value */
			if (geo_kind != VPQ_GEO_NONE && crs.rfind("projjson:", 0) == 0 && kv)
			{
				auto		v = kv->Get(crs.substr(9));

				if (v.ok())
					crs = *v;
			}
		}
		else
		{
			std::vector<vpq_type> kids;
			std::vector<uint8_t> kids_nullable;

			f.leaf_of.push_back(-1);
			/* PQ19: a list or a map of flat children keeps its type, read by its leaves */
			if (nested_children(f.schema->field(i)->type(), &kids, &kids_nullable))
			{
				f.children.push_back(kids);
				f.child_nullable.push_back(kids_nullable);
				f.has_nested = true;
			}
			else
				t.id = VPQ_T_OTHER;
		}
		if (f.children.size() < f.leaf_of.size())
		{
			f.children.emplace_back();
			f.child_nullable.emplace_back();
		}
		f.leaf_first.push_back(leaf);
		f.leaf_count.push_back(count_leaves(node));
		f.geo_kind.push_back(geo_kind);
		f.geo_crs.push_back(crs);
		if (geo_kind != VPQ_GEO_NONE)
			f.has_geo = true;
		f.types.push_back(t);
		f.field_ids.push_back(node->field_id());
		leaf += count_leaves(node);
	}
	f.bytes = f.md->size() + sizeof(Footer) + 256 * root->field_count() + f.geo.size();
	for (const auto &crs : f.geo_crs)
		f.bytes += crs.size();
	return arrow::Status::OK();
}

// PQ16: a column chunk's GeospatialStatistics' bounds of X and Y -- xmin, xmax, ymin, ymax -- where valid and finite
bool
geo_bounds(const parquet::FileMetaData &md, int rg, int leaf, double *b)
{
	if (leaf < 0 || rg < 0 || rg >= md.num_row_groups())
		return false;
	try
	{
		auto		gs = md.RowGroup(rg)->ColumnChunk(leaf)->geo_statistics();

		if (!gs || !gs->is_valid())
			return false;

		auto		valid = gs->dimension_valid();
		auto		empty = gs->dimension_empty();
		auto		lo = gs->lower_bound();
		auto		hi = gs->upper_bound();

		if (!valid[0] || !valid[1] || empty[0] || empty[1])
			return false;
		b[0] = lo[0];
		b[1] = hi[0];
		b[2] = lo[1];
		b[3] = hi[1];
		return std::isfinite(b[0]) && std::isfinite(b[1]) && std::isfinite(b[2]) && std::isfinite(b[3]) &&
			b[0] <= b[1] && b[2] <= b[3];
	}
	catch (...)
	{
		return false;
	}
}

// A statistic's scalar as a vpq_value, a string's scalar kept for its bytes; false for a type the C side does not compare
bool
scalar_value(std::deque<std::shared_ptr<arrow::Scalar>> &keep, const std::shared_ptr<arrow::Scalar> &s,
			 vpq_value *v)
{
	memset(v, 0, sizeof(*v));
	if (!s || !s->is_valid)
		return false;
	switch (s->type->id())
	{
		case arrow::Type::BOOL:
			v->i = static_cast<const arrow::BooleanScalar &>(*s).value ? 1 : 0;
			return true;
		case arrow::Type::INT8:
			v->i = static_cast<const arrow::Int8Scalar &>(*s).value;
			return true;
		case arrow::Type::INT16:
			v->i = static_cast<const arrow::Int16Scalar &>(*s).value;
			return true;
		case arrow::Type::INT32:
			v->i = static_cast<const arrow::Int32Scalar &>(*s).value;
			return true;
		case arrow::Type::INT64:
			v->i = static_cast<const arrow::Int64Scalar &>(*s).value;
			return true;
		case arrow::Type::UINT8:
			v->i = static_cast<const arrow::UInt8Scalar &>(*s).value;
			return true;
		case arrow::Type::UINT16:
			v->i = static_cast<const arrow::UInt16Scalar &>(*s).value;
			return true;
		case arrow::Type::UINT32:
			v->i = static_cast<const arrow::UInt32Scalar &>(*s).value;
			return true;
		case arrow::Type::UINT64:
			{
				uint64_t	u = static_cast<const arrow::UInt64Scalar &>(*s).value;

				if (u > (uint64_t) INT64_MAX)
					return false;
				v->i = (int64_t) u;
				return true;
			}
		case arrow::Type::FLOAT:
			v->d = static_cast<const arrow::FloatScalar &>(*s).value;
			return true;
		case arrow::Type::DOUBLE:
			v->d = static_cast<const arrow::DoubleScalar &>(*s).value;
			return true;
		case arrow::Type::DATE32:
			v->i = static_cast<const arrow::Date32Scalar &>(*s).value;
			return true;
		case arrow::Type::DATE64:
			v->i = static_cast<const arrow::Date64Scalar &>(*s).value;
			return true;
		case arrow::Type::TIME32:
			v->i = static_cast<const arrow::Time32Scalar &>(*s).value;
			return true;
		case arrow::Type::TIME64:
			v->i = static_cast<const arrow::Time64Scalar &>(*s).value;
			return true;
		case arrow::Type::TIMESTAMP:
			v->i = static_cast<const arrow::TimestampScalar &>(*s).value;
			return true;
		case arrow::Type::DECIMAL32:
			v->i = static_cast<const arrow::Decimal32Scalar &>(*s).value.value();
			v->i128_lo = (uint64_t) v->i;
			v->i128_hi = v->i < 0 ? -1 : 0;
			return true;
		case arrow::Type::DECIMAL64:
			v->i = static_cast<const arrow::Decimal64Scalar &>(*s).value.value();
			v->i128_lo = (uint64_t) v->i;
			v->i128_hi = v->i < 0 ? -1 : 0;
			return true;
		case arrow::Type::DECIMAL128:
			{
				const auto &d = static_cast<const arrow::Decimal128Scalar &>(*s).value;

				v->i128_lo = d.low_bits();
				v->i128_hi = d.high_bits();
				v->i = (int64_t) d.low_bits();
				return true;
			}
		case arrow::Type::STRING:
		case arrow::Type::BINARY:
		case arrow::Type::LARGE_STRING:
		case arrow::Type::LARGE_BINARY:
		case arrow::Type::FIXED_SIZE_BINARY:
			{
				const auto &b = static_cast<const arrow::BaseBinaryScalar &>(*s);

				keep.push_back(s);
				v->bytes = reinterpret_cast<const char *>(b.value->data());
				v->len = (int32_t) b.value->size();
				return true;
			}
		case arrow::Type::EXTENSION:
			{
				const auto &e = static_cast<const arrow::ExtensionScalar &>(*s);

				keep.push_back(s);
				return scalar_value(keep, e.value, v);
			}
		default:
			return false;
	}
}

// A column chunk's statistics, as vpq_file_column_stats() hands them (engine.h)
bool
chunk_stats(const parquet::FileMetaData &md, int rg, int leaf, std::deque<std::shared_ptr<arrow::Scalar>> &keep,
			int64_t *null_count, bool *has_minmax, vpq_value *min, vpq_value *max, bool *min_exact,
			bool *max_exact)
{
	*null_count = -1;
	*has_minmax = false;
	*min_exact = *max_exact = false;

	auto		cc = md.RowGroup(rg)->ColumnChunk(leaf);

	if (!cc->is_stats_set())
		return false;

	auto		st = cc->statistics();

	if (!st)
		return false;
	if (st->HasNullCount())
		*null_count = st->null_count();
	if (st->HasMinMax())
	{
		std::shared_ptr<arrow::Scalar> smin;
		std::shared_ptr<arrow::Scalar> smax;

		if (parquet::arrow::StatisticsAsScalars(*st, &smin, &smax).ok() &&
			scalar_value(keep, smin, min) && scalar_value(keep, smax, max))
		{
			*has_minmax = true;
			*min_exact = st->is_min_value_exact().value_or(true);
			*max_exact = st->is_max_value_exact().value_or(true);
		}
	}
	return true;
}

// A statistic's value in facts: by the fields it fills
void
put_value(std::string *out, const vpq_value &v)
{
	uint64_t	dbits;

	memcpy(&dbits, &v.d, sizeof(dbits));
	if (v.bytes != nullptr)
	{
		put<uint8_t>(out, 2);
		put<int32_t>(out, v.len);
		out->append(v.bytes, (size_t) v.len);
	}
	else if (v.i128_lo == 0 && v.i128_hi == 0 && dbits == 0)
	{
		put<uint8_t>(out, 0);
		put<int64_t>(out, v.i);
	}
	else if (v.i == 0 && v.i128_lo == 0 && v.i128_hi == 0)
	{
		put<uint8_t>(out, 3);
		put<double>(out, v.d);
	}
	else
	{
		put<uint8_t>(out, 1);
		put<int64_t>(out, v.i);
		put<uint64_t>(out, v.i128_lo);
		put<int64_t>(out, v.i128_hi);
		put<double>(out, v.d);
	}
}

bool
take_value(FactsReader &r, vpq_value *v)
{
	uint8_t		tag;

	memset(v, 0, sizeof(*v));
	if (!r.take(&tag))
		return false;
	switch (tag)
	{
		case 0:
			return r.take(&v->i);
		case 3:
			return r.take(&v->d);
		case 1:
			return r.take(&v->i) && r.take(&v->i128_lo) && r.take(&v->i128_hi) && r.take(&v->d);
		case 2:
			if (!r.take(&v->len) || v->len < 0 || (size_t) (r.end - r.p) < (size_t) v->len)
				return false;
			v->bytes = r.p;
			r.p += v->len;
			return true;
		default:
			return false;
	}
}

// A footer's facts, of the flags' forms; versions: its first data pages' read so far, or none
std::string
build_facts(const Footer &f, int flags, const std::vector<int8_t> *versions)
{
	const parquet::FileMetaData &md = *f.md;
	int			nrg = md.num_row_groups();
	int			nleaves = md.num_columns();
	int			ncols = (int) f.types.size();
	uint8_t		codec = VPQ_CODEC_OTHER;
	std::string out;
	std::string created_by = md.created_by();

	if (flags & VPQ_FACTS_STATS)
		flags |= VPQ_FACTS_CHUNKS;
	/* PQ16: a footer's geospatial facts in every form of its facts, so that its chunks begin where facts_chunks_at says */
	if (f.has_geo)
		flags |= VPQ_FACTS_GEO;
	/* PQ19: a nested column's children, so that a handle of facts types it */
	if (f.has_nested)
		flags |= VPQ_FACTS_NESTED;
	if (nrg > 0 && nleaves > 0)
		codec = (uint8_t) codec_id(md.RowGroup(0)->ColumnChunk(0)->compression());
	put<uint32_t>(&out, FACTS_MAGIC);
	put<uint16_t>(&out, FACTS_FORM);
	put<uint16_t>(&out, (uint16_t) flags);
	put<int64_t>(&out, md.num_rows());
	put<int32_t>(&out, nrg);
	put<int32_t>(&out, ncols);
	put<int32_t>(&out, nleaves);
	put<uint8_t>(&out, codec);
	out.append(3, '\0');
	put<uint32_t>(&out, (uint32_t) created_by.size());
	out += created_by;
	for (int g = 0; g < nrg; g++)
		put<int64_t>(&out, md.RowGroup(g)->num_rows());
	for (int c = 0; c < ncols; c++)
	{
		const auto &field = f.schema->field(c);
		const vpq_type &t = f.types[c];
		std::string tname(t.name);

		put<uint16_t>(&out, (uint16_t) std::min<size_t>(field->name().size(), UINT16_MAX));
		out.append(field->name(), 0, std::min<size_t>(field->name().size(), UINT16_MAX));
		put<int32_t>(&out, f.field_ids[c]);
		put<uint8_t>(&out, field->nullable() ? 1 : 0);
		put<int32_t>(&out, f.leaf_of[c]);
		put<int32_t>(&out, t.id);
		put<int32_t>(&out, t.unit);
		put<int32_t>(&out, t.precision);
		put<int32_t>(&out, t.scale);
		put<int32_t>(&out, t.byte_width);
		put<int32_t>(&out, t.value_id);
		put<int32_t>(&out, t.index_width);
		put<uint8_t>(&out, t.has_tz ? 1 : 0);
		put<uint8_t>(&out, t.uuid ? 1 : 0);
		put<uint8_t>(&out, (uint8_t) std::min<size_t>(tname.size(), 63));
		out.append(tname, 0, std::min<size_t>(tname.size(), 63));
	}
	if (flags & VPQ_FACTS_GEO)
	{
		put<uint32_t>(&out, (uint32_t) f.geo.size());
		out += f.geo;
		for (int c = 0; c < ncols; c++)
		{
			put<uint8_t>(&out, f.geo_kind[c]);
			put<uint32_t>(&out, (uint32_t) f.geo_crs[c].size());
			out += f.geo_crs[c];
		}
		for (int g = 0; g < nrg; g++)
			for (int c = 0; c < ncols; c++)
			{
				double		b[4] = {0, 0, 0, 0};
				bool		valid;

				if (f.geo_kind[c] == VPQ_GEO_NONE)
					continue;
				valid = geo_bounds(md, g, f.leaf_of[c], b);
				put<uint8_t>(&out, valid ? 1 : 0);
				out.append(7, '\0');
				for (int i = 0; i < 4; i++)
					put<double>(&out, valid ? b[i] : 0.0);
			}
	}
	if (flags & VPQ_FACTS_NESTED)
		for (int c = 0; c < ncols; c++)
		{
			put<uint8_t>(&out, (uint8_t) f.children[c].size());
			for (size_t k = 0; k < f.children[c].size(); k++)
			{
				put_type(&out, f.children[c][k]);
				put<uint8_t>(&out, f.child_nullable[c][k]);
			}
		}
	if (flags & VPQ_FACTS_CHUNKS)
	{
		std::vector<bool> flat(nleaves, false);
		std::string stats;

		for (int c = 0; c < ncols; c++)
			if (f.leaf_of[c] >= 0 && f.leaf_of[c] < nleaves)
				flat[f.leaf_of[c]] = true;
		for (int g = 0; g < nrg; g++)
		{
			auto		rg = md.RowGroup(g);

			for (int leaf = 0; leaf < nleaves; leaf++)
			{
				auto		cc = rg->ColumnChunk(leaf);
				size_t		at = (size_t) g * nleaves + leaf;
				int8_t		version = 0;
				uint32_t	soff = 0;

				if (flags & VPQ_FACTS_PAGES_V1)
					version = 1;
				else if (versions != nullptr && at < versions->size())
					version = (*versions)[at];
				if ((flags & VPQ_FACTS_STATS) && flat[leaf])
				{
					std::deque<std::shared_ptr<arrow::Scalar>> keep;
					int64_t		nulls;
					bool		has_minmax;
					vpq_value	lo;
					vpq_value	hi;
					bool		lo_exact;
					bool		hi_exact;
					bool		ok;

					try
					{
						ok = chunk_stats(md, g, leaf, keep, &nulls, &has_minmax, &lo, &hi, &lo_exact, &hi_exact);
					}
					catch (...)
					{
						ok = false;
					}
					if (ok)
					{
						soff = (uint32_t) stats.size() + 1;
						put<uint8_t>(&stats, has_minmax ? 2 : 1);
						put<uint8_t>(&stats, lo_exact ? 1 : 0);
						put<uint8_t>(&stats, hi_exact ? 1 : 0);
						put<uint8_t>(&stats, 0);
						put<int64_t>(&stats, nulls);
						if (has_minmax)
						{
							put_value(&stats, lo);
							put_value(&stats, hi);
						}
					}
				}
				put<uint8_t>(&out, (uint8_t) codec_id(cc->compression()));
				put<int8_t>(&out, version);
				{
					// W38: the chunk's bytes before compression, as a ratio to its bytes
					int64_t		comp = cc->total_compressed_size();
					int64_t		raw = cc->total_uncompressed_size();

					put<uint16_t>(&out, comp > 0 && raw > 0 ?
								  (uint16_t) std::min<int64_t>(UINT16_MAX, (raw * 256 + comp - 1) / comp) : 0);
				}
				put<uint32_t>(&out, soff);
				put<int64_t>(&out, cc->data_page_offset());
				put<int64_t>(&out, cc->total_compressed_size());
			}
		}
		out += stats;
	}
	return out;
}

}								// namespace

//
// The calls
//

struct vpq_file
{
	std::string location;
	Located		loc;
	int64_t		size = -1;
	int64_t		mtime = -1;
	std::shared_ptr<Footer> footer;	// null for a handle of facts until its footer is read
	std::shared_ptr<Facts> facts;
	std::deque<std::shared_ptr<arrow::Scalar>> scalars;	// statistics' values handed out
	std::string created_by;
};

namespace
{

// The file's footer: from the process's cache, or read from the file and kept
arrow::Status
read_footer(vpq_file *f)
{
	std::string key = footer_key(f->location, f->size, f->mtime);

	f->footer = footer_find(key);
	if (f->footer)
		return arrow::Status::OK();

	fs::FileInfo info(f->loc.path, fs::FileType::File);

	info.set_size(f->size);
	ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::io::RandomAccessFile> input, open_input(f->loc.fs, info));

	parquet::ReaderProperties props(pool());
	auto		reader = parquet::ParquetFileReader::Open(input, props);
	auto		footer = std::make_shared<Footer>();

	footer->md = reader->metadata();
	ARROW_RETURN_NOT_OK(describe_footer(*footer));
	counters.footers_read++;
	if (f->facts)
		counters.footers_late++;
	footer_keep(key, footer);
	f->footer = footer;
	return arrow::Status::OK();
}

// A chunk's first data page's version, read from its header (vpq_file_column_pages()): 1, 2, -1 not told
int8_t
read_page_version(vpq_file *f, int64_t data_page_offset)
{
	fs::FileInfo info(f->loc.path, fs::FileType::File);
	uint8_t		header[2] = {0, 0};
	int8_t		version = -1;

	info.set_size(f->size);

	auto		input = open_input(f->loc.fs, info);

	if (input.ok())
	{
		auto		n = (*input)->ReadAt(data_page_offset, 2, header);

		if (n.ok() && *n == 2 && header[0] == 0x15)
			version = header[1] == 0x00 ? 1 : header[1] == 0x06 ? 2 : -1;
		counters.versions_read++;
		(void) (*input)->Close();
	}
	return version;
}

}								// namespace

struct vpq_chunk
{
	int			refs = 1;
	int64_t		rows = 0;
	std::vector<std::shared_ptr<arrow::Array>> arrays;
	std::deque<vpq_array> desc;	// the columns', then their dictionaries'
	std::deque<std::vector<const uint8_t *>> view_buffers;
	std::deque<std::vector<int64_t>> view_sizes;
	std::vector<const vpq_array *> columns;
	std::vector<std::shared_ptr<arrow::Array>> kept;	// PQ19: a map's keys and values, sliced as its entries are
};

struct vpq_reader
{
	vpq_file   *file = nullptr;
	std::unique_ptr<parquet::arrow::FileReader> reader;
	std::shared_ptr<arrow::io::RandomAccessFile> input;	// the file, for point reads' pages
	// W55: a local file's point reads' pages walked through a buffered stream, its footer the reader's
	std::once_flag walk_once;
	std::unique_ptr<parquet::ParquetFileReader> walk;
	// PQ11: a reader over a footer of some columns -- its metadata, and each file column's place in it, -1 none
	bool		projected = false;
	std::shared_ptr<parquet::FileMetaData> md;	// the reader's footer, projected or the file's
	std::vector<int> col_of;	// projected: per file column, the reader's column, -1 none
	std::shared_ptr<arrow::Schema> schema;	// the reader's columns as Arrow's reader makes them
};

extern "C" const char *
vpq_error(void)
{
	return last_error.c_str();
}

extern "C" void
vpq_set_wait_hook(vpq_wait_hook hook)
{
	wait_hook = hook;
}

extern "C" int
vpq_configure(const vpq_s3_config *c)
{
	ENGINE_TRY
	{
		Config		n;

		n.endpoint = c->endpoint ? c->endpoint : "";
		n.region = c->region ? c->region : "";
		n.access_key = c->access_key ? c->access_key : "";
		n.secret_key = c->secret_key ? c->secret_key : "";
		n.session_token = c->session_token ? c->session_token : "";
		n.scheme = c->scheme ? c->scheme : "";
		n.virtual_addressing = c->virtual_addressing;
		n.anonymous = c->anonymous;
		n.connect_timeout = c->connect_timeout;
		n.request_timeout = c->request_timeout;
		n.prefetch = c->prefetch;
		n.io_threads = c->io_threads > 0 ? c->io_threads : 1;
		if (!configured || !(n == config))
			s3_filesystems.clear();
		config = n;
		if (!configured)
		{
			/* Arrow's CPU pool unused: a reader's columns are decoded on the caller's thread */
			CHECK_OK(arrow::SetCpuThreadPoolCapacity(1));
		}
		if (io_threads_set != config.io_threads)
		{
			CHECK_OK(arrow::io::SetIOThreadPoolCapacity(config.io_threads));
			io_threads_set = config.io_threads;
		}
		configured = true;
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" bool
vpq_is_s3(const char *location)
{
	return location != nullptr && strncmp(location, "s3://", 5) == 0;
}

extern "C" void
vpq_shutdown(void)
{
	SignalBlock signal_block;

	try
	{
		/*
		 * PQ10 (pg_vector_executor.md §3.37): the footers and facts left to
		 * the process's end, not freed one by one -- a parallel worker's exit,
		 * which its leader waits for, freed ClickBench's footer of 23,730
		 * chunks in a sixth of Q1's cycles at 100M rows -- moved out of the
		 * statics whose destructors exit() runs
		 */
		new decltype(footer_lru)(std::move(footer_lru));
		new decltype(footer_index)(std::move(footer_index));
		new decltype(facts_lru)(std::move(facts_lru));
		new decltype(facts_index)(std::move(facts_index));
		footer_lru.clear();
		footer_index.clear();
		footer_cached = 0;
		facts_lru.clear();
		facts_index.clear();
		facts_cached = 0;
		s3_filesystems.clear();
		if (s3_initialized)
		{
			(void) fs::FinalizeS3();
			s3_initialized = false;
		}
	}
	catch (...)
	{
	}
}

extern "C" int
vpq_list(const char *location, vpq_entry **entries, int64_t *n)
{
	ENGINE_TRY
	{
		std::string loc_s(location);
		auto		[bare, query] = split_query(loc_s);
		Located		loc;
		std::vector<std::pair<std::string, fs::FileInfo>> found;

		*entries = nullptr;
		*n = 0;
		if (has_glob(bare))
		{
			std::string head = bare.substr(0, bare.find_first_of("*?["));
			size_t		slash = head.rfind('/');

			if (slash == std::string::npos)
				return fail("a glob needs a directory before its first wildcard: \"" + loc_s + "\"");
			ASSIGN_OR_FAIL(Located base, locate(head.substr(0, slash + 1) + query));
			ASSIGN_OR_FAIL(Located pattern, locate(loc_s));

			fs::FileSelector sel;

			sel.base_dir = base.path;
			sel.allow_not_found = true;
			sel.recursive = true;
			ASSIGN_OR_FAIL(std::vector<fs::FileInfo> infos, base.fs->GetFileInfo(sel));
			for (const auto &info : infos)
				if (info.type() == fs::FileType::File &&
					glob_match(pattern.path.c_str(), info.path().c_str()))
					found.emplace_back(location_of(base, info.path()), info);
			loc = base;
		}
		else
		{
			ASSIGN_OR_FAIL(loc, locate(loc_s));
			ASSIGN_OR_FAIL(fs::FileInfo info, loc.fs->GetFileInfo(loc.path));

			if (info.type() == fs::FileType::NotFound)
				return VPQ_NOT_FOUND;
			if (info.type() == fs::FileType::File)
				found.emplace_back(location_of(loc, info.path()), info);
			else
			{
				fs::FileSelector sel;

				sel.base_dir = loc.path;
				sel.recursive = true;
				ASSIGN_OR_FAIL(std::vector<fs::FileInfo> infos, loc.fs->GetFileInfo(sel));
				for (const auto &f : infos)
					if (f.type() == fs::FileType::File && is_parquet_name(f.path()))
						found.emplace_back(location_of(loc, f.path()), f);
			}
		}
		std::sort(found.begin(), found.end(),
				  [](const auto &a, const auto &b) { return a.first < b.first; });
		return fill_entries(found, entries, n);
	}
	ENGINE_CATCH
}

extern "C" void
vpq_list_free(vpq_entry *entries, int64_t n)
{
	if (entries == nullptr)
		return;
	for (int64_t i = 0; i < n; i++)
		free(entries[i].path);
	free(entries);
}

extern "C" int
vpq_list_dir(const char *dir, vpq_entry **entries, int64_t *n)
{
	ENGINE_TRY
	{
		std::vector<std::pair<std::string, fs::FileInfo>> found;
		fs::FileSelector sel;

		*entries = nullptr;
		*n = 0;
		ASSIGN_OR_FAIL(Located loc, locate(dir));
		sel.base_dir = loc.path;
		sel.allow_not_found = true;
		sel.recursive = false;
		ASSIGN_OR_FAIL(std::vector<fs::FileInfo> infos, loc.fs->GetFileInfo(sel));
		for (const auto &f : infos)
			if (f.type() == fs::FileType::File)
				found.emplace_back(location_of(loc, f.path()), f);
		std::sort(found.begin(), found.end(),
				  [](const auto &a, const auto &b) { return a.first < b.first; });
		return fill_entries(found, entries, n);
	}
	ENGINE_CATCH
}

extern "C" int
vpq_stat(const char *path, vpq_entry *entry)
{
	ENGINE_TRY
	{
		ASSIGN_OR_FAIL(Located loc, locate(path));
		ASSIGN_OR_FAIL(fs::FileInfo info, loc.fs->GetFileInfo(loc.path));

		if (info.type() != fs::FileType::File)
			return VPQ_NOT_FOUND;
		entry->path = nullptr;
		entry->size = info.size();
		entry->mtime_ns = mtime_ns(info);
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" int
vpq_make_dir(const char *path)
{
	ENGINE_TRY
	{
		ASSIGN_OR_FAIL(Located loc, locate(path));

		if (!loc.s3)
			CHECK_OK(loc.fs->CreateDir(loc.path, true));
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" int
vpq_delete_file(const char *path)
{
	ENGINE_TRY
	{
		ASSIGN_OR_FAIL(Located loc, locate(path));
		ASSIGN_OR_FAIL(fs::FileInfo info, loc.fs->GetFileInfo(loc.path));

		if (info.type() == fs::FileType::NotFound)
			return VPQ_OK;
		CHECK_OK(loc.fs->DeleteFile(loc.path));
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" int
vpq_delete_dir(const char *path)
{
	ENGINE_TRY
	{
		ASSIGN_OR_FAIL(Located loc, locate(path));
		ASSIGN_OR_FAIL(fs::FileInfo info, loc.fs->GetFileInfo(loc.path));

		if (info.type() == fs::FileType::NotFound)
			return VPQ_OK;
		if (info.type() == fs::FileType::File)
			return fail("not a directory: " + std::string(path));
		CHECK_OK(loc.fs->DeleteDir(loc.path));
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" int
vpq_copy_file(const char *from, const char *to)
{
	ENGINE_TRY
	{
		ASSIGN_OR_FAIL(Located src, locate(from));
		ASSIGN_OR_FAIL(Located dst, locate(to));

		if (src.fs == dst.fs)
		{
			CHECK_OK(src.fs->CopyFile(src.path, dst.path));
			return VPQ_OK;
		}
		ASSIGN_OR_FAIL(std::shared_ptr<arrow::io::InputStream> in, src.fs->OpenInputStream(src.path));
		ASSIGN_OR_FAIL(std::shared_ptr<arrow::io::OutputStream> out, dst.fs->OpenOutputStream(dst.path));
		for (;;)
		{
			ASSIGN_OR_FAIL(std::shared_ptr<arrow::Buffer> buf, in->Read(8 << 20));
			if (buf->size() == 0)
				break;
			CHECK_OK(out->Write(buf));
		}
		CHECK_OK(out->Close());
		CHECK_OK(in->Close());
		return VPQ_OK;
	}
	ENGINE_CATCH
}

//
// Files and their footers
//

extern "C" int
vpq_file_open(const char *path, int64_t size, int64_t mtime_ns_, vpq_file **file)
{
	ENGINE_TRY
	{
		auto		f = std::make_unique<vpq_file>();

		*file = nullptr;
		f->location = path;
		ASSIGN_OR_FAIL(f->loc, locate(f->location));
		if (size < 0)
		{
			ASSIGN_OR_FAIL(fs::FileInfo info, f->loc.fs->GetFileInfo(f->loc.path));

			if (info.type() != fs::FileType::File)
				return VPQ_NOT_FOUND;
			size = info.size();
			mtime_ns_ = mtime_ns(info);
		}
		f->size = size;
		f->mtime = mtime_ns_;
		CHECK_OK(read_footer(f.get()));
		f->created_by = f->footer->md->created_by();
		*file = f.release();
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" int
vpq_file_open_facts(const char *path, int64_t size, int64_t mtime_ns_, const char *facts, size_t len,
					vpq_file **file)
{
	ENGINE_TRY
	{
		auto		f = std::make_unique<vpq_file>();
		uint16_t	flags = 0;

		*file = nullptr;
		if (size < 0 || facts == nullptr || len < 8)
			return VPQ_NOT_FOUND;
		f->location = path;
		ASSIGN_OR_FAIL(f->loc, locate(f->location));
		f->size = size;
		f->mtime = mtime_ns_;

		std::string key = footer_key(f->location, size, mtime_ns_);

		/* a footer the process read already serves as well, and with everything */
		f->footer = footer_find(key);
		if (f->footer)
		{
			f->created_by = f->footer->md->created_by();
			*file = f.release();
			return VPQ_OK;
		}
		memcpy(&flags, facts + 6, sizeof(flags));
		f->facts = facts_find(key);
		if (!f->facts || (f->facts->flags & flags) != flags)
		{
			f->facts = parse_facts(facts, len);
			if (!f->facts)
				return VPQ_NOT_FOUND;
			facts_keep(key, f->facts);
		}
		counters.facts_opened++;
		f->created_by = f->facts->created_by;
		*file = f.release();
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" int
vpq_file_facts(vpq_file *file, int flags, char **facts, size_t *len)
{
	ENGINE_TRY
	{
		std::string b;

		*facts = nullptr;
		*len = 0;
		if (flags & VPQ_FACTS_STATS)
			flags |= VPQ_FACTS_CHUNKS;
		if (file->facts && !file->footer && (file->facts->flags & flags) == flags)
			b = file->facts->blob;
		else
		{
			if (!file->footer)
				CHECK_OK(read_footer(file));
			b = build_facts(*file->footer, flags, &file->footer->page_versions);
		}
		*facts = (char *) malloc(std::max<size_t>(b.size(), 1));
		if (*facts == nullptr)
			return fail("out of memory making a file's facts");
		memcpy(*facts, b.data(), b.size());
		*len = b.size();
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" bool
vpq_file_has_footer(vpq_file *file)
{
	return file->footer != nullptr;
}

extern "C" int
vpq_file_page_version(vpq_file *file, int rg, int col)
{
	SignalBlock signal_block;

	try
	{
		if (!file->footer && !read_footer(file).ok())
			return -1;

		int			leaf = file->footer->leaf_of[col];

		if (leaf < 0 || rg < 0 || rg >= file->footer->md->num_row_groups())
			return -1;
		return read_page_version(file, file->footer->md->RowGroup(rg)->ColumnChunk(leaf)->data_page_offset());
	}
	catch (...)
	{
		return -1;
	}
}

extern "C" void
vpq_engine_stats_get(vpq_engine_stats *out, bool reset)
{
	*out = counters;
	read_pool()->stats(&out->pool_mapped, &out->pool_reused, &out->pool_moved, &out->pool_kept_bytes);
	if (reset)
	{
		counters.footers_read = counters.facts_opened = counters.footers_late = counters.versions_read = 0;
	}
}

extern "C" void
vpq_engine_footer_counts(int64_t *footers_read, int64_t *facts_opened)
{
	*footers_read = counters.footers_read;
	*facts_opened = counters.facts_opened;
}

extern "C" void
vpq_engine_stats_reset_scans(void)
{
	read_pool()->reset_stats();
	counters.footers_projected = counters.projections_refused = 0;
	counters.pages_decoded = counters.pages_passed = 0;
}

extern "C" void
vpq_pool_set_keep(int64_t bytes)
{
	try
	{
		read_pool()->set_keep(bytes);
	}
	catch (...)
	{
	}
}

extern "C" void
vpq_pool_release(void)
{
	try
	{
		read_pool()->release();
	}
	catch (...)
	{
	}
}

extern "C" void
vpq_file_close(vpq_file *file)
{
	delete file;
}

extern "C" const char *
vpq_file_location(vpq_file *file)
{
	return file->location.c_str();
}

extern "C" int64_t
vpq_file_size(vpq_file *file)
{
	return file->size;
}

extern "C" int64_t
vpq_file_mtime(vpq_file *file)
{
	return file->mtime;
}

extern "C" int
vpq_file_row_groups(vpq_file *file)
{
	if (!file->footer)
		return file->facts->nrg;
	return file->footer->md->num_row_groups();
}

extern "C" int64_t
vpq_file_rows(vpq_file *file)
{
	if (!file->footer)
		return file->facts->num_rows;
	return file->footer->md->num_rows();
}

extern "C" int64_t
vpq_file_row_group_rows(vpq_file *file, int rg)
{
	if (!file->footer)
		return rg >= 0 && rg < file->facts->nrg ? file->facts->rg_rows[rg] : 0;
	return file->footer->md->RowGroup(rg)->num_rows();
}

extern "C" int
vpq_file_columns(vpq_file *file)
{
	if (!file->footer)
		return (int) file->facts->cols.size();
	return (int) file->footer->types.size();
}

extern "C" bool
vpq_file_column(vpq_file *file, int col, const char **name, int *field_id, vpq_type *type,
				bool *nullable)
{
	if (!file->footer)
	{
		const FactsColumn &c = file->facts->cols[col];

		*name = c.name.c_str();
		*field_id = c.field_id;
		*type = c.type;
		*nullable = c.nullable;
		return c.leaf >= 0 && c.type.id != VPQ_T_OTHER;
	}

	const Footer &f = *file->footer;
	const auto &field = f.schema->field(col);

	*name = field->name().c_str();
	*field_id = f.field_ids[col];
	*type = f.types[col];
	*nullable = field->nullable();
	return f.leaf_of[col] >= 0 && f.types[col].id != VPQ_T_OTHER;
}

extern "C" int64_t
vpq_file_column_bytes(vpq_file *file, int rg, int col)
{
	if (!file->footer)
	{
		FactsChunk	c;

		return facts_chunk(*file->facts, rg, file->facts->cols[col].leaf, &c) ? c.bytes : 0;
	}

	int			leaf = file->footer->leaf_of[col];

	if (leaf < 0)
		return 0;
	try
	{
		return file->footer->md->RowGroup(rg)->ColumnChunk(leaf)->total_compressed_size();
	}
	catch (...)
	{
		return 0;
	}
}

extern "C" int64_t
vpq_file_column_raw_bytes(vpq_file *file, int rg, int col)
{
	if (!file->footer)
	{
		FactsChunk	c;

		if (col < 0 || col >= (int) file->facts->cols.size() ||
			!facts_chunk(*file->facts, rg, file->facts->cols[col].leaf, &c) || c.expand == 0)
			return 0;
		return (int64_t) (((__int128) c.bytes * c.expand + 255) / 256);
	}

	int			leaf = col >= 0 && col < (int) file->footer->leaf_of.size() ? file->footer->leaf_of[col] : -1;

	if (leaf < 0)
		return 0;
	try
	{
		return file->footer->md->RowGroup(rg)->ColumnChunk(leaf)->total_uncompressed_size();
	}
	catch (...)
	{
		return 0;
	}
}

extern "C" const char *
vpq_file_created_by(vpq_file *file)
{
	return file->created_by.c_str();
}

extern "C" int
vpq_file_column_children(vpq_file *file, int col, vpq_type *children, bool *nullable)
{
	const std::vector<std::vector<vpq_type>> &kids = file->footer ? file->footer->children : file->facts->children;
	const std::vector<std::vector<uint8_t>> &kn = file->footer ? file->footer->child_nullable :
		file->facts->child_nullable;

	if (col < 0 || col >= (int) kids.size())
		return 0;
	for (size_t k = 0; k < kids[col].size() && k < 2; k++)
	{
		children[k] = kids[col][k];
		nullable[k] = kn[col][k] != 0;
	}
	return (int) std::min<size_t>(kids[col].size(), 2);
}

extern "C" const char *
vpq_file_geo_metadata(vpq_file *file, size_t *len)
{
	const std::string &geo = file->footer ? file->footer->geo : file->facts->geo;

	*len = geo.size();
	return geo.empty() ? nullptr : geo.data();
}

extern "C" int
vpq_file_column_geo(vpq_file *file, int col, const char **crs, size_t *crs_len)
{
	const std::vector<uint8_t> &kind = file->footer ? file->footer->geo_kind : file->facts->geo_kind;
	const std::vector<std::string> &crss = file->footer ? file->footer->geo_crs : file->facts->geo_crs;

	*crs = "";
	*crs_len = 0;
	if (col < 0 || col >= (int) kind.size())
		return VPQ_GEO_NONE;
	*crs = crss[col].data();
	*crs_len = crss[col].size();
	return kind[col];
}

extern "C" bool
vpq_file_column_geo_bounds(vpq_file *file, int rg, int col, double *xmin, double *xmax, double *ymin,
						   double *ymax)
{
	SignalBlock signal_block;
	double		b[4];

	if (file->footer)
	{
		const Footer &f = *file->footer;

		if (col < 0 || col >= (int) f.geo_kind.size() || f.geo_kind[col] == VPQ_GEO_NONE ||
			!geo_bounds(*f.md, rg, f.leaf_of[col], b))
			return false;
	}
	else
	{
		const Facts &f = *file->facts;
		uint8_t		valid;

		if (col < 0 || col >= (int) f.geo_slot.size() || f.geo_slot[col] < 0 || rg < 0 || rg >= f.nrg)
			return false;

		FactsReader r{f.blob.data() + f.geo_bounds_at + GEO_BOUNDS_BYTES * ((size_t) rg * f.ngeo + f.geo_slot[col]),
					  f.blob.data() + f.blob.size()};

		if (!r.take(&valid) || valid == 0 || !r.skip(7))
			return false;
		for (int i = 0; i < 4; i++)
			if (!r.take(&b[i]))
				return false;
	}
	*xmin = b[0];
	*xmax = b[1];
	*ymin = b[2];
	*ymax = b[3];
	return true;
}

extern "C" int64_t
vpq_file_footer_bytes(vpq_file *file)
{
	if (!file->footer)
	{
		SignalBlock signal_block;

		try
		{
			if (!read_footer(file).ok())
				return 0;
		}
		catch (...)
		{
			return 0;
		}
	}
	return file->footer->md->size();
}

extern "C" bool
vpq_file_column_stats(vpq_file *file, int rg, int col, int64_t *null_count, bool *has_minmax,
					  vpq_value *min, vpq_value *max, bool *min_exact, bool *max_exact)
{
	SignalBlock signal_block;

	*null_count = -1;
	*has_minmax = false;
	*min_exact = *max_exact = false;
	try
	{
		if (!file->footer)
		{
			const Facts &f = *file->facts;
			FactsChunk	c;
			uint8_t		kind;
			uint8_t		lo_exact;
			uint8_t		hi_exact;
			uint8_t		pad;

			if (!(f.flags & VPQ_FACTS_STATS) || !facts_chunk(f, rg, f.cols[col].leaf, &c) || c.stats == 0)
				return false;

			FactsReader r{f.blob.data() + f.stats_at + c.stats - 1, f.blob.data() + f.blob.size()};

			if (!r.take(&kind) || !r.take(&lo_exact) || !r.take(&hi_exact) || !r.take(&pad) ||
				!r.take(null_count))
				return false;
			if (kind == 2)
			{
				if (!take_value(r, min) || !take_value(r, max))
				{
					*null_count = -1;
					return false;
				}
				*has_minmax = true;
				*min_exact = lo_exact != 0;
				*max_exact = hi_exact != 0;
			}
			return true;
		}

		int			leaf = file->footer->leaf_of[col];

		if (leaf < 0)
			return false;
		return chunk_stats(*file->footer->md, rg, leaf, file->scalars, null_count, has_minmax, min, max,
						   min_exact, max_exact);
	}
	catch (...)
	{
		return false;
	}
}

bool
vpq_file_column_pages(vpq_file *file, int rg, int col, int *codec, int *version)
{
	int64_t		at;

	return vpq_file_column_pages_at(file, rg, col, codec, version, &at);
}

extern "C" bool
vpq_file_column_pages_at(vpq_file *file, int rg, int col, int *codec, int *version, int64_t *facts_at)
{
	SignalBlock signal_block;

	*codec = VPQ_CODEC_OTHER;
	*version = 0;
	*facts_at = -1;
	try
	{
		/*
		 * The first data page's type, the first field of its Thrift header:
		 * 0x15 -- field 1, an i32 -- then its zigzag varint, 0x00 a data
		 * page, 0x06 one of version 2.  Its footer does not tell: Arrow's
		 * writer counts every data page as of version 1 in its encoding
		 * statistics.  Read once a file's footer or facts are kept, for its
		 * chunks under the codecs whose version matters; known without a
		 * read of the extension's own files, whose writer makes version 1's
		 */
		if (!file->footer)
		{
			Facts	   &f = *file->facts;
			int			leaf = f.cols[col].leaf;
			FactsChunk	c;

			if (leaf < 0)
				return false;
			if (!facts_chunk(f, rg, leaf, &c))
			{
				if (rg < 0 || rg >= f.nrg)
					return false;
				/* no chunk kept: the file's one codec, its pages of version 1 where so made */
				*codec = f.codec;
				*version = (*codec != VPQ_CODEC_OTHER && (f.flags & VPQ_FACTS_PAGES_V1)) ? 1 : 0;
				return true;
			}
			*codec = c.codec;
			if (*codec == VPQ_CODEC_OTHER)
				return true;

			size_t		at = (size_t) rg * f.nleaves + leaf;
			int8_t		v = c.version;

			if (v == 0)
			{
				if (f.versions.empty())
					f.versions.assign((size_t) f.nrg * f.nleaves, 0);
				if (f.versions[at] == 0)
				{
					f.versions[at] = read_page_version(file, c.data_page_offset);
					/* PQ7: its byte in the facts, the chunk's record's second */
					*facts_at = (int64_t) (f.chunks_at + CHUNK_BYTES * at + 1);
				}
				v = f.versions[at];
			}
			*version = v > 0 ? v : 0;
			return true;
		}

		int			leaf = file->footer->leaf_of[col];

		if (leaf < 0)
			return false;

		auto		cc = file->footer->md->RowGroup(rg)->ColumnChunk(leaf);

		*codec = codec_id(cc->compression());
		if (*codec == VPQ_CODEC_OTHER)
			return true;

		std::vector<int8_t> &versions = file->footer->page_versions;
		size_t		at = (size_t) rg * file->footer->md->num_columns() + leaf;

		if (versions.empty())
			versions.assign((size_t) file->footer->md->num_row_groups() * file->footer->md->num_columns(), 0);
		if (versions[at] == 0)
		{
			versions[at] = read_page_version(file, cc->data_page_offset());
			/*
			 * PQ7: its byte in the facts this footer makes -- their header's
			 * bytes, the facts of no chunk's, then the chunk's record's second
			 */
			if (file->footer->facts_chunks_at < 0)
				file->footer->facts_chunks_at = (int64_t) build_facts(*file->footer, 0, nullptr).size();
			*facts_at = file->footer->facts_chunks_at + (int64_t) (CHUNK_BYTES * at) + 1;
		}
		*version = versions[at] > 0 ? versions[at] : 0;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

extern "C" bool
vpq_file_text_checked(vpq_file *file, int rg, int col)
{
	if (!file->facts || col < 0 || col >= (int) file->facts->cols.size())
		return false;

	FactsChunk	c;
	int			leaf = file->facts->cols[col].leaf;

	return leaf >= 0 && facts_chunk(*file->facts, rg, leaf, &c) && c.text_checked;
}

extern "C" int64_t
vpq_file_text_mark(vpq_file *file, int rg, int col, uint8_t *byte)
{
	SignalBlock signal_block;

	try
	{
		if (file->facts)
		{
			Facts	   &f = *file->facts;
			FactsChunk	c;

			if (col < 0 || col >= (int) f.cols.size() || f.cols[col].leaf < 0 ||
				!facts_chunk(f, rg, f.cols[col].leaf, &c))
				return -1;

			size_t		at = f.chunks_at + CHUNK_BYTES * ((size_t) rg * f.nleaves + f.cols[col].leaf);

			*byte = (uint8_t) (f.blob[at] | CHUNK_TEXT_CHECKED);
			return (int64_t) at;
		}
		if (!file->footer || col < 0 || col >= (int) file->footer->leaf_of.size())
			return -1;

		int			leaf = file->footer->leaf_of[col];
		auto	   &md = file->footer->md;

		if (leaf < 0 || rg < 0 || rg >= md->num_row_groups())
			return -1;
		/* as PQ7's: the chunk's record's first byte in the facts this footer makes */
		if (file->footer->facts_chunks_at < 0)
			file->footer->facts_chunks_at = (int64_t) build_facts(*file->footer, 0, nullptr).size();
		*byte = (uint8_t) (codec_id(md->RowGroup(rg)->ColumnChunk(leaf)->compression()) | CHUNK_TEXT_CHECKED);
		return file->footer->facts_chunks_at + (int64_t) (CHUNK_BYTES * ((size_t) rg * md->num_columns() + leaf));
	}
	catch (...)
	{
		return -1;
	}
}

//
// Reading
//

namespace
{

void
fill_array(vpq_chunk *chunk, const std::shared_ptr<arrow::ArrayData> &d, vpq_array *a)
{
	memset(a, 0, sizeof(*a));
	describe(d->type, &a->type);
	a->length = d->length;
	a->offset = d->offset;
	a->null_count = d->GetNullCount();
	if (a->type.id == VPQ_T_NULL)
	{
		a->null_count = d->length;
		return;
	}
	if (a->null_count > 0 && d->buffers.size() > 0 && d->buffers[0])
		a->validity = d->buffers[0]->data();

	switch (a->type.id)
	{
		case VPQ_T_STRING:
		case VPQ_T_BINARY:
		case VPQ_T_LARGE_STRING:
		case VPQ_T_LARGE_BINARY:
			a->values = d->buffers[1] ? d->buffers[1]->data() : nullptr;
			if (d->buffers.size() > 2 && d->buffers[2])
			{
				a->data = d->buffers[2]->data();
				a->data_size = d->buffers[2]->size();
			}
			break;
		case VPQ_T_STRING_VIEW:
		case VPQ_T_BINARY_VIEW:
			{
				auto	   &ptrs = chunk->view_buffers.emplace_back();
				auto	   &sizes = chunk->view_sizes.emplace_back();

				a->values = d->buffers[1] ? d->buffers[1]->data() : nullptr;
				for (size_t i = 2; i < d->buffers.size(); i++)
				{
					ptrs.push_back(d->buffers[i] ? d->buffers[i]->data() : nullptr);
					sizes.push_back(d->buffers[i] ? d->buffers[i]->size() : 0);
				}
				a->nbuffers = (int) ptrs.size();
				a->buffers = ptrs.data();
				a->buffer_sizes = sizes.data();
			}
			break;
		case VPQ_T_DICTIONARY:
			{
				a->values = d->buffers[1] ? d->buffers[1]->data() : nullptr;
				vpq_array  &dict = chunk->desc.emplace_back();

				fill_array(chunk, d->dictionary, &dict);
				a->dictionary = &dict;
			}
			break;
		case VPQ_T_LIST:
		case VPQ_T_LARGE_LIST:
			{
				/* PQ19: the offsets, which index the elements past the child's own offset */
				vpq_array  &elems = chunk->desc.emplace_back();

				a->values = d->buffers.size() > 1 && d->buffers[1] ? d->buffers[1]->data() : nullptr;
				fill_array(chunk, d->child_data[0], &elems);
				a->child = &elems;
			}
			break;
		case VPQ_T_MAP:
			{
				/* the entries' keys and values sliced as the entries are (StructArray::field()) */
				auto		map = std::static_pointer_cast<arrow::MapArray>(arrow::MakeArray(d));
				auto		entries = std::static_pointer_cast<arrow::StructArray>(map->values());
				vpq_array  &keys = chunk->desc.emplace_back();
				vpq_array  &items = chunk->desc.emplace_back();

				a->values = d->buffers.size() > 1 && d->buffers[1] ? d->buffers[1]->data() : nullptr;
				chunk->kept.push_back(entries->field(0));
				chunk->kept.push_back(entries->field(1));
				fill_array(chunk, entries->field(0)->data(), &keys);
				fill_array(chunk, entries->field(1)->data(), &items);
				a->child = &keys;
				a->child2 = &items;
			}
			break;
		default:
			if (d->buffers.size() > 1 && d->buffers[1])
				a->values = d->buffers[1]->data();
			break;
	}
}

}								// namespace

extern "C" int64_t
vpq_chunk_rows(vpq_chunk *chunk)
{
	return chunk->rows;
}

extern "C" const vpq_array *
vpq_chunk_array(vpq_chunk *chunk, int i)
{
	return chunk->columns[i];
}

extern "C" void
vpq_chunk_retain(vpq_chunk *chunk)
{
	chunk->refs++;
}

extern "C" void
vpq_chunk_release(vpq_chunk *chunk)
{
	if (chunk != nullptr && --chunk->refs == 0)
	{
		SignalBlock signal_block;

		delete chunk;
	}
}

//
// PQ11 (pg_vector_executor.md §3.37): a footer's index and a footer of some
// of its columns, as vexec_gpu's executor makes them for libcudf (W44, its
// footer.cpp, §3.35).  Each participant of a parallel scan parsed the file's
// whole footer as it read its first unit on the CPU -- ClickBench's 2.4 MB,
// 226 row groups by 105 columns, about 16 ms of each process, and 42% of
// its Q1's cycles at 100M rows.  The process that reads a footer walks it
// once by thrift's compact protocol, its bytes untouched: where each
// schema element, row group and column chunk lies.  That index is kept in
// shared memory beside the file's facts (footers.c), and a participant
// makes of it and of the footer's bytes, read from the file, a footer of
// the file's every row group and of the scan's columns alone, which Arrow's
// reader parses in a part of the time.  Left out: the key-value metadata,
// whose Arrow schema names every column -- so a reader's columns are
// checked to be of the types the file's own reader makes, and a file where
// one is not is read through its whole footer -- and each row group's
// sorting columns, which name the file's columns by their places.  A
// footer that does not walk, a schema not flat, an encrypted file: none.
//
namespace
{

// thrift's compact protocol: a field's or a list's element's type
enum
{
	T_STOP = 0, T_TRUE = 1, T_FALSE = 2, T_I8 = 3, T_I16 = 4, T_I32 = 5, T_I64 = 6, T_DOUBLE = 7,
	T_BINARY = 8, T_LIST = 9, T_SET = 10, T_MAP = 11, T_STRUCT = 12
};

struct Span
{
	uint32_t	b = 0;
	uint32_t	e = 0;
};

// a struct's field: its id, its type and its value's bytes, no header -- a boolean's value is its header's type
struct TField
{
	int			id = 0;
	int			type = 0;
	Span		v;
};

struct Bad : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

// a walk of bytes [p, end)
struct Walk
{
	const uint8_t *base;
	const uint8_t *p;
	const uint8_t *end;

	Walk(const uint8_t *b, Span s) : base(b), p(b + s.b), end(b + s.e) {}

	uint32_t at() const
	{
		return (uint32_t) (p - base);
	}

	uint8_t byte()
	{
		if (p >= end)
			throw Bad("past its end");
		return *p++;
	}

	uint64_t varint()
	{
		uint64_t	v = 0;

		for (int s = 0; s < 64; s += 7)
		{
			uint8_t		b = byte();

			v |= (uint64_t) (b & 0x7f) << s;
			if (!(b & 0x80))
				return v;
		}
		throw Bad("a varint too long");
	}

	int64_t zigzag()
	{
		uint64_t	v = varint();

		return (int64_t) (v >> 1) ^ -(int64_t) (v & 1);
	}

	void skip_bytes(uint64_t n)
	{
		if ((uint64_t) (end - p) < n)
			throw Bad("past its end");
		p += n;
	}

	// a field's header, its id by the struct's last: false at the struct's end
	bool field(int &last, TField &f)
	{
		uint8_t		b = byte();

		if (b == T_STOP)
			return false;
		f.type = b & 0x0f;

		int			delta = b >> 4;

		f.id = delta != 0 ? last + delta : (int) (int16_t) zigzag();
		last = f.id;
		return true;
	}

	void list(int &elem, uint64_t &n)
	{
		uint8_t		b = byte();

		elem = b & 0x0f;
		n = b >> 4;
		if (n == 15)
			n = varint();
	}

	void skip(int type, bool element, int depth = 0)
	{
		if (depth > 64)
			throw Bad("nested too deep");
		switch (type)
		{
			case T_TRUE:
			case T_FALSE:
				if (element)
					byte();
				return;
			case T_I8:
				byte();
				return;
			case T_I16:
			case T_I32:
			case T_I64:
				varint();
				return;
			case T_DOUBLE:
				skip_bytes(8);
				return;
			case T_BINARY:
				skip_bytes(varint());
				return;
			case T_LIST:
			case T_SET:
				{
					int			e;
					uint64_t	n;

					list(e, n);
					for (uint64_t i = 0; i < n; i++)
						skip(e, true, depth + 1);
					return;
				}
			case T_MAP:
				{
					uint64_t	n = varint();

					if (n == 0)
						return;

					uint8_t		kv = byte();

					for (uint64_t i = 0; i < n; i++)
					{
						skip(kv >> 4, true, depth + 1);
						skip(kv & 0x0f, true, depth + 1);
					}
					return;
				}
			case T_STRUCT:
				{
					int			last = 0;
					TField		f;

					while (field(last, f))
						skip(f.type, false, depth + 1);
					return;
				}
			default:
				throw Bad("a value of no type");
		}
	}

	// a struct's fields, their values' bytes
	std::vector<TField> fields()
	{
		std::vector<TField> out;
		int			last = 0;
		TField		f;

		while (field(last, f))
		{
			f.v.b = at();
			skip(f.type, false);
			f.v.e = at();
			out.push_back(f);
		}
		return out;
	}

	// a list's elements' bytes
	std::vector<Span> elements(int &elem)
	{
		uint64_t	n;
		std::vector<Span> out;

		list(elem, n);
		for (uint64_t i = 0; i < n; i++)
		{
			Span		s;

			s.b = at();
			skip(elem, true);
			s.e = at();
			out.push_back(s);
		}
		return out;
	}
};

struct Put
{
	std::string b;

	void byte(uint8_t x)
	{
		b.push_back((char) x);
	}

	void varint(uint64_t v)
	{
		for (; v >= 0x80; v >>= 7)
			b.push_back((char) (v | 0x80));
		b.push_back((char) v);
	}

	void zigzag(int64_t v)
	{
		varint(((uint64_t) v << 1) ^ (uint64_t) (v >> 63));
	}

	void raw(const uint8_t *base, Span s)
	{
		b.append((const char *) base + s.b, s.e - s.b);
	}

	void field(int type, int id, int &last)
	{
		int			delta = id - last;

		if (delta > 0 && delta <= 15)
			byte((uint8_t) (delta << 4 | type));
		else
		{
			byte((uint8_t) type);
			zigzag(id);
		}
		last = id;
	}

	void list(int elem, uint64_t n)
	{
		if (n < 15)
			byte((uint8_t) (n << 4 | elem));
		else
		{
			byte((uint8_t) (0xf0 | elem));
			varint(n);
		}
	}

	// a field as it was, its header written anew
	void copy(const uint8_t *base, const TField &f, int &last)
	{
		field(f.type, f.id, last);
		raw(base, f.v);
	}
};

// FileMetaData's, SchemaElement's and RowGroup's fields the projection rewrites or leaves out
const int	FMD_SCHEMA = 2,
			FMD_NUM_ROWS = 3,
			FMD_ROW_GROUPS = 4,
			FMD_KEY_VALUE = 5,
			FMD_COLUMN_ORDERS = 7,
			FMD_ENCRYPTION = 8,
			FMD_SIGNING_KEY = 9;
const int	SE_NAME = 4,
			SE_NUM_CHILDREN = 5;
const int	RG_COLUMNS = 1,
			RG_SORTING = 4;

// A footer's index: where its parts lie in its bytes, which lie at foot_start in the file
struct FooterIndex
{
	int64_t		foot_start = 0;
	int64_t		foot_len = 0;
	std::vector<TField> top;	// FileMetaData's fields
	std::vector<TField> root;	// the schema's root's fields
	int			schema_elem = T_STRUCT;
	std::vector<Span> leaves;	// the root's children, each a leaf
	struct RowGroup
	{
		std::vector<TField> fields;
		int			col_elem = T_STRUCT;
		std::vector<Span> cols;
	};
	std::vector<RowGroup> rgs;
	int			orders_elem = T_STRUCT;
	std::vector<Span> orders;
};

// The index of a footer's bytes [0, len); false where it does not walk, or is not one projected
bool
walk_footer(const uint8_t *B, uint32_t len, FooterIndex &x)
{
	try
	{
		Walk		in(B, Span{0, len});
		bool		schema = false;
		bool		row_groups = false;

		x.top = in.fields();
		for (const TField &f : x.top)
		{
			if (f.id == FMD_ENCRYPTION || f.id == FMD_SIGNING_KEY)
				return false;
			if (f.id == FMD_SCHEMA && f.type == T_LIST)
			{
				Walk		w(B, f.v);
				std::vector<Span> elems = w.elements(x.schema_elem);

				if (x.schema_elem != T_STRUCT || elems.empty())
					return false;
				for (size_t i = 0; i < elems.size(); i++)
				{
					Walk		e(B, elems[i]);
					std::vector<TField> ef = e.fields();
					int64_t		children = 0;

					for (const TField &g : ef)
						if (g.id == SE_NUM_CHILDREN && g.type == T_I32)
						{
							Walk		n(B, g.v);

							children = n.zigzag();
						}
					if (i == 0)
					{
						if ((uint64_t) children != elems.size() - 1)
							return false;	/* not a flat schema */
						x.root = std::move(ef);
						continue;
					}
					if (children != 0)
						return false;
					x.leaves.push_back(elems[i]);
				}
				schema = true;
			}
			else if (f.id == FMD_ROW_GROUPS && f.type == T_LIST)
			{
				Walk		w(B, f.v);
				int			elem;

				for (const Span &s : w.elements(elem))
				{
					FooterIndex::RowGroup rg;
					Walk		r(B, s);
					bool		cols = false;

					if (elem != T_STRUCT)
						return false;
					rg.fields = r.fields();
					for (const TField &g : rg.fields)
						if (g.id == RG_COLUMNS && g.type == T_LIST)
						{
							Walk		c(B, g.v);

							rg.cols = c.elements(rg.col_elem);
							cols = true;
						}
					if (!cols)
						return false;
					x.rgs.push_back(std::move(rg));
				}
				row_groups = true;
			}
			else if (f.id == FMD_COLUMN_ORDERS && f.type == T_LIST)
			{
				Walk		w(B, f.v);

				x.orders = w.elements(x.orders_elem);
			}
		}
		if (!schema || !row_groups)
			return false;
		for (const FooterIndex::RowGroup &rg : x.rgs)
			if (rg.cols.size() != x.leaves.size())
				return false;
		if (!x.orders.empty() && x.orders.size() != x.leaves.size())
			return false;
		return true;
	}
	catch (const Bad &)
	{
		return false;
	}
}

// A footer of the file's every row group and the leaves (ascending), from the footer's bytes B
bool
project_footer(const FooterIndex &x, const uint8_t *B, const std::vector<int> &leaves, std::string *out)
{
	Put			o;
	int			last = 0;

	for (int c : leaves)
		if (c < 0 || c >= (int) x.leaves.size())
			return false;
	for (const TField &f : x.top)
	{
		if (f.id == FMD_SCHEMA && f.type == T_LIST)
		{
			int			l = 0;

			o.field(T_LIST, FMD_SCHEMA, last);
			o.list(x.schema_elem, 1 + leaves.size());
			for (const TField &g : x.root)
			{
				if (g.id == SE_NUM_CHILDREN && g.type == T_I32)
				{
					o.field(T_I32, SE_NUM_CHILDREN, l);
					o.zigzag((int64_t) leaves.size());
				}
				else
					o.copy(B, g, l);
			}
			o.byte(T_STOP);
			for (int c : leaves)
				o.raw(B, x.leaves[c]);
		}
		else if (f.id == FMD_ROW_GROUPS && f.type == T_LIST)
		{
			o.field(T_LIST, FMD_ROW_GROUPS, last);
			o.list(T_STRUCT, x.rgs.size());
			for (const FooterIndex::RowGroup &rg : x.rgs)
			{
				int			l = 0;

				for (const TField &g : rg.fields)
				{
					if (g.id == RG_COLUMNS && g.type == T_LIST)
					{
						o.field(T_LIST, RG_COLUMNS, l);
						o.list(rg.col_elem, leaves.size());
						for (int c : leaves)
							o.raw(B, rg.cols[c]);
					}
					else if (g.id != RG_SORTING)
						o.copy(B, g, l);
				}
				o.byte(T_STOP);
			}
		}
		else if (f.id == FMD_COLUMN_ORDERS && f.type == T_LIST)
		{
			o.field(T_LIST, FMD_COLUMN_ORDERS, last);
			o.list(x.orders_elem, leaves.size());
			for (int c : leaves)
				o.raw(B, x.orders[c]);
		}
		else if (f.id != FMD_KEY_VALUE)
			o.copy(B, f, last);
	}
	o.byte(T_STOP);
	*out = std::move(o.b);
	return true;
}

//
// The index's bytes, the form kept in shared memory: uint32 magic "VPQX",
// uint16 form, uint16 pad, int64 the footer's place and bytes; uint32 a
// count and its fields -- int16 id, uint8 type, uint8 pad, uint32 begin and
// end -- for the footer's top, then the schema root's; uint8 the schema's
// and the column orders' element types, uint16 pad; uint32 a count and its
// spans -- uint32 begin and end -- for the leaves, then the column orders;
// uint32 the row groups, each: its fields as above, uint8 its columns'
// element type, 3 pad, and a span a leaf.  Every integer native.
//
const uint32_t INDEX_MAGIC = 0x58515056;	// "VPQX"
const uint16_t INDEX_FORM = 1;

void
put_fields(std::string *out, const std::vector<TField> &fs)
{
	put<uint32_t>(out, (uint32_t) fs.size());
	for (const TField &f : fs)
	{
		put<int16_t>(out, (int16_t) f.id);
		put<uint8_t>(out, (uint8_t) f.type);
		put<uint8_t>(out, 0);
		put<uint32_t>(out, f.v.b);
		put<uint32_t>(out, f.v.e);
	}
}

void
put_spans(std::string *out, const std::vector<Span> &ss)
{
	put<uint32_t>(out, (uint32_t) ss.size());
	for (const Span &s : ss)
	{
		put<uint32_t>(out, s.b);
		put<uint32_t>(out, s.e);
	}
}

std::string
footer_index_bytes(const FooterIndex &x)
{
	std::string out;

	put<uint32_t>(&out, INDEX_MAGIC);
	put<uint16_t>(&out, INDEX_FORM);
	put<uint16_t>(&out, 0);
	put<int64_t>(&out, x.foot_start);
	put<int64_t>(&out, x.foot_len);
	put_fields(&out, x.top);
	put_fields(&out, x.root);
	put<uint8_t>(&out, (uint8_t) x.schema_elem);
	put<uint8_t>(&out, (uint8_t) x.orders_elem);
	put<uint16_t>(&out, 0);
	put_spans(&out, x.leaves);
	put_spans(&out, x.orders);
	put<uint32_t>(&out, (uint32_t) x.rgs.size());
	for (const FooterIndex::RowGroup &rg : x.rgs)
	{
		put_fields(&out, rg.fields);
		put<uint8_t>(&out, (uint8_t) rg.col_elem);
		out.append(3, '\0');
		for (const Span &s : rg.cols)
		{
			put<uint32_t>(&out, s.b);
			put<uint32_t>(&out, s.e);
		}
	}
	return out;
}

bool
take_fields(FactsReader &r, std::vector<TField> *fs, int64_t len)
{
	uint32_t	n;

	if (!r.take(&n) || n > (1u << 20))
		return false;
	fs->resize(n);
	for (uint32_t i = 0; i < n; i++)
	{
		int16_t		id;
		uint8_t		type;
		uint8_t		pad;
		TField	   &f = (*fs)[i];

		if (!r.take(&id) || !r.take(&type) || !r.take(&pad) || !r.take(&f.v.b) || !r.take(&f.v.e) ||
			f.v.b > f.v.e || f.v.e > len)
			return false;
		f.id = id;
		f.type = type;
	}
	return true;
}

bool
take_spans(FactsReader &r, std::vector<Span> *ss, uint32_t n, int64_t len)
{
	if (n > (1u << 24))
		return false;
	ss->resize(n);
	for (uint32_t i = 0; i < n; i++)
	{
		Span	   &s = (*ss)[i];

		if (!r.take(&s.b) || !r.take(&s.e) || s.b > s.e || s.e > len)
			return false;
	}
	return true;
}

bool
parse_footer_index(const char *data, size_t len, FooterIndex *x)
{
	FactsReader r{data, data + len};
	uint32_t	magic;
	uint16_t	form;
	uint16_t	pad;
	uint8_t		se;
	uint8_t		oe;
	uint32_t	n;

	if (!r.take(&magic) || magic != INDEX_MAGIC || !r.take(&form) || form != INDEX_FORM || !r.take(&pad) ||
		!r.take(&x->foot_start) || !r.take(&x->foot_len) || x->foot_start < 4 || x->foot_len <= 0 ||
		x->foot_len > UINT32_MAX ||
		!take_fields(r, &x->top, x->foot_len) || !take_fields(r, &x->root, x->foot_len) ||
		!r.take(&se) || !r.take(&oe) || !r.take(&pad) ||
		!r.take(&n) || !take_spans(r, &x->leaves, n, x->foot_len) ||
		!r.take(&n) || !take_spans(r, &x->orders, n, x->foot_len) || !r.take(&n) || n > (1u << 24))
		return false;
	x->schema_elem = se;
	x->orders_elem = oe;
	x->rgs.resize(n);
	for (FooterIndex::RowGroup &rg : x->rgs)
	{
		uint8_t		ce;

		if (!take_fields(r, &rg.fields, x->foot_len) || !r.take(&ce) || !r.skip(3) ||
			!take_spans(r, &rg.cols, (uint32_t) x->leaves.size(), x->foot_len))
			return false;
		rg.col_elem = ce;
	}
	return r.p == r.end;
}

// A file column's leaf, of its footer or of its facts: -1 a nested one
int
leaf_of_column(vpq_file *file, int col)
{
	if (file->footer)
		return col >= 0 && col < (int) file->footer->leaf_of.size() ? file->footer->leaf_of[col] : -1;
	return col >= 0 && col < (int) file->facts->cols.size() ? file->facts->cols[col].leaf : -1;
}

// A file column's place among a reader's columns, -1 none
int
reader_column(vpq_reader *reader, int col)
{
	if (reader->projected)
		return col >= 0 && col < (int) reader->col_of.size() ? reader->col_of[col] : -1;
	return leaf_of_column(reader->file, col);
}

}								// namespace

extern "C" int
vpq_file_index(vpq_file *file, char **index, size_t *len)
{
	ENGINE_TRY
	{
		fs::FileInfo info(file->loc.path, fs::FileType::File);
		uint8_t		tail[8];
		uint32_t	foot_len;
		FooterIndex x;

		*index = nullptr;
		*len = 0;
		if (file->size < 12)
			return VPQ_NOT_FOUND;
		info.set_size(file->size);
		ASSIGN_OR_FAIL(std::shared_ptr<arrow::io::RandomAccessFile> input, open_input(file->loc.fs, info));
		ASSIGN_OR_FAIL(int64_t got, input->ReadAt(file->size - 8, 8, tail));
		if (got != 8 || memcmp(tail + 4, "PAR1", 4) != 0)
			return VPQ_NOT_FOUND;
		memcpy(&foot_len, tail, 4);
		if ((int64_t) foot_len + 8 > file->size - 4)
			return VPQ_NOT_FOUND;
		ASSIGN_OR_FAIL(std::shared_ptr<arrow::Buffer> foot, input->ReadAt(file->size - 8 - foot_len, foot_len));
		if (foot->size() != foot_len || !walk_footer(foot->data(), foot_len, x))
			return VPQ_NOT_FOUND;
		x.foot_start = file->size - 8 - foot_len;
		x.foot_len = foot_len;

		std::string bytes = footer_index_bytes(x);

		*index = (char *) malloc(bytes.size());
		if (*index == nullptr)
			return fail("out of memory for a footer's index");
		memcpy(*index, bytes.data(), bytes.size());
		*len = bytes.size();
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" int
vpq_reader_open(vpq_file *file, const bool *dictionaries, vpq_reader **reader)
{
	ENGINE_TRY
	{
		auto		r = std::make_unique<vpq_reader>();
		fs::FileInfo info(file->loc.path, fs::FileType::File);

		*reader = nullptr;
		/* a handle of facts reads its footer now, for Arrow's reader */
		if (!file->footer)
			CHECK_OK(read_footer(file));
		info.set_size(file->size);
		ASSIGN_OR_FAIL(std::shared_ptr<arrow::io::RandomAccessFile> input, open_input(file->loc.fs, info));

		parquet::ReaderProperties props(pool());
		parquet::ArrowReaderProperties ap = reader_properties();

		ap.set_pre_buffer(file->loc.s3 && config.prefetch);
		ap.set_io_context(arrow::io::IOContext(pool()));
		if (dictionaries != nullptr)
			for (size_t c = 0; c < file->footer->leaf_of.size(); c++)
				if (dictionaries[c] && file->footer->leaf_of[c] >= 0)
					ap.set_read_dictionary(file->footer->leaf_of[c], true);

		auto		pq = parquet::ParquetFileReader::Open(input, props, file->footer->md);

		ASSIGN_OR_FAIL(r->reader, parquet::arrow::FileReader::Make(pool(), std::move(pq), ap));
		r->file = file;
		r->input = input;
		r->md = file->footer->md;
		r->schema = file->footer->schema;
		*reader = r.release();
		return VPQ_OK;
	}
	ENGINE_CATCH
}

namespace
{

// PQ11: the reader of a projected footer, or VPQ_NOT_FOUND where its types are not the file's
int
open_projected(vpq_file *file, const bool *dictionaries, const std::vector<int> &cols, const FooterIndex &x,
			   vpq_reader **reader)
{
	auto		r = std::make_unique<vpq_reader>();
	fs::FileInfo info(file->loc.path, fs::FileType::File);
	std::vector<int> leaves;
	int			ncols = vpq_file_columns(file);

	info.set_size(file->size);
	ASSIGN_OR_FAIL(std::shared_ptr<arrow::io::RandomAccessFile> input, open_input(file->loc.fs, info));

	/* the file's leaves of the columns, in the file's order */
	r->col_of.assign(ncols, -1);
	for (int c : cols)
	{
		const char *name;
		int			field_id;
		vpq_type	t;
		bool		nullable;

		if (c < 0 || c >= ncols || !vpq_file_column(file, c, &name, &field_id, &t, &nullable))
			return VPQ_NOT_FOUND;
		if (leaf_of_column(file, c) < 0)
			return VPQ_NOT_FOUND;
		leaves.push_back(leaf_of_column(file, c));
	}
	std::sort(leaves.begin(), leaves.end());
	leaves.erase(std::unique(leaves.begin(), leaves.end()), leaves.end());

	/* the footer's bytes, and a footer of the file's row groups and those leaves made of them */
	ASSIGN_OR_FAIL(std::shared_ptr<arrow::Buffer> foot, input->ReadAt(x.foot_start, x.foot_len));
	if (foot->size() != x.foot_len)
		return VPQ_NOT_FOUND;

	std::string proj;

	if (!project_footer(x, foot->data(), leaves, &proj))
		return VPQ_NOT_FOUND;

	std::shared_ptr<parquet::FileMetaData> md;

	try
	{
		md = parquet::FileMetaData::Make(proj.data(), (int64_t) proj.size(), parquet::ReaderProperties(pool()));
	}
	catch (const std::exception &)
	{
		return VPQ_NOT_FOUND;
	}
	if (md->num_columns() != (int) leaves.size() || md->num_row_groups() != vpq_file_row_groups(file))
		return VPQ_NOT_FOUND;

	parquet::ReaderProperties props(pool());
	parquet::ArrowReaderProperties ap = reader_properties();

	ap.set_pre_buffer(file->loc.s3 && config.prefetch);
	ap.set_io_context(arrow::io::IOContext(pool()));
	for (int c : cols)
	{
		int			j = (int) (std::lower_bound(leaves.begin(), leaves.end(), leaf_of_column(file, c)) - leaves.begin());

		r->col_of[c] = j;
		if (dictionaries != nullptr && dictionaries[c])
			ap.set_read_dictionary(j, true);
	}

	auto		pq = parquet::ParquetFileReader::Open(input, props, md);

	ASSIGN_OR_FAIL(r->reader, parquet::arrow::FileReader::Make(pool(), std::move(pq), ap));
	CHECK_OK(r->reader->GetSchema(&r->schema));
	if (r->schema->num_fields() != (int) leaves.size())
		return VPQ_NOT_FOUND;

	/* each column as the file's own reader makes it, else the footer whole: its Arrow schema is not here */
	for (int c : cols)
	{
		const char *name;
		int			field_id;
		vpq_type	want;
		vpq_type	got;
		bool		nullable;
		int			j = r->col_of[c];
		const auto *node = md->schema()->Column(j)->schema_node().get();

		(void) vpq_file_column(file, c, &name, &field_id, &want, &nullable);
		describe(r->schema->field(j)->type(), &got);
		if (node->logical_type() && node->logical_type()->is_UUID())
			got.uuid = true;
		if (got.id != want.id || got.unit != want.unit || got.has_tz != want.has_tz ||
			got.precision != want.precision || got.scale != want.scale || got.byte_width != want.byte_width ||
			got.value_id != want.value_id || got.index_width != want.index_width || got.uuid != want.uuid ||
			r->schema->field(j)->name() != name)
		{
			counters.projections_refused++;
			return VPQ_NOT_FOUND;
		}
	}
	r->file = file;
	r->input = input;
	r->md = md;
	r->projected = true;
	counters.footers_projected++;
	*reader = r.release();
	return VPQ_OK;
}

}								// namespace

extern "C" int
vpq_reader_open_cols(vpq_file *file, const bool *dictionaries, const int *cols, int ncols, const char *index,
					 size_t index_len, vpq_reader **reader)
{
	ENGINE_TRY
	{
		FooterIndex x;

		*reader = nullptr;
		if (index != nullptr && !file->footer && parse_footer_index(index, index_len, &x) &&
			x.foot_start + x.foot_len + 8 == file->size &&
			open_projected(file, dictionaries, std::vector<int>(cols, cols + ncols), x, reader) == VPQ_OK)
			return VPQ_OK;
	}
	ENGINE_CATCH
	/* the footer whole: no index, a file not projected, a column not of its file's type */
	return vpq_reader_open(file, dictionaries, reader);
}

extern "C" void
vpq_reader_close(vpq_reader *reader)
{
	SignalBlock signal_block;

	delete reader;
}

extern "C" int
vpq_reader_read(vpq_reader *reader, int rg, const int *cols, int ncols, vpq_chunk **chunk)
{
	ENGINE_TRY
	{
		std::vector<int> leaves;
		auto		c = std::make_unique<vpq_chunk>();

		*chunk = nullptr;
		for (int i = 0; i < ncols; i++)
		{
			int			leaf = reader_column(reader, cols[i]);
			const Footer *f = reader->file->footer.get();

			/* PQ19: a nested column of flat children, every leaf of it: Arrow's reader makes the field of them */
			if (leaf < 0 && !reader->projected && f != nullptr && cols[i] >= 0 &&
				cols[i] < (int) f->children.size() && !f->children[cols[i]].empty())
			{
				for (int k = 0; k < f->leaf_count[cols[i]]; k++)
					leaves.push_back(f->leaf_first[cols[i]] + k);
				continue;
			}
			if (leaf < 0)
				return fail("column " + std::to_string(cols[i]) + " of the file cannot be read");
			leaves.push_back(leaf);
		}
		c->rows = reader->md->RowGroup(rg)->num_rows();
		if (ncols > 0)
		{
			ASSIGN_OR_FAIL(std::shared_ptr<arrow::Table> table, reader->reader->ReadRowGroup(rg, leaves));

			if (table->num_columns() != ncols)
				return fail("the reader made " + std::to_string(table->num_columns()) +
							" columns of " + std::to_string(ncols));
			c->rows = table->num_rows();
			for (int i = 0; i < ncols; i++)
			{
				const auto &ca = table->column(i);
				std::shared_ptr<arrow::Array> a;

				if (ca->num_chunks() == 1)
					a = ca->chunk(0);
				else if (ca->num_chunks() == 0)
				{
					ASSIGN_OR_FAIL(a, arrow::MakeEmptyArray(ca->type(), pool()));
				}
				else
				{
					ASSIGN_OR_FAIL(a, arrow::Concatenate(ca->chunks(), pool()));
				}
				if (a->length() != c->rows)
					return fail("a column of the row group has " + std::to_string(a->length()) +
								" values where it has " + std::to_string(c->rows) + " rows");
				c->arrays.push_back(a);
			}
			for (int i = 0; i < ncols; i++)
			{
				vpq_array  &d = c->desc.emplace_back();

				fill_array(c.get(), c->arrays[i]->data(), &d);
				c->columns.push_back(&d);
			}
		}
		*chunk = c.release();
		return VPQ_OK;
	}
	ENGINE_CATCH
}

//
// PQ12 and PQ13 (pg_vector_executor.md §3.37): a row group's columns read
// forward in ranges of rows -- a later step's batches that have rows left
// (PQ13), a unit of a range of the row group's rows (PQ12) -- each column
// by one page reader of its chunk's bytes, read whole as Arrow's reader
// reads them: its dictionary decoded once, a data page wholly before a
// range passed over undecompressed (W55's filter of the pages' headers),
// the rows of a page before it skipped, and the range's rows read as arrays
// of their own.  The pages' rows are walked from their headers as the
// column is opened.  A column of no such read -- repeated, read as a
// dictionary, INT96, encrypted, of a type page_array() does not make, its
// chunk's headers not walked -- is VPQ_NOT_FOUND: the caller reads the
// column chunk whole.
//
struct vpq_range
{
	struct Col
	{
		const parquet::ColumnDescriptor *descr = nullptr;
		std::shared_ptr<arrow::DataType> target;	// the type the whole read makes
		parquet::ReaderProperties props{pool()};
		std::shared_ptr<arrow::Buffer> bytes;	// the column chunk's
		std::vector<int64_t> first;	// each data page's first row, then the chunk's rows
		std::shared_ptr<parquet::internal::RecordReader> rr;
		int64_t		pos = 0;		// the row of the reader's next record
		int64_t		cur = -1;		// the data page it holds, -1 none
		int64_t		skip_below = 0; // data pages below it passed over as the reader meets them
		int64_t		seen = 0;		// data pages the reader has met
	};
	std::vector<std::unique_ptr<Col>> cols;
	int64_t		rows = 0;
};

namespace
{

std::shared_ptr<arrow::ArrayData> page_array(parquet::internal::RecordReader &rr, const parquet::ColumnDescriptor *descr,
											 const std::shared_ptr<arrow::DataType> &target, int64_t n);

// A column chunk's data pages: each one's first row, then the chunk's rows; false where its headers do not walk
bool
data_pages(const uint8_t *B, uint32_t len, std::vector<int64_t> *first)
{
	try
	{
		uint32_t	at = 0;
		int64_t		rows = 0;

		while (at < len)
		{
			Walk		w(B, Span{at, len});
			int64_t		type = -1;
			int64_t		compressed = -1;
			int64_t		nvalues = -1;
			int64_t		nrows = -1;

			for (const TField &f : w.fields())
			{
				if (f.id == 1 && f.type == T_I32)
					type = Walk(B, f.v).zigzag();
				else if (f.id == 3 && f.type == T_I32)
					compressed = Walk(B, f.v).zigzag();
				else if ((f.id == 5 || f.id == 8) && f.type == T_STRUCT)
				{
					/* DataPageHeader's num_values, DataPageHeaderV2's num_rows */
					Walk		h(B, f.v);

					for (const TField &g : h.fields())
						if (g.type == T_I32 && g.id == (f.id == 5 ? 1 : 3))
							(f.id == 5 ? nvalues : nrows) = Walk(B, g.v).zigzag();
				}
			}
			if (compressed < 0 || (uint64_t) w.at() + (uint64_t) compressed > len)
				return false;
			if (type == 0 || type == 3)
			{
				int64_t		n = type == 0 ? nvalues : nrows;

				if (n < 0)
					return false;
				first->push_back(rows);
				rows += n;
			}
			at = w.at() + (uint32_t) compressed;
		}
		first->push_back(rows);
		return true;
	}
	catch (const Bad &)
	{
		return false;
	}
}

void
skip_rows(vpq_range::Col &k, int64_t m)
{
	if (m > 0 && k.rr->SkipRecords(m) != m)
		throw parquet::ParquetException("a column chunk of fewer rows than its pages' headers say");
	k.pos += m;
}

}								// namespace

extern "C" int
vpq_range_open(vpq_reader *reader, int rg, const int *cols, int ncols, vpq_range **out)
{
	ENGINE_TRY
	{
		auto		r = std::make_unique<vpq_range>();
		const parquet::FileMetaData &md = *reader->md;
		auto		rgmd = md.RowGroup(rg);
		const auto &version = md.writer_version();
		bool		always_compressed =
			version.VersionLt(parquet::ApplicationVersion::PARQUET_CPP_10353_FIXED_VERSION());

		*out = nullptr;
		/* parquet-mr's chunks before PARQUET-816 counted no dictionary page header in their bytes */
		if (version.VersionLt(parquet::ApplicationVersion::PARQUET_816_FIXED_VERSION()))
			return VPQ_NOT_FOUND;
		r->rows = rgmd->num_rows();
		for (int i = 0; i < ncols; i++)
		{
			int			leaf = reader_column(reader, cols[i]);
			int			field = reader->projected ? leaf : cols[i];

			if (leaf < 0 || field < 0 || field >= reader->schema->num_fields())
				return VPQ_NOT_FOUND;

			const parquet::ColumnDescriptor *descr = md.schema()->Column(leaf);
			auto		cc = rgmd->ColumnChunk(leaf);
			auto		c = std::make_unique<vpq_range::Col>();
			int64_t		start = cc->data_page_offset();
			int64_t		len = cc->total_compressed_size();

			c->descr = descr;
			c->target = reader->schema->field(field)->type();
			if (descr->max_repetition_level() > 0 || c->target->id() == arrow::Type::DICTIONARY ||
				descr->physical_type() == parquet::Type::INT96 || cc->crypto_metadata() != nullptr)
				return VPQ_NOT_FOUND;
			if (cc->has_dictionary_page() && cc->dictionary_page_offset() > 0 && start > cc->dictionary_page_offset())
				start = cc->dictionary_page_offset();
			if (start < 0 || len <= 0 || len > (int64_t) UINT32_MAX || start + len > reader->file->size)
				return VPQ_NOT_FOUND;
			ASSIGN_OR_FAIL(c->bytes, reader->input->ReadAt(start, len));
			if (c->bytes->size() != len || !data_pages(c->bytes->data(), (uint32_t) len, &c->first) ||
				c->first.size() < 2 || c->first.back() != r->rows)
				return VPQ_NOT_FOUND;

			auto		pager = parquet::PageReader::Open(std::make_shared<arrow::io::BufferReader>(c->bytes), r->rows,
														  cc->compression(), c->props, *descr, always_compressed);
			vpq_range::Col *k = c.get();
			std::shared_ptr<arrow::DataType> as = nullptr;

			pager->set_data_page_filter([k](const parquet::DataPageStats &) {
				int64_t		p = k->seen++;

				if (p < k->skip_below)
				{
					counters.pages_passed++;
					return true;
				}
				k->cur = p;
				counters.pages_decoded++;
				return false;
			});
			/* a byte array read as the string or binary type the whole read makes */
			if (descr->physical_type() == parquet::Type::BYTE_ARRAY)
				as = c->target->id() == arrow::Type::EXTENSION ?
					static_cast<const arrow::ExtensionType &>(*c->target).storage_type() : c->target;
			c->rr = parquet::internal::RecordReader::Make(descr, parquet::internal::LevelInfo::ComputeLevelInfo(descr),
														  pool(), false, false, as);
			c->rr->SetPageReader(std::move(pager));
			r->cols.push_back(std::move(c));
		}
		*out = r.release();
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" int
vpq_range_read(vpq_range *range, int64_t lo, int64_t n, vpq_chunk **chunk)
{
	ENGINE_TRY
	{
		auto		c = std::make_unique<vpq_chunk>();

		*chunk = nullptr;
		if (lo < 0 || n < 0 || lo + n > range->rows)
			return fail("rows [" + std::to_string(lo) + ", " + std::to_string(lo + n) + ") of a row group of " +
						std::to_string(range->rows));
		c->rows = n;
		for (auto &kp : range->cols)
		{
			vpq_range::Col &k = *kp;

			if (lo < k.pos)
				return fail("a range of a row group read back from row " + std::to_string(k.pos));
			if (lo > k.pos)
			{
				if (k.cur >= 0 && lo < k.first[k.cur + 1])
					skip_rows(k, lo - k.pos);	/* within the page the reader holds */
				else
				{
					size_t		p;

					/* the held page's rest, then the pages before lo's passed over as the reader meets them */
					if (k.cur >= 0 && k.pos < k.first[k.cur + 1])
						skip_rows(k, k.first[k.cur + 1] - k.pos);
					p = std::upper_bound(k.first.begin(), k.first.end() - 1, lo) - k.first.begin() - 1;
					k.skip_below = (int64_t) p;
					k.pos = k.first[p];
					skip_rows(k, lo - k.pos);
				}
			}
			k.rr->Reset();

			int64_t		got = 0;

			while (got < n)
			{
				int64_t		m = k.rr->ReadRecords(n - got);

				if (m <= 0)
					break;
				got += m;
			}
			if (got != n)
				return fail("a column chunk of fewer rows than its pages' headers say");
			k.pos += n;

			std::shared_ptr<arrow::ArrayData> d = page_array(*k.rr, k.descr, k.target, n);

			if (d == nullptr)
				return VPQ_NOT_FOUND;
			c->arrays.push_back(arrow::MakeArray(d));
		}
		for (size_t i = 0; i < c->arrays.size(); i++)
		{
			vpq_array  &d = c->desc.emplace_back();

			fill_array(c.get(), c->arrays[i]->data(), &d);
			c->columns.push_back(&d);
		}
		*chunk = c.release();
		return VPQ_OK;
	}
	ENGINE_CATCH
}

extern "C" void
vpq_range_close(vpq_range *range)
{
	SignalBlock signal_block;

	delete range;
}

//
// Point reads (pg_vector_executor.md §3.34; ch_table_engines.md's VC34): of a
// column chunk, the data page that holds a row, read and decoded alone
//

namespace
{

/* Whether a timestamp's or a time's logical type is of the Arrow unit */
bool
same_unit(const std::shared_ptr<const parquet::LogicalType> &lt, arrow::TimeUnit::type unit)
{
	parquet::LogicalType::TimeUnit::unit u;

	if (lt == nullptr)
		return false;
	if (lt->is_timestamp())
		u = static_cast<const parquet::TimestampLogicalType &>(*lt).time_unit();
	else if (lt->is_time())
		u = static_cast<const parquet::TimeLogicalType &>(*lt).time_unit();
	else
		return false;
	return (u == parquet::LogicalType::TimeUnit::MILLIS && unit == arrow::TimeUnit::MILLI) ||
		(u == parquet::LogicalType::TimeUnit::MICROS && unit == arrow::TimeUnit::MICRO) ||
		(u == parquet::LogicalType::TimeUnit::NANOS && unit == arrow::TimeUnit::NANO);
}

/* Whether two of Arrow's byte types share a layout: a string's and a binary's of one offset width */
bool
same_bytes_layout(const arrow::DataType &a, const arrow::DataType &b)
{
	auto		narrow = [](arrow::Type::type t) { return t == arrow::Type::STRING || t == arrow::Type::BINARY; };
	auto		wide = [](arrow::Type::type t) { return t == arrow::Type::LARGE_STRING || t == arrow::Type::LARGE_BINARY; };

	return a.Equals(b) || (narrow(a.id()) && narrow(b.id())) || (wide(a.id()) && wide(b.id()));
}

/* A buffer of n values of width w, from a record reader's int32s or int64s, each made Arrow's */
template <typename From, typename F>
std::shared_ptr<arrow::Buffer>
made_values(const std::shared_ptr<arrow::Buffer> &values, int64_t n, int w, F make)
{
	auto		r = arrow::AllocateBuffer(std::max<int64_t>(n * w, 1), pool());

	if (!r.ok())
		return nullptr;

	std::shared_ptr<arrow::Buffer> out = std::move(*r);
	const From *v = reinterpret_cast<const From *>(values->data());
	uint8_t    *o = out->mutable_data();

	for (int64_t i = 0; i < n; i++)
		make(v[i], o + i * w);
	return out;
}

/* An int128's sixteen bytes of an integer, little-endian */
void
int128_of(int64_t v, uint8_t *o)
{
	uint64_t	lo = static_cast<uint64_t>(v);
	uint64_t	hi = v < 0 ? ~UINT64_C(0) : 0;

	memcpy(o, &lo, 8);
	memcpy(o + 8, &hi, 8);
}

/*
 * The values a record reader read of a page -- n rows of a flat column -- as
 * the Arrow type the whole read makes of the column, sharing its layout or
 * made so as Arrow's reader makes it: nullptr where the type is not one
 * made here, which the whole read then reads
 */
std::shared_ptr<arrow::ArrayData>
page_array(parquet::internal::RecordReader &rr, const parquet::ColumnDescriptor *descr,
		   const std::shared_ptr<arrow::DataType> &target, int64_t n)
{
	auto		physical = descr->physical_type();
	const auto &lt = descr->logical_type();
	arrow::Type::type id = target->id();
	const arrow::DataType &storage = id == arrow::Type::EXTENSION ?
		*static_cast<const arrow::ExtensionType &>(*target).storage_type() : *target;

	if (physical == parquet::Type::BYTE_ARRAY || physical == parquet::Type::FIXED_LEN_BYTE_ARRAY)
	{
		auto	   *br = dynamic_cast<parquet::internal::BinaryRecordReader *>(&rr);

		if (br == nullptr)
			return nullptr;

		auto		chunks = br->GetBuilderChunks();
		std::shared_ptr<arrow::Array> a;

		if (chunks.size() == 1)
			a = chunks[0];
		else if (chunks.size() > 1)
		{
			auto		r = arrow::Concatenate(chunks, pool());

			if (!r.ok())
				return nullptr;
			a = *r;
		}
		if (a == nullptr || a->length() != n)
			return nullptr;

		std::shared_ptr<arrow::ArrayData> d = a->data()->Copy();

		/* a decimal of fixed-width big-endian bytes, made Decimal128's little-endian sixteen */
		if (id == arrow::Type::DECIMAL128 && physical == parquet::Type::FIXED_LEN_BYTE_ARRAY &&
			d->type->id() == arrow::Type::FIXED_SIZE_BINARY)
		{
			int			w = descr->type_length();
			auto		r = arrow::AllocateBuffer(std::max<int64_t>(n * 16, 1), pool());

			if (w < 1 || w > 16 || !r.ok() || d->buffers.size() < 2 || d->buffers[1] == nullptr)
				return nullptr;

			std::shared_ptr<arrow::Buffer> out = std::move(*r);
			const uint8_t *in = d->buffers[1]->data() + d->offset * w;

			for (int64_t i = 0; i < n; i++)
			{
				const uint8_t *b = in + i * w;
				uint8_t		fill = (b[0] & 0x80) ? 0xff : 0x00;

				for (int k = 0; k < 16; k++)
					out->mutable_data()[i * 16 + k] = k < w ? b[w - 1 - k] : fill;
			}
			return arrow::ArrayData::Make(target, n, {d->offset == 0 ? d->buffers[0] : nullptr, out},
										  d->offset == 0 ? d->GetNullCount() : 0);
		}
		if (d->offset != 0 && d->GetNullCount() > 0)
			return nullptr;
		if (same_bytes_layout(storage, *d->type) || storage.Equals(*d->type))
		{
			d->type = target;
			return d;
		}
		return nullptr;
	}

	if (rr.values_written() != n)
		return nullptr;

	int64_t		nulls = rr.null_count();
	std::shared_ptr<arrow::Buffer> validity;
	std::shared_ptr<arrow::Buffer> values = rr.ReleaseValues();

	if (nulls > 0)
		validity = rr.ReleaseIsValid();
	if (values == nullptr)
		return nullptr;
	switch (physical)
	{
		case parquet::Type::BOOLEAN:
			if (id != arrow::Type::BOOL)
				return nullptr;
			{
				auto		r = arrow::AllocateEmptyBitmap(n, pool());

				if (!r.ok())
					return nullptr;

				std::shared_ptr<arrow::Buffer> bits = *r;
				const uint8_t *v = values->data();

				for (int64_t i = 0; i < n; i++)
					if (v[i])
						arrow::bit_util::SetBit(bits->mutable_data(), i);
				return arrow::ArrayData::Make(target, n, {validity, bits}, nulls);
			}
		case parquet::Type::INT32:
			if (id == arrow::Type::INT32 || id == arrow::Type::UINT32 || id == arrow::Type::DATE32 ||
				(id == arrow::Type::TIME32 && same_unit(lt, static_cast<const arrow::Time32Type &>(*target).unit())))
				return arrow::ArrayData::Make(target, n, {validity, values}, nulls);
			if (id == arrow::Type::INT8 || id == arrow::Type::UINT8)
				values = made_values<int32_t>(values, n, 1, [](int32_t v, uint8_t *o) { *o = static_cast<uint8_t>(v); });
			else if (id == arrow::Type::INT16 || id == arrow::Type::UINT16)
				values = made_values<int32_t>(values, n, 2, [](int32_t v, uint8_t *o) {
					uint16_t	u = static_cast<uint16_t>(v);

					memcpy(o, &u, 2);
				});
			else if (id == arrow::Type::DECIMAL128)
				values = made_values<int32_t>(values, n, 16, [](int32_t v, uint8_t *o) { int128_of(v, o); });
			else
				return nullptr;
			return values ? arrow::ArrayData::Make(target, n, {validity, values}, nulls) : nullptr;
		case parquet::Type::INT64:
			if (id == arrow::Type::INT64 || id == arrow::Type::UINT64 ||
				(id == arrow::Type::TIMESTAMP &&
				 same_unit(lt, static_cast<const arrow::TimestampType &>(*target).unit())) ||
				(id == arrow::Type::TIME64 && same_unit(lt, static_cast<const arrow::Time64Type &>(*target).unit())))
				return arrow::ArrayData::Make(target, n, {validity, values}, nulls);
			if (id == arrow::Type::DECIMAL128)
			{
				values = made_values<int64_t>(values, n, 16, [](int64_t v, uint8_t *o) { int128_of(v, o); });
				return values ? arrow::ArrayData::Make(target, n, {validity, values}, nulls) : nullptr;
			}
			return nullptr;
		case parquet::Type::FLOAT:
			return id == arrow::Type::FLOAT ? arrow::ArrayData::Make(target, n, {validity, values}, nulls) : nullptr;
		case parquet::Type::DOUBLE:
			return id == arrow::Type::DOUBLE ? arrow::ArrayData::Make(target, n, {validity, values}, nulls) : nullptr;
		default:
			return nullptr;		/* INT96: the whole read's coercion */
	}
}

}								// namespace

extern "C" int
vpq_reader_read_page(vpq_reader *reader, int rg, int col, int64_t row, vpq_chunk **chunk,
					 int64_t *first, int64_t *nrows, bool *indexed)
{
	ENGINE_TRY
	{
		/* a fetch's reader: its file's footer whole (PQ11's readers are a scan's) */
		if (reader->projected || !reader->file->footer)
			return VPQ_NOT_FOUND;

		const Footer &f = *reader->file->footer;

		*chunk = nullptr;
		*first = *nrows = 0;
		*indexed = false;
		if (col < 0 || col >= (int) f.leaf_of.size() || f.leaf_of[col] < 0 || reader->input == nullptr ||
			rg < 0 || rg >= f.md->num_row_groups())
			return VPQ_NOT_FOUND;

		int			leaf = f.leaf_of[col];
		const parquet::ColumnDescriptor *descr = f.md->schema()->Column(leaf);
		const std::shared_ptr<arrow::DataType> &target = f.schema->field(col)->type();
		auto		rgmd = f.md->RowGroup(rg);
		auto		cc = rgmd->ColumnChunk(leaf);
		int64_t		rg_rows = rgmd->num_rows();
		int64_t		page_first = -1;
		int64_t		page_rows = 0;
		parquet::ParquetFileReader *pq = reader->reader->parquet_reader();
		std::shared_ptr<parquet::RowGroupReader> rgr;
		parquet::ReaderProperties props(pool());
		std::unique_ptr<parquet::PageReader> pager;
		std::shared_ptr<parquet::OffsetIndex> oi;

		if (row < 0 || row >= rg_rows || descr->max_repetition_level() > 0 ||
			target->id() == arrow::Type::DICTIONARY || descr->physical_type() == parquet::Type::INT96)
			return VPQ_NOT_FOUND;

		/* the page by the file's offset index: its bytes and its dictionary page's alone */
		if (auto pir = pq->GetPageIndexReader())
			if (auto rgi = pir->RowGroup(rg))
				oi = rgi->GetOffsetIndex(leaf);
		if (oi != nullptr && !oi->page_locations().empty())
		{
			const auto &locs = oi->page_locations();
			size_t		p = 0;
			std::vector<std::shared_ptr<arrow::Buffer>> parts;

			while (p + 1 < locs.size() && locs[p + 1].first_row_index <= row)
				p++;
			page_first = locs[p].first_row_index;
			page_rows = (p + 1 < locs.size() ? locs[p + 1].first_row_index : rg_rows) - page_first;
			if (page_first > row || page_rows <= 0 || locs[p].compressed_page_size <= 0)
				return VPQ_NOT_FOUND;
			if (cc->has_dictionary_page())
			{
				int64_t		doff = cc->dictionary_page_offset();

				if (doff <= 0 || doff >= locs[0].offset)
					return VPQ_NOT_FOUND;
				ASSIGN_OR_FAIL(std::shared_ptr<arrow::Buffer> dict, reader->input->ReadAt(doff, locs[0].offset - doff));
				if (dict->size() != locs[0].offset - doff)
					return fail("a dictionary page read short");
				parts.push_back(dict);
			}
			ASSIGN_OR_FAIL(std::shared_ptr<arrow::Buffer> page,
						   reader->input->ReadAt(locs[p].offset, locs[p].compressed_page_size));
			if (page->size() != locs[p].compressed_page_size)
				return fail("a data page read short");
			parts.push_back(page);
			ASSIGN_OR_FAIL(std::shared_ptr<arrow::Buffer> bytes, arrow::ConcatenateBuffers(parts, pool()));

			pager = parquet::PageReader::Open(std::make_shared<arrow::io::BufferReader>(bytes), page_rows,
											  cc->compression(), props, *descr);
			*indexed = true;
		}
		else
		{
			/*
			 * else the pages' headers walked to the row's, the others' bytes
			 * passed over undecompressed -- W55 (pg_vector_executor.md
			 * §3.36), a local file's through a buffered stream, its column
			 * chunk not read whole for them: ClickBench's own file has no
			 * offset index, and a row's 105 columns read their row group's
			 * 65 MB
			 */
			int64_t		at = 0;
			parquet::ParquetFileReader *walker = pq;

			if (!reader->file->loc.s3)
			{
				std::call_once(reader->walk_once, [&]() {
					parquet::ReaderProperties bp(pool());

					bp.enable_buffered_stream();
					reader->walk = parquet::ParquetFileReader::Open(reader->input, bp, f.md);
				});
				if (reader->walk)
					walker = reader->walk.get();
			}
			rgr = walker->RowGroup(rg);
			pager = rgr->GetColumnPageReader(leaf);
			pager->set_data_page_filter([&page_first, &page_rows, at, row](const parquet::DataPageStats &st) mutable {
				int64_t		n = st.num_rows.has_value() ? *st.num_rows : st.num_values;
				bool		skip = page_first >= 0 || at + n <= row;

				if (!skip)
				{
					page_first = at;
					page_rows = n;
				}
				at += n;
				return skip;
			});
		}

		arrow::Type::type id = target->id();
		std::shared_ptr<arrow::DataType> as = nullptr;

		/* a byte array read as the string or binary type the whole read makes */
		if (descr->physical_type() == parquet::Type::BYTE_ARRAY)
			as = id == arrow::Type::EXTENSION ? static_cast<const arrow::ExtensionType &>(*target).storage_type() : target;

		auto		rr = parquet::internal::RecordReader::Make(descr,
															   parquet::internal::LevelInfo::ComputeLevelInfo(descr),
															   pool(), false, false, as);

		rr->SetPageReader(std::move(pager));
		if (!*indexed)
		{
			/* the row's page known as its first record is read */
			if (rr->ReadRecords(1) != 1 || page_first < 0 || page_first > row || row >= page_first + page_rows)
				return fail("no page of row " + std::to_string(row) + " of a column chunk of " +
							std::to_string(rg_rows) + " rows");
			if (page_rows > 1 && rr->ReadRecords(page_rows - 1) != page_rows - 1)
				return fail("a data page of fewer rows than its header says");
		}
		else if (rr->ReadRecords(page_rows) != page_rows)
			return fail("a data page of fewer rows than the offset index says");

		std::shared_ptr<arrow::ArrayData> data = page_array(*rr, descr, target, page_rows);

		if (data == nullptr)
			return VPQ_NOT_FOUND;

		auto		c = std::make_unique<vpq_chunk>();

		c->rows = page_rows;
		c->arrays.push_back(arrow::MakeArray(data));

		vpq_array  &d = c->desc.emplace_back();

		fill_array(c.get(), c->arrays[0]->data(), &d);
		c->columns.push_back(&d);
		*chunk = c.release();
		*first = page_first;
		*nrows = page_rows;
		return VPQ_OK;
	}
	ENGINE_CATCH
}

namespace
{

// W55: the point reads' threads, apart from Arrow's CPU pool (one thread: vpq_configure()), made in the backend
// as first asked for -- inside a call, so that they inherit its blocked signals
std::shared_ptr<arrow::internal::ThreadPool> fetch_pool;

}								// namespace

extern "C" int
vpq_reader_read_pages(vpq_reader *reader, int rg, int n, const int *cols, int64_t row, int threads,
					  vpq_chunk **chunks, int64_t *firsts, int64_t *nrows, bool *indexed, int *rcs)
{
	ENGINE_TRY
	{
		std::vector<std::string> errs(n);

		for (int i = 0; i < n; i++)
			chunks[i] = nullptr;
		/* no more threads than the machine's processors */
		threads = std::max(threads, 1);
		if (std::thread::hardware_concurrency() > 0)
			threads = std::min(threads, (int) std::thread::hardware_concurrency());
		if (!fetch_pool)
		{
			ASSIGN_OR_FAIL(fetch_pool, arrow::internal::ThreadPool::Make(threads));
		}
		else if (fetch_pool->GetCapacity() != threads)
			CHECK_OK(fetch_pool->SetCapacity(threads));
		/* the page index's reader made once here, not by the threads at once */
		if (reader->reader != nullptr)
			reader->reader->parquet_reader()->GetPageIndexReader();
		/* OT4: the backend waits for the threads' reads under VPQ_WAIT_FETCH */
		WaitScope	fetch_wait(VPQ_WAIT_FETCH);
		arrow::Status st = arrow::internal::ParallelFor(n, [&](int i) {
			rcs[i] = vpq_reader_read_page(reader, rg, cols[i], row, &chunks[i], &firsts[i], &nrows[i], &indexed[i]);
			if (rcs[i] == VPQ_ERROR)
				errs[i] = vpq_error();
			return arrow::Status::OK();
		}, fetch_pool.get());

		if (!st.ok())
			return fail(st);
		for (int i = 0; i < n; i++)
			if (rcs[i] == VPQ_ERROR)
				return fail(errs[i]);
		return VPQ_OK;
	}
	ENGINE_CATCH
}

//
// Writing
//

namespace
{

arrow::Result<std::shared_ptr<arrow::DataType>>
storage_type(const vpq_column_spec &s)
{
	switch (s.type.id)
	{
		case VPQ_T_BOOL:
			return arrow::boolean();
		case VPQ_T_INT8:
			return arrow::int8();
		case VPQ_T_INT16:
			return arrow::int16();
		case VPQ_T_INT32:
			return arrow::int32();
		case VPQ_T_INT64:
			return arrow::int64();
		case VPQ_T_UINT32:
			return arrow::uint32();
		case VPQ_T_FLOAT32:
			return arrow::float32();
		case VPQ_T_FLOAT64:
			return arrow::float64();
		case VPQ_T_DATE32:
			return arrow::date32();
		case VPQ_T_TIME64:
			return arrow::time64(arrow::TimeUnit::MICRO);
		case VPQ_T_TIMESTAMP:
			return arrow::timestamp(arrow::TimeUnit::MICRO, s.type.has_tz ? "UTC" : "");
		case VPQ_T_DECIMAL128:
			return arrow::Decimal128Type::Make(s.type.precision, s.type.scale);
		case VPQ_T_STRING:
			return arrow::utf8();
		case VPQ_T_BINARY:
			return arrow::binary();
		case VPQ_T_FIXED_BINARY:
			return arrow::fixed_size_binary(s.type.byte_width);
		default:
			return arrow::Status::NotImplemented("no file column of type ", s.type.id);
	}
}

std::shared_ptr<arrow::Buffer>
borrow(const uint8_t *data, int64_t size)
{
	static const uint8_t empty[8] = {0};

	return std::make_shared<arrow::Buffer>(data ? data : empty, data ? size : 0);
}

arrow::Compression::type
codec_of(const char *name, bool *ok)
{
	std::string n(name ? name : "zstd");

	*ok = true;
	if (n == "zstd")
		return arrow::Compression::ZSTD;
	if (n == "lz4")
		return arrow::Compression::LZ4;
	if (n == "snappy")
		return arrow::Compression::SNAPPY;
	if (n == "gzip")
		return arrow::Compression::GZIP;
	if (n == "brotli")
		return arrow::Compression::BROTLI;
	if (n == "none" || n == "uncompressed")
		return arrow::Compression::UNCOMPRESSED;
	*ok = false;
	return arrow::Compression::UNCOMPRESSED;
}

int
sync_path(const std::string &path, bool directory)
{
	int			fd = open(path.c_str(), directory ? O_RDONLY | O_DIRECTORY : O_RDONLY);

	if (fd < 0)
		return fail("could not open \"" + path + "\" to sync it: " + strerror(errno));
	if (fsync(fd) != 0)
	{
		int			e = errno;

		close(fd);
		return fail("could not fsync \"" + path + "\": " + strerror(e));
	}
	close(fd);
	return VPQ_OK;
}

}								// namespace

extern "C" int
vpq_write_file(const char *path, int ncols, const vpq_column_spec *specs,
			   const vpq_column_data *cols, int64_t nrows, const vpq_write_options *options,
			   int64_t *bytes_written, int64_t *chunk_bytes, char **facts, size_t *facts_len)
{
	ENGINE_TRY
	{
		std::vector<std::shared_ptr<arrow::Field>> fields;
		std::vector<std::shared_ptr<arrow::Array>> arrays;
		bool		codec_ok;
		arrow::Compression::type codec = codec_of(options->codec, &codec_ok);

		*bytes_written = 0;
		if (facts != nullptr)
		{
			*facts = nullptr;
			*facts_len = 0;
		}
		if (!codec_ok)
			return fail(std::string("unknown codec \"") + options->codec + "\"");
		for (int i = 0; i < ncols; i++)
		{
			const vpq_column_spec &s = specs[i];
			const vpq_column_data &c = cols[i];
			std::shared_ptr<arrow::DataType> type;

			ASSIGN_OR_FAIL(type, storage_type(s));

			std::vector<std::shared_ptr<arrow::Buffer>> buffers;

			buffers.push_back(c.null_count > 0 ? borrow(c.validity, (nrows + 7) / 8) : nullptr);
			buffers.push_back(borrow(c.values, c.values_size));
			if (s.type.id == VPQ_T_STRING || s.type.id == VPQ_T_BINARY)
				buffers.push_back(borrow(c.data, c.data_size));

			auto		data = arrow::ArrayData::Make(type, nrows, std::move(buffers), c.null_count);
			std::shared_ptr<arrow::Array> array = arrow::MakeArray(data);

			CHECK_OK(array->Validate());

			std::shared_ptr<arrow::DataType> field_type = type;

			if (s.type.uuid && s.type.id == VPQ_T_FIXED_BINARY && s.type.byte_width == 16)
			{
				field_type = arrow::extension::uuid();
				array = arrow::ExtensionType::WrapArray(field_type, array);
			}
			else if (s.json && s.type.id == VPQ_T_STRING)
			{
				field_type = arrow::extension::json(arrow::utf8());
				array = arrow::ExtensionType::WrapArray(field_type, array);
			}
			fields.push_back(arrow::field(s.name, field_type, s.nullable,
										  arrow::key_value_metadata({"PARQUET:field_id"},
																	{std::to_string(s.field_id)})));
			arrays.push_back(array);
		}

		auto		schema = arrow::schema(fields, arrow::key_value_metadata({"vexec_parquet"}, {"1"}));
		auto		table = arrow::Table::Make(schema, arrays, nrows);

		parquet::WriterProperties::Builder wb;

		wb.memory_pool(pool());
		wb.compression(codec);
		if (options->level != INT32_MIN && codec != arrow::Compression::UNCOMPRESSED &&
			codec != arrow::Compression::SNAPPY)
			wb.compression_level(options->level);
		wb.max_row_group_length(std::max<int64_t>(options->row_group_rows, 1));
		if (options->page_bytes > 0)
			wb.data_pagesize(options->page_bytes);
		if (options->dictionary)
			wb.enable_dictionary();
		else
			wb.disable_dictionary();
		if (options->page_index)
			wb.enable_write_page_index();
		wb.enable_statistics();
		wb.enable_store_decimal_as_integer();
		wb.version(parquet::ParquetVersion::PARQUET_2_6);
		/* data pages of version 1, which every reader takes and the facts say (VPQ_FACTS_PAGES_V1) */
		wb.data_page_version(parquet::ParquetDataPageVersion::V1);

		parquet::ArrowWriterProperties::Builder ab;

		ab.store_schema();
		ab.set_use_threads(false);

		ASSIGN_OR_FAIL(Located loc, locate(path));
		std::string target = loc.path;
		std::string written = loc.s3 ? target : target + ".tmp";
		std::shared_ptr<arrow::io::OutputStream> sink;

		if (loc.s3)
		{
			ASSIGN_OR_FAIL(sink, loc.fs->OpenOutputStream(written));
		}
		else
		{
			ASSIGN_OR_FAIL(sink, arrow::io::FileOutputStream::Open(written));
		}

		std::unique_ptr<parquet::arrow::FileWriter> writer;

		ASSIGN_OR_FAIL(writer, parquet::arrow::FileWriter::Open(*schema, pool(), sink, wb.build(), ab.build()));
		CHECK_OK(writer->WriteTable(*table, std::max<int64_t>(options->row_group_rows, 1)));
		CHECK_OK(writer->Close());
		if (chunk_bytes != nullptr)
		{
			auto		md = writer->metadata();

			for (int g = 0; md && g < md->num_row_groups(); g++)
			{
				auto		rg = md->RowGroup(g);

				for (int c = 0; c < ncols && c < rg->num_columns(); c++)
					chunk_bytes[(int64_t) g * ncols + c] = rg->ColumnChunk(c)->total_compressed_size();
			}
		}
		ASSIGN_OR_FAIL(int64_t size, sink->Tell());
		CHECK_OK(sink->Close());
		if (!loc.s3)
		{
			std::string dir = target.substr(0, target.rfind('/'));

			if (options->sync && sync_path(written, false) != VPQ_OK)
				return VPQ_ERROR;
			if (rename(written.c_str(), target.c_str()) != 0)
				return fail("could not rename \"" + written + "\" to \"" + target + "\": " + strerror(errno));
			if (options->sync && sync_path(dir.empty() ? "/" : dir, true) != VPQ_OK)
				return VPQ_ERROR;
		}
		*bytes_written = size;

		/*
		 * The file's footer as Arrow's reader makes it, from the writer's own
		 * metadata -- the footer it wrote, parsed again -- kept in the process
		 * as an own file's is next opened, and its facts for the catalog
		 */
		if (auto md = writer->metadata())
		{
			auto		footer = std::make_shared<Footer>();

			footer->md = md;
			if (describe_footer(*footer).ok())
			{
				footer_keep(footer_key(path, size, -1), footer);
				if (facts != nullptr)
				{
					std::string b = build_facts(*footer, VPQ_FACTS_PAGES_V1, nullptr);

					*facts = (char *) malloc(std::max<size_t>(b.size(), 1));
					if (*facts == nullptr)
						return fail("out of memory making a file's facts");
					memcpy(*facts, b.data(), b.size());
					*facts_len = b.size();
				}
			}
		}
		return VPQ_OK;
	}
	ENGINE_CATCH
}
