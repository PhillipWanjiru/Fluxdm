/* FluxDM — ipc/IPCEventListener.java
 * Callbacks for everything fluxdm-core tells the daemon on its own initiative,
 * plus the state of the connection itself. Register with
 * CoreIPCClient.addListener().
 *
 * All callbacks run on the client's single dispatcher thread, in the order the
 * core produced them, and never on the socket reader. A callback may do
 * ordinary blocking work (database writes) but should not wait on a command
 * future: the dispatcher thread is the one that completes those futures.
 */
package com.fluxdm.ipc;

public interface IPCEventListener {

    /** Newest progress of a running download (older undelivered ones are dropped). */
    default void onProgress(CoreIPCClient.ProgressInfo info) {}

    /** A download finished and, if a checksum was supplied, verified. */
    default void onComplete(CoreIPCClient.CompleteInfo info) {}

    /** A download failed after it was started (probe, network, disk, checksum, ...). */
    default void onError(long downloadId, int errorCode, String errorMessage) {}

    /** A row of the reply to a STATUS / STATUS_ALL command. */
    default void onStatus(CoreIPCClient.StatusInfo info) {}

    /** A connection to the core came up. Also called right after addListener()
     *  if the client is already connected. The core may be a fresh process that
     *  knows nothing about downloads the daemon believes are running. */
    default void onConnected() {}

    /** The connection dropped without disconnect() being called. Commands that
     *  were waiting for a reply have already failed with ERR_CONNECTION_LOST. */
    default void onConnectionLost(String reason) {}
}
