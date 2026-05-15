#include "code/framework_hints.h"

#include <set>
#include <unordered_map>
#include <unordered_set>

namespace vectis::code::hints {

namespace {

using Table = std::unordered_map<std::string_view, FrameworkHint>;

/// Annotations whose name unambiguously implies HTTP routing or web
/// app entry. Curated to keep false positives near zero — `Controller`
/// is included because the threshold (≥3 hits) suppresses a project
/// with one stray non-web `@Controller`; bare HTTP methods like `GET`
/// / `POST` are NOT included because they collide with unrelated
/// constants in many languages.
const std::unordered_set<std::string_view>& web_backend_annotations() noexcept
{
    static const std::unordered_set<std::string_view> t = {
        // Spring (Java) — single application marker + per-route mappings.
        "RestController",
        "Controller",
        "RequestMapping",
        "GetMapping",
        "PostMapping",
        "PutMapping",
        "DeleteMapping",
        "PatchMapping",
        "SpringBootApplication",
        "EnableWebMvc",
        "EnableWebFlux",
        // ASP.NET (C#) — controller marker + HTTP method attributes.
        "ApiController",
        "HttpGet",
        "HttpPost",
        "HttpPut",
        "HttpDelete",
        "HttpPatch",
        // Flask / Quart (Python) — route + per-method shortcuts on
        // both the app and any blueprint instance. The matcher strips
        // the call expression, so `@app.route("/")` lands as
        // `app.route`.
        "app.route",
        "app.get",
        "app.post",
        "app.put",
        "app.delete",
        "app.patch",
        "blueprint.route",
        // FastAPI / Starlette (Python) — router method shortcuts.
        "router.get",
        "router.post",
        "router.put",
        "router.delete",
        "router.patch",
        // Django REST framework — view marker.
        "api_view",
    };
    return t;
}

constexpr std::size_t k_min_annotation_count = 3;

/// Curated framework keywords for each ecosystem. Keys must match the
/// post-normalisation dep string exactly. Add new entries here when a
/// new framework demonstrably correlates with an architectural shape.
/// Avoid generic libraries (logging, JSON parsing, HTTP clients) — they
/// fire on too many projects to be useful corroborators.

const Table& npm_table()
{
    static const Table t = {
        // Browser-rendering frameworks (frontend).
        {"react", FrameworkHint::WebFrontend},
        {"react-dom", FrameworkHint::WebFrontend},
        {"vue", FrameworkHint::WebFrontend},
        {"svelte", FrameworkHint::WebFrontend},
        {"preact", FrameworkHint::WebFrontend},
        {"solid-js", FrameworkHint::WebFrontend},
        {"@angular/core", FrameworkHint::WebFrontend},
        // Meta-frameworks — SSR/SSG but primarily a frontend shape.
        {"next", FrameworkHint::WebFrontend},
        {"nuxt", FrameworkHint::WebFrontend},
        {"astro", FrameworkHint::WebFrontend},
        {"@remix-run/react", FrameworkHint::WebFrontend},
        {"@sveltejs/kit", FrameworkHint::WebFrontend},
        // Node backend frameworks.
        {"express", FrameworkHint::WebBackend},
        {"koa", FrameworkHint::WebBackend},
        {"fastify", FrameworkHint::WebBackend},
        {"@nestjs/core", FrameworkHint::WebBackend},
        {"@hapi/hapi", FrameworkHint::WebBackend},
        // Desktop wrappers.
        {"electron", FrameworkHint::DesktopUI},
        {"@tauri-apps/api", FrameworkHint::DesktopUI},
    };
    return t;
}

const Table& pyproject_table()
{
    static const Table t = {
        {"django", FrameworkHint::WebBackend},    {"flask", FrameworkHint::WebBackend},
        {"fastapi", FrameworkHint::WebBackend},   {"sanic", FrameworkHint::WebBackend},
        {"tornado", FrameworkHint::WebBackend},   {"bottle", FrameworkHint::WebBackend},
        {"pyramid", FrameworkHint::WebBackend},   {"falcon", FrameworkHint::WebBackend},
        {"aiohttp", FrameworkHint::WebBackend},   {"quart", FrameworkHint::WebBackend},
        {"starlette", FrameworkHint::WebBackend}, {"litestar", FrameworkHint::WebBackend},
        {"masonite", FrameworkHint::WebBackend},
    };
    return t;
}

const Table& cargo_table()
{
    static const Table t = {
        {"actix-web", FrameworkHint::WebBackend}, {"rocket", FrameworkHint::WebBackend},
        {"axum", FrameworkHint::WebBackend},      {"warp", FrameworkHint::WebBackend},
        {"poem", FrameworkHint::WebBackend},      {"tide", FrameworkHint::WebBackend},
        {"salvo", FrameworkHint::WebBackend},
    };
    return t;
}

const Table& go_table()
{
    // Module paths are normalised by stripping a trailing /vN before
    // lookup, so keys here use the un-versioned form.
    static const Table t = {
        {"github.com/gin-gonic/gin", FrameworkHint::WebBackend},
        {"github.com/labstack/echo", FrameworkHint::WebBackend},
        {"github.com/gofiber/fiber", FrameworkHint::WebBackend},
        {"github.com/gorilla/mux", FrameworkHint::WebBackend},
        {"github.com/go-chi/chi", FrameworkHint::WebBackend},
        {"github.com/julienschmidt/httprouter", FrameworkHint::WebBackend},
        {"github.com/beego/beego", FrameworkHint::WebBackend},
        {"github.com/revel/revel", FrameworkHint::WebBackend},
    };
    return t;
}

const Table& composer_table()
{
    static const Table t = {
        {"laravel/framework", FrameworkHint::WebBackend},
        {"symfony/framework-bundle", FrameworkHint::WebBackend},
        {"symfony/symfony", FrameworkHint::WebBackend},
        {"cakephp/cakephp", FrameworkHint::WebBackend},
        {"yiisoft/yii2", FrameworkHint::WebBackend},
        {"slim/slim", FrameworkHint::WebBackend},
        {"codeigniter/framework", FrameworkHint::WebBackend},
    };
    return t;
}

const Table& gemfile_table()
{
    static const Table t = {
        {"rails", FrameworkHint::WebBackend},  {"sinatra", FrameworkHint::WebBackend},
        {"hanami", FrameworkHint::WebBackend}, {"roda", FrameworkHint::WebBackend},
        {"grape", FrameworkHint::WebBackend},
    };
    return t;
}

const Table& maven_table()
{
    // Keys are post-normalised groupId:artifactId (no version).
    static const Table t = {
        {"org.springframework.boot:spring-boot-starter-web", FrameworkHint::WebBackend},
        {"org.springframework.boot:spring-boot-starter-webflux", FrameworkHint::WebBackend},
        {"org.springframework.boot:spring-boot-starter", FrameworkHint::WebBackend},
        {"org.springframework:spring-web", FrameworkHint::WebBackend},
        {"org.springframework:spring-webmvc", FrameworkHint::WebBackend},
        {"io.javalin:javalin", FrameworkHint::WebBackend},
        {"io.micronaut:micronaut-http-server", FrameworkHint::WebBackend},
        {"io.micronaut:micronaut-http-server-netty", FrameworkHint::WebBackend},
        {"io.quarkus:quarkus-resteasy", FrameworkHint::WebBackend},
        {"io.quarkus:quarkus-resteasy-reactive", FrameworkHint::WebBackend},
        {"jakarta.servlet:jakarta.servlet-api", FrameworkHint::WebBackend},
        {"javax.servlet:javax.servlet-api", FrameworkHint::WebBackend},
        {"io.vertx:vertx-web", FrameworkHint::WebBackend},
        {"com.sparkjava:spark-core", FrameworkHint::WebBackend},
        // JavaFX is a desktop UI toolkit.
        {"org.openjfx:javafx-controls", FrameworkHint::DesktopUI},
        {"org.openjfx:javafx-fxml", FrameworkHint::DesktopUI},
    };
    return t;
}

const Table& dotnet_table()
{
    // Keys are post-normalised PackageId (no version). Comparison is
    // case-sensitive — .NET package IDs use canonical capitalisation.
    static const Table t = {
        {"Microsoft.AspNetCore.App", FrameworkHint::WebBackend},
        {"Microsoft.AspNetCore.Mvc", FrameworkHint::WebBackend},
        {"Microsoft.AspNetCore.WebApi", FrameworkHint::WebBackend},
        {"Microsoft.AspNet.WebApi", FrameworkHint::WebBackend},
        {"NancyFx", FrameworkHint::WebBackend},
        // Desktop UI stacks.
        {"Avalonia", FrameworkHint::DesktopUI},
        {"Avalonia.Desktop", FrameworkHint::DesktopUI},
        {"Microsoft.WindowsAppSDK", FrameworkHint::DesktopUI},
        {"Microsoft.Maui.Controls", FrameworkHint::DesktopUI},
        {"WindowsBase", FrameworkHint::DesktopUI},
    };
    return t;
}

/// Strip a trailing `/v<digits>` major-version suffix from a Go module
/// path. `github.com/labstack/echo/v4` → `github.com/labstack/echo`.
/// Returns the input unchanged if no such suffix is present.
[[nodiscard]] std::string_view strip_go_version_suffix(std::string_view path) noexcept
{
    const std::size_t last_slash = path.rfind('/');
    if (last_slash == std::string_view::npos || last_slash + 2 > path.size()) {
        return path;
    }
    if (path[last_slash + 1] != 'v') {
        return path;
    }
    for (std::size_t i = last_slash + 2; i < path.size(); ++i) {
        if (path[i] < '0' || path[i] > '9') {
            return path;
        }
    }
    return path.substr(0, last_slash);
}

/// Normalise a single dep string for table lookup.
[[nodiscard]] std::string_view normalise_for(Ecosystem eco, std::string_view dep) noexcept
{
    switch (eco) {
    case Ecosystem::Maven: {
        // groupId:artifactId:version → groupId:artifactId.
        const std::size_t first = dep.find(':');
        if (first == std::string_view::npos) {
            return dep;
        }
        const std::size_t second = dep.find(':', first + 1);
        return second == std::string_view::npos ? dep : dep.substr(0, second);
    }
    case Ecosystem::DotNet: {
        // PackageId:Version → PackageId.
        const std::size_t colon = dep.find(':');
        return colon == std::string_view::npos ? dep : dep.substr(0, colon);
    }
    case Ecosystem::GoMod:
        return strip_go_version_suffix(dep);
    case Ecosystem::Npm:
    case Ecosystem::Pyproject:
    case Ecosystem::Cargo:
    case Ecosystem::Composer:
    case Ecosystem::Gemfile:
        return dep;
    }
    return dep;
}

[[nodiscard]] const Table& table_for(Ecosystem eco) noexcept
{
    switch (eco) {
    case Ecosystem::Npm:
        return npm_table();
    case Ecosystem::Pyproject:
        return pyproject_table();
    case Ecosystem::Cargo:
        return cargo_table();
    case Ecosystem::GoMod:
        return go_table();
    case Ecosystem::Composer:
        return composer_table();
    case Ecosystem::Gemfile:
        return gemfile_table();
    case Ecosystem::Maven:
        return maven_table();
    case Ecosystem::DotNet:
        return dotnet_table();
    }
    static const Table empty;
    return empty;
}

} // namespace

std::string_view hint_signal(FrameworkHint h) noexcept
{
    switch (h) {
    case FrameworkHint::WebBackend:
        return "hint:web-backend";
    case FrameworkHint::WebFrontend:
        return "hint:web-frontend";
    case FrameworkHint::DesktopUI:
        return "hint:desktop-ui";
    }
    return "hint:unknown";
}

std::vector<FrameworkHint> match(Ecosystem ecosystem, std::span<const std::string> deps)
{
    const Table& table = table_for(ecosystem);
    if (table.empty() || deps.empty()) {
        return {};
    }
    std::set<FrameworkHint> hits;
    for (const std::string& dep : deps) {
        const std::string_view key = normalise_for(ecosystem, dep);
        if (const auto it = table.find(key); it != table.end()) {
            hits.insert(it->second);
        }
    }
    return {hits.begin(), hits.end()};
}

namespace {

/// Strip a trailing call expression. `app.route("/users")` →
/// `app.route`, `HttpGet("/api")` → `HttpGet`, `RestController` is
/// returned unchanged.
[[nodiscard]] std::string_view annotation_name(std::string_view dec) noexcept
{
    const std::size_t paren = dec.find('(');
    return paren == std::string_view::npos ? dec : dec.substr(0, paren);
}

} // namespace

std::vector<FrameworkHint> match_annotations(std::span<const std::string> annotations)
{
    if (annotations.empty()) {
        return {};
    }
    const auto& table = web_backend_annotations();
    std::size_t web_backend_hits = 0;
    for (const std::string& dec : annotations) {
        if (table.contains(annotation_name(dec))) {
            ++web_backend_hits;
        }
    }
    std::vector<FrameworkHint> hits;
    if (web_backend_hits >= k_min_annotation_count) {
        hits.push_back(FrameworkHint::WebBackend);
    }
    return hits;
}

} // namespace vectis::code::hints
