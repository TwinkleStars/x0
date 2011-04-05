/* <HttpConnection.h>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#ifndef x0_connection_h
#define x0_connection_h (1)

#include <x0/http/HttpMessageProcessor.h>
#include <x0/http/HttpWorker.h>
#include <x0/io/CompositeSource.h>
#include <x0/io/SocketSink.h>
#include <x0/CustomDataMgr.h>
#include <x0/Socket.h>
#include <x0/Buffer.h>
#include <x0/Property.h>
#include <x0/Logging.h>
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

/**
 * @brief HTTP client connection object.
 * @see HttpRequest, HttpServer
 */
class X0_API HttpConnection :
#ifndef NDEBUG
	public Logging,
#endif
	public HttpMessageProcessor,
	public CustomDataMgr
{
public:
	HttpConnection& operator=(const HttpConnection&) = delete;
	HttpConnection(const HttpConnection&) = delete;

	/**
	 * creates an HTTP connection object.
	 * \param srv a ptr to the server object this connection belongs to.
	 */
	HttpConnection(HttpListener& listener, HttpWorker& worker, int fd);

	~HttpConnection();

	void close();

	ValueProperty<bool> secure;					//!< true if this is a secure (HTTPS) connection, false otherwise.

	Socket *socket() const;						//!< Retrieves a pointer to the connection socket.
	HttpWorker& worker();						//!< Retrieves a reference to the owning worker.

	std::string remoteIP() const;				//!< Retrieves the IP address of the remote end point (client).
	unsigned int remotePort() const;			//!< Retrieves the TCP port numer of the remote end point (client).

	std::string localIP() const;
	unsigned int localPort() const;

	const HttpListener& listener() const;

	bool isSecure() const;

	void write(const SourcePtr& buffer);
	template<class T, class... Args> void write(Args&&... args);

	bool isOutputPending() const;

private:
	friend class HttpRequest;
	friend class HttpListener;
	friend class HttpWorker;

	// overrides from HttpMessageProcessor:
	virtual void messageBegin(BufferRef&& method, BufferRef&& entity, int version_major, int version_minor);
	virtual void messageHeader(BufferRef&& name, BufferRef&& value);
	virtual bool messageHeaderEnd();
	virtual bool messageContent(BufferRef&& chunk);
	virtual bool messageEnd();

	void start();
	void resume();

	bool isClosed() const;

	void handshakeComplete(Socket *);
	void startRead();

	void processInput();
	void processOutput();

	void process();
	void io(Socket *socket, int revents);
	void timeout(Socket *socket);

	struct ::ev_loop *loop() const;

	unsigned long long bytesTransferred() const;

	void abort();
	void checkFinish();

private:
	HttpListener& listener_;
	HttpWorker& worker_;

	Socket *socket_;					//!< underlying communication socket
	bool active_;						//!< socket is active (some I/O event raised within this cycle)

	// HTTP HttpRequest
	Buffer buffer_;						//!< buffer for incoming data.
	std::size_t offset_;				//!< number of bytes in buffer_ successfully processed already.
	int request_count_;					//!< number of requests already and fully processed within this connection.
	HttpRequest *request_;				//!< currently parsed http HttpRequest, may be NULL

	void (*abortHandler_)(void *);
	void *abortData_;

	CompositeSource source_;
	SocketSink sink_;
	unsigned long long bytesTransferred_;

#if !defined(NDEBUG)
	ev::tstamp ctime_;
#endif
};

// {{{ inlines
inline struct ::ev_loop* HttpConnection::loop() const
{
	return worker_.loop();
}

inline Socket *HttpConnection::socket() const
{
	return socket_;
}

inline HttpWorker& HttpConnection::worker()
{
	return worker_;
}

template<class T, class... Args>
inline void HttpConnection::write(Args&&... args)
{
	write(std::make_shared<T>(args...));
}

inline const HttpListener& HttpConnection::listener() const
{
	return listener_;
}

/** tests whether HttpConnection::close() was invoked already.
 */
inline bool HttpConnection::isClosed() const
{
	return !socket_ || socket_->isClosed();
}

/** retrieves the number of transmitted bytes. */
inline unsigned long long HttpConnection::bytesTransferred() const
{
	//! \todo rename this to bytesTransmitted, and introduce bytesReceived property, so, that bytesTransferred is the sum of both.
	return bytesTransferred_;
}

/*! tests whether or not this connection has pending data to sent to the client.
 */
inline bool HttpConnection::isOutputPending() const
{
	return !source_.empty();
}
// }}}

//@}

} // namespace x0

#endif
