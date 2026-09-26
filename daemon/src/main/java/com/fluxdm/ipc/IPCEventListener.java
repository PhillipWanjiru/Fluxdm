/* FluxDM — ipc/IPCEventListener.java
 * Callback interface for asynchronous events pushed by the C core
 * (progress, completion, error, status) over the persistent IPC connection.
 * Implementations (e.g. DownloadService) register via CoreIPCClient.addListener().
 */
package com.fluxdm.ipc;

public interface IPCEventListener {
    default void onProgress(CoreIPCClient.ProgressInfo info) {}
    default void onComplete(CoreIPCClient.CompleteInfo info) {}
    default void onError(int downloadId, int errorCode, String errorMessage) {}
    default void onStatus(CoreIPCClient.StatusInfo info) {}
}
