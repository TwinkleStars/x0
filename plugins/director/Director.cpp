/* <plugins/director/Director.cpp>
 *
 * This file is part of the x0 web server project and is released under GPL-3.
 * http://www.xzero.io/
 *
 * (c) 2009-2012 Christian Parpart <trapni@gentoo.org>
 */

#include "Director.h"
#include "Backend.h"
#include "HttpBackend.h"
#include "FastCgiBackend.h"

#include <x0/io/BufferSource.h>
#include <x0/StringTokenizer.h>
#include <x0/IniFile.h>
#include <x0/Url.h>
#include <fstream>

#if !defined(NDEBUG)
#	define TRACE(msg...) (this->Logging::debug(msg))
#else
#	define TRACE(msg...) do {} while (0)
#endif

using namespace x0;

/**
 * Initializes a director object (load balancer instance).
 *
 * \param worker the worker associated with this director's local jobs (e.g. backend health checks).
 * \param name the unique human readable name of this director (load balancer) instance.
 */
Director::Director(HttpWorker* worker, const std::string& name) :
#ifndef NDEBUG
	Logging("Director/%s", name.c_str()),
#endif
	worker_(worker),
	name_(name),
	mutable_(false),
	healthCheckHostHeader_("backend-healthcheck"),
	healthCheckRequestPath_("/"),
	healthCheckFcgiScriptFilename_(),
	stickyOfflineMode_(false),
	backends_(),
	queue_(),
	queueLimit_(128),
	queueTimeout_(TimeSpan::fromSeconds(60)),
	queueTimer_(worker_->loop()),
	retryAfter_(TimeSpan::fromSeconds(10)),
	connectTimeout_(TimeSpan::fromSeconds(10)),
	readTimeout_(TimeSpan::fromSeconds(120)),
	writeTimeout_(TimeSpan::fromSeconds(10)),
	load_(),
	queued_(),
	lastBackend_(0),
	maxRetryCount_(6),
	storagePath_()
{
	backends_.resize(4);

	worker_->registerStopHandler(std::bind(&Director::onStop, this));

	queueTimer_.set<Director, &Director::updateQueueTimer>(this);
}

Director::~Director()
{
}

/**
 * Callback, invoked when the owning worker thread is to be stopped.
 *
 * We're unregistering any possible I/O watchers and timers, as used
 * by proxying connections and health checks.
 */
void Director::onStop()
{
	TRACE("onStop()");

	for (auto& br: backends_) {
		for (auto backend: br) {
			backend->disable();
			backend->healthMonitor().stop();
		}
	}
}

size_t Director::capacity() const
{
	size_t result = 0;

	for (auto& br: backends_)
		for (auto b: br)
			result += b->capacity();

	return result;
}

Backend* Director::createBackend(const std::string& name, const std::string& url)
{
	std::string protocol, hostname, path, query;
	int port = 0;

	if (!parseUrl(url, protocol, hostname, port, path, query)) {
		TRACE("invalid URL: %s", url.c_str());
		return false;
	}

	return createBackend(name, protocol, hostname, port, path, query);
}

Backend* Director::createBackend(const std::string& name, const std::string& protocol,
	const std::string& hostname, int port, const std::string& path, const std::string& query)
{
	int capacity = 1;

	// TODO createBackend<HttpBackend>(hostname, port);
	if (protocol == "http")
		return createBackend<HttpBackend>(name, SocketSpec::fromInet(IPAddress(hostname), port), capacity);

	return nullptr;
}

Backend* Director::findBackend(const std::string& name)
{
	for (auto& br: backends_)
		for (auto b: br)
			if (b->name() == name)
				return b;

	return nullptr;
}

/**
 * Schedules the passed request to the best fitting backend.
 * \param r the request to be processed by one of the directors backends.
 */
