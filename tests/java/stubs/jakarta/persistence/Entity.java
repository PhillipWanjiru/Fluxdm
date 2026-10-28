/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */
package jakarta.persistence;
import java.lang.annotation.*;

@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.TYPE})
public @interface Entity {
}
