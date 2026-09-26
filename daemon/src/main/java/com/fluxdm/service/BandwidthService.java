/* FluxDM — service/BandwidthService.java */
package com.fluxdm.service;
import com.fluxdm.ipc.CoreIPCClient;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Service;

@Service
public class BandwidthService {

    @Autowired private CoreIPCClient ipcClient;

    private volatile long globalLimitBps = 0;

    public void setGlobalLimit(long bps) {
        this.globalLimitBps = bps;
        ipcClient.setLimit(0, bps);   /* id=0 → global, matches CMD_SET_LIMIT semantics */
    }

    public long getGlobalLimit() { return globalLimitBps; }
}
