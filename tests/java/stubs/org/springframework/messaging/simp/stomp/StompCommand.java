/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.messaging.simp.stomp;

public enum StompCommand {
    STOMP, CONNECT, DISCONNECT, SUBSCRIBE, UNSUBSCRIBE, SEND, ACK, NACK, BEGIN, COMMIT, ABORT,
    CONNECTED, RECEIPT, MESSAGE, ERROR
}
