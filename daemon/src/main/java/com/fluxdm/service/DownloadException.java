/* FluxDM — service/DownloadException.java
 * A failure a REST client should hear about, with the kind of failure it was.
 * The services throw it; api/ApiExceptionHandler turns it into an HTTP status
 * and a {"error": "..."} body. Keeping the HTTP vocabulary out of the services
 * lets them be tested without a web layer.
 */
package com.fluxdm.service;

public class DownloadException extends RuntimeException {

    private static final long serialVersionUID = 1L;

    public enum Kind { BAD_REQUEST, NOT_FOUND, CONFLICT, UNAVAILABLE }

    private final Kind kind;

    private DownloadException(Kind kind, String message) {
        super(message);
        this.kind = kind;
    }

    public Kind kind() { return kind; }

    /** The request itself is wrong (400). */
    public static DownloadException badRequest(String message) { return new DownloadException(Kind.BAD_REQUEST, message); }
    /** No such download (404). */
    public static DownloadException notFound(long id)          { return new DownloadException(Kind.NOT_FOUND, "no download with id " + id); }
    /** The download is in a state that does not allow this (409). */
    public static DownloadException conflict(String message)   { return new DownloadException(Kind.CONFLICT, message); }
    /** fluxdm-core cannot be reached or did not answer (503). */
    public static DownloadException unavailable(String message){ return new DownloadException(Kind.UNAVAILABLE, message); }
}
