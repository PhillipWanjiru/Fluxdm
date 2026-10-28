/* FluxDM — core/include/version.h
 *
 * The one place the core's release version is written down. It is reported by
 * `fluxdm-core --version`, printed in the start-up banner and sent in the
 * User-Agent header of every request.
 *
 * Semantic Versioning (https://semver.org). Keep it in step with the daemon
 * (daemon/pom.xml) and the newest entry of CHANGELOG.md.
 */
#pragma once

#define FLUXDM_VERSION "0.1.0"
