/* FluxDM — api/errors.js
 * Every failed call to the daemon becomes an ApiError, whose message is fit to show the user as it is.
 *
 * The daemon answers refusals with a status and a body {"error": "..."} (api/ApiExceptionHandler): 400 for a
 * request it will not accept, 404 for an unknown download, 409 for a conflict, 503 while its core is not
 * ready. Before the request is looked at at all, 401 is a missing or wrong token and 403 a Host or Origin
 * that is not on the allow-list (SECURITY.md). */
import axios from "axios";
import { DAEMON_ORIGIN } from "./config";

export class ApiError extends Error {
  /* kind: bad_request | unauthorized | forbidden | not_found | conflict | unavailable | server | http
   *       | unreachable | timeout | cancelled | protocol | unknown */
  constructor(message, { kind = "unknown", status = null, cause } = {}) {
    super(message);
    this.name = "ApiError";
    this.kind = kind;
    this.status = status;
    if (cause) this.cause = cause;
  }
}

const KIND_OF_STATUS = {
  400: "bad_request",
  401: "unauthorized",
  403: "forbidden",
  404: "not_found",
  409: "conflict",
  503: "unavailable",
};

const FALLBACK_MESSAGE = {
  401: "The daemon did not accept this app's access token.",
  403: "The daemon refused the request: its Host or Origin is not on the allow-list.",
  404: "The daemon does not know that.",
  503: "The daemon is not ready (is fluxdm-core running?).",
};

const UNREACHABLE =
  `Cannot reach the FluxDM daemon at ${DAEMON_ORIGIN}. It may not be running, ` +
  "or it may have refused this page's origin (fluxdm.api.allowed-origins).";

function messageOf(status, body) {
  if (body && typeof body.error === "string" && body.error) return body.error;
  return FALLBACK_MESSAGE[status] || `The daemon answered HTTP ${status}.`;
}

/* Anything a request can fail with, as an ApiError. */
export function toApiError(error) {
  if (error instanceof ApiError) return error;
  if (axios.isCancel(error) || (error && error.code === "ERR_CANCELED")) {
    return new ApiError("Cancelled.", { kind: "cancelled", cause: error });
  }
  if (!axios.isAxiosError(error)) {
    return new ApiError((error && error.message) || "Unknown error.", { kind: "unknown", cause: error });
  }
  const { response } = error;
  if (response) {
    const status = response.status;
    const kind = KIND_OF_STATUS[status] || (status >= 500 ? "server" : "http");
    return new ApiError(messageOf(status, response.data), { kind, status, cause: error });
  }
  if (error.code === "ECONNABORTED" || error.code === "ETIMEDOUT") {
    return new ApiError("The daemon did not answer in time.", { kind: "timeout", cause: error });
  }
  return new ApiError(UNREACHABLE, { kind: "unreachable", cause: error });
}
