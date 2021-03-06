/*! \brief Very simple HTTP server. Everything's done for you.
 *
 * It just serves static pages.
 */
#include <x0/http/HttpServer.h>
#include <x0/http/HttpRequest.h>
#include <x0/io/BufferSource.h>
#include <iostream>
#include <fstream>
#include <memory>
#include <ev++.h>

class MyServer {
private:
	struct ev_loop* loop_;
	ev::sig sigterm_;
	std::unique_ptr<x0::HttpServer> http_;

public:
	MyServer() :
		loop_(ev_default_loop()),
		sigterm_(loop_),
		http_()
	{
	}

	~MyServer()
	{
		if (sigterm_.is_active()) {
			ev_ref(loop_);
			sigterm_.stop();
		}
	}

	bool requestHandler(x0::HttpRequest* r)
	{
		x0::Buffer body;
		body.push_back("Hello, World\n");

		r->status = x0::HttpStatus::Ok;
		r->responseHeaders.push_back("Content-Type", "text/plain");
		r->write<x0::BufferSource>(body);
		r->finish();

		return true;
	}

	int run()
	{
		std::clog << "Initializing ..." << std::endl;
		sigterm_.set<MyServer, &MyServer::terminate_handler>(this);
		sigterm_.start(SIGTERM);
		ev_unref(loop_);

		http_.reset(new x0::HttpServer(loop_));
		http_->requestHandler = std::bind(&MyServer::requestHandler, this, std::placeholders::_1);
		http_->setupListener("0.0.0.0", 3000);

		std::clog << "Running ..." << std::endl;
		int rv = http_->run();

		std::clog << "Quitting ..." << std::endl;
		return rv;
	}

private:
	void terminate_handler(ev::sig& sig, int)
	{
		std::clog << "Terminate signal received ..." << std::endl;

		ev_ref(loop_);
		sig.stop();

		http_->stop();
	}
};

int main(int argc, const char *argv[])
{
	MyServer s;
	return s.run();
}
