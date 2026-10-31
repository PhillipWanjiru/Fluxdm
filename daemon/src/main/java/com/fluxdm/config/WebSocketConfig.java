/* FluxDM — config/WebSocketConfig.java
 * STOMP over SockJS at /ws. Who may connect:
 *   - the handshake is only accepted from the origins in fluxdm.api.allowed-origins (the request filter has already
 *     checked Host and Origin; this is the second lock on the same door);
 *   - the CONNECT frame has to carry the API token (security/StompAuthInterceptor). */
package com.fluxdm.config;

import com.fluxdm.security.ApiGuard;
import com.fluxdm.security.StompAuthInterceptor;
import org.springframework.context.annotation.Configuration;
import org.springframework.messaging.simp.config.ChannelRegistration;
import org.springframework.messaging.simp.config.MessageBrokerRegistry;
import org.springframework.web.socket.config.annotation.EnableWebSocketMessageBroker;
import org.springframework.web.socket.config.annotation.StompEndpointRegistry;
import org.springframework.web.socket.config.annotation.WebSocketMessageBrokerConfigurer;

@Configuration
@EnableWebSocketMessageBroker
public class WebSocketConfig implements WebSocketMessageBrokerConfigurer {

    private final ApiGuard guard;

    public WebSocketConfig(ApiGuard guard) { this.guard = guard; }

    @Override
    public void registerStompEndpoints(StompEndpointRegistry registry) {
        registry.addEndpoint("/ws").setAllowedOrigins(guard.allowedOrigins()).withSockJS();
    }

    @Override
    public void configureClientInboundChannel(ChannelRegistration registration) {
        registration.interceptors(new StompAuthInterceptor(guard));
    }

    @Override
    public void configureMessageBroker(MessageBrokerRegistry config) {
        config.enableSimpleBroker("/topic");
        config.setApplicationDestinationPrefixes("/app");
    }
}
