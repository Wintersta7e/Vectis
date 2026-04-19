#include "core/app.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include "core/config_manager.h"
#include "core/context_bus.h"
#include "core/log.h"
#include "core/mode.h"
#include "core/service_registry.h"
#include "platform/file_io.h"
#include "services/index_engine/index_engine.h"
#include "services/storage_engine/storage_engine.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace vectis::core {

namespace {

constexpr const char* k_version           = "0.1.0";
constexpr const char* k_window_title      = "Vectis v0.1.0";
constexpr int         k_window_width      = 1280;
constexpr int         k_window_height     = 720;
constexpr const char* k_glsl_version      = "#version 150";
constexpr const char* k_config_file_name  = "vectis.toml";

/// Bitmask flags tracking which initialization steps completed, so
/// shutdown_internal() can safely unwind partial init failures without
/// calling teardown on uninitialized subsystems.
enum InitStage : std::uint32_t {
    Stage_None        = 0,
    Stage_DataDir     = 1U << 0U,
    Stage_Log         = 1U << 1U,
    Stage_Services    = 1U << 2U,
    Stage_Config      = 1U << 3U,
    Stage_Storage     = 1U << 4U,
    Stage_Sdl         = 1U << 5U,
    Stage_Window      = 1U << 6U,
    Stage_GlContext   = 1U << 7U,
    Stage_ImGui       = 1U << 8U,
    Stage_ImGuiSdl    = 1U << 9U,
    Stage_ImGuiGl     = 1U << 10U,
    Stage_Modes       = 1U << 11U,
};

} // namespace

struct App::Impl {
    SDL_Window*                           window     = nullptr;
    SDL_GLContext                         gl_context = nullptr;
    std::unique_ptr<ServiceRegistry>      services;
    std::vector<std::unique_ptr<IMode>>   modes;
    std::size_t                           active_mode = 0;
    bool                                  running    = false;
    std::uint32_t                         init_stage = Stage_None;

    [[nodiscard]] bool has_stage(InitStage stage) const noexcept
    {
        return (init_stage & stage) != 0U;
    }

    void mark_stage(InitStage stage) noexcept
    {
        init_stage |= stage;
    }
};

App::App() : m_impl(std::make_unique<Impl>()) {}

App::~App()
{
    // Guard against leaks if initialize() failed or caller forgot to run().
    if (m_impl && m_impl->init_stage != Stage_None) {
        shutdown_internal();
    }
}

void App::register_mode(std::unique_ptr<IMode> mode)
{
    if (mode) {
        m_impl->modes.push_back(std::move(mode));
    }
}

void App::switch_mode(std::string_view mode_id)
{
    for (std::size_t i = 0; i < m_impl->modes.size(); ++i) {
        if (m_impl->modes[i]->id() == mode_id) {
            if (i != m_impl->active_mode && m_impl->active_mode < m_impl->modes.size()) {
                m_impl->modes[m_impl->active_mode]->on_deactivate();
            }
            m_impl->active_mode = i;
            m_impl->modes[i]->on_activate();
            return;
        }
    }
}

