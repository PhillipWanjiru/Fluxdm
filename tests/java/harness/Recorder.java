/* FluxDM — tests/java/harness/Recorder.java
 * Records everything the daemon pushes to the UI (stands in for SimpMessagingTemplate). */
package harness;

import org.springframework.messaging.simp.SimpMessagingTemplate;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;

public final class Recorder extends SimpMessagingTemplate {

    public record Msg(String destination, Map<?, ?> payload) {
        public String type()   { return String.valueOf(payload.get("type")); }
        public long   id()     { Object o = payload.get("id"); return o instanceof Number n ? n.longValue() : -1; }
        public Object get(String k) { return payload.get(k); }
    }

    public final List<Msg> all = new CopyOnWriteArrayList<>();

    @Override public void convertAndSend(String destination, Object payload) {
        if (payload instanceof Map<?, ?> m) all.add(new Msg(destination, m));
    }

    /** Messages on the global topic of the given type, optionally for one download. */
    public List<Msg> of(String type, long id) {
        List<Msg> out = new ArrayList<>();
        for (Msg m : all)
            if (m.destination().equals("/topic/downloads") && m.type().equals(type) && (id < 0 || m.id() == id)) out.add(m);
        return out;
    }

    public List<Msg> states(long id) { return of("state", id); }

    /** Statuses a download was announced as, in order. */
    public List<Integer> stateHistory(long id) {
        List<Integer> out = new ArrayList<>();
        for (Msg m : states(id)) out.add(((Number) m.get("status")).intValue());
        return out;
    }
}
