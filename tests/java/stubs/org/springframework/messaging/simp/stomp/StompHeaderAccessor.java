/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.messaging.simp.stomp;

import java.security.Principal;
import java.util.HashMap;
import java.util.Map;
import org.springframework.messaging.support.MessageHeaderAccessor;

public class StompHeaderAccessor extends MessageHeaderAccessor {
    private final StompCommand        command;
    private final Map<String, String> nativeHeaders = new HashMap<>();
    private Principal                 user;

    private StompHeaderAccessor(StompCommand command) { this.command = command; }

    public static StompHeaderAccessor create(StompCommand command) { return new StompHeaderAccessor(command); }

    public StompCommand getCommand() { return command; }

    public void setNativeHeader(String name, String value) { nativeHeaders.put(name, value); }
    public String getFirstNativeHeader(String name) { return nativeHeaders.get(name); }

    public void setUser(Principal user) { this.user = user; }
    public Principal getUser() { return user; }
}
