import React from "react";
import { act, render, screen } from "@testing-library/react";
import { Provider } from "react-redux";
import { makeStore } from "../../store";
import { eventReceived, received } from "../../store/downloadsSlice";
import { limitKnown } from "../../store/bandwidthSlice";
import { fromDaemon } from "../../model/downloads";
import { daemonDownload } from "../../testing/fixtures";
import BandwidthMeter from "./BandwidthMeter";

const dl = (id, status, speedBps) => fromDaemon(daemonDownload({ id, status, speedBps, filePath: `/d/${id}.bin`, url: `https://example.com/${id}.bin` }));

function setup({ downloads = [], limit = null } = {}) {
  const store = makeStore();
  store.dispatch(received(downloads));
  if (limit !== null) store.dispatch(limitKnown(limit));
  render(
    <Provider store={store}>
      <BandwidthMeter />
    </Provider>
  );
  return { store, dispatch: (action) => act(() => void store.dispatch(action)) };
}

const MB = 1024 * 1024;

describe("the bandwidth meter", () => {
  test("shows the speed of the running downloads together, in the units the list uses", () => {
    setup({ downloads: [dl(1, 1, 1.5 * MB), dl(2, 1, 0.5 * MB), dl(3, 2, 9 * MB), dl(4, 3, 9 * MB)] });
    expect(screen.getByText("BANDWIDTH")).toBeInTheDocument();
    expect(screen.getByText(/↓ 2\.0 MB\/s/)).toBeInTheDocument();
  });

  test("shows zero when nothing is running", () => {
    setup();
    expect(screen.getByText(/↓ 0 B\/s/)).toBeInTheDocument();
  });

  test("follows the downloads as they report progress", () => {
    const { dispatch } = setup({ downloads: [dl(1, 1, 1 * MB)] });
    expect(screen.getByText(/↓ 1\.0 MB\/s/)).toBeInTheDocument();
    dispatch(eventReceived({ type: "progress", id: 1, status: 1, totalBytes: 1000, downloadedBytes: 500, speedBps: 3 * MB, etaSeconds: 1, activeSegments: 4 }));
    expect(screen.getByText(/↓ 3\.0 MB\/s/)).toBeInTheDocument();
  });

  describe("against the limit on all downloads together", () => {
    test("says how much of it is in use, with a meter", () => {
      setup({ downloads: [dl(1, 1, 1 * MB)], limit: 4 * MB });
      expect(screen.getByText(/↓ 1\.0 MB\/s/)).toHaveTextContent("↓ 1.0 MB/s of 4.0 MB/s");
      const meter = screen.getByRole("meter", { name: "Bandwidth in use" });
      expect(meter).toHaveAttribute("aria-valuemin", "0");
      expect(meter).toHaveAttribute("aria-valuemax", String(4 * MB));
      expect(meter).toHaveAttribute("aria-valuenow", String(1 * MB));
      expect(meter).toHaveAttribute("aria-valuetext", "1.0 MB/s of 4.0 MB/s");
      expect(screen.getByTestId("bandwidth-fill")).toHaveStyle({ width: "25%" });
    });

    test("never shows more than all of it, however big a burst", () => {
      setup({ downloads: [dl(1, 1, 6 * MB)], limit: 4 * MB });
      const meter = screen.getByRole("meter");
      expect(meter).toHaveAttribute("aria-valuenow", String(4 * MB));
      expect(screen.getByTestId("bandwidth-fill")).toHaveStyle({ width: "100%" });
      expect(screen.getByText(/↓ 6\.0 MB\/s/)).toBeInTheDocument(); /* the speed itself is not clipped */
    });

    test("an empty meter when nothing runs", () => {
      setup({ limit: 4 * MB });
      expect(screen.getByRole("meter")).toHaveAttribute("aria-valuenow", "0");
      expect(screen.getByTestId("bandwidth-fill")).toHaveStyle({ width: "0%" });
    });

    test("without a limit there is nothing to measure against: no meter, and it says so", () => {
      setup({ downloads: [dl(1, 1, 1 * MB)], limit: 0 });
      expect(screen.queryByRole("meter")).not.toBeInTheDocument();
      expect(screen.getByText("No limit")).toBeInTheDocument();
    });

    test("until the daemon has said, it claims neither", () => {
      setup({ downloads: [dl(1, 1, 1 * MB)] });
      expect(screen.queryByRole("meter")).not.toBeInTheDocument();
      expect(screen.queryByText("No limit")).not.toBeInTheDocument();
    });

    test("moves when the limit is changed", () => {
      const { dispatch } = setup({ downloads: [dl(1, 1, 1 * MB)], limit: 4 * MB });
      dispatch(limitKnown(2 * MB));
      expect(screen.getByRole("meter")).toHaveAttribute("aria-valuemax", String(2 * MB));
      expect(screen.getByTestId("bandwidth-fill")).toHaveStyle({ width: "50%" });
      dispatch(limitKnown(0));
      expect(screen.queryByRole("meter")).not.toBeInTheDocument();
    });
  });
});
