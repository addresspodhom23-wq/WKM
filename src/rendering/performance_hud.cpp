#include "core/logger.hpp"
#include "rendering/performance_hud.hpp"
#include "rendering/renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/terrain_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/grass_renderer.hpp"
#include "rendering/skybox.hpp"
#include "rendering/celestial.hpp"
#include "rendering/starfield.hpp"
#include "rendering/clouds.hpp"
#include "rendering/lens_flare.hpp"
#include "rendering/weather.hpp"
#include "rendering/lightning.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/camera.hpp"
#include "ui/ui_colors.hpp"
#include <imgui.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <string_view>

namespace wowee {
namespace rendering {

namespace {
    using namespace wowee::ui;
    constexpr ImVec4 kSectionHeader = {0.8f, 0.8f, 0.5f, 1.0f};
    const auto& kHelpText = colors::kGray;
    const auto& kTitle    = colors::kLightGray;
} // namespace

PerformanceHUD::PerformanceHUD() {
}

PerformanceHUD::~PerformanceHUD() {
}

void PerformanceHUD::update(float deltaTime) {
    if (!enabled) {
        return;
    }

    // Store frame time
    frameTime = deltaTime;
    frameTimeHistory.push_back(deltaTime);

    // Keep history size limited
    while (frameTimeHistory.size() > MAX_FRAME_HISTORY) {
        frameTimeHistory.pop_front();
    }

    // Update stats periodically
    updateTimer += deltaTime;
    if (updateTimer >= UPDATE_INTERVAL) {
        updateTimer = 0.0f;
        calculateFPS();
    }
}

void PerformanceHUD::calculateFPS() {
    if (frameTimeHistory.empty()) {
        return;
    }

    // Current FPS (from last frame time)
    currentFPS = frameTime > 0.0001f ? 1.0f / frameTime : 0.0f;

    // Average FPS
    float sum = 0.0f;
    for (float ft : frameTimeHistory) {
        sum += ft;
    }
    float avgFrameTime = sum / frameTimeHistory.size();
    averageFPS = avgFrameTime > 0.0001f ? 1.0f / avgFrameTime : 0.0f;

    // Min/Max FPS (from last 2 seconds)
    minFPS = 10000.0f;
    maxFPS = 0.0f;
    for (float ft : frameTimeHistory) {
        if (ft > 0.0001f) {
            float fps = 1.0f / ft;
            minFPS = std::min(minFPS, fps);
            maxFPS = std::max(maxFPS, fps);
        }
    }
}

void PerformanceHUD::render(Renderer* renderer, const Camera* camera) {
    if (!enabled || !renderer) {
        return;
    }

    // Set window position based on setting
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                            ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoSavedSettings |
                            ImGuiWindowFlags_NoFocusOnAppearing |
                            ImGuiWindowFlags_NoNav;

    const float PADDING = 10.0f;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 work_pos = viewport->WorkPos;
    ImVec2 work_size = viewport->WorkSize;
    ImVec2 window_pos, window_pos_pivot;

    switch (position) {
        case Position::TOP_LEFT:
            window_pos.x = work_pos.x + PADDING;
            window_pos.y = work_pos.y + PADDING;
            window_pos_pivot.x = 0.0f;
            window_pos_pivot.y = 0.0f;
            break;
        case Position::TOP_RIGHT:
            window_pos.x = work_pos.x + work_size.x - PADDING;
            window_pos.y = work_pos.y + PADDING;
            window_pos_pivot.x = 1.0f;
            window_pos_pivot.y = 0.0f;
            break;
        case Position::BOTTOM_LEFT:
            window_pos.x = work_pos.x + PADDING;
            window_pos.y = work_pos.y + work_size.y - PADDING;
            window_pos_pivot.x = 0.0f;
            window_pos_pivot.y = 1.0f;
            break;
        case Position::BOTTOM_RIGHT:
            window_pos.x = work_pos.x + work_size.x - PADDING;
            window_pos.y = work_pos.y + work_size.y - PADDING;
            window_pos_pivot.x = 1.0f;
            window_pos_pivot.y = 1.0f;
            break;
    }

    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
    ImGui::SetNextWindowBgAlpha(0.7f);  // Transparent background

    if (!ImGui::Begin("Производительность###Performance", nullptr, flags)) {
        ImGui::End();
        return;
    }

    // FPS section
    if (showFPS) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "ПРОИЗВОДИТЕЛЬНОСТЬ");
        ImGui::Separator();

