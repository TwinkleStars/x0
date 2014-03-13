/* <HttpConnection.h>
 *
 * This file is part of the x0 web server project and is released under AGPL-3.
 * http://www.xzero.io/
 *
 * (c) 2009-2014 Christian Parpart <trapni@gmail.com>
 */

#ifndef x0_connection_h
#define x0_connection_h (1)

#include <x0/http/HttpMessageParser.h>
#include <x0/http/HttpStatus.h>
#include <x0/io/CompositeSource.h>
#include <x0/io/SocketSink.h>
#include <x0/CustomDataMgr.h>
#include <x0/LogMessage.h>
#include <x0/TimeSpan.h>
#include <x0/Socket.h>
#include <x0/Buffer.h>
#include <x0/Property.h>
#include <x0/Types.h>
#include <x0/Api.h>

#include <x0/sysconfig.h>

#include <functional>
#include <memory>
#include <string>
#include <ev++.h>

namespace x0 {

//! \addtogroup http
//@{

class HttpRequest;
class HttpWorker;
class ServerSocket;

/**
 * @brief HTTP client connection object.
 * @see HttpRequest, HttpServer
 */
class X0_API HttpConnection :
	public HttpMessageParser
{
	CUSTOMDATA_API_INLINE

public:
	enum Status {
		Undefined = 0,			//!< Object got just constructed.
		ReadingRequest,			//!< Parses HTTP request.
		ProcessingRequest,		//!< request handler: has taken over but not sent out anythng
		SendingReply,			//!< request handler: response headers written, sending body
		SendingReplyDone,		//!< request handler: populating message done, still pending data to sent.
		KeepAliveRead			//!< Waiting for next HTTP request in keep-alive state.
	};

public:
	HttpConnection& operator=(const HttpConnection&) = delete;
	HttpConnection(const HttpConnection&) = delete;

	/**
	 * creates an HTTP connection object.
	 * \param srv a ptr to the server object this connection belongs to.
	 */
	HttpConnection(HttpWorker* worker, unsigned long long id);

	~HttpConnection();

	unsigned long long id() const;				//!< returns the (mostly) unique, worker-local, ID to this connection

	unsigned requestCount() const { return requestCount_; }

	Status status() const { return status_; }
	void setStatus(Status value);
	const char* status_str() const;

	void close();

	Socket *socket() const;						//!< Retrieves a pointer to the connection socket.
	HttpWorker& worker() const;					//!< Retrieves a reference to the owning worker.

    const IPAddress& remoteIP() const { return socket_->remoteIP(); }
	unsigned int remotePort() const;			//!< Retrieves the TCP port numer of the remote end point (client).

    const IPAddress& localIP() const { return socket_->localIP(); }
	unsigned int localPort() const;

	const ServerSocket& listener() const;

	bool isSecure() const;

	void write(Source* buffer);
	template<class T, class... Args> void write(Args&&... args);

	void flush();
	bool autoFlush() const { return autoFlush_; }
	void setAutoFlush(bool value) { autoFlush_ = value; if (value) { flush(); } }

	bool isOutputPending() const { return !output_.empty(); }
	bool isHandlingRequest() const { return flags_ & IsHandlingRequest; }

	const HttpRequest* request() const { return request_; }
	HttpRequest* request() { return request_; }

	std::size_t inputSize() const { return input_.size(); }
	std::size_t inputOffset() const { return inputOffset_; }

	bool isInputPending() const { return inputOffset_ < input_.size(); }

	unsigned refCount() const;

	void post(const std::function<void()>& function);

	bool isAborted() const;
	bool isClosed() const;

	void ref(const char* msg = "");
	void unref(const char* msg = "");

private:
	friend class HttpRequest;
	friend class HttpWorker;

	void reinitialize();

	// overrides from HttpMessageParser:
	virtual bool onMessageBegin(const BufferRef& method, const BufferRef& entity, int versionMajor, int versionMinor);
	virtual bool onMessageHeader(const BufferRef& name, const BufferRef& value);
	virtual bool onMessageHeaderEnd();
	virtual bool onMessageContent(const BufferRef& chunk);
	virtual bool onMessageEnd();

	void clear();

	void start(ServerSocket* listener, Socket* client);
	void resume();

	void handshakeComplete(Socket *);

	void watchInput(const TimeSpan& timeout = TimeSpan::Zero);
	void watchOutput();

	bool readSome();
	bool writeSome();
	bool process();
	void io(Socket *socket, int revents);
	void timeout(Socket *socket);

	void abort();
	void abort(HttpStatus status);

	template<typename... Args>
	void log(Severity s, const char* fmt, Args... args);

	void log(LogMessage&& msg);

	Buffer& inputBuffer() { return input_; }
	const Buffer& inputBuffer() const { return input_; }

	void setShouldKeepAlive(bool enabled);
	bool shouldKeepAlive() const { return flags_ & IsKeepAliveEnabled; }

private:
	unsigned refCount_;

	Status status_;

	ServerSocket* listener_;
	HttpWorker* worker_;

	unsigned long long id_;				//!< the worker-local connection-ID
	unsigned requestCount_;				//!< the number of requests already processed or currently in process

	// we could make these things below flags
	unsigned flags_;
	static const unsigned IsHandlingRequest  = 0x0002; //!< is this connection (& request) currently passed to a request handler?
	static const unsigned IsResuming         = 0x0004; //!< resume() was invoked and we've something in the pipeline (flag needed?)
	static const unsigned IsKeepAliveEnabled = 0x0008; //!< connection should keep-alive to accept further requests
	static const unsigned IsAborted          = 0x0010; //!< abort() was invoked, merely meaning, that the client aborted the connection early
	static const unsigned IsClosed           = 0x0020; //!< closed() invoked, but close-action delayed

	// HTTP HttpRequest
	Buffer input_;						//!< buffer for incoming data.
	std::size_t inputOffset_;			//!< number of bytes in input_ successfully processed already.
	std::size_t inputRequestOffset_;	//!< offset to the first byte of the currently processed request
	HttpRequest* request_;				//!< currently parsed http HttpRequest, may be NULL

	// output
	CompositeSource output_;			//!< pending write-chunks
	Socket* socket_;					//!< underlying communication socket
	SocketSink sink_;					//!< sink wrapper for socket_
	bool autoFlush_;					//!< true if flush() is invoked automatically after every write()

	// connection abort callback
	void (*abortHandler_)(void*);
	void* abortData_;

	HttpConnection* prev_;
	HttpConnection* next_;
};

// {{{ inlines
inline Socket* HttpConnection::socket() const
{
	return socket_;
}

inline unsigned long long HttpConnection::id() const
{
	return id_;
}

inline unsigned HttpConnection::refCount() const
{
	return refCount_;
}

inline const char* HttpConnection::status_str() const
{
	static const char* str[] = {
		"undefined",
		"reading-request",
		"processing-request",
		"sending-reply",
		"sending-reply-done",
		"keep-alive-read"
	};
	return str[static_cast<size_t>(status_)];
}

inline HttpWorker& HttpConnection::worker() const
{
	return *worker_;
}

template<class T, class... Args>
inline void HttpConnection::write(Args&&... args)
{
	if (!isAborted()) {
		write(new T(args...));
	}
}

inline const ServerSocket& HttpConnection::listener() const
{
	return *listener_;
}

/*! Tests whether if the connection to the client (remote end-point) has * been aborted early.
 */
inline bool HttpConnection::isAborted() const
{
	return (flags_ & IsAborted) || isClosed();
}

inline bool HttpConnection::isClosed() const
{
	return !socket_ || (flags_ & IsClosed);
}

template<typename... Args>
inline void HttpConnection::log(Severity s, const char* fmt, Args... args)
{
	log(LogMessage(s, fmt, args...));
}
// }}}

//@}

} // namespace x0

#endif
