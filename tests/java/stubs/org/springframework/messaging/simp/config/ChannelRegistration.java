/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.messaging.simp.config;

import java.util.ArrayList;
import java.util.List;
import org.springframework.messaging.support.ChannelInterceptor;

public class ChannelRegistration {
    /** What was registered, so a test can look. */
    public final List<ChannelInterceptor> registered = new ArrayList<>();

    public ChannelRegistration interceptors(ChannelInterceptor... interceptors) {
        registered.addAll(List.of(interceptors));
        return this;
    }
}
