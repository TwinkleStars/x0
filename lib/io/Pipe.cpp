/* <src/Pipe.cpp>
 *
 * This file is part of the x0 web server project and is released under AGPL-3.
 * http://www.xzero.io/
 *
 * (c) 2009-2014 Christian Parpart <trapni@gmail.com>
 */

#include <x0/io/Pipe.h>
#include <x0/Socket.h>

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

namespace x0 {

/* creates a pipe
 *
 * \param flags an OR'ed value of O_NONBLOCK and O_CLOEXEC
 */
Pipe::Pipe(int flags) :
	size_(0)
{
#ifdef __APPLE__
	if (::pipe(pipe_) < 0) {
		pipe_[0] = -errno;
		pipe_[1] = -1;
	} else {
		fcntl(pipe_[0], F_SETFL, fcntl(pipe_[0], F_GETFL) | flags);
		fcntl(pipe_[1], F_SETFL, fcntl(pipe_[1], F_GETFL) | flags);
	}
#else
	if (::pipe2(pipe_, flags) < 0) {
		pipe_[0] = -errno;
		pipe_[1] = -1;
	}
#endif
}

void Pipe::clear()
{
	char buf[4096];
	ssize_t rv;

	do rv = ::read(readFd(), buf, sizeof(buf));
	while (rv > 0);

	size_ = 0;
}

ssize_t Pipe::write(const void* buf, size_t size)
{
	ssize_t rv = ::write(writeFd(), buf, size);

	if (rv > 0)
		size_ += rv;

	return rv;
}

ssize_t Pipe::write(Socket* socket, size_t size)
{
	return socket->write(this, size);
}

ssize_t Pipe::write(Pipe* pipe, size_t size)
{
	ssize_t rv = 0;

#ifdef __APPLE__
	assert(__APPLE__);
#else
	rv = splice(pipe->readFd(), NULL, writeFd(), NULL, pipe->size_, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
#endif

	if (rv > 0) {
		pipe->size_ -= rv;
		size_ += rv;
	}

	return rv;
}

ssize_t Pipe::write(int fd, off_t* fd_off, size_t size)
{
	ssize_t rv = 0;

#ifdef __APPLE__
	assert(__APPLE__);
#else
	rv = splice(fd, fd_off, writeFd(), NULL, size, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
#endif

	if (rv > 0)
		size_ += rv;

	return rv;
}

ssize_t Pipe::read(void* buf, size_t size)
{
	ssize_t rv = ::read(readFd(), buf, size);

	if (rv > 0)
		size_ -= rv;

	return rv;
}

ssize_t Pipe::read(Socket* socket, size_t size)
{
	return socket->read(this, size);
}

ssize_t Pipe::read(Pipe* pipe, size_t size)
{
	return pipe->write(this, size);
}

ssize_t Pipe::read(int fd, off_t* fd_off, size_t size)
{
	ssize_t rv = 0;

#ifdef __APPLE__
	assert(__APPLE__);
#else
	rv = splice(readFd(), fd_off, fd, NULL, size, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
#endif

	if (rv > 0)
		size_ -= rv;

	return rv;
}

} // namespace x0
