/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.messaging.support;

import org.springframework.messaging.Message;
import org.springframework.messaging.MessageChannel;

public interface ChannelInterceptor {
    default Message<?> preSend(Message<?> message, MessageChannel channel) { return message; }
}