        // Color-code FPS
        ImVec4 fpsColor;
        if (currentFPS >= 60.0f) {
            fpsColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // Green
        } else if (currentFPS >= 30.0f) {
            fpsColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // Yellow
        } else {
            fpsColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);  // Red
        }

        ImGui::Text("Кадров/с (FPS): ");
        ImGui::SameLine();
        ImGui::TextColored(fpsColor, "%.1f", currentFPS);

        ImGui::Text("Среднее: %.1f", averageFPS);
        ImGui::Text("Минимум: %.1f", minFPS);
        ImGui::Text("Максимум: %.1f", maxFPS);
        ImGui::Text("Время кадра: %.2f мс", frameTime * 1000.0f);
#ifdef __ANDROID__
        ImGui::TextUnformatted("ПРОВЕРКА ГРАФИКИ");
        ImGui::TextUnformatted("Галочка = включено, пусто = выключено");
        ImGui::TextUnformatted("Осадки: ВЫКЛ");
        bool buildingsEnabled = renderer->isBuildingDiagnosticEnabled();
        if (ImGui::Checkbox("Здания (WMO)###buildings", &buildingsEnabled)) {
            renderer->setBuildingDiagnosticEnabled(buildingsEnabled);
            LOG_WARNING("[Scene diagnostic] Buildings ", buildingsEnabled ? "ON" : "OFF");
        }
        bool modelsEnabled = renderer->isModelDiagnosticEnabled();
        if (ImGui::Checkbox("Деревья и другие модели (M2)###models", &modelsEnabled)) {
            renderer->setModelDiagnosticEnabled(modelsEnabled);
            LOG_WARNING("[Scene diagnostic] Models ", modelsEnabled ? "ON" : "OFF");
        }
        bool skyEnabled = renderer->isSkyDiagnosticEnabled();
        if (ImGui::Checkbox("Небо целиком###Sky and clouds", &skyEnabled)) {
            renderer->setSkyDiagnosticEnabled(skyEnabled);
            LOG_WARNING("[Sky diagnostic] Sky and clouds ", skyEnabled ? "ON" : "OFF");
        }
        if (!skyEnabled)
            ImGui::TextUnformatted("Небо и облака сейчас не рисуются");
        if (auto* clouds = renderer->getClouds()) {
            bool cachedNoise = clouds->isCachedNoiseEnabled();
            if (ImGui::Checkbox("Ускоренный расчёт облаков###Cached cloud noise", &cachedNoise)) {
                clouds->setCachedNoiseEnabled(cachedNoise);
                LOG_WARNING("[Cloud noise] Cached mode ", cachedNoise ? "ON" : "OFF");
            }
            ImGui::TextUnformatted("Без ускорения: исходный расчёт облаков");
            bool cloudsEnabled = clouds->isEnabled();
            if (ImGui::Checkbox("Слой облаков###Cloud layer", &cloudsEnabled)) {
                clouds->setEnabled(cloudsEnabled);
                LOG_WARNING("[Sky diagnostic] Cloud layer ", cloudsEnabled ? "ON" : "OFF");
            }
        }
        if (auto* water = renderer->getWaterRenderer()) {
            bool reflection = water->isReflectionSceneEnabled();
            if (ImGui::Checkbox("Отражения мира в воде###Reflection scene", &reflection))
                water->setReflectionSceneEnabled(reflection);
        }
        if (auto* grass = renderer->getGrassRenderer()) {
            bool enabled = grass->isEnabled();
            if (ImGui::Checkbox("Объёмная трава###Procedural grass", &enabled))
                grass->setEnabled(enabled);
            ImGui::Text("Создано травинок: %u", grass->sourceBladeCount());
        } else {
            ImGui::TextUnformatted("Объёмная трава: недоступна");
        }
#endif

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.6f, 1.0f), "ПРОЦЕССОР (мс)");
        ImGui::Text("Обновление: %.2f (камера: %.2f)", renderer->getLastUpdateMs(), renderer->getLastCameraUpdateMs());
        ImGui::Text("Отрисовка: %.2f (земля: %.2f, WMO: %.2f, M2: %.2f)",
                    renderer->getLastRenderMs(),
                    renderer->getLastTerrainRenderMs(),
                    renderer->getLastWMORenderMs(),
                    renderer->getLastM2RenderMs());
        if (auto* ctx = renderer->getVkContext(); ctx && ctx->gpuTimingSupported()) {
            const auto& timings = ctx->gpuTimings();
            double gpuTotalMs = 0.0;
            for (const auto& [name, ms] : timings) {
                (void)name;
                gpuTotalMs += ms;
            }
            if (!timings.empty()) {
                ImGui::Text("Видеочип: %.2f мс (этапов: %zu)", gpuTotalMs, timings.size());
                for (const auto& [name, ms] : timings) {
                    const std::string_view label(name);
                    if (label == "reflection-on" || label == "reflection-off" || label == "terrain+grass")
                        ImGui::Text("  %s: %.2f мс",
                            label == "terrain+grass" ? "Земля и трава" :
                            label == "reflection-on" ? "Отражения вкл." : "Отражения выкл.", ms);
                }
            }
        }
        auto* wmoRenderer = renderer->getWMORenderer();
        auto* m2Renderer = renderer->getM2Renderer();
        auto* terrainManager = renderer->getTerrainManager();
        auto* terrainRenderer = renderer->getTerrainRenderer();
        if (terrainManager || terrainRenderer) {
            ImGui::Text("Мир: %d тайлов, видно участков: %d",
                        terrainManager ? terrainManager->getLoadedTileCount() : 0,
                        terrainRenderer ? terrainRenderer->getRenderedChunkCount() : 0);
        }
        if (wmoRenderer || m2Renderer) {
            ImGui::Text("Объекты: WMO %u/%u, M2 %u/%u",
                        wmoRenderer ? wmoRenderer->getInstanceCount() : 0,
                        wmoRenderer ? wmoRenderer->getDrawCallCount() : 0,
                        m2Renderer ? m2Renderer->getInstanceCount() : 0,
                        m2Renderer ? m2Renderer->getDrawCallCount() : 0);
        }
        if (wmoRenderer || m2Renderer)
            ImGui::TextUnformatted("WMO: здания, M2: модели; объекты/отрисовки");
        if (!compact && (wmoRenderer || m2Renderer)) {
            ImGui::Text("Проверки столкновений:");
            if (wmoRenderer) {
                ImGui::Text("  WMO: %.2f мс (вызовов: %u)",
                            wmoRenderer->getQueryTimeMs(), wmoRenderer->getQueryCallCount());
            }
            if (m2Renderer) {
                ImGui::Text("  M2: %.2f мс (вызовов: %u)",
                            m2Renderer->getQueryTimeMs(), m2Renderer->getQueryCallCount());
            }
        }

        // Frame time graph
        if (!compact && !frameTimeHistory.empty()) {
            std::vector<float> frameTimesMs;
            frameTimesMs.reserve(frameTimeHistory.size());
            for (float ft : frameTimeHistory) {
                frameTimesMs.push_back(ft * 1000.0f);  // Convert to ms
            }
            ImGui::PlotLines("##frametime", frameTimesMs.data(), static_cast<int>(frameTimesMs.size()),
                           0, nullptr, 0.0f, 33.33f, ImVec2(200, 40));
        }

        // FSR info
        if (!renderer->getPostProcessPipeline()->isFSREnabled() &&
            !renderer->getPostProcessPipeline()->isFSR2Enabled())
            ImGui::TextUnformatted("FSR: ВЫКЛ");
        if (renderer->getPostProcessPipeline()->isFSREnabled()) {
            ImGui::TextColored(colors::kGreen, "FSR 1.0: ВКЛ");
            auto* ctx = renderer->getVkContext();
            if (ctx) {
                auto ext = ctx->getSwapchainExtent();
                float sf = renderer->getPostProcessPipeline()->getFSRScaleFactor();
                uint32_t iw = static_cast<uint32_t>(ext.width * sf) & ~1u;
                uint32_t ih = static_cast<uint32_t>(ext.height * sf) & ~1u;
                ImGui::Text("  %ux%u -> %ux%u (%.0f%%)", iw, ih, ext.width, ext.height, sf * 100.0f);
            }
        }
        if (renderer->getPostProcessPipeline()->isFSR2Enabled()) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "Масштабирование FSR 3: ВКЛ");
            ImGui::Text("  Сдвиг выборки: %.2f", renderer->getPostProcessPipeline()->getFSR2JitterSign());
            const bool fgEnabled = renderer->getPostProcessPipeline()->isAmdFsr3FramegenEnabled();
            const bool fgReady = renderer->getPostProcessPipeline()->isAmdFsr3FramegenRuntimeReady();
            const bool fgActive = renderer->getPostProcessPipeline()->isAmdFsr3FramegenRuntimeActive();
            const char* fgStatus = "Выключено";
            if (fgEnabled) {
                fgStatus = fgActive ? "Работает" : (fgReady ? "Готово (ожидание/резерв)" : "Недоступно");
            }
            ImGui::Text("  Генерация кадров FSR3: %s (%s)", fgStatus, renderer->getPostProcessPipeline()->getAmdFsr3FramegenRuntimePath());
            const std::string& fgErr = renderer->getPostProcessPipeline()->getAmdFsr3FramegenRuntimeError();
            if (!fgErr.empty()) {
                ImGui::TextWrapped("  Ошибка генерации кадров: %s", fgErr.c_str());
            }
            ImGui::Text("  Запусков генерации кадров: %zu", renderer->getPostProcessPipeline()->getAmdFsr3FramegenDispatchCount());
            ImGui::Text("  Запусков масштабирования: %zu", renderer->getPostProcessPipeline()->getAmdFsr3UpscaleDispatchCount());
            ImGui::Text("  Переходов в резервный режим: %zu", renderer->getPostProcessPipeline()->getAmdFsr3FallbackCount());
        }
        if (renderer->getPostProcessPipeline()->isFXAAEnabled()) {
            if (renderer->getPostProcessPipeline()->isFSR2Enabled()) {
                ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.8f, 1.0f), "Сглаживание FXAA: ВКЛ (с FSR3)");
            } else {
                ImGui::TextColored(ImVec4(0.8f, 1.0f, 0.6f, 1.0f), "Сглаживание FXAA: ВКЛ");
            }
        }

        ImGui::Spacing();
    }

    // Renderer stats
    if (showRenderer) {
        auto* terrainRenderer = renderer->getTerrainRenderer();
        if (terrainRenderer) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "ОТРИСОВКА");
            ImGui::Separator();

            int totalChunks = terrainRenderer->getChunkCount();
            int rendered = terrainRenderer->getRenderedChunkCount();
            int culled = terrainRenderer->getCulledChunkCount();
            int triangles = terrainRenderer->getTriangleCount();

            ImGui::Text("Участков: %d", totalChunks);
            ImGui::Text("Отрисовано: %d", rendered);
            ImGui::Text("Отсечено: %d", culled);

            if (totalChunks > 0) {
                float visiblePercent = (rendered * 100.0f) / totalChunks;
                ImGui::Text("Видимо: %.1f%%", visiblePercent);
            }

            ImGui::Text("Треугольников: %s",
                       triangles >= 1000000 ?
                       (std::to_string(triangles / 1000) + "K").c_str() :
                       std::to_string(triangles).c_str());

            ImGui::Spacing();
        }
    }

    // Terrain streaming info
    if (showTerrain) {
        auto* terrainManager = renderer->getTerrainManager();
        if (terrainManager) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "ЛАНДШАФТ");
            ImGui::Separator();

            ImGui::Text("Загружено тайлов: %d", terrainManager->getLoadedTileCount());

            auto currentTile = terrainManager->getCurrentTile();
            ImGui::Text("Текущий тайл: [%d,%d]", currentTile.x, currentTile.y);

            ImGui::Spacing();
        }

        // Water info
        auto* waterRenderer = renderer->getWaterRenderer();
        if (waterRenderer) {
            ImGui::TextColored(ImVec4(0.2f, 0.5f, 1.0f, 1.0f), "ВОДА");
            ImGui::Separator();

            ImGui::Text("Поверхностей: %d", waterRenderer->getSurfaceCount());
            ImGui::Text("Включено: %s", waterRenderer->isEnabled() ? "ДА" : "НЕТ");

            ImGui::Spacing();
        }
    }

    // Skybox info
    if (showTerrain) {
        auto* skybox = renderer->getSkybox();
        if (skybox) {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "НЕБО");
            ImGui::Separator();

            float time = skybox->getTimeOfDay();
            int hours = static_cast<int>(time);
            int minutes = static_cast<int>((time - hours) * 60);

            ImGui::Text("Время: %02d:%02d", hours, minutes);
            ImGui::Text("Смена времени: %s", skybox->isTimeProgressionEnabled() ? "ДА" : "НЕТ");

            // Celestial info
            auto* celestial = renderer->getCelestial();
            if (celestial) {
                ImGui::Text("Солнце и луна: %s", celestial->isEnabled() ? "ДА" : "НЕТ");

                // Moon phase info
                float phase = celestial->getMoonPhase();
                const char* phaseName = "Неизвестно";
                if (phase < 0.0625f || phase >= 0.9375f) phaseName = "Новолуние";
                else if (phase < 0.1875f) phaseName = "Растущий серп";
                else if (phase < 0.3125f) phaseName = "Первая четверть";
                else if (phase < 0.4375f) phaseName = "Растущая луна";
                else if (phase < 0.5625f) phaseName = "Полнолуние";
                else if (phase < 0.6875f) phaseName = "Убывающая луна";
                else if (phase < 0.8125f) phaseName = "Последняя четверть";
                else phaseName = "Убывающий серп";

                ImGui::Text("Луна: %s (%.0f%%)", phaseName, phase * 100.0f);
                ImGui::Text("Смена фаз: %s", celestial->isMoonPhaseCycling() ? "ДА" : "НЕТ");
            }

            // Star field info
            auto* starField = renderer->getStarField();
            if (starField) {
                ImGui::Text("Звёзд: %d (%s)", starField->getStarCount(),
                           starField->isEnabled() ? "ВКЛ" : "ВЫКЛ");
            }

            // Cloud info
            auto* clouds = renderer->getClouds();
            if (clouds) {
                ImGui::Text("Облака: %s (%.0f%%)",
                           clouds->isEnabled() ? "ВКЛ" : "ВЫКЛ",
                           clouds->getDensity() * 100.0f);
            }

            // Lens flare info
            auto* lensFlare = renderer->getLensFlare();
            if (lensFlare) {
                ImGui::Text("Солнечные блики: %s (%.0f%%)",
                           lensFlare->isEnabled() ? "ВКЛ" : "ВЫКЛ",
                           lensFlare->getIntensity() * 100.0f);
            }

            ImGui::Spacing();
        }
    }

    // Weather info
    if (showRenderer) {
        auto* weather = renderer->getWeather();
        if (weather) {
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "ПОГОДА");
            ImGui::Separator();

            const char* typeName = "Нет осадков";
            using WeatherType = rendering::Weather::Type;
            auto type = weather->getWeatherType();
            if (type == WeatherType::RAIN) typeName = "Дождь";
            else if (type == WeatherType::SNOW) typeName = "Снег";

            ImGui::Text("Тип: %s", typeName);
            if (weather->isEnabled()) {
                ImGui::Text("Частиц: %d", weather->getParticleCount());
                ImGui::Text("Интенсивность: %.0f%%", weather->getIntensity() * 100.0f);
            }

            auto* lightning = renderer->getLightning();
            if (lightning && lightning->isEnabled()) {
                ImGui::Text("Молнии: включены (%.0f%%)", lightning->getIntensity() * 100.0f);
            }

            ImGui::Spacing();
        }
    }

    // Fog info
    if (showRenderer) {
        auto* terrainRenderer = renderer->getTerrainRenderer();
        if (terrainRenderer) {
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 0.9f, 1.0f), "ТУМАН");
            ImGui::Separator();

            ImGui::Text("Туман вдали: %s", terrainRenderer->isFogEnabled() ? "ВКЛ" : "ВЫКЛ");

            ImGui::Spacing();
        }
    }

    // Character info
    if (showRenderer) {
        auto* charRenderer = renderer->getCharacterRenderer();
        if (charRenderer) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "ПЕРСОНАЖИ");
            ImGui::Separator();

            ImGui::Text("Экземпляров: %zu", charRenderer->getInstanceCount());

            ImGui::Spacing();
        }
    }

    // WMO building info
    if (showRenderer) {
        auto* wmoRenderer = renderer->getWMORenderer();
        if (wmoRenderer) {
            ImGui::TextColored(ImVec4(0.8f, 0.7f, 0.6f, 1.0f), "ЗДАНИЯ (WMO)");
            ImGui::Separator();

            ImGui::Text("Моделей: %u", wmoRenderer->getModelCount());
            ImGui::Text("Экземпляров: %u", wmoRenderer->getInstanceCount());
            ImGui::Text("Треугольников: %u", wmoRenderer->getTotalTriangleCount());
            ImGui::Text("Вызовов отрисовки: %u", wmoRenderer->getDrawCallCount());
            ImGui::Text("Кэш поверхностей: %zu", wmoRenderer->getFloorCacheSize());
            ImGui::Text("Отсечено по дальности: %u групп", wmoRenderer->getDistanceCulledGroups());
            if (wmoRenderer->isOcclusionCullingEnabled()) {
                ImGui::Text("Скрыто препятствиями: %u групп", wmoRenderer->getOcclusionCulledGroups());
            }
            if (wmoRenderer->isPortalCullingEnabled()) {
                ImGui::Text("Отсечено порталами: %u групп", wmoRenderer->getPortalCulledGroups());
            }

            ImGui::Spacing();
        }
    }

    // Zone info
    {
        const std::string& zoneName = renderer->getCurrentZoneName();
        if (!zoneName.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "ЛОКАЦИЯ");
            ImGui::Separator();
            ImGui::Text("%s", zoneName.c_str());
            ImGui::Spacing();
        }
    }

    // Camera info
    if (showCamera && camera) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "КАМЕРА");
        ImGui::Separator();

        glm::vec3 pos = camera->getPosition();
        ImGui::Text("Позиция: %.1f, %.1f, %.1f", pos.x, pos.y, pos.z);

        glm::vec3 forward = camera->getForward();
        ImGui::Text("Направление: %.2f, %.2f, %.2f", forward.x, forward.y, forward.z);

        ImGui::Spacing();
    }

    // Controls help
    if (showControls) {
        ImGui::TextColored(kTitle, "УПРАВЛЕНИЕ");
        ImGui::Separator();

        ImGui::TextColored(kSectionHeader, "Перемещение");
        ImGui::TextColored(kHelpText, "WASD: движение");
        ImGui::TextColored(kHelpText, "Q/E: шаг влево/вправо");
        ImGui::TextColored(kHelpText, "Пробел: прыжок");
        ImGui::TextColored(kHelpText, "X: сесть/встать");
        ImGui::TextColored(kHelpText, "~: автобег");
        ImGui::TextColored(kHelpText, "Z: убрать оружие");

        ImGui::Spacing();
        ImGui::TextColored(kSectionHeader, "Окна интерфейса");
        ImGui::TextColored(kHelpText, "B: сумки");
        ImGui::TextColored(kHelpText, "C: персонаж");
        ImGui::TextColored(kHelpText, "L: задания");
        ImGui::TextColored(kHelpText, "N: таланты");
        ImGui::TextColored(kHelpText, "P: заклинания");
        ImGui::TextColored(kHelpText, "M: карта мира");

        ImGui::Spacing();
        ImGui::TextColored(kSectionHeader, "Бой и чат");
        ImGui::TextColored(kHelpText, "1-0,-,=: панель действий");
        ImGui::TextColored(kHelpText, "Tab: смена цели");
        ImGui::TextColored(kHelpText, "Enter: чат");
        ImGui::TextColored(kHelpText, "/: команда чата");

        ImGui::Spacing();
        ImGui::TextColored(kSectionHeader, "Диагностика");
        ImGui::TextColored(kHelpText, "F1: показать/скрыть счётчик");
        ImGui::TextColored(kHelpText, "F4: включить/выключить тени");
        ImGui::TextColored(kHelpText, "F7: эффект повышения уровня");
        ImGui::TextColored(kHelpText, "Esc: настройки/закрыть");
    }

    ImGui::End();
}

} // namespace rendering
} // namespace wowee
