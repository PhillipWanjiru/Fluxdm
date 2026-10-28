/* FluxDM — db/DownloadRepository.java */
package com.fluxdm.db;
import com.fluxdm.model.Download;
import org.springframework.data.jpa.repository.JpaRepository;
import org.springframework.stereotype.Repository;

import java.util.Collection;
import java.util.List;

@Repository
public interface DownloadRepository extends JpaRepository<Download, Long> {
    long countByStatus(int status);

    List<Download> findByStatusIn(Collection<Integer> statuses);

    boolean existsByFilePathAndStatusIn(String filePath, Collection<Integer> statuses);
}