void Director::schedule(HttpRequest* r)
{
	r->responseHeaders.push_back("X-Director-Cluster", name_);

	auto notes = r->setCustomData<DirectorNotes>(this, worker_->now());

	bool allDisabled = false;

	if (notes->backend) {
		if (notes->backend->healthMonitor().isOnline()) {
			pass(r, notes, notes->backend);
		} else {
			// pre-selected a backend, but this one is not online, so generate a 503 to give the client some feedback
			r->log(Severity::error, "director: Requested backend '%s' is %s, and is unable to process requests.",
				notes->backend->name().c_str(), notes->backend->healthMonitor().state_str().c_str());
			r->status = x0::HttpStatus::ServiceUnavailable;
			r->finish();
		}
	}
	else if (Backend* backend = findLeastLoad(Backend::Role::Active, &allDisabled)) {
		pass(r, notes, backend);
	}
	else if (Backend* backend = findLeastLoad(Backend::Role::Standby, &allDisabled)) {
		pass(r, notes, backend);
	}
	else if (queue_.size() < queueLimit_ && !allDisabled) {
		enqueue(r);
	}
	else if (Backend* backend = findLeastLoad(Backend::Role::Backup)) {
		pass(r, notes, backend);
	}
	else if (queue_.size() < queueLimit_) {
		enqueue(r);
	}
	else {
		r->log(Severity::error, "director: '%s' queue limit %zu reached. Rejecting request.", name_.c_str(), queueLimit_);
		r->status = HttpStatus::ServiceUnavailable;
		if (retryAfter_) {
			char value[64];
			snprintf(value, sizeof(value), "%zu", retryAfter_.totalSeconds());
			r->responseHeaders.push_back("Retry-After", value);
		}
		r->finish();
	}
}

Backend* Director::findLeastLoad(Backend::Role role, bool* allDisabled)
{
	Backend* best = nullptr;
	size_t bestAvail = 0;
	size_t enabledAndOnline = 0;

	for (auto backend: backendsWith(role)) {
		if (!backend->isEnabled() || !backend->healthMonitor().isOnline()) {
			TRACE("findLeastLoad: skip %s (disabled)", backend->name().c_str());
			continue;
		}

		++enabledAndOnline;

		size_t l = backend->load().current();
		size_t c = backend->capacity();
		size_t avail = c - l;

#ifndef NDEBUG
		worker_->log(Severity::debug, "findLeastLoad: test %s (%zi/%zi, %zi)", backend->name().c_str(), l, c, avail);
#endif

		if (avail > bestAvail) {
#ifndef NDEBUG
			worker_->log(Severity::debug, " - select (%zi > %zi, %s, %s)", avail, bestAvail, backend->name().c_str(), 
					best ? best->name().c_str() : "(null)");
#endif
			bestAvail = avail;
			best = backend;
		}
	}

	if (allDisabled != nullptr) {
		*allDisabled = enabledAndOnline == 0;
	}

	if (bestAvail > 0) {
#ifndef NDEBUG
		worker_->log(Severity::debug, "selecting backend %s", best->name().c_str());
#endif
		return best;
	}

#ifndef NDEBUG
	worker_->log(Severity::debug, "selecting backend (role %d) failed", static_cast<int>(role));
#endif
	return nullptr;
}

void Director::pass(HttpRequest* r, DirectorNotes* notes, Backend* backend)
{
	notes->backend = backend;

	++load_;
	++backend->load_;

	backend->process(r);
}

void Director::link(Backend* backend)
{
	backends_[static_cast<size_t>(backend->role_)].push_back(backend);
}

void Director::unlink(Backend* backend)
{
	auto& br = backends_[static_cast<size_t>(backend->role_)];
	auto i = std::find(br.begin(), br.end(), backend);

	if (i != br.end()) {
		br.erase(i);
	}
}

