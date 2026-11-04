import React from "react";
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import ConfirmModal from "./ConfirmModal";

function setup(props = {}) {
  const onConfirm = jest.fn();
  const onCancel = jest.fn();
  const user = userEvent.setup();
  render(<ConfirmModal title="Remove this download?" message="a.iso stops." confirmLabel="Remove" onConfirm={onConfirm} onCancel={onCancel} {...props} />);
  return { onConfirm, onCancel, user };
}

describe("the confirmation dialog", () => {
  test("is a dialog with a name and a description, and the safe choice has the focus", () => {
    setup();
    const dialog = screen.getByRole("alertdialog", { name: "Remove this download?" });
    expect(dialog).toHaveAttribute("aria-modal", "true");
    expect(dialog).toHaveAccessibleDescription("a.iso stops.");
    expect(screen.getByRole("button", { name: "Cancel" })).toHaveFocus();
  });

  test("confirms with the button that says what it does, and cancels with Cancel or Escape", async () => {
    const { onConfirm, onCancel, user } = setup();

    await user.click(screen.getByRole("button", { name: "Remove" }));
    expect(onConfirm).toHaveBeenCalledTimes(1);
    expect(onCancel).not.toHaveBeenCalled();

    await user.click(screen.getByRole("button", { name: "Cancel" }));
    await user.keyboard("{Escape}");
    expect(onCancel).toHaveBeenCalledTimes(2);
    expect(onConfirm).toHaveBeenCalledTimes(1);
  });

  test("Enter with the focus where it starts does not confirm", async () => {
    const { onConfirm, onCancel, user } = setup();
    await user.keyboard("{Enter}");
    expect(onConfirm).not.toHaveBeenCalled();
    expect(onCancel).toHaveBeenCalledTimes(1); // it pressed Cancel
  });

  test("has defaults for the title and the label", () => {
    setup({ title: undefined, confirmLabel: undefined });
    expect(screen.getByRole("alertdialog", { name: "Are you sure?" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Confirm" })).toBeInTheDocument();
  });
});
