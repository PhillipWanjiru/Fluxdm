/* FluxDM — FluxDMApplication.java */
package com.fluxdm;
import com.fluxdm.security.DataDir;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.scheduling.annotation.EnableScheduling;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

@SpringBootApplication
@EnableScheduling
public class FluxDMApplication {
    public static void main(String[] args) {
        ensureDataDirectory();
        SpringApplication.run(FluxDMApplication.class, args);
    }

    /**
     * SQLite's JDBC driver creates the .db FILE on first connect, but will
     * NOT create missing parent directories — it fails with a raw
     * SQLException instead. ~/.fluxdm never exists on a fresh machine, so
     * this must run before Spring's DataSource/HikariCP/Hibernate bean
     * initialization even starts, which is why it's the very first line
     * of main() rather than a @PostConstruct bean (those all run too late,
     * after the DataSource has already tried and failed to connect).
     *
     * The directory is also made private (mode 0700): the database in it
     * lists every URL and destination path, and it sits next to the API token.
     */
    private static void ensureDataDirectory() {
        Path dataDir = Path.of(System.getProperty("user.home"), ".fluxdm");
        try {
            if (DataDir.ensurePrivate(dataDir)) {
                System.err.println("[fluxdm-daemon] " + dataDir
                    + " was open to other users; it is now private (mode 0700)");
            }
        } catch (IOException e) {
            System.err.println("[fluxdm-daemon] WARNING: " + e.getMessage());
            if (!Files.isDirectory(dataDir)) {
                System.err.println("[fluxdm-daemon] SQLite connection will likely fail — "
                    + "create it manually: mkdir -p " + dataDir);
            }
        }
    }
}

