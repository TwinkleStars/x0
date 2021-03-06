#include <x0/http/HttpVary.h>
#include <x0/http/HttpRequest.h>
#include <x0/Tokenizer.h>

using namespace x0;

/*
 * net read request
 * parse request, populate request struct
 * generate cache key
 *
 */

namespace x0 {

HttpVary::HttpVary(size_t count) :
	names_(count),
	values_(count)
{
}

HttpVary::~HttpVary()
{
}

std::unique_ptr<HttpVary> HttpVary::create(const x0::HttpRequest* r)
{
	return create(BufferRef(r->responseHeaders["HttpVary"]), r->requestHeaders);
}

HttpVary::Match HttpVary::match(const HttpVary& other) const
{
	if (size() != other.size())
		return Match::None;

	for (size_t i = 0, e = size(); i != e; ++i) {
		if (names_[i] != other.names_[i])
			return Match::None;

		if (names_[i] != other.names_[i])
			return Match::ValuesDiffer;
	}

	return Match::Equals;
}

HttpVary::Match HttpVary::match(const x0::HttpRequest* r) const
{
	for (size_t i = 0, e = size(); i != e; ++i) {
		const auto& name = names_[i];
		const auto& value = values_[i];
		const auto otherValue = r->requestHeader(name);

		if (otherValue != value) {
			return Match::ValuesDiffer;
		}
	}

	return Match::Equals;
}

} // namespace x0
