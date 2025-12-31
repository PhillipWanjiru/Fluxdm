/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.web.socket.config.annotation;

import org.springframework.messaging.simp.config.MessageBrokerRegistry;

public interface WebSocketMessageBrokerConfigurer {
    default void registerStompEndpoints(StompEndpointRegistry registry) {}
    default void configureMessageBroker(MessageBrokerRegistry registry) {}
}
