import React from "react";
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Provider } from "react-redux";
import { makeStore } from "../../store";
import { initialSettings } from "../../store/settingsSlice";
import { loadSettings, persistSettings } from "../../store/persist";
import SettingsView from "./SettingsView";

afterEach(() => window.localStorage.clear());

function setup() {
  const store = makeStore({ settings: { ...initialSettings } });
  persistSettings(store);
  const user = userEvent.setup();
  render(
    <Provider store={store}>
      <SettingsView />
    </Provider>
  );
  return { store, user };
}

describe("storage settings", () => {
  test("the default folder can be changed, and is kept for the next start", async () => {
    const { store, user } = setup();
    await user.click(screen.getByRole("button", { name: "Storage" }));
    const input = screen.getByLabelText("DEFAULT DOWNLOAD FOLDER");
    expect(input).toHaveValue("~/Downloads");

    await user.clear(input);
    await user.type(input, "/mnt/big/dl");

    expect(store.getState().settings.downloadDir).toBe("/mnt/big/dl");
    expect(loadSettings().downloadDir).toBe("/mnt/big/dl");
    expect(input).toHaveAttribute("aria-invalid", "false");
  });

  test("says so when what is typed would not be kept", async () => {
    const { user } = setup();
    await user.click(screen.getByRole("button", { name: "Storage" }));
    const input = screen.getByLabelText("DEFAULT DOWNLOAD FOLDER");

    await user.clear(input);
    await user.type(input, "Downloads");

    expect(input).toHaveAttribute("aria-invalid", "true");
    expect(screen.getByText(/Not saved/)).toBeInTheDocument();
    expect(loadSettings().downloadDir).toBeUndefined(); // nothing unusable is stored: the next start uses the default
  });
});

describe("network settings", () => {
  test("the default segment count is chosen from the same list the dialog offers, and kept", async () => {
    const { store, user } = setup();
    const group = screen.getByRole("group", { name: "Default segments" });
    expect(group.querySelectorAll("button")).toHaveLength(4);
    expect(screen.getByRole("button", { name: "8×" })).toHaveAttribute("aria-pressed", "true");

    await user.click(screen.getByRole("button", { name: "16×" }));

    expect(screen.getByRole("button", { name: "16×" })).toHaveAttribute("aria-pressed", "true");
    expect(screen.getByRole("button", { name: "8×" })).toHaveAttribute("aria-pressed", "false");
    expect(store.getState().settings.defaultSegments).toBe(16);
    expect(loadSettings().defaultSegments).toBe(16);
  });
});
