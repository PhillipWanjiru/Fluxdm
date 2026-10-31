/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.messaging;

import java.util.HashMap;

public class MessageHeaders extends HashMap<String, Object> {
    private static final long serialVersionUID = 1L;
}
