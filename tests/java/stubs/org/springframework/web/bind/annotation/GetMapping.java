/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */
package org.springframework.web.bind.annotation;
import java.lang.annotation.*;

@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.METHOD})
public @interface GetMapping {
    String[] value() default {};
}
