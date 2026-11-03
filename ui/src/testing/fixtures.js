/* Shared by the tests: what the daemon sends. Not a test file itself. */

/* One Download as GET /api/downloads returns it (model/Download.java). */
export const daemonDownload = (over = {}) => ({
  id: 1,
  url: "https://example.com/files/ubuntu-24.04.iso",
  filePath: "/home/user/Downloads/ubuntu-24.04.iso",
  totalSize: 1000,
  downloaded: 250,
  segments: 8,
  status: 1,
  priority: 5,
  checksum: null,
  checksumType: 0,
  errorMessage: null,
  bandwidthLimit: 0,
  speedBps: 100,
  ...over,
});

/* One download in each state, as the REST list would carry them. */
export const daemonList = () => [
  daemonDownload({ id: 1, status: 1, filePath: "/d/a.iso", url: "https://example.com/a.iso" }),
  daemonDownload({ id: 2, status: 2, filePath: "/d/b.zip", url: "https://example.com/b.zip", speedBps: 0 }),
  daemonDownload({ id: 3, status: 0, filePath: "/d/c.mp4", url: "https://example.com/c.mp4", downloaded: 0, speedBps: 0 }),
  daemonDownload({ id: 4, status: 3, filePath: "/d/d.pdf", url: "https://example.com/d.pdf", downloaded: 1000, speedBps: 0 }),
  daemonDownload({
    id: 5, status: 4, filePath: "/d/e.bin", url: "https://example.com/e.bin", speedBps: 0,
    errorMessage: "connection reset by peer",
  }),
];
