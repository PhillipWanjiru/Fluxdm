/* FluxDM — security/StompAuthInterceptor.java
 * The token check for the WebSocket (STOMP) side of the API.
 *
 * A browser cannot put an Authorization header on a WebSocket handshake, so the token travels in the STOMP CONNECT
 * frame instead, as the native header "Authorization: Bearer <token>" (stompjs: connectHeaders). The handshake
 * request itself has already passed the Host and Origin checks (ApiGuardFilter).
 *
 *   CONNECT / STOMP   must carry the token; the session is then marked as authenticated (a user is set on it,
 *                     and Spring keeps it for every later frame of the session)
 *   DISCONNECT        always allowed
 *   heartbeats        always allowed
 *   anything else     (SEND, SUBSCRIBE, ...) only on an authenticated session
 *
 * A rejected frame makes Spring answer with a STOMP ERROR frame, and the client has to start over.
 */
package com.fluxdm.security;

import com.fluxdm.security.ApiGuard.Verdict;
import org.springframework.messaging.Message;
import org.springframework.messaging.MessageChannel;
import org.springframework.messaging.MessagingException;
import org.springframework.messaging.simp.stomp.StompCommand;
import org.springframework.messaging.simp.stomp.StompHeaderAccessor;
import org.springframework.messaging.support.ChannelInterceptor;
import org.springframework.messaging.support.MessageHeaderAccessor;

import java.security.Principal;

public final class StompAuthInterceptor implements ChannelInterceptor {

    /** Every authenticated session is the same user: the API has one token and no accounts. */
    private static final Principal API_USER = () -> "fluxdm-api";

    /** What makes Spring answer with a STOMP ERROR frame. Spring logs every frame refused this way together with the
     *  exception's stack trace. The refusal is by design and the trace says nothing, so this one carries none: a UI
     *  that keeps reconnecting with an old token then costs one short log entry per try instead of a page of them. */
    static final class Refused extends MessagingException {
        private static final long serialVersionUID = 1L;
        Refused(String message) { super(message); }
        @Override public Throwable fillInStackTrace() { return this; }
    }

    private final ApiGuard guard;

    public StompAuthInterceptor(ApiGuard guard) { this.guard = guard; }

    @Override
    public Message<?> preSend(Message<?> message, MessageChannel channel) {
        StompHeaderAccessor accessor = MessageHeaderAccessor.getAccessor(message, StompHeaderAccessor.class);
        StompCommand        command  = accessor == null ? null : accessor.getCommand();
        if (command == null) return message;                    // a heartbeat, or traffic that is not STOMP

        if (command == StompCommand.CONNECT || command == StompCommand.STOMP) {
            if (!guard.authorized(accessor.getFirstNativeHeader("Authorization"))) {
                guard.noteRejected(Verdict.UNAUTHORIZED, "STOMP " + command);
                throw new Refused(Verdict.UNAUTHORIZED.message());
            }
            accessor.setUser(API_USER);
        } else if (command != StompCommand.DISCONNECT && guard.authEnabled() && accessor.getUser() == null) {
            guard.noteRejected(Verdict.UNAUTHORIZED, "STOMP " + command + " before CONNECT");
            throw new Refused("not connected: send CONNECT with an Authorization header first");
        }
        return message;
    }
}
