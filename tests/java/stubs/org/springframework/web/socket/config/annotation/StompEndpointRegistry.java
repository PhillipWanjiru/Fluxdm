/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.web.socket.config.annotation;

public interface StompEndpointRegistry {
    StompWebSocketEndpointRegistration addEndpoint(String... paths);
}
