/* FluxDM — model/Download.java */
package com.fluxdm.model;
import jakarta.persistence.*;

@Entity
@Table(name = "downloads")
public class Download {

    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long    id;
    private String  url;
    private String  filePath;
    private Long    totalSize   = 0L;
    private Long    downloaded  = 0L;
    private Integer segments    = 8;
    private Integer status      = 0;  /* 0=pending 1=running 2=paused 3=done 4=error */
    private Integer priority    = 5;  /* 1..10, higher starts first when downloads are queued */
    private String  checksum;         /* hex digest to verify the finished file against, if any */
    private Integer checksumType = 0; /* 0=none 1=md5 2=sha256 */
    private String  errorMessage;     /* why the download failed (text from fluxdm-core), if it did */
    private Long    bandwidthLimit = 0L;  /* bytes/s for this download, 0 = unlimited */

    /* Live speed in bytes/s while the download is running. Not stored: filled in
     * from the latest progress event whenever a download is read through the API. */
    @Transient
    private long    speedBps;

    /* getters + setters */
    public Long    getId()          { return id; }
    public void    setId(Long id)   { this.id = id; }
    public String  getUrl()         { return url; }
    public void    setUrl(String u) { this.url = u; }
    public String  getFilePath()    { return filePath; }
    public void    setFilePath(String p){ this.filePath = p; }
    public Long    getTotalSize()   { return totalSize; }
    public void    setTotalSize(Long s){ this.totalSize = s; }
    public Long    getDownloaded()  { return downloaded; }
    public void    setDownloaded(Long d){ this.downloaded = d; }
    public Integer getSegments()    { return segments; }
    public void    setSegments(Integer s){ this.segments = s; }
    public Integer getStatus()      { return status; }
    public void    setStatus(Integer s){ this.status = s; }
    public Integer getPriority()    { return priority; }
    public void    setPriority(Integer p){ this.priority = p; }
    public String  getChecksum()    { return checksum; }
    public void    setChecksum(String c){ this.checksum = c; }
    public Integer getChecksumType(){ return checksumType; }
    public void    setChecksumType(Integer t){ this.checksumType = t; }
    public String  getErrorMessage(){ return errorMessage; }
    public void    setErrorMessage(String m){ this.errorMessage = m; }
    public Long    getBandwidthLimit(){ return bandwidthLimit != null ? bandwidthLimit : 0L; }
    public void    setBandwidthLimit(Long b){ this.bandwidthLimit = b; }
    public long    getSpeedBps()    { return speedBps; }
    public void    setSpeedBps(long s){ this.speedBps = s; }
}
