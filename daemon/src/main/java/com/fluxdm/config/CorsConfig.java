/* FluxDM — config/CorsConfig.java
 * Which web pages may read the API's answers. The list is fluxdm.api.allowed-origins, the same one the request
 * filter enforces (security/ApiGuard), so the two cannot disagree. CORS only tells a browser what it may show to
 * a page: it is the filter that refuses the requests. */
package com.fluxdm.config;

import com.fluxdm.security.ApiGuard;
import org.springframework.context.annotation.Configuration;
import org.springframework.web.servlet.config.annotation.CorsRegistry;
import org.springframework.web.servlet.config.annotation.WebMvcConfigurer;

@Configuration
public class CorsConfig implements WebMvcConfigurer {

    private final ApiGuard guard;

    public CorsConfig(ApiGuard guard) { this.guard = guard; }

    @Override
    public void addCorsMappings(CorsRegistry registry) {
        registry.addMapping("/api/**")
                .allowedOrigins(guard.allowedOrigins())
                .allowedMethods("GET", "POST", "PUT", "DELETE", "OPTIONS")
                .allowedHeaders("Authorization", "Content-Type", "Accept")
                .maxAge(600);
    }
}
