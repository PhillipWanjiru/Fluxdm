/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.http;

public class ResponseEntity<T> {
    private final HttpStatus status;
    private final T body;
    ResponseEntity(HttpStatus status, T body) { this.status = status; this.body = body; }
    public HttpStatus getStatusCode() { return status; }
    public T getBody() { return body; }

    public static BodyBuilder status(HttpStatus status) { return new BodyBuilder(status); }

    public static final class BodyBuilder {
        private final HttpStatus status;
        BodyBuilder(HttpStatus status) { this.status = status; }
        public <T> ResponseEntity<T> body(T body) { return new ResponseEntity<>(status, body); }
    }
}
