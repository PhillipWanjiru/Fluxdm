/* FluxDM — config/CorsConfig.java */
package com.fluxdm.config;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.web.servlet.config.annotation.*;

@Configuration
public class CorsConfig implements WebMvcConfigurer {
    @Override
    public void addCorsMappings(CorsRegistry registry) {
        registry.addMapping("/api/**")
                .allowedOrigins("http://localhost:3000", "app://-")
                .allowedMethods("GET","POST","PUT","DELETE","OPTIONS");
    }
}
