/* FluxDM — ipc/PythonBridge.java */
package com.fluxdm.ipc;
import org.springframework.stereotype.Component;
import org.springframework.web.client.RestTemplate;
import java.util.Map;

@Component
public class PythonBridge {

    private static final String BASE_URL = "http://127.0.0.1:17172";
    private final RestTemplate rest = new RestTemplate();

    @SuppressWarnings("unchecked")
    public Map<String,Object> post(String path, Object body) {
        try {
            return rest.postForObject(BASE_URL + path, body, Map.class);
        } catch (Exception e) {
            return Map.of("error", e.getMessage());
        }
    }
}