bool Director::reschedule(HttpRequest* r, Backend* backend)
{
	auto notes = r->customData<DirectorNotes>(this);

	--backend->load_;
	notes->backend = nullptr;

	TRACE("requeue (retry-count: %zi / %zi)", notes->retryCount, maxRetryCount());

	if (notes->retryCount == maxRetryCount()) {
		--load_;

		r->status = HttpStatus::ServiceUnavailable;
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
void Director::enqueue(HttpRequest* r)
{
	// direct delivery failed, due to overheated director. queueing then.

#ifndef NDEBUG
	r->log(Severity::debug, "Director %s overloaded. Queueing request.", name_.c_str());
#endif

	queue_.push_back(r);
	++queued_;

	updateQueueTimer();
}

void Director::updateQueueTimer()
{
	TRACE("updateQueueTimer()");

	// quickly return if queue-timer is already running
	if (queueTimer_.is_active()) {
		TRACE("updateQueueTimer: timer is active, returning");
		return;
	}

	// finish already timed out requests
	while (!queue_.empty()) {
		HttpRequest* r = queue_.front();
		auto notes = r->customData<DirectorNotes>(this);
		TimeSpan age(worker_->now() - notes->ctime);
		if (age < queueTimeout_)
			break;

		TRACE("updateQueueTimer: dequeueing timed out request");
		queue_.pop_front();
		--queued_;

		r->post([this, r]() {
			TRACE("updateQueueTimer: killing request with 503");

			r->status = HttpStatus::ServiceUnavailable;
			if (retryAfter_) {
				char value[64];
				snprintf(value, sizeof(value), "%zu", retryAfter_.totalSeconds());
				r->responseHeaders.push_back("Retry-After", value);
			}
			r->finish();
		});
	}

	if (queue_.empty()) {
		TRACE("updateQueueTimer: queue empty. not starting new timer.");
		return;
	}

	// setup queue timer to wake up after next timeout is reached.
	HttpRequest* r = queue_.front();
	auto notes = r->customData<DirectorNotes>(this);
	TimeSpan age(worker_->now() - notes->ctime);
	TimeSpan ttl(queueTimeout_ - age);
	TRACE("updateQueueTimer: starting new timer with ttl %f (%llu)", ttl.value(), ttl.totalMilliseconds());
	queueTimer_.start(ttl.value(), 0);
}

Backend* Director::nextBackend(Backend* backend, HttpRequest* r)
{
	auto& backends = backendsWith(backend->role());
	auto i = std::find(backends.begin(), backends.end(), backend);

	if (i != backends.end()) {
		auto k = i;
		++k;

		for (; k != backends.end(); ++k) {
			if (backend->isEnabled() && backend->healthMonitor().isOnline() && (*k)->load().current() < (*k)->capacity())
				return *k;

			TRACE("nextBackend: skip %s", backend->name().c_str());
		}

		for (k = backends.begin(); k != i; ++k) {
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
void Director::release(Backend* backend)
{
	--load_;

	if (!backend->isTerminating())
		dequeueTo(backend);
	else
		backend->tryTermination();
}

HttpRequest* Director::dequeue()
{
	if (!queue_.empty()) {
		HttpRequest* r = queue_.front();
		queue_.pop_front();
		--queued_;
		return r;
	}

	return nullptr;
}

/**
 * Pops an enqueued request from the front of the queue and passes it to the backend for serving.
 *
 * \param backend the backend to pass the dequeued request to.
 * \todo thread safety (queue_)
 */
void Director::dequeueTo(Backend* backend)
{
	if (HttpRequest* r = dequeue()) {
#ifndef NDEBUG
		r->log(Severity::debug, "Dequeueing request to backend %s", backend->name().c_str());
#endif
		r->connection.post([this, backend, r]() {
			pass(r, r->customData<DirectorNotes>(this), backend);
		});
	}
}

void Director::writeJSON(Buffer& output)
{
	output << "\"" << name_ << "\": {\n"
		   << "  \"load\": " << load_ << ",\n"
		   << "  \"queued\": " << queued_ << ",\n"
		   << "  \"queue-limit\": " << queueLimit_ << ",\n"
		   << "  \"queue-timeout\": " << queueTimeout_.totalMilliseconds() << ",\n"
		   << "  \"retry-after\": " << retryAfter_.totalSeconds() << ",\n"
		   << "  \"max-retry-count\": " << maxRetryCount_ << ",\n"
		   << "  \"sticky-offline-mode\": " << (stickyOfflineMode_ ? "true" : "false") << ",\n"
		   << "  \"connect-timeout\": " << connectTimeout_.totalMilliseconds() << ",\n"
		   << "  \"read-timeout\": " << readTimeout_.totalMilliseconds() << ",\n"
		   << "  \"write-timeout\": " << writeTimeout_.totalMilliseconds() << ",\n"
		   << "  \"health-check-host-header\": \"" << healthCheckHostHeader_ << "\",\n"
		   << "  \"health-check-request-path\": \"" << healthCheckRequestPath_ << "\",\n"
		   << "  \"health-check-fcgi-script-name\": \"" << healthCheckFcgiScriptFilename_ << "\",\n"
		   << "  \"mutable\": " << (isMutable() ? "true" : "false") << ",\n"
		   << "  \"members\": [";

	size_t backendNum = 0;

	for (auto& br: backends_) {
		for (auto backend: br) {
			if (backendNum++)
				output << ", ";

			output << "\n    {";
			backend->writeJSON(output);
			output << "}";
		}
	}

	output << "\n  ]\n}\n";
}

/**
 * Loads director configuration from given file.
 *
 * \param path The path to the file holding the configuration.
 *
 * \retval true Success.
 * \retval false Failed, detailed message in errno.
 */
bool Director::load(const std::string& path)
{
	// treat director as loaded if db file behind given path does not exist
	struct stat st;
	if (stat(path.c_str(), &st) < 0 && errno == ENOENT) {
		storagePath_ = path;
		setMutable(true);
		return save();
	}

	IniFile settings;
	if (!settings.loadFile(path)) {
		worker_->log(Severity::error, "director: Could not load director settings from file '%s'. %s", path.c_str(), strerror(errno));
		return false;
	}

	std::string value;
	if (!settings.load("director", "queue-limit", value)) {
		worker_->log(Severity::error, "director: Could not load settings value director.queue-limit in file '%s'", path.c_str());
		return false;
	}
	queueLimit_ = std::atoll(value.c_str());

	if (!settings.load("director", "queue-timeout", value)) {
		worker_->log(Severity::error, "director: Could not load settings value director.queue-timeout in file '%s'", path.c_str());
		return false;
	}
	queueTimeout_ = TimeSpan::fromMilliseconds(std::atoll(value.c_str()));

	if (!settings.load("director", "retry-after", value)) {
		worker_->log(Severity::error, "director: Could not load settings value director.retry-after in file '%s'", path.c_str());
		return false;
	}
	retryAfter_ = TimeSpan::fromSeconds(std::atoll(value.c_str()));

	if (!settings.load("director", "connect-timeout", value)) {
		worker_->log(Severity::error, "director: Could not load settings value director.connect-timeout in file '%s'", path.c_str());
		return false;
	}
	connectTimeout_ = TimeSpan::fromMilliseconds(std::atoll(value.c_str()));

	if (!settings.load("director", "read-timeout", value)) {
		worker_->log(Severity::error, "director: Could not load settings value director.read-timeout in file '%s'", path.c_str());
		return false;
	}
	readTimeout_ = TimeSpan::fromMilliseconds(std::atoll(value.c_str()));

	if (!settings.load("director", "write-timeout", value)) {
		worker_->log(Severity::error, "director: Could not load settings value director.write-timeout in file '%s'", path.c_str());
		return false;
	}
	writeTimeout_ = TimeSpan::fromMilliseconds(std::atoll(value.c_str()));

	if (!settings.load("director", "max-retry-count", value)) {
		worker_->log(Severity::error, "director: Could not load settings value director.queue-retry-count in file '%s'", path.c_str());
		return false;
	}

	if (!settings.load("director", "sticky-offline-mode", value)) {
		worker_->log(Severity::error, "director: Could not load settings value director.sticky-offline-mode in file '%s'", path.c_str());
		return false;
	}
	stickyOfflineMode_ = value == "true";

	maxRetryCount_ = std::atoll(value.c_str());

	if (!settings.load("director", "health-check-host-header", healthCheckHostHeader_)) {
		worker_->log(Severity::error, "director: Could not load settings value director.health-check-host-header in file '%s'", path.c_str());
		return false;
	}

	if (!settings.load("director", "health-check-request-path", healthCheckRequestPath_)) {
		worker_->log(Severity::error, "director: Could not load settings value director.health-check-request-path in file '%s'", path.c_str());
		return false;
	}

	if (!settings.load("director", "health-check-fcgi-script-filename", healthCheckFcgiScriptFilename_)) {
		healthCheckFcgiScriptFilename_ = "";
	}

	for (auto& section: settings) {
		static const std::string backendSectionPrefix("backend=");

		std::string key = section.first;
		if (key == "director")
			continue;

		if (key.find(backendSectionPrefix) != 0) {
			worker_->log(Severity::error, "director: Invalid configuration section '%s' in file '%s'.", key.c_str(), path.c_str());
			return false;
		}

		std::string name = key.substr(backendSectionPrefix.size());

		// role
		std::string roleStr;
		if (!settings.load(key, "role", roleStr)) {
			worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'role' not found in section '%s'.", path.c_str(), key.c_str());
			return false;
		}

		Backend::Role role = Backend::Role::Terminate; // aka. Undefined
		if (roleStr == "active")
			role = Backend::Role::Active;
		else if (roleStr == "standby")
			role = Backend::Role::Standby;
		else if (roleStr == "backup")
			role = Backend::Role::Backup;
		else {
			worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'role' for backend '%s' contains invalid data '%s'.", path.c_str(), key.c_str(), roleStr.c_str());
			return false;
		}

		// capacity
		std::string capacityStr;
		if (!settings.load(key, "capacity", capacityStr)) {
			worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'capacity' not found in section '%s'.", path.c_str(), key.c_str());
			return false;
		}
		size_t capacity = std::atoll(capacityStr.c_str());

		// protocol
		std::string protocol;
		if (!settings.load(key, "protocol", protocol)) {
			worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'protocol' not found in section '%s'.", path.c_str(), key.c_str());
			return false;
		}

		// enabled
		std::string enabledStr;
		if (!settings.load(key, "enabled", enabledStr)) {
			worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'enabled' not found in section '%s'.", path.c_str(), key.c_str());
			return false;
		}
		bool enabled = enabledStr == "true";

		// health-check-interval
		std::string hcIntervalStr;
		if (!settings.load(key, "health-check-interval", hcIntervalStr)) {
			worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'health-check-interval' not found in section '%s'.", path.c_str(), key.c_str());
			return false;
		}
		TimeSpan hcInterval = TimeSpan::fromMilliseconds(std::atoll(hcIntervalStr.c_str()));

		// health-check-mode
		std::string hcModeStr;
		if (!settings.load(key, "health-check-mode", hcModeStr)) {
			worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'health-check-mode' not found in section '%s'.", path.c_str(), hcModeStr.c_str(), key.c_str());
			return false;
		}

		HealthMonitor::Mode hcMode;
		if (hcModeStr == "paranoid")
			hcMode = HealthMonitor::Mode::Paranoid;
		else if (hcModeStr == "opportunistic")
			hcMode = HealthMonitor::Mode::Opportunistic;
		else if (hcModeStr == "lazy")
			hcMode = HealthMonitor::Mode::Lazy;
		else {
			worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'health-check-mode' invalid ('%s') in section '%s'.", path.c_str(), hcModeStr.c_str(), key.c_str());
			return false;
		}

		SocketSpec socketSpec;
		std::string path;
		if (settings.load(key, "path", path)) {
			socketSpec = SocketSpec::fromLocal(path);
		} else {
			// host
			std::string host;
			if (!settings.load(key, "host", host)) {
				worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'host' not found in section '%s'.", path.c_str(), key.c_str());
				return false;
			}

			// port
			std::string portStr;
			if (!settings.load(key, "port", portStr)) {
				worker_->log(Severity::error, "director: Error loading configuration file '%s'. Item 'port' not found in section '%s'.", path.c_str(), key.c_str());
				return false;
			}

			int port = std::atoi(portStr.c_str());
			if (port <= 0) {
				worker_->log(Severity::error, "director: Error loading configuration file '%s'. Invalid port number '%s' for backend '%s'", path.c_str(), portStr.c_str(), name.c_str());
				return false;
			}

			socketSpec = SocketSpec::fromInet(IPAddress(host), port);
		}

		// spawn backend (by protocol)
		Backend* backend = nullptr;
		if (protocol == "fastcgi") {
			backend = new FastCgiBackend(this, name, socketSpec, capacity);
		} else if (protocol == "http") {
			backend = new HttpBackend(this, name, socketSpec, capacity);
		} else {
			worker_->log(Severity::error, "director: Invalid protocol '%s' for backend '%s' in configuration file '%s'.", protocol.c_str(), name.c_str(), path.c_str());
		}

		if (!backend)
			return false;

		backend->setEnabled(enabled);
		backend->setRole(role);
		backend->healthMonitor().setMode(hcMode);
		backend->healthMonitor().setInterval(hcInterval);
	}

	storagePath_ = path;

	setMutable(true);

	return true;
}

/**
 * stores director configuration in a plaintext file.
 *
 * \todo this must happen asynchronousely, never ever block within the callers thread (or block in a dedicated thread).
 */
bool Director::save()
{
	static const std::string header("name,role,capacity,protocol,enabled,transport,host,port");
	std::string path = storagePath_;
	std::ofstream out(path, std::ios_base::out | std::ios_base::trunc);

	out << "# vim:syntax=dosini\n"
		<< "# !!! DO NOT EDIT !!! THIS FILE IS GENERATED AUTOMATICALLY !!!\n\n"
		<< "[director]\n"
		<< "queue-limit=" << queueLimit_ << "\n"
		<< "queue-timeout=" << queueTimeout_.totalMilliseconds() << "\n"
		<< "retry-after=" << retryAfter_.totalSeconds() << "\n"
		<< "max-retry-count=" << maxRetryCount_ << "\n"
		<< "sticky-offline-mode=" << (stickyOfflineMode_ ? "true" : "false") << "\n"
		<< "connect-timeout=" << connectTimeout_.totalMilliseconds() << "\n"
		<< "read-timeout=" << readTimeout_.totalMilliseconds() << "\n"
		<< "write-timeout=" << writeTimeout_.totalMilliseconds() << "\n"
		<< "health-check-host-header=" << healthCheckHostHeader_ << "\n"
		<< "health-check-request-path=" << healthCheckRequestPath_ << "\n"
		<< "health-check-fcgi-script-filename=" << healthCheckFcgiScriptFilename_ << "\n"
		<< "\n";

	for (auto& br: backends_) {
		for (auto b: br) {
			out << "[backend=" << b->name() << "]\n"
				<< "role=" << b->role_str() << "\n"
				<< "capacity=" << b->capacity() << "\n"
				<< "enabled=" << (b->isEnabled() ? "true" : "false") << "\n"
				<< "transport=" << (b->socketSpec().isLocal() ? "local" : "tcp") << "\n"
				<< "protocol=" << b->protocol() << "\n"
				<< "health-check-mode=" << b->healthMonitor().mode_str() << "\n"
				<< "health-check-interval=" << b->healthMonitor().interval().totalMilliseconds() << "\n";

			if (b->socketSpec().isInet()) {
				out << "host=" << b->socketSpec().ipaddr().str() << "\n";
				out << "port=" << b->socketSpec().port() << "\n";
			} else {
				out << "path=" << b->socketSpec().local() << "\n";
			}

			out << "\n";
		}
	}

	return true;
}
