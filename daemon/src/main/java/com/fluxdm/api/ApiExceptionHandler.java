/* FluxDM — api/ApiExceptionHandler.java
 * Turns DownloadException into the HTTP status that fits it, with a small JSON
 * body {"error": "..."} the UI can show as it is. */
package com.fluxdm.api;
import com.fluxdm.service.DownloadException;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;
import java.util.Map;

@RestControllerAdvice
public class ApiExceptionHandler {

    @ExceptionHandler(DownloadException.class)
    public ResponseEntity<Map<String, String>> handle(DownloadException e) {
        HttpStatus status = switch (e.kind()) {
            case BAD_REQUEST -> HttpStatus.BAD_REQUEST;
            case NOT_FOUND   -> HttpStatus.NOT_FOUND;
            case CONFLICT    -> HttpStatus.CONFLICT;
            case UNAVAILABLE -> HttpStatus.SERVICE_UNAVAILABLE;
        };
        return ResponseEntity.status(status).body(Map.of("error", String.valueOf(e.getMessage())));
    }
}
