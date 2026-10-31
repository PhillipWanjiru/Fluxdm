/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.core.annotation;

import java.lang.annotation.*;
import org.springframework.core.Ordered;

@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.TYPE, ElementType.METHOD, ElementType.FIELD})
public @interface Order {
    int value() default Ordered.LOWEST_PRECEDENCE;
}
