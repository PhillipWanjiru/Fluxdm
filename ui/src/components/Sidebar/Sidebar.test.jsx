import React from "react";
import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Provider } from "react-redux";
import { makeStore } from "../../store";
import Sidebar from "./Sidebar";

const VIEWS = [
  { id: "downloads", label: "Downloads" },
  { id: "scheduler", label: "Scheduler" },
  { id: "settings", label: "Settings" },
];

function setup(active = "downloads") {
  const onNavigate = jest.fn();
  const user = userEvent.setup();
  render(
    <Provider store={makeStore()}>
      <Sidebar views={VIEWS} active={active} onNavigate={onNavigate} />
    </Provider>
  );
  return { onNavigate, user };
}

describe("the sidebar", () => {
  test("lists the views, in order, as the main navigation", () => {
    setup();
    const nav = screen.getByRole("navigation", { name: "Main" });
    expect(within(nav).getAllByRole("button").map((b) => b.textContent)).toEqual(["Downloads", "Scheduler", "Settings"]);
  });

  test("marks the view on show as the current page, and only that one", () => {
    setup("scheduler");
    const nav = screen.getByRole("navigation", { name: "Main" });
    expect(within(nav).getByRole("button", { name: "Scheduler" })).toHaveAttribute("aria-current", "page");
    for (const name of ["Downloads", "Settings"]) {
      expect(within(nav).getByRole("button", { name })).not.toHaveAttribute("aria-current");
    }
  });

  test("goes to the view that was clicked", async () => {
    const { onNavigate, user } = setup();
    await user.click(screen.getByRole("button", { name: "Settings" }));
    expect(onNavigate).toHaveBeenCalledWith("settings");
    expect(onNavigate).toHaveBeenCalledTimes(1);
  });

  test("is reached with the keyboard", async () => {
    const { onNavigate, user } = setup();
    await user.tab();
    expect(screen.getByRole("button", { name: "Downloads" })).toHaveFocus();
    await user.tab();
    await user.keyboard("{Enter}");
    expect(onNavigate).toHaveBeenCalledWith("scheduler");
  });

  test("shows the bandwidth and the daemon indicators under the navigation", () => {
    setup();
    expect(screen.getByText("BANDWIDTH")).toBeInTheDocument();
    expect(screen.getByText("DAEMON")).toBeInTheDocument();
  });
});
