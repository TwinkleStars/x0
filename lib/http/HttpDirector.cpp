#include <x0/http/HttpDirector.h>
#include <x0/http/HttpBackend.h>
#include <x0/io/BufferSource.h>
#include <x0/StringTokenizer.h>
#include <x0/Url.h>
#include <fstream>

#if !defined(NDEBUG)
#	define TRACE(msg...) (this->Logging::debug(msg))
#else
#	define TRACE(msg...) do {} while (0)
#endif

namespace x0 {

struct HttpDirectorNotes :
	public CustomData
{
	size_t retryCount;
	HttpBackend* backend;

	HttpDirectorNotes() :
		retryCount(0),
		backend(nullptr)
	{}
};

HttpDirector::HttpDirector(HttpWorker* worker, const std::string& name) :
#ifndef NDEBUG
	Logging("HttpDirector/%s", name.c_str()),
#endif
	worker_(worker),
	name_(name),
	mutable_(false),
	backends_(),
	queue_(),
	load_(),
	queued_(),
	lastBackend_(0),
	cloakOrigin_(true),
	maxRetryCount_(6),
	storagePath_()
{
	worker_->registerStopHandler(std::bind(&HttpDirector::onStop, this));
}

HttpDirector::~HttpDirector()
{
}

/**
 * Callback, invoked when the owning worker thread is to be stopped.
 *
 * We're unregistering any possible I/O watchers and timers, as used
 * by proxying connections and health checks.
 */
void HttpDirector::onStop()
{
	TRACE("onStop()");

	for (auto backend: backends_) {
		backend->disable();
		backend->healthMonitor().stop();
	}
}

size_t HttpDirector::capacity() const
{
	size_t result = 0;

	for (auto b: backends_)
		result += b->capacity();

	return result;
}

HttpBackend* HttpDirector::createBackend(const std::string& name, const std::string& url)
{
	std::string protocol, hostname, path, query;
	int port = 0;

	if (!parseUrl(url, protocol, hostname, port, path, query)) {
		TRACE("invalid URL: %s", url.c_str());
		return false;
	}

	return createBackend(name, protocol, hostname, port, path, query);
}

HttpBackend* HttpDirector::createBackend(const std::string& name, const std::string& protocol,
	const std::string& hostname, int port, const std::string& path, const std::string& query)
{
	int capacity = 1;

	// TODO createBackend<HttpProxy>(hostname, port);
	if (protocol == "http")
		return createBackend<HttpProxy>(name, capacity, hostname, port);

	return nullptr;
}

void HttpDirector::schedule(HttpRequest* r)
{
	r->responseHeaders.push_back("X-Director-Cluster", name_);

	r->setCustomData<HttpDirectorNotes>(this);

	auto notes = r->customData<HttpDirectorNotes>(this);

	// try delivering request directly
	if (HttpBackend* backend = selectBackend(r)) {
		notes->backend = backend;

		++load_;
		++backend->load_;

		backend->process(r);
	} else {
		enqueue(r);
	}
}

bool HttpDirector::reschedule(HttpRequest* r, HttpBackend* backend)
{
	auto notes = r->customData<HttpDirectorNotes>(this);

	--backend->load_;
	notes->backend = nullptr;

	TRACE("requeue (retry-count: %zi / %zi)", notes->retryCount, maxRetryCount());

	if (notes->retryCount == maxRetryCount()) {
		--load_;

		r->status = HttpError::ServiceUnavailable;
		r->finish();

		return false;
	}

	++notes->retryCount;

	backend = nextBackend(backend, r);
	if (backend != nullptr) {
		notes->backend = backend;
		++backend->load_;

		if (backend->process(r)) {
			return true;
		}
	}

	TRACE("requeue (retry-count: %zi / %zi): giving up", notes->retryCount, maxRetryCount());

	--load_;
	enqueue(r);

	return false;
}

/**
 * Enqueues given request onto the request queue.
 */
void HttpDirector::enqueue(HttpRequest* r)
{
	// direct delivery failed, due to overheated director. queueing then.

#ifndef NDEBUG
	r->log(Severity::debug, "Director %s overloaded. Queueing request.", name_.c_str());
#endif

	queue_.push_back(r);
	++queued_;
}

HttpBackend* HttpDirector::selectBackend(HttpRequest* r)
{
	HttpBackend* best = nullptr;
	size_t bestAvail = 0;

	for (HttpBackend* backend: backends_) {
		if (!backend->isEnabled() || !backend->healthMonitor().isOnline()) {
			TRACE("selectBackend: skip %s (disabled)", backend->name().c_str());
			continue;
		}

		size_t l = backend->load().current();
		size_t c = backend->capacity();
		size_t avail = c - l;

#ifndef NDEBUG
		r->log(Severity::debug, "selectBackend: test %s (%zi/%zi, %zi)", backend->name().c_str(), l, c, avail);
#endif

		if (avail > bestAvail) {
#ifndef NDEBUG
			r->log(Severity::debug, " - select (%zi > %zi, %s, %s)", avail, bestAvail, backend->name().c_str(), 
					best ? best->name().c_str() : "(null)");
#endif
			bestAvail = avail;
			best = backend;
		}
	}

	if (bestAvail > 0) {
#ifndef NDEBUG
		r->log(Severity::debug, "selecting backend %s", best->name().c_str());
#endif
		return best;
	} else {
#ifndef NDEBUG
		r->log(Severity::debug, "selecting backend failed");
#endif
		return nullptr;
	}
}

HttpBackend* HttpDirector::nextBackend(HttpBackend* backend, HttpRequest* r)
{
	auto i = std::find(backends_.begin(), backends_.end(), backend);
	if (i != backends_.end()) {
		auto k = i;
		++k;

		for (; k != backends_.end(); ++k) {
			if (backend->isEnabled() && backend->healthMonitor().isOnline() && (*k)->load().current() < (*k)->capacity())
				return *k;

			TRACE("nextBackend: skip %s", backend->name().c_str());
		}

		for (k = backends_.begin(); k != i; ++k) {
			if (backend->isEnabled() && backend->healthMonitor().isOnline() && (*k)->load().current() < (*k)->capacity())
				return *k;

			TRACE("nextBackend: skip %s", backend->name().c_str());
		}
	}

	TRACE("nextBackend: no next backend chosen.");
	return nullptr;
}

/**
 * Notifies the director, that the given backend has just completed processing a request.
 *
 * \param backend the backend that just completed a request, and thus, is able to potentially handle one more.
 *
 * This method is to be invoked by backends, that
 * just completed serving a request, and thus, invoked
 * finish() on it, so it could potentially process
 * the next one, if, and only if, we have
 * already queued pending requests.
 *
 * Otherwise this call will do nothing.
 *
 * \see schedule(), reschedule(), enqueue(), dequeueTo()
 */
void HttpDirector::release(HttpBackend* backend)
{
	--load_;

	dequeueTo(backend);
}

/**
 * Pops an enqueued request from the front of the queue and passes it to the backend for serving.
 *
 * \param backend the backend to pass the dequeued request to.
 */
void HttpDirector::dequeueTo(HttpBackend* backend)
{
	if (!queue_.empty()) {
		HttpRequest* r = queue_.front();
		queue_.pop_front();
		--queued_;

#ifndef NDEBUG
		r->log(Severity::debug, "Dequeueing request to backend %s", backend->name().c_str());
#endif

		auto notes = r->customData<HttpDirectorNotes>(this);
		notes->backend = backend;
		++backend->load_;
		++load_;

		backend->process(r);
	}
}

/**
 * Loads director configuration from given file.
 *
 * \param path The path to the file holding the configuration.
 *
 * \retval true Success.
 * \retval false Failed, detailed message in errno.
 */
bool HttpDirector::load(const std::string& path)
{
	std::ifstream in(path);

	while (in.good()) {
		char buf[4096];
		in.getline(buf, sizeof(buf));
		size_t len = in.gcount();

		if (!len || buf[0] == '#')
			continue;

		StringTokenizer st(buf, ",", '\\');
		std::vector<std::string> values(st.tokenize());

		if (values.size() < 8) {
			worker_->log(Severity::error, "director: Invalid record in director file.");
			continue;
		}

		std::string name(values[0]);
		std::string role(values[1]);
		size_t capacity = std::atoi(values[2].c_str());
		std::string protocol(values[3]);
		bool enabled = values[4] == "true";
		std::string transport(values[5]);
		std::string hostname(values[6]);
		int port = std::atoi(values[7].c_str());

		HttpBackend* backend = new HttpProxy(this, name, capacity, hostname, port);
		if (!enabled)
			backend->disable();
		if (role == "active")
			backend->setRole(HttpBackend::Role::Active);
		else if (role == "standby")
			backend->setRole(HttpBackend::Role::Standby);
// TODO
//		else if (role == "backup")
//			backend->setRole(HttpBackend::Role::Backup);
		else
			worker_->log(Severity::error, "Invalid backend role '%s'", role.c_str());

		backends_.push_back(backend);
	}

	setMutable(true);

	return true;
}

/**
 * stores director configuration in a plaintext file.
 *
 * \param pathOverride if not empty, store data into this file's path, oetherwise use the one we loaded from.
 * \todo this must happen asynchronousely, never ever block within the callers thread (or block in a dedicated thread).
 */
bool HttpDirector::store(const std::string& pathOverride)
{
	static const std::string header("name,role,capacity,protocol,enabled,transport,host,port");
	std::string path = !pathOverride.empty() ? pathOverride : storagePath_;
	std::ofstream out(path);

	// TODO

	return true;
}

} // namespace x0