/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.messaging.support;

import org.springframework.messaging.Message;

public class MessageHeaderAccessor {
    /** Where the stand-in Message keeps its accessor (the real one keeps it inside its header map). */
    public static final String ACCESSOR_KEY = "stub.accessor";

    public static <T extends MessageHeaderAccessor> T getAccessor(Message<?> message, Class<T> requiredType) {
        Object accessor = message.getHeaders().get(ACCESSOR_KEY);
        return requiredType != null && requiredType.isInstance(accessor) ? requiredType.cast(accessor) : null;
    }
}
