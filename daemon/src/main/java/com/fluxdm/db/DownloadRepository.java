/* FluxDM — db/DownloadRepository.java */
package com.fluxdm.db;
import com.fluxdm.model.Download;
import org.springframework.data.jpa.repository.JpaRepository;
import org.springframework.stereotype.Repository;

@Repository
public interface DownloadRepository extends JpaRepository<Download, Long> {
    long countByStatus(int status);
}
