/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.web.socket.config.annotation;

public class StompWebSocketEndpointRegistration {
    public StompWebSocketEndpointRegistration setAllowedOriginPatterns(String... patterns) { return this; }
    public StompWebSocketEndpointRegistration withSockJS() { return this; }
}
