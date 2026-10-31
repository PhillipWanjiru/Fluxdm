/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.web.socket.config.annotation;

/** Records what was configured, so a test can look. */
public class StompWebSocketEndpointRegistration {
    public String[] origins        = {};
    public String[] originPatterns = {};
    public boolean  sockJs;

    public StompWebSocketEndpointRegistration setAllowedOrigins(String... origins)               { this.origins = origins; return this; }
    public StompWebSocketEndpointRegistration setAllowedOriginPatterns(String... originPatterns) { this.originPatterns = originPatterns; return this; }
    public StompWebSocketEndpointRegistration withSockJS()                                       { sockJs = true; return this; }
}
