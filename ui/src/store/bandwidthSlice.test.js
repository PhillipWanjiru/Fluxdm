import { makeStore } from "./index";
import { limitKnown, selectLimitBps } from "./bandwidthSlice";

describe("bandwidthSlice", () => {
  test("the limit is not known until the daemon has said", () => {
    expect(selectLimitBps(makeStore().getState())).toBeNull();
  });

  test("keeps what the daemon says, including no limit", () => {
    const store = makeStore();
    store.dispatch(limitKnown(2097152));
    expect(selectLimitBps(store.getState())).toBe(2097152);
    store.dispatch(limitKnown(0));
    expect(selectLimitBps(store.getState())).toBe(0);
  });
});
