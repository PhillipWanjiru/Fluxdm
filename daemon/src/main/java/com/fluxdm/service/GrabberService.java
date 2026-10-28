/* FluxDM — service/GrabberService.java */
package com.fluxdm.service;
import com.fluxdm.ipc.PythonBridge;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Service;
import java.util.Map;

@Service
public class GrabberService {

    @Autowired private PythonBridge pythonBridge;

    public Map<String,Object> detect(String url) {
        return pythonBridge.post("/detect?url=" + url, null);
    }

    public Map<String,Object> grab(String url, String formatId) {
        return pythonBridge.post("/grab?url=" + url + "&format_id=" + formatId, null);
    }
}
