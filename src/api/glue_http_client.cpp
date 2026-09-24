#include "api/glue_http_client.hpp"

#include "duckdb/common/http_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#ifdef _WIN32
#include <aws/core/http/windows/WinHttpSyncHttpClient.h>
#else
#include <aws/core/http/curl/CurlHttpClient.h>
#endif

#include <sstream>

// Bridge between the AWS SDK and DuckDB's HTTP layer, modeled after duckdb-aws PR #173: an aws-sdk-cpp
// HttpClientFactory whose HttpClient forwards every request to HTTPUtil::Get(db). The concrete transport is whatever
// the DatabaseInstance has registered (httpfs's curl client on native). Requests go through HTTPUtil::Request rather
// than the client directly so that DuckDB's retry and HTTP logging apply.

namespace duckdb {

static constexpr const char *NETWORK_VIA_DUCKDB_SETTING = "glue_network_calls_via_duckdb";

//===--------------------------------------------------------------------===//
// Context scope
//===--------------------------------------------------------------------===//
static thread_local optional_ptr<ClientContext> current_glue_context;

GlueHttpClientContextScope::GlueHttpClientContextScope(ClientContext &context) : previous(current_glue_context) {
	current_glue_context = &context;
}

GlueHttpClientContextScope::~GlueHttpClientContextScope() {
	current_glue_context = previous;
}

optional_ptr<ClientContext> GlueHttpClientContextScope::Current() {
	return current_glue_context;
}

namespace {

//! Read the toggle, preferring the calling connection (a plain SET is session scoped) over the database settings.
//! Defaults to true.
bool NetworkCallsViaDuckDB(DatabaseInstance &db) {
	Value value;
	auto context = GlueHttpClientContextScope::Current();
	if (context && context->TryGetCurrentSetting(NETWORK_VIA_DUCKDB_SETTING, value) && !value.IsNull()) {
		return BooleanValue::Get(value);
	}
	if (db.TryGetCurrentSetting(NETWORK_VIA_DUCKDB_SETTING, value) && !value.IsNull()) {
		return BooleanValue::Get(value);
	}
	return true;
}

RequestType ToDuckDBRequestType(Aws::Http::HttpMethod method) {
	switch (method) {
	case Aws::Http::HttpMethod::HTTP_GET:
		return RequestType::GET_REQUEST;
	case Aws::Http::HttpMethod::HTTP_PUT:
		return RequestType::PUT_REQUEST;
	case Aws::Http::HttpMethod::HTTP_HEAD:
		return RequestType::HEAD_REQUEST;
	case Aws::Http::HttpMethod::HTTP_DELETE:
		return RequestType::DELETE_REQUEST;
	default:
		// Glue (JSON protocol) and STS (query protocol) both POST
		return RequestType::POST_REQUEST;
	}
}

//! HTTPUtil::DecomposeURL requires a '/' after the authority. The AWS SDK serializes some endpoints as a bare host
//! ("https://glue.eu-central-1.amazonaws.com"), so add the missing '/'. SigV4 canonicalizes an empty path to "/"
//! as well, so this stays consistent with what was signed.
string EnsureUrlHasPath(string url) {
	auto scheme_pos = url.find("://");
	idx_t authority_start = (scheme_pos == string::npos) ? 0 : scheme_pos + 3;
	auto sep_pos = url.find_first_of("/?#", authority_start);
	if (sep_pos == string::npos) {
		return url + "/";
	}
	if (url[sep_pos] != '/') {
		url.insert(sep_pos, "/");
	}
	return url;
}

//! Read the request body fully. The SDK already read the stream to hash it for the SigV4 payload signature, so
//! rewind before reading and again afterwards.
string ReadRequestBody(const std::shared_ptr<Aws::Http::HttpRequest> &request) {
	const auto &body = request->GetContentBody();
	if (!body) {
		return string();
	}
	body->clear();
	body->seekg(0, std::ios_base::beg);
	std::stringstream ss;
	ss << body->rdbuf();
	body->clear();
	body->seekg(0, std::ios_base::beg);
	return ss.str();
}

class GlueDuckDBHttpClient : public Aws::Http::HttpClient {
public:
	explicit GlueDuckDBHttpClient(DatabaseInstance &db_p) : db(db_p) {
	}

