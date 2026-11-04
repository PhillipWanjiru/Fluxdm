/* react-scripts runs this file before every test file. */
import "@testing-library/jest-dom";
import { TextDecoder, TextEncoder } from "util";

/* recharts' ResponsiveContainer measures its parent with a ResizeObserver, which jsdom does not have. */
global.ResizeObserver =
  global.ResizeObserver ||
  class {
    observe() {}
    unobserve() {}
    disconnect() {}
  };

/* @stomp/stompjs encodes and decodes frames with TextEncoder and TextDecoder, which jsdom does not have. */
global.TextEncoder = global.TextEncoder || TextEncoder;
global.TextDecoder = global.TextDecoder || TextDecoder;
