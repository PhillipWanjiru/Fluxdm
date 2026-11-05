import { makeStore } from "./index";
import { loadSettings, persistSettings } from "./persist";
import { initialSettings, update } from "./settingsSlice";

const KEY = "fluxdm.settings.v1";

afterEach(() => {
  window.localStorage.clear();
  jest.restoreAllMocks();
});

describe("loadSettings", () => {
  test("nothing stored: nothing to override", () => {
    expect(loadSettings()).toEqual({});
  });

  test("returns what was stored", () => {
    window.localStorage.setItem(KEY, JSON.stringify({ downloadDir: "/data/dl", defaultSegments: 16, theme: "dark" }));
    expect(loadSettings()).toEqual({ downloadDir: "/data/dl", defaultSegments: 16, theme: "dark" });
  });

  test("drops values that are not usable and keeps the rest", () => {
    window.localStorage.setItem(KEY, JSON.stringify({ downloadDir: "relative/dir", defaultSegments: 500, theme: "pink", extra: 1 }));
    expect(loadSettings()).toEqual({});
    window.localStorage.setItem(KEY, JSON.stringify({ downloadDir: "~/Videos", defaultSegments: 2.5 }));
    expect(loadSettings()).toEqual({ downloadDir: "~/Videos" });
  });

  test("damaged text, or something that is not an object, is ignored", () => {
    for (const text of ["{not json", "null", "42", '"text"', "[1,2]"]) {
      window.localStorage.setItem(KEY, text);
      expect(loadSettings()).toEqual({});
    }
  });

  test("storage that throws is ignored", () => {
    jest.spyOn(Storage.prototype, "getItem").mockImplementation(() => {
      throw new Error("denied");
    });
    expect(loadSettings()).toEqual({});
  });
});

describe("persistSettings", () => {
  const fresh = () => {
    const store = makeStore({ settings: { ...initialSettings } });
    persistSettings(store);
    return store;
  };

  test("saves a changed setting, and only the ones the UI owns", () => {
    const store = fresh();
    store.dispatch(update({ defaultSegments: 16, scratch: 12345 }));
    expect(JSON.parse(window.localStorage.getItem(KEY))).toEqual({ downloadDir: "~/Downloads", defaultSegments: 16, theme: "dark" });
  });

  test("does not write while something else in the store changes", () => {
    const store = fresh();
    const setItem = jest.spyOn(Storage.prototype, "setItem");
    store.dispatch({ type: "downloads/eventReceived", payload: { type: "progress", id: 1 } });
    store.dispatch(update({ scratch: 5 })); // not stored
    expect(setItem).not.toHaveBeenCalled();
    store.dispatch(update({ downloadDir: "/data" }));
    expect(setItem).toHaveBeenCalledTimes(1);
  });

  test("a store that cannot write carries on", () => {
    const store = fresh();
    jest.spyOn(Storage.prototype, "setItem").mockImplementation(() => {
      throw new Error("quota");
    });
    expect(() => store.dispatch(update({ defaultSegments: 4 }))).not.toThrow();
    expect(store.getState().settings.defaultSegments).toBe(4);
  });

  test("what was saved is what the next start loads", () => {
    fresh().dispatch(update({ downloadDir: "/mnt/big", defaultSegments: 32 }));
    expect(loadSettings()).toEqual({ downloadDir: "/mnt/big", defaultSegments: 32, theme: "dark" });
  });
});
