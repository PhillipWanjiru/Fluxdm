/* FluxDM — tests/java/harness/MemDownloads.java
 * In-memory DownloadRepository that behaves like JPA with open-in-view off:
 * what is stored and what is returned are COPIES, so a test (or the service)
 * that forgets to call save() after changing an entity loses the change,
 * exactly as it would against the real database. */
package harness;

import com.fluxdm.db.DownloadRepository;
import com.fluxdm.model.Download;

import java.util.ArrayList;
import java.util.Collection;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

public final class MemDownloads implements DownloadRepository {

    private final Map<Long, Download> rows = new ConcurrentHashMap<>();
    private final AtomicLong seq = new AtomicLong();
    public final AtomicInteger saves = new AtomicInteger();

    public static Download copy(Download s) {
        Download d = new Download();
        d.setId(s.getId());
        d.setUrl(s.getUrl());
        d.setFilePath(s.getFilePath());
        d.setTotalSize(s.getTotalSize());
        d.setDownloaded(s.getDownloaded());
        d.setSegments(s.getSegments());
        d.setStatus(s.getStatus());
        d.setPriority(s.getPriority());
        d.setChecksum(s.getChecksum());
        d.setChecksumType(s.getChecksumType());
        d.setErrorMessage(s.getErrorMessage());
        d.setBandwidthLimit(s.getBandwidthLimit());
        return d;
    }

    @Override public List<Download> findAll() {
        List<Download> out = new ArrayList<>();
        for (Download d : rows.values()) out.add(copy(d));
        out.sort(Comparator.comparingLong(Download::getId));
        return out;
    }

    @Override public Optional<Download> findById(Long id) {
        Download d = rows.get(id);
        return d == null ? Optional.empty() : Optional.of(copy(d));
    }

    @SuppressWarnings("unchecked")
    @Override public <S extends Download> S save(S e) {
        saves.incrementAndGet();
        boolean isNew = e.getId() == null;
        if (isNew) e.setId(seq.incrementAndGet());
        rows.put(e.getId(), copy(e));
        return isNew ? e : (S) copy(e);
    }

    @Override public void deleteById(Long id) { rows.remove(id); }
    @Override public long count() { return rows.size(); }
    @Override public boolean existsById(Long id) { return rows.containsKey(id); }

    @Override public long countByStatus(int status) {
        return rows.values().stream().filter(d -> d.getStatus() == status).count();
    }

    @Override public List<Download> findByStatusIn(Collection<Integer> statuses) {
        List<Download> out = new ArrayList<>();
        for (Download d : rows.values()) if (statuses.contains(d.getStatus())) out.add(copy(d));
        return out;
    }

    @Override public boolean existsByFilePathAndStatusIn(String filePath, Collection<Integer> statuses) {
        return rows.values().stream().anyMatch(d -> filePath.equals(d.getFilePath()) && statuses.contains(d.getStatus()));
    }

    /** Test shortcut: store a row as it is (assigning an id if it has none). */
    public Download insert(Download d) { save(d); return findById(d.getId()).orElseThrow(); }

    public int status(long id) { return rows.get(id).getStatus(); }
}