bool App::initialize()
{
    // 1. Data dir ----------------------------------------------------------
    auto data_dir_result = vectis::platform::default_data_dir();
    if (!data_dir_result) {
        // No logger yet — fall back to stderr directly.
        std::fprintf(
            stderr,
            "[vectis] fatal: %s (%s)\n",
            data_dir_result.error().message.c_str(),
            data_dir_result.error().context.c_str());
        return false;
    }
    const std::filesystem::path data_dir = *data_dir_result;

    if (auto r = vectis::platform::ensure_dir(data_dir); !r) {
        std::fprintf(
            stderr,
            "[vectis] fatal: %s (%s)\n",
            r.error().message.c_str(),
            r.error().context.c_str());
        return false;
    }
    m_impl->mark_stage(Stage_DataDir);

    // 2. Logger ------------------------------------------------------------
    if (auto r = log::init(data_dir); !r) {
        std::fprintf(
            stderr,
            "[vectis] fatal: log init failed: %s (%s)\n",
            r.error().message.c_str(),
            r.error().context.c_str());
        return false;
    }
    m_impl->mark_stage(Stage_Log);

    VECTIS_LOG_INFO("Vectis {} starting", k_version);
    VECTIS_LOG_INFO("Data directory: {}", data_dir.string());

    // 3. Services ----------------------------------------------------------
    m_impl->services = std::make_unique<ServiceRegistry>();
    m_impl->mark_stage(Stage_Services);

    // 4. Config ------------------------------------------------------------
    auto exe_dir_result = vectis::platform::executable_dir();
    if (!exe_dir_result) {
        VECTIS_LOG_ERROR(
            "failed to resolve executable dir: {}", exe_dir_result.error().message);
        return false;
    }
    const std::filesystem::path config_path = *exe_dir_result / k_config_file_name;

    if (auto r = m_impl->services->config().load(config_path); !r) {
        VECTIS_LOG_ERROR(
            "config load failed: [{}] {}",
            error_kind_to_string(r.error().kind),
            r.error().message);
        return false;
    }
    m_impl->mark_stage(Stage_Config);

    // Warn about ignored override (Step 1 uses default data_dir always).
    const std::string override_dir =
        m_impl->services->config().get_string("general.data_dir", "");
    if (!override_dir.empty() && override_dir != "./vectis-data") {
        VECTIS_LOG_WARN(
            "general.data_dir='{}' is ignored in this build step; using '{}'",
            override_dir, data_dir.string());
    }

    // 4b. Storage -----------------------------------------------------------
    {
        const std::filesystem::path db_path = data_dir / "vectis.db";
        if (auto r = m_impl->services->storage().open(db_path); !r) {
            VECTIS_LOG_ERROR(
                "storage engine open failed: {}", r.error().message);
            return false;
        }
        if (auto r = m_impl->services->storage().migrate(); !r) {
            VECTIS_LOG_ERROR(
                "storage migration failed: {}", r.error().message);
            return false;
        }
        m_impl->mark_stage(Stage_Storage);

        m_impl->services->index().initialize(m_impl->services->storage());
    }

    // 5. SDL2 --------------------------------------------------------------
    // Make the process DPI-aware on Windows BEFORE SDL_Init. Without this,
    // Windows reports virtual (100 %-scaled) window dimensions and does a
    // blurry DWM upscale for everything we draw. SDL 2.24+ accepts this
    // hint; older SDLs ignore it harmlessly.
#ifdef _WIN32
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        VECTIS_LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    m_impl->mark_stage(Stage_Sdl);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    const SDL_WindowFlags window_flags = static_cast<SDL_WindowFlags>(
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    // Pick a window size proportional to the primary display's usable
    // area so 4K-at-100 % users don't get a tiny 1280x720 window and
    // 1080p users don't get something that barely fits on screen.
    int window_w = k_window_width;
    int window_h = k_window_height;
    SDL_Rect usable_bounds{};
    if (SDL_GetDisplayUsableBounds(0, &usable_bounds) == 0 &&
        usable_bounds.w > 0 && usable_bounds.h > 0)
    {
        window_w = std::max(k_window_width,  (usable_bounds.w * 7) / 10);
        window_h = std::max(k_window_height, (usable_bounds.h * 7) / 10);
    }

    m_impl->window = SDL_CreateWindow(
        k_window_title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        window_w, window_h,
        window_flags);
    if (m_impl->window == nullptr) {
        VECTIS_LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    m_impl->mark_stage(Stage_Window);

    m_impl->gl_context = SDL_GL_CreateContext(m_impl->window);
    if (m_impl->gl_context == nullptr) {
        VECTIS_LOG_ERROR("SDL_GL_CreateContext failed: {}", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(m_impl->window, m_impl->gl_context);
    SDL_GL_SetSwapInterval(1); // vsync
    m_impl->mark_stage(Stage_GlContext);

    // 6. ImGui -------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    m_impl->mark_stage(Stage_ImGui);

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ui::apply_vectis_theme(ImGui::GetStyle());

    // HiDPI + font loading.
    //
    // ImGui's bundled ProggyClean was designed for 13 px — scaling it up
    // produces a blurry, pixelated glyph set that's a lot of the "ugly UI"
    // complaint. Load a system TrueType font instead (Segoe UI on Windows,
    // DejaVu on most Linux distros) and fall back to the default only when
    // neither exists. Base size bumped from 13 to 16 for readability on
    // 4K displays; `ui.font_scale` in vectis.toml lets users tune further.
    {
        int win_w = 0;
        int win_h = 0;
        int fb_w  = 0;
        int fb_h  = 0;
        SDL_GetWindowSize(m_impl->window, &win_w, &win_h);
        SDL_GL_GetDrawableSize(m_impl->window, &fb_w, &fb_h);

        // Two DPI signals combine:
        //   1. drawable/window ratio — non-1.0 when Windows display
        //      scaling is >100 % (SDL reports virtual logical size).
        //   2. physical display DPI via SDL_GetDisplayDPI — picks up
        //      "4K-at-100 %" setups where (1) gives 1.0 but the panel's
        //      163 DPI makes 16 px text a pinpoint.
        // Use the larger of the two so neither blind spot leaves us
        // rendering postage-stamp fonts.
        float dpi_scale = (win_w > 0)
            ? static_cast<float>(fb_w) / static_cast<float>(win_w)
            : 1.0F;

        const int display_index = SDL_GetWindowDisplayIndex(m_impl->window);
        float     hdpi          = 0.0F;
        if (display_index >= 0 &&
            SDL_GetDisplayDPI(display_index, nullptr, &hdpi, nullptr) == 0 &&
            hdpi > 1.0F)
        {
            constexpr float k_reference_dpi = 96.0F;
            dpi_scale = std::max(dpi_scale, hdpi / k_reference_dpi);
        }

        const double user_scale_d =
            m_impl->services->config().get_double("ui.font_scale", 1.0);
        const float user_scale = (user_scale_d > 0.1 && user_scale_d < 8.0)
            ? static_cast<float>(user_scale_d)
            : 1.0F;

        constexpr float k_base_font_px = 16.0F;
        const float     font_px        = k_base_font_px * dpi_scale * user_scale;

        // Portable TTF probe list. First existing file wins. Kept short
        // so the common happy path is fast.
        static constexpr const char* k_font_candidates[] = {
#ifdef _WIN32
            "C:\\Windows\\Fonts\\segoeui.ttf",
            "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
            "/System/Library/Fonts/SFNS.ttf",
            "/Library/Fonts/Arial.ttf",
#else
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
#endif
        };

        ImFont*     loaded_font = nullptr;
        const char* loaded_path = nullptr;
        for (const char* candidate : k_font_candidates) {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) {
                loaded_font = io.Fonts->AddFontFromFileTTF(candidate, font_px);
                if (loaded_font != nullptr) {
                    loaded_path = candidate;
                    break;
                }
            }
        }
        if (loaded_font == nullptr) {
            ImFontConfig cfg;
            cfg.SizePixels = font_px;
            io.Fonts->AddFontDefault(&cfg);
        }

        if (dpi_scale > 1.0F) {
            ImGui::GetStyle().ScaleAllSizes(dpi_scale);
        }
        VECTIS_LOG_INFO(
            "HiDPI scale={:.2f}, user_scale={:.2f}, font_px={:.1f}, "
            "font={} (window {}x{}, drawable {}x{}, hdpi={:.0f})",
            dpi_scale, user_scale, font_px,
            loaded_path != nullptr ? loaded_path : "ProggyClean(default)",
            win_w, win_h, fb_w, fb_h, hdpi);
    }

    if (!ImGui_ImplSDL2_InitForOpenGL(m_impl->window, m_impl->gl_context)) {
        VECTIS_LOG_ERROR("ImGui_ImplSDL2_InitForOpenGL failed");
        return false;
    }
    m_impl->mark_stage(Stage_ImGuiSdl);

    if (!ImGui_ImplOpenGL3_Init(k_glsl_version)) {
        VECTIS_LOG_ERROR("ImGui_ImplOpenGL3_Init failed");
        return false;
    }
    m_impl->mark_stage(Stage_ImGuiGl);

    // 7. Modes -------------------------------------------------------------
    for (auto& mode : m_impl->modes) {
        mode->initialize(*m_impl->services);
    }
    m_impl->mark_stage(Stage_Modes);

    if (!m_impl->modes.empty()) {
        m_impl->active_mode = 0;
        m_impl->modes[0]->on_activate();
    }

    m_impl->running = true;
    VECTIS_LOG_INFO(
        "Initialization complete ({} modes registered)", m_impl->modes.size());
    return true;
}

int App::run()
{
    const ImVec4 clear_color = vectis::ui::k_dark_palette.bg;

    while (m_impl->running) {
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                request_shutdown();
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(m_impl->window))
            {
                request_shutdown();
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        render_frame();

        ImGui::Render();
        int display_w = 0;
        int display_h = 0;
        SDL_GL_GetDrawableSize(m_impl->window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(
            clear_color.x * clear_color.w,
            clear_color.y * clear_color.w,
            clear_color.z * clear_color.w,
            clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(m_impl->window);
    }

    shutdown_internal();
    return 0;
}

void App::request_shutdown()
{
    if (m_impl) {
        m_impl->running = false;
    }
}

void App::render_frame()
{
    // Full-window dockspace so modes can drag panels around.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    ImGui::Begin("##vectis_host", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    render_mode_tabs();

    // Reserve the bottom row for the status bar so the dockspace
    // auto-sizes to everything above it.
    const float  status_h       = ImGui::GetFrameHeightWithSpacing();
    const ImGuiID dockspace_id  = ImGui::GetID("VectisDockspace");
    ImGui::DockSpace(
        dockspace_id, ImVec2(0.0F, -status_h), ImGuiDockNodeFlags_PassthruCentralNode);

    if (m_impl->modes.empty()) {
        render_welcome_screen();
    } else if (m_impl->active_mode < m_impl->modes.size()) {
        m_impl->modes[m_impl->active_mode]->render();
    }

    ImGui::Separator();
    ImGui::Text("Vectis v%s   |   modes: %zu   |   %s",
                k_version,
                m_impl->modes.size(),
                m_impl->services->config().loaded_from_file()
                    ? "config: loaded"
                    : "config: defaults");

    ImGui::End();
}

void App::render_mode_tabs()
{
    if (ImGui::BeginTabBar("##vectis_mode_tabs", ImGuiTabBarFlags_None)) {
        if (m_impl->modes.empty()) {
            ImGui::BeginDisabled(true);
            if (ImGui::BeginTabItem("(no modes registered)", nullptr,
                                    ImGuiTabItemFlags_NoTooltip)) {
                ImGui::EndTabItem();
            }
            ImGui::EndDisabled();
        } else {
            for (std::size_t i = 0; i < m_impl->modes.size(); ++i) {
                const std::string label{m_impl->modes[i]->name()};
                if (ImGui::BeginTabItem(label.c_str(), nullptr, ImGuiTabItemFlags_None)) {
                    if (i != m_impl->active_mode) {
                        switch_mode(m_impl->modes[i]->id());
                    }
                    ImGui::EndTabItem();
                }
            }
        }
        ImGui::EndTabBar();
    }
}

void App::render_welcome_screen()
{
    ui::render_welcome_screen(k_version);
}

// -----------------------------------------------------------------------------
// Shutdown
// -----------------------------------------------------------------------------

void App::shutdown_internal()
{
    if (m_impl->has_stage(Stage_Modes)) {
        for (auto it = m_impl->modes.rbegin(); it != m_impl->modes.rend(); ++it) {
            (*it)->shutdown();
        }
        m_impl->modes.clear();
    }

    if (m_impl->has_stage(Stage_ImGuiGl)) {
        ImGui_ImplOpenGL3_Shutdown();
    }
    if (m_impl->has_stage(Stage_ImGuiSdl)) {
        ImGui_ImplSDL2_Shutdown();
    }
    if (m_impl->has_stage(Stage_ImGui)) {
        ImGui::DestroyContext();
    }
    if (m_impl->has_stage(Stage_GlContext) && m_impl->gl_context != nullptr) {
        SDL_GL_DeleteContext(m_impl->gl_context);
        m_impl->gl_context = nullptr;
    }
    if (m_impl->has_stage(Stage_Window) && m_impl->window != nullptr) {
        SDL_DestroyWindow(m_impl->window);
        m_impl->window = nullptr;
    }
    if (m_impl->has_stage(Stage_Sdl)) {
        SDL_Quit();
    }
    if (m_impl->has_stage(Stage_Storage)) {
        m_impl->services->storage().close();
    }
    if (m_impl->has_stage(Stage_Services)) {
        m_impl->services.reset();
    }

    if (m_impl->has_stage(Stage_Log)) {
        VECTIS_LOG_INFO("Vectis exited cleanly");
        log::shutdown();
    }

    m_impl->init_stage = Stage_None;
}

} // namespace vectis::core
