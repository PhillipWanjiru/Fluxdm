/* FluxDM — api/GrabberController.java */
package com.fluxdm.api;
import com.fluxdm.service.GrabberService;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.web.bind.annotation.*;
import java.util.Map;

@RestController
@RequestMapping("/api/grabber")
public class GrabberController {
    @Autowired private GrabberService grabberService;

    @PostMapping("/detect")
    public Map<String,Object> detect(@RequestParam String url) {
        return grabberService.detect(url);
    }

    @PostMapping("/grab")
    public Map<String,Object> grab(@RequestParam String url,
                                    @RequestParam(defaultValue="best") String formatId) {
        return grabberService.grab(url, formatId);
    }
}
