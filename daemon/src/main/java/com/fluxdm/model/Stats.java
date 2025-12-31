/* FluxDM — model/Stats.java */
package com.fluxdm.model;

public class Stats {
    private long total;
    private long active;
    private long queued;
    private long complete;
    private long speedBps;

    public long getTotal()    { return total; }
    public void setTotal(long t){ this.total = t; }
    public long getActive()   { return active; }
    public void setActive(long a){ this.active = a; }
    public long getQueued()   { return queued; }
    public void setQueued(long q){ this.queued = q; }
    public long getComplete() { return complete; }
    public void setComplete(long c){ this.complete = c; }
    public long getSpeedBps() { return speedBps; }
    public void setSpeedBps(long s){ this.speedBps = s; }
}