	std::shared_ptr<Aws::Http::HttpResponse>
	MakeRequest(const std::shared_ptr<Aws::Http::HttpRequest> &request, Aws::Utils::RateLimits::RateLimiterInterface *,
	            Aws::Utils::RateLimits::RateLimiterInterface *) const override {
		auto aws_response = Aws::MakeShared<Aws::Http::Standard::StandardHttpResponse>("GlueDuckDBHttp", request);
		try {
			auto &http_util = HTTPUtil::Get(db);
			string url = EnsureUrlHasPath(request->GetUri().GetURIString(true).c_str());

			// Parameters from the calling connection carry its logger, which is what makes the request show up in
			// that connection's HTTP log. SDK calls made outside a GlueHttpClientContextScope (e.g. credential
			// refreshes) fall back to the database level parameters.
			auto context = GlueHttpClientContextScope::Current();
			auto params =
			    context ? http_util.InitializeParameters(*context, url) : http_util.InitializeParameters(db, url);

			HTTPHeaders headers(db);
			for (const auto &header : request->GetHeaders()) {
				// DuckDB's client sets these itself, from the URL and the body
				auto lower = StringUtil::Lower(header.first.c_str());
				if (lower == "host" || lower == "content-length") {
					continue;
				}
				headers.Insert(header.first.c_str(), header.second.c_str());
			}

			string path;
			string proto_host_port;
			HTTPUtil::DecomposeURL(url, path, proto_host_port);
			auto client = http_util.InitializeClient(*params, proto_host_port);

			unique_ptr<HTTPResponse> response;
			string body_buffer;
			switch (ToDuckDBRequestType(request->GetMethod())) {
			case RequestType::GET_REQUEST: {
				GetRequestInfo info(
				    url, headers, *params, [](const HTTPResponse &) { return true; },
				    [&](const_data_ptr_t data, idx_t len) {
					    aws_response->GetResponseBody().write(const_char_ptr_cast(data), NumericCast<int64_t>(len));
					    return true;
				    });
				info.try_request = true;
				response = http_util.Request(info, client);
				break;
			}
			case RequestType::POST_REQUEST: {
				body_buffer = ReadRequestBody(request);
				PostRequestInfo info(url, headers, *params, const_data_ptr_cast(body_buffer.c_str()),
				                     body_buffer.size());
				info.try_request = true;
				response = http_util.Request(info, client);
				if (response) {
					aws_response->GetResponseBody().write(info.buffer_out.data(),
					                                      NumericCast<int64_t>(info.buffer_out.size()));
				}
				break;
			}
			case RequestType::PUT_REQUEST: {
				body_buffer = ReadRequestBody(request);
				string content_type = request->GetContentType().c_str();
				PutRequestInfo info(url, headers, *params, const_data_ptr_cast(body_buffer.c_str()), body_buffer.size(),
				                    content_type);
				info.try_request = true;
				response = http_util.Request(info, client);
				break;
			}
			case RequestType::HEAD_REQUEST: {
				HeadRequestInfo info(url, headers, *params);
				info.try_request = true;
				response = http_util.Request(info, client);
				break;
			}
			case RequestType::DELETE_REQUEST: {
				DeleteRequestInfo info(url, headers, *params);
				info.try_request = true;
				response = http_util.Request(info, client);
				break;
			}
			default:
				break;
			}

			if (!response) {
				aws_response->SetResponseCode(Aws::Http::HttpResponseCode::REQUEST_NOT_MADE);
				return aws_response;
			}
			aws_response->SetResponseCode(static_cast<Aws::Http::HttpResponseCode>(static_cast<int>(response->status)));
			for (const auto &header : response->headers) {
				aws_response->AddHeader(header.first.c_str(), header.second.c_str());
			}
			// Bodies that did not stream through a handler (PUT / HEAD / DELETE) are copied here
			if (!response->body.empty() && request->GetMethod() != Aws::Http::HttpMethod::HTTP_GET &&
			    request->GetMethod() != Aws::Http::HttpMethod::HTTP_POST) {
				aws_response->GetResponseBody().write(response->body.data(),
				                                      NumericCast<int64_t>(response->body.size()));
			}
		} catch (std::exception &ex) {
			aws_response->SetResponseCode(Aws::Http::HttpResponseCode::REQUEST_NOT_MADE);
			aws_response->SetClientErrorType(Aws::Client::CoreErrors::NETWORK_CONNECTION);
			aws_response->SetClientErrorMessage(ex.what());
		}
		return aws_response;
	}

private:
	DatabaseInstance &db;
};

class GlueDuckDBHttpClientFactory : public Aws::Http::HttpClientFactory {
public:
	explicit GlueDuckDBHttpClientFactory(DatabaseInstance &db_p) : db(db_p) {
	}

	std::shared_ptr<Aws::Http::HttpClient>
	CreateHttpClient(const Aws::Client::ClientConfiguration &config) const override {
		// Fall back to the SDK's own transport when disabled, or when there is no DuckDB HTTP transport yet
		// (httpfs provides it)
		if (!NetworkCallsViaDuckDB(db) || !db.ExtensionIsLoaded("httpfs")) {
#ifdef _WIN32
			return Aws::MakeShared<Aws::Http::WinHttpSyncHttpClient>("GlueDuckDBHttp", config);
#else
			return Aws::MakeShared<Aws::Http::CurlHttpClient>("GlueDuckDBHttp", config);
#endif
		}
		return Aws::MakeShared<GlueDuckDBHttpClient>("GlueDuckDBHttp", db);
	}

	std::shared_ptr<Aws::Http::HttpRequest>
	CreateHttpRequest(const Aws::String &uri, Aws::Http::HttpMethod method,
	                  const Aws::IOStreamFactory &stream_factory) const override {
		return CreateHttpRequest(Aws::Http::URI(uri), method, stream_factory);
	}

	std::shared_ptr<Aws::Http::HttpRequest>
	CreateHttpRequest(const Aws::Http::URI &uri, Aws::Http::HttpMethod method,
	                  const Aws::IOStreamFactory &stream_factory) const override {
		auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>("GlueDuckDBHttp", uri, method);
		request->SetResponseStreamFactory(stream_factory);
		return request;
	}

private:
	DatabaseInstance &db;
};

} // namespace

bool GlueNetworkCallsViaDuckDB(DatabaseInstance &db) {
	return NetworkCallsViaDuckDB(db);
}

void RegisterGlueHttpClientFactory(DatabaseInstance &db) {
	Aws::Http::SetHttpClientFactory(Aws::MakeShared<GlueDuckDBHttpClientFactory>("GlueDuckDBHttp", db));
}

} // namespace duckdb
