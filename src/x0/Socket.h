#ifndef sw_x0_Socket_h
#define sw_x0_Socket_h (1)

#include <ev++.h>
#include <unistd.h>
#include <system_error>

namespace x0 {

class Buffer;
class BufferRef;

/** \brief represents a network socket.
 *
 * Features:
 * * nonblocking reads/writes,
 * * I/O and timeout event callbacks.
 */
class Socket
{
public:
	enum Mode {
		IDLE, READ, WRITE
	};

	enum State {
		HANDSHAKE, OPERATIONAL, FAILURE
	};

private:
	struct ev_loop *loop_;
	int fd_;
	ev::io watcher_;
	int timeout_;
	ev::timer timer_;
	bool secure_;
	State state_;
	Mode mode_;
	void (*callback_)(Socket *, void *);
	void *callbackData_;

public:
	explicit Socket(struct ev_loop *loop, int fd);
	virtual ~Socket();

	int handle() const;
	bool isClosed() const;

	bool isSecure() const;
	void setSecure(bool enabled);

	bool setNonBlocking(bool enabled);
	bool setTcpNoDelay(bool enable);

	int timeout() const;
	void setTimeout(int value);

	State state() const;
	void setState(State s);

	Mode mode() const;
	void setMode(Mode m);
	template<class K, void (K::*cb)(Socket *)> void setReadyCallback(K *object);
	void clearReadyCallback();

	// initiates the handshake
	template<class K, void (K::*cb)(Socket *)> void handshake(K *object);

	void close();

	struct ev_loop *loop() const;

	// synchronous non-blocking I/O
	virtual ssize_t read(Buffer& result);
	virtual ssize_t write(const BufferRef& source);
	virtual ssize_t write(int fd, off_t *offset, size_t nbytes);

private:
	template<class K, void (K::*cb)(Socket *)>
	static void method_thunk(Socket *socket, void *object);

	void io(ev::io& io, int revents);
	void timeout(ev::timer& timer, int revents);

protected:
	virtual void handshake();

	void callback();
};

// {{{ inlines
inline int Socket::handle() const
{
	return fd_;
}

inline int Socket::timeout() const
{
	return timeout_;
}

inline Socket::State Socket::state() const
{
	return state_;
}

inline void Socket::setState(State s)
{
	state_ = s;
}

inline Socket::Mode Socket::mode() const
{
	return mode_;
}

inline bool Socket::isClosed() const
{
	return fd_ < 0;
}

inline bool Socket::isSecure() const
{
	return secure_;
}

inline void Socket::setSecure(bool enabled)
{
	secure_ = enabled;
}

template<class K, void (K::*cb)(Socket *)>
inline void Socket::method_thunk(Socket *socket, void *object)
{
	(static_cast<K *>(object)->*cb)(socket);
}

template<class K, void (K::*cb)(Socket *)>
inline void Socket::setReadyCallback(K *object)
{
	callback_ = &method_thunk<K, cb>;
	callbackData_ = object;
}

template<class K, void (K::*cb)(Socket *)>
inline void Socket::handshake(K *object)
{
	callback_ = &method_thunk<K, cb>;
	callbackData_ = object;

	handshake();
}

inline void Socket::callback()
{
	if (callback_)
		callback_(this, callbackData_);
}

inline struct ev_loop *Socket::loop() const
{
	return loop_;
}
// }}}

} // namespace x0

#endif