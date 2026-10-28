/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.http;

public enum HttpStatus { OK, BAD_REQUEST, NOT_FOUND, CONFLICT, SERVICE_UNAVAILABLE, INTERNAL_SERVER_ERROR }
