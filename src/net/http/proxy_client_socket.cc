// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/proxy_client_socket.h"

#include <unordered_set>

#include "base/base64.h"
#include "crypto/random.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_errors.h"
#include "net/http/http_auth_controller.h"
#include "net/http/http_request_info.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_response_info.h"
#include "url/gurl.h"

namespace net {

void ProxyClientSocket::SetStreamPriority(RequestPriority priority) {}

// static
void ProxyClientSocket::BuildTunnelRequest(
    const HostPortPair& endpoint,
    const HttpRequestHeaders& extra_headers,
    const std::string& user_agent,
    std::string* request_line,
    HttpRequestHeaders* request_headers) {
  std::string host_and_port = endpoint.ToString();
  std::string ws_path;
  if (extra_headers.GetHeader("X-Websocket-Path", &ws_path)) {
    *request_line = "GET " + ws_path + " HTTP/1.1\r\n";

    request_headers->SetHeader("Upgrade", "websocket");
    request_headers->SetHeader("Connection", "Upgrade");
    request_headers->SetHeader("X-Connect-Host", host_and_port);

    std::string raw_challenge(16, '\0');
    crypto::RandBytes(std::data(raw_challenge), raw_challenge.length());
    std::string encoded_challenge;
    base::Base64Encode(raw_challenge, &encoded_challenge);
    request_headers->SetHeader("Sec-Websocket-Key", encoded_challenge);
  } else {
    // RFC 7230 Section 5.4 says a client MUST send a Host header
    // field in all HTTP/1.1 request messages, and Host SHOULD be the
    // first header field following the request-line.  Add
    // "Proxy-Connection: keep-alive" for compat with HTTP/1.0 proxies
    // such as Squid (required for NTLM authentication).
    *request_line =
        base::StringPrintf("CONNECT %s HTTP/1.1\r\n", host_and_port.c_str());
  }

  if (!user_agent.empty())
    request_headers->SetHeader(HttpRequestHeaders::kUserAgent, user_agent);

  request_headers->MergeFrom(extra_headers);
  if (!ws_path.empty()) {
      request_headers->RemoveHeader("X-Websocket-Path");
  }
}

// static
int ProxyClientSocket::HandleProxyAuthChallenge(
    HttpAuthController* auth,
    HttpResponseInfo* response,
    const NetLogWithSource& net_log) {
  DCHECK(response->headers.get());
  int rv = auth->HandleAuthChallenge(response->headers, response->ssl_info,
                                     false, true, net_log);
  auth->TakeAuthInfo(&response->auth_challenge);
  if (rv == OK)
    return ERR_PROXY_AUTH_REQUESTED;
  return rv;
}

// static
void ProxyClientSocket::SanitizeProxyAuth(HttpResponseInfo& response) {
  DCHECK(response.headers);

  // Copy status line and all hop-by-hop headers to preserve keep-alive
  // behavior.
  const char* kHeadersToKeep[] = {
      "connection",         "proxy-connection", "keep-alive", "trailer",
      "transfer-encoding",  "upgrade",

      "content-length",

      "proxy-authenticate",
  };

  // Create a list of all present header not in |kHeadersToKeep|, and then
  // remove them.
  size_t iter = 0;
  std::string header_name;
  std::string header_value;
  std::unordered_set<std::string> headers_to_remove;
  while (response.headers->EnumerateHeaderLines(&iter, &header_name,
                                                &header_value)) {
    bool remove = true;
    for (const char* header : kHeadersToKeep) {
      if (base::EqualsCaseInsensitiveASCII(header, header_name)) {
        remove = false;
        break;
      }
    }
    if (remove)
      headers_to_remove.insert(header_name);
  }

  response.headers->RemoveHeaders(headers_to_remove);
}

}  // namespace net
