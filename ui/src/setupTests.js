/* react-scripts runs this file before every test file. */
import "@testing-library/jest-dom";

/* recharts' ResponsiveContainer measures its parent with a ResizeObserver, which jsdom does not have. */
global.ResizeObserver =
  global.ResizeObserver ||
  class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };
