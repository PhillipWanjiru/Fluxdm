/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.web.client;

public class RestTemplate {
    public <T> T postForObject(String url, Object request, Class<T> responseType, Object... uriVariables) { return null; }
}
