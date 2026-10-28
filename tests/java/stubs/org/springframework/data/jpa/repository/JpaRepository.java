/* Test-only stand-in for a third-party type. Just enough surface for the daemon to compile
 * without Maven Central; it is NOT the real library. */

package org.springframework.data.jpa.repository;

import java.util.List;
import java.util.Optional;

public interface JpaRepository<T, ID> {
    List<T> findAll();
    Optional<T> findById(ID id);
    <S extends T> S save(S entity);
    void deleteById(ID id);
    long count();
    boolean existsById(ID id);
}
