/* A stand-in for the daemon behind axios' adapter, for tests: no sockets, but the request goes through the
 * client's real interceptors and the answer through its real error handling. Not a test file itself.
 *
 *   const daemon = installFakeDaemon(client, {
 *     "GET /downloads":      () => ({ data: [...] }),
 *     "POST /downloads/:id": ({ params }) => ({ status: 409, data: { error: "..." } }),
 *     "GET /health":         ({ config }) => { throw new AxiosError("Network Error", "ERR_NETWORK", config); },
 *   });
 *   ... await getAll() ...
 *   daemon.calls   what was asked: [{ method, path, query, body, headers, timeout, signal }]
 *
 * A handler gets { params (the :names in the route), query, body, headers, config } and returns
 * { status = 200, data }. A status outside 2xx rejects the way a real one does. */
import { AxiosError } from "axios";

function compile(route) {
  const [method, path] = route.split(" ");
  const names = [];
  const pattern = path.replace(/:([A-Za-z]+)/g, (_, name) => {
    names.push(name);
    return "([^/]+)";
  });
  return { method, names, regex: new RegExp(`^${pattern}$`) };
}

export function installFakeDaemon(client, routes) {
  const compiled = Object.entries(routes).map(([route, handler]) => ({ ...compile(route), handler }));
  const calls = [];

  client.defaults.adapter = async (config) => {
    const method = config.method.toUpperCase();
    const path = config.url;
    const body = typeof config.data === "string" && config.data ? JSON.parse(config.data) : config.data ?? null;
    calls.push({ method, path, query: config.params || {}, body, headers: config.headers, timeout: config.timeout, signal: config.signal });

    for (const { method: m, regex, names, handler } of compiled) {
      const match = m === method && regex.exec(path);
      if (!match) continue;
      const params = Object.fromEntries(names.map((name, i) => [name, match[i + 1]]));
      const { status = 200, data } = (await handler({ params, query: config.params || {}, body, headers: config.headers, config })) || {};
      const response = { data, status, statusText: String(status), headers: {}, config };
      if (config.validateStatus(status)) return response;
      throw new AxiosError(`Request failed with status code ${status}`, AxiosError.ERR_BAD_REQUEST, config, null, response);
    }
    throw new Error(`the fake daemon has no route for ${method} ${path}`);
  };

  return { calls };
}
