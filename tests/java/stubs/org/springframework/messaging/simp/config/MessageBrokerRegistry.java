/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.messaging.simp.config;

public class MessageBrokerRegistry {
    public Object enableSimpleBroker(String... destinationPrefixes) { return null; }
    public MessageBrokerRegistry setApplicationDestinationPrefixes(String... prefixes) { return this; }
}
