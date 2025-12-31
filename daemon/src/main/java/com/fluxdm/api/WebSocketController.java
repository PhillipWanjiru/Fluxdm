/* FluxDM — api/WebSocketController.java
 * STOMP message-mapped endpoints for client-initiated requests over the
 * WebSocket connection. Server-initiated push (progress/complete/error/
 * status events from the C core) does NOT go through here — those are
 * published directly to /topic/downloads and /topic/downloads/{id} by
 * DownloadService via SimpMessagingTemplate, since clients can subscribe
 * to broker-managed topics without any corresponding @MessageMapping.
 *
 * This controller only handles the reverse direction: the UI asking the
 * server to do something over the same WebSocket connection it's already
 * holding open, instead of opening a separate REST call.
 */
package com.fluxdm.api;

import com.fluxdm.ipc.CoreIPCClient;
import org.springframework.messaging.handler.annotation.DestinationVariable;
import org.springframework.messaging.handler.annotation.MessageMapping;
import org.springframework.stereotype.Controller;

@Controller
public class WebSocketController {

    private final CoreIPCClient ipcClient;

    public WebSocketController(CoreIPCClient ipcClient) {
        this.ipcClient = ipcClient;
    }

    /** Client sends to /app/status/{id} to request an immediate status push
     *  for one download; the reply arrives asynchronously on
     *  /topic/downloads/{id} as a normal status message. */
    @MessageMapping("/status/{id}")
    public void requestStatus(@DestinationVariable long id) {
        ipcClient.requestStatus(id);
    }

    /** Client sends to /app/status-all to request a status push for every
     *  download the core knows; replies arrive on /topic/downloads/{id} per item. */
    @MessageMapping("/status-all")
    public void requestStatusAll() {
        ipcClient.requestStatusAll();
    }
}
