#pragma once

// What a failed OPDS feed fetch should tell the user (from upstream CrossPoint
// #3872). Pure: `status` is the HTTP status of the final answer, or 0 when no
// answer arrived.
enum class OpdsFeedError {
  Unreachable,   // DNS, TCP, TLS or timeout: no HTTP response at all
  Unauthorized,  // 401: the catalog wants credentials, or different ones
  Redirected,    // 401 from another origin after a redirect, where credentials are withheld
  ServerError,   // 5xx, including Cloudflare's 530 for an origin tunnel that is down
  Other,         // anything else; the generic "failed to fetch"
};

// `offOrigin`: the answer came from a different origin than the catalog's.
constexpr OpdsFeedError classifyFeedFailure(const int status, const bool offOrigin = false) {
  if (status <= 0) return OpdsFeedError::Unreachable;
  if (status == 401) return offOrigin ? OpdsFeedError::Redirected : OpdsFeedError::Unauthorized;
  if (status >= 500) return OpdsFeedError::ServerError;
  return OpdsFeedError::Other;
}

static_assert(classifyFeedFailure(0) == OpdsFeedError::Unreachable, "no answer");
static_assert(classifyFeedFailure(401) == OpdsFeedError::Unauthorized, "credentials asked for");
static_assert(classifyFeedFailure(401, true) == OpdsFeedError::Redirected, "credentials withheld");
static_assert(classifyFeedFailure(403) == OpdsFeedError::Other, "403 is often a WAF, not a password");
static_assert(classifyFeedFailure(530, true) == OpdsFeedError::ServerError, "Cloudflare tunnel down");
static_assert(classifyFeedFailure(404, true) == OpdsFeedError::Other, "redirect only changes the 401 message");
