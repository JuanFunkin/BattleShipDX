// SPDX-License-Identifier: MIT
//
// Portions of this file are derived from the Ship of Harkinian (SoH) project
//   Copyright (c) The Harbour Masters
//   https://github.com/HarbourMasters/Shipwright
// Licensed under the MIT License; see LICENSE at repository root.

#include "PortMenu.h"

#include "Compat.h"
#include "../enhancements/enhancements.h"
#include "../interpolation/frame_interpolation.h"
#ifdef PORT_HIRES_ENABLED
#include "../hires/HiResPack.h"
#endif

#include <fast/backends/gfx_rendering_api.h>
#include <fast/postprocess/PostProcessSourceLoader.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#ifndef DISABLE_SCRIPTING
#include <ship/scripting/ScriptLoader.h>
#include "../mods/HookManager.h"
#include "../mods/ModRegistry.h"

namespace ssb64 { void MountModsDir(); void UnmountMissingMods(); }
#endif

#include <imgui.h>
#include "../audio/music_override.h"
#include "../netplay/netplay.h"
#include "../widescreen/widescreen.h"
#include <chrono>
#include <cfloat>

#include <SDL2/SDL.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace ssb64 {

using namespace UIWidgets;

namespace {
static const std::map<int32_t, const char*> kMenuThemeOptions = {
    { UIWidgets::Colors::Red, "Red" },
    { UIWidgets::Colors::DarkRed, "Dark Red" },
    { UIWidgets::Colors::Orange, "Orange" },
    { UIWidgets::Colors::Green, "Green" },
    { UIWidgets::Colors::DarkGreen, "Dark Green" },
    { UIWidgets::Colors::LightBlue, "Light Blue" },
    { UIWidgets::Colors::Blue, "Blue" },
    { UIWidgets::Colors::DarkBlue, "Dark Blue" },
    { UIWidgets::Colors::Indigo, "Indigo" },
    { UIWidgets::Colors::Violet, "Violet" },
    { UIWidgets::Colors::Purple, "Purple" },
    { UIWidgets::Colors::Brown, "Brown" },
    { UIWidgets::Colors::Gray, "Gray" },
    { UIWidgets::Colors::DarkGray, "Dark Gray" },
};

static const std::map<int32_t, const char*> kTextureFilteringMap = {
    { Fast::FILTER_THREE_POINT, "Three-Point" },
    { Fast::FILTER_LINEAR, "Linear" },
    { Fast::FILTER_NONE, "None" },
};

// Enhanced framerate mode: CVar stores the target render fps; game logic
// always stays at 60 Hz (see port/interpolation/frame_interpolation.h).
static const std::map<int32_t, const char*> kEnhancedFpsMap = {
    { 0, "Off (60 FPS)" },
    { 120, "120 FPS" },
    { 180, "180 FPS" },
    { 240, "240 FPS" },
};

// Mirrors the LowResMode switch in libultraship Gui::CalculateGameViewport /
// Gui::DrawGame. 0 keeps the framebuffer at the window resolution; 1 forces a
// 320x240 4:3 framebuffer centered with side strips; 2/3 keep the window's
// aspect but lock vertical pixel count.
// Keys 0..3 hand off to libultraship's gLowResMode (Gui.cpp LowResMode switch);
// keys 4..7 take the PixelPerfectMode path of gAdvancedResolution and ignore
// gLowResMode. port.cpp's boot latch translates the pending menu value into
// the right combination of LUS cvars so libultraship sees a stable state for
// the whole session.
static const std::map<int32_t, const char*> kLowResModeMap = {
    { 0, "Off (window resolution)" },
    { 1, "N64 (320x240, stretched 4:3)" },
    { 2, "240p (window aspect, stretched)" },
    { 3, "480p (window aspect, stretched)" },
    { 4, "N64 integer-scaled (auto-fit, pixel-perfect)" },
    { 5, "N64 integer-scaled (2x, pixel-perfect)" },
    { 6, "N64 integer-scaled (3x, pixel-perfect)" },
    { 7, "N64 integer-scaled (4x, pixel-perfect)" },
};

// Mirrors dbObjectDisplayMode (src/sys/develop.h). 0 disables the override and
// returns the engine's normal rendering.
static const std::map<int32_t, const char*> kHitboxViewMap = {
    { 0, "Off" },
    { 1, "Filled (red cubes)" },
    { 2, "Outline + opaque hurtboxes" },
};

// Bundled (in-archive) post-process shaders get a friendly label
// in the picker. Names not in this map render with the shader's
// short name directly (the user-supplied taxonomy is good enough).
static const char* PostProcessShaderLabel(const std::string& name) {
    if (name == "scanlines")  return "Scanlines";
    if (name == "crt-lottes") return "CRT - Lottes";
    return nullptr;
}

// Draws the compat badge after a picker entry. `Any` shaders render
// at any resolution; `Native` shaders ratio against InputSize and
// only look right when the source FB is locked to ~240p (Low
// Resolution Mode in the Settings → Graphics menu). The badge text
// itself is colored, license hover-text comes from the sidecar.
static void DrawPostProcessCompatBadge(const Fast::PostProcessShaderInfo& info) {
    const char* text = info.compat == Fast::PostProcessCompat::Any ? "[any res]" : "[needs 240p]";
    const ImVec4 color = info.compat == Fast::PostProcessCompat::Any
        ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
        : ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
    ImGui::SameLine();
    ImGui::TextColored(color, "%s", text);
    if (!info.license.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("License: %s", info.license.c_str());
    }
}

// Selecting a `compat=native` shader while Low Resolution Mode is
// disabled is the common UX trap — the shader compiles and runs but
// produces a tiny render in the corner of the FB. We open this
// pending-modal token from the picker so the modal can be drawn at
// the top of the next frame outside any combo / popup context.
static std::string s_lowResWarnShader;
static bool s_lowResWarnNeedsOpen = false;

// Render the post-process shader picker as a single combo widget
// that opens onto a tree of bundled + user-installed shaders. Drives
// the two LUS cvars LUS::Run reads each frame:
//   gPostProcessEnabled (int, 0/1)
//   gPostProcessShader  (string, short name as accepted by
//                        Fast::LoadPostProcessShader)
// The combo's preview text reflects the current cvar state, so a
// fresh launch picks up the previously-selected shader without the
// picker needing to know about state-restore.
void RenderPostProcessShaderPicker(WidgetInfo& /*widget*/) {
    const bool enabled = CVarGetInteger("gPostProcessEnabled", 0) != 0;
    const char* currentRaw = CVarGetString("gPostProcessShader", "");
    const std::string current = currentRaw ? currentRaw : std::string();

    std::string preview = "Off";
    if (enabled && !current.empty()) {
        if (const char* friendly = PostProcessShaderLabel(current)) {
            preview = friendly;
        } else {
            preview = current;
        }
    }

    ImGui::TextUnformatted("Post-Process Shader");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (!ImGui::BeginCombo("##gPostProcessShaderPicker", preview.c_str())) {
        return;
    }

    // Filter input persists across frames inside the open popup
    // only — closing the combo clears it. ImGui draws the combo
    // content inside its own internal child window each frame, so
    // a static-local is the simplest persistence story without a
    // companion cvar.
    static char filterBuf[128] = "";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##gPostProcessShaderFilter", "Filter...", filterBuf, sizeof(filterBuf));
    const std::string filter = filterBuf;

    auto matchesFilter = [&filter](const std::string& s) -> bool {
        if (filter.empty()) {
            return true;
        }
        // Case-insensitive substring match.
        std::string hay = s;
        std::string needle = filter;
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return hay.find(needle) != std::string::npos;
    };

    auto selectShader = [](const std::string& name) {
        if (name.empty()) {
            CVarSetInteger("gPostProcessEnabled", 0);
            return;
        }
        CVarSetString("gPostProcessShader", name.c_str());
        CVarSetInteger("gPostProcessEnabled", 1);

        // Trigger the Low Resolution Mode warn modal when the picked
        // shader expects a native-resolution source FB but the user's
        // current latched mode is "off". The shader still loads — we
        // never block selection — but the modal nudges them toward
        // enabling N64 mode + restart so the picture isn't a tiny
        // render in the corner of the window.
        const Fast::PostProcessShaderInfo info = Fast::GetPostProcessShaderInfo(name);
        if (info.compat == Fast::PostProcessCompat::Native &&
            CVarGetInteger("gLowResModePending", 0) == 0) {
            s_lowResWarnShader = name;
            s_lowResWarnNeedsOpen = true;
        }
    };

    // Per-frame compat-info cache. Filesystem stat + sidecar parse
    // happens here so each picker entry below can show a badge without
    // re-probing on every Selectable. The map's lifetime is the
    // visible combo body; rebuilt every frame the combo is open since
    // the user's shader set rarely changes mid-popup and the cost is
    // dwarfed by ImGui itself.
    std::map<std::string, Fast::PostProcessShaderInfo> infoCache;
    auto cachedInfo = [&infoCache](const std::string& name) -> const Fast::PostProcessShaderInfo& {
        auto it = infoCache.find(name);
        if (it != infoCache.end()) return it->second;
        Fast::PostProcessShaderInfo info = Fast::GetPostProcessShaderInfo(name);
        return infoCache.emplace(name, info).first->second;
    };

    // "Off" sits at the top regardless of filter — it's not a shader
    // name, just the disable affordance, and hiding it behind the
    // filter would leave the user with no way to turn the effect off
    // once they've narrowed the list.
    if (ImGui::Selectable("Off", !enabled)) {
        selectShader(std::string());
    }
    ImGui::Separator();

    // Bundled group: pulled from the LUS API rather than hardcoded
    // here so adding a builtin shader to f3d.o2r exposes it in the
    // picker without a port-side change.
    const std::vector<std::string> bundled = Fast::ListBuiltinPostProcessShaders();
    std::vector<std::string> bundledFiltered;
    bundledFiltered.reserve(bundled.size());
    for (const std::string& name : bundled) {
        const char* label = PostProcessShaderLabel(name);
        if (matchesFilter(name) || (label && matchesFilter(label))) {
            bundledFiltered.push_back(name);
        }
    }
    if (!bundledFiltered.empty()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
        if (ImGui::TreeNodeEx("Bundled", ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (const std::string& name : bundledFiltered) {
                const char* friendly = PostProcessShaderLabel(name);
                const std::string label = friendly ? friendly : name;
                const bool active = enabled && current == name;
                if (ImGui::Selectable(label.c_str(), active)) {
                    selectShader(name);
                }
                DrawPostProcessCompatBadge(cachedInfo(name));
            }
            ImGui::TreePop();
        }
    }

    // User-installed shaders, one tree node per folder under the
    // per-user shaders dir. The "(loose)" bucket holds shaders that
    // live directly in the root.
    const std::vector<Fast::UserPostProcessShaderFolder> userFolders =
        Fast::ListUserPostProcessShaders();
    for (const auto& folder : userFolders) {
        std::vector<std::string> matching;
        matching.reserve(folder.shaderNames.size());
        for (const std::string& name : folder.shaderNames) {
            if (matchesFilter(name) || matchesFilter(folder.displayName)) {
                matching.push_back(name);
            }
        }
        if (matching.empty()) {
            continue;
        }
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
        if (ImGui::TreeNodeEx(folder.displayName.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            for (const std::string& name : matching) {
                const Fast::PostProcessShaderInfo& info = cachedInfo(name);
                const std::string label = info.label.empty() ? name : info.label;
                const bool active = enabled && current == name;
                if (ImGui::Selectable(label.c_str(), active)) {
                    selectShader(name);
                }
                DrawPostProcessCompatBadge(info);
            }
            ImGui::TreePop();
        }
    }

    if (bundledFiltered.empty() && userFolders.empty() && filter.empty()) {
        ImGui::TextDisabled("No user shaders installed.");
        ImGui::TextDisabled("Drop .glsl / .glslp files into:");
        const std::string userDir = Ship::Context::GetPathRelativeToAppDirectory("shaders");
        ImGui::TextDisabled("%s", userDir.c_str());
    }

    ImGui::EndCombo();
}

// Live status panel for the post-process chain. Reads
// Fast::GetPostProcessRuntimeDiagnostics(), which the chain publishes
// on every load / unload. Lets the user confirm at a glance:
//   * which shader is actually active (matches the cvar)
//   * which load path produced it (legacy .glsl / .glslp, or .slang)
//   * how many passes compiled
//   * the last error if a load failed
// Pair the on-screen state with the matching SPDLOG_INFO lines in
// ssb64.log to walk through the load pipeline — see
// docs/crt_shader_testing_protocol.md.
void RenderPostProcessDiagnostics(WidgetInfo& /*widget*/) {
    const Fast::PostProcessRuntimeDiagnostics diag = Fast::GetPostProcessRuntimeDiagnostics();

    ImGui::TextDisabled("Shader runtime diagnostics");
    if (diag.active) {
        ImGui::Text("Active:  %s", diag.name.c_str());
        ImGui::Text("Flavor:  %s", diag.flavor.c_str());
        ImGui::Text("Passes:  %zu", diag.passCount);
    } else if (!diag.lastError.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "Inactive — last error:");
        ImGui::TextWrapped("%s", diag.lastError.c_str());
    } else {
        ImGui::TextDisabled("No shader loaded.");
    }

    // Reload button — flips the enabled cvar off then on to force the
    // interpreter to redo the load (which re-runs all of the
    // SPDLOG_INFO instrumentation in the load path). Helpful when
    // tweaking sidecars or replacing files on disk during testing.
    if (ImGui::Button("Reload active shader")) {
        const char* currentRaw = CVarGetString("gPostProcessShader", "");
        const std::string current = currentRaw ? currentRaw : std::string();
        if (!current.empty()) {
            CVarSetInteger("gPostProcessEnabled", 0);
            // The interpreter's transition detection keys on the (name,
            // enabled) pair, so we re-write both even though the name
            // is unchanged — that re-arms the load path.
            CVarSetString("gPostProcessShader", current.c_str());
            CVarSetInteger("gPostProcessEnabled", 1);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Force the runtime to re-resolve, re-load, and re-compile the\n"
            "currently-selected shader. Watch ssb64.log for the matching\n"
            "Post-process: ... INFO lines. Useful after editing a shader\n"
            "file on disk or its .lus.json sidecar.");
    }
}

#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
// Shader-pack downloader UI is gated off on Android (the libretro
// catalog fetch + ZIP-extract pipeline doesn't fit Play Store distro;
// port/enhancements/ShaderDownloader.cpp is not compiled into libmain.so).
// The matching menu entry under Settings → Graphics is wrapped in the
// same guard, so this block is dead code on Android — gate the whole
// definition so the link doesn't reach for the missing symbols.

// State scoped to the shader-pack modal, mirroring the Low-Res-warn
// pattern. We keep a "needs to open this frame" flag so the
// RenderShaderPackModal hook (called every frame from
// RenderMenuTopLevel) can issue the OpenPopup against the correct
// ImGui ID — clicking the picker's "Get more shaders…" button only
// sets the flag; OpenPopup must run on the same stack as the
// matching BeginPopupModal.
static bool s_shaderPackModalNeedsOpen = false;

// User-side selection state. Vector<bool> packs the picks alongside
// the candidate list returned by GetShaderPackCandidates(); we keep
// the candidate copy here so toggles don't reach into the worker
// thread's data, and so cancel-then-reopen produces a fresh list
// instead of stale state.
static std::vector<ssb64::enhancements::ShaderPackCandidate> s_shaderPackCandidatesUi;
static std::vector<bool>                                     s_shaderPackSelected;
// Last phase we saw on the previous frame — used to detect the
// AwaitingSelection edge so we snapshot the candidate list once
// (instead of once per frame, which would also reset the user's
// checkbox state).
static ssb64::enhancements::ShaderPackPhase s_shaderPackLastPhase =
    ssb64::enhancements::ShaderPackPhase::Idle;
static char s_shaderPackFilter[128] = {};

// Renders the "Get more shaders…" affordance immediately beneath the
// picker. Click opens the modal; the modal handles the catalog fetch,
// the user's selection, and the install in sequence.
void RenderShaderPackDownloader(WidgetInfo& /*widget*/) {
    using namespace ssb64::enhancements;

    const ShaderPackPhase phase = GetShaderPackPhase();
    const bool busy =
        phase == ShaderPackPhase::DownloadingCatalog ||
        phase == ShaderPackPhase::ExtractingCatalog ||
        phase == ShaderPackPhase::EnumeratingCatalog ||
        phase == ShaderPackPhase::AwaitingSelection ||
        phase == ShaderPackPhase::InstallingSelected;

    if (UIWidgets::Button(
            "Get more shaders...",
            UIWidgets::ButtonOptions()
                .Color(UIWidgets::Colors::LightBlue)
                .Tooltip("Browse and install community CRT/scanline/NTSC shaders\n"
                         "from libretro/glsl-shaders. These shaders are GPL'd and\n"
                         "are installed by the user at runtime - the binary itself\n"
                         "stays MIT. Most expect Low Resolution Mode for correct\n"
                         "rendering scale."))) {
        SPDLOG_INFO("Shader pack: button clicked, current phase={} busy={}",
                    static_cast<int>(phase), busy);
        s_shaderPackModalNeedsOpen = true;
        if (!busy) {
            ssb64::enhancements::FetchShaderPackCatalogAsync();
        }
    }

    const std::string status = GetShaderPackStatus();
    if (!status.empty() && phase == ShaderPackPhase::Done) {
        // Persistent post-install summary directly under the button
        // (the modal closes on Install, so the user wouldn't see this
        // otherwise).
        ImGui::TextDisabled("%s", status.c_str());
    }
}

// Drawn from RenderMenuTopLevel each frame so the modal survives the
// picker combo collapsing. Six conceptual states laid out in one
// function because they share the same window+title+button layout
// scaffold; switching them out frame-to-frame is the entire UX:
//
//   1. Loading (DownloadingCatalog / ExtractingCatalog /
//      EnumeratingCatalog) — spinner + status string + Cancel.
//   2. Awaiting selection — checkbox list + filter + Select All /
//      None + Install + Cancel.
//   3. Installing — spinner + status + (no Cancel; copy is fast).
//   4. Done / Error — status line + OK.
void RenderShaderPackModal() {
    using namespace ssb64::enhancements;

    if (s_shaderPackModalNeedsOpen) {
        SPDLOG_INFO("Shader pack: opening modal");
        ImGui::OpenPopup("Shader pack##downloader");
        s_shaderPackModalNeedsOpen = false;
    }

    const ShaderPackPhase phase = GetShaderPackPhase();

    // Detect the just-arrived AwaitingSelection edge — when we cross
    // it we snapshot the candidate list and reset the selection
    // bitmap to "all on" so the user's first interaction is to
    // de-select rather than to re-tick everything.
    if (phase == ShaderPackPhase::AwaitingSelection &&
        s_shaderPackLastPhase != ShaderPackPhase::AwaitingSelection) {
        s_shaderPackCandidatesUi = GetShaderPackCandidates();
        s_shaderPackSelected.assign(s_shaderPackCandidatesUi.size(), true);
        s_shaderPackFilter[0] = '\0';
    }
    s_shaderPackLastPhase = phase;

    // Always re-pin position + size on every open. NoSavedSettings
    // prevents imgui.ini from restoring stale geometry that could
    // place the modal off-screen or at zero size — a known cause of
    // "modal looks stuck because it's invisible". Matches the flags
    // used by the Re-extract modal lower in this file.
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 480.0f), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Shader pack##downloader", nullptr,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    const std::string status = GetShaderPackStatus();

    auto closeWithCancel = [&]() {
        CancelShaderPackFlow();
        s_shaderPackCandidatesUi.clear();
        s_shaderPackSelected.clear();
        s_shaderPackLastPhase = ShaderPackPhase::Idle;
        ImGui::CloseCurrentPopup();
    };

    // Fixed sizes for the action-row buttons so the modal layout
     // doesn't reflow as their labels change between phases. The
     // theme's default Fill size would consume the entire content
     // region and we want the rest of the modal (text, list) to keep
     // its space.
    const ImVec2 kSmallButton(120.0f, 0.0f);
    const ImVec2 kMediumButton(160.0f, 0.0f);

    switch (phase) {
        case ShaderPackPhase::Idle:
        case ShaderPackPhase::Error: {
            // Fallback for the rare case where the modal opens before
            // the catalog phase has actually fired. Offer to retry
            // the fetch rather than leaving the user with an empty
            // window.
            ImGui::TextWrapped("%s", !status.empty()
                ? status.c_str()
                : "No catalog loaded. Try fetching again.");
            ImGui::Spacing();
            if (UIWidgets::Button("Retry",
                                  UIWidgets::ButtonOptions()
                                      .Size(kSmallButton)
                                      .Color(UIWidgets::Colors::LightBlue))) {
                ssb64::enhancements::FetchShaderPackCatalogAsync();
            }
            ImGui::SameLine();
            if (UIWidgets::Button("Close",
                                  UIWidgets::ButtonOptions()
                                      .Size(kSmallButton)
                                      .Color(UIWidgets::Colors::Gray))) {
                ImGui::CloseCurrentPopup();
            }
            break;
        }
        case ShaderPackPhase::DownloadingCatalog:
        case ShaderPackPhase::ExtractingCatalog:
        case ShaderPackPhase::EnumeratingCatalog: {
            ImGui::TextWrapped("%s", status.empty() ? "Working..." : status.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Validating each shader against this build's\n"
                                "post-process pipeline. Takes a few seconds.");
            ImGui::Spacing();
            if (UIWidgets::Button("Cancel",
                                  UIWidgets::ButtonOptions()
                                      .Size(kSmallButton)
                                      .Color(UIWidgets::Colors::Gray))) {
                closeWithCancel();
            }
            break;
        }
        case ShaderPackPhase::AwaitingSelection: {
            ImGui::TextWrapped("%zu shaders available. Tick the ones you'd like to install.",
                               s_shaderPackCandidatesUi.size());
            ImGui::Spacing();

            if (UIWidgets::Button("Select all",
                                  UIWidgets::ButtonOptions()
                                      .Size(kMediumButton)
                                      .Color(UIWidgets::Colors::Gray))) {
                s_shaderPackSelected.assign(s_shaderPackCandidatesUi.size(), true);
            }
            ImGui::SameLine();
            if (UIWidgets::Button("Select none",
                                  UIWidgets::ButtonOptions()
                                      .Size(kMediumButton)
                                      .Color(UIWidgets::Colors::Gray))) {
                s_shaderPackSelected.assign(s_shaderPackCandidatesUi.size(), false);
            }
            ImGui::SameLine();
            // The InputString widget has its own padded layout; let it
            // claim the remaining row width so the placeholder doesn't
            // crowd the buttons.
            std::string filterValue = s_shaderPackFilter;
            ImGui::SetNextItemWidth(-1.0f);
            if (UIWidgets::InputString(
                    "##shaderPackFilter", &filterValue,
                    UIWidgets::InputOptions()
                        .LabelPosition(UIWidgets::LabelPositions::None)
                        .PlaceholderText("Filter by name..."))) {
                std::strncpy(s_shaderPackFilter, filterValue.c_str(),
                             sizeof(s_shaderPackFilter) - 1);
                s_shaderPackFilter[sizeof(s_shaderPackFilter) - 1] = '\0';
            }

            // Lower-case copy of the filter — case-insensitive substring
            // match is what users expect from a name filter.
            std::string filterLower = s_shaderPackFilter;
            std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            // Reserve room above for the action row at the bottom so
            // the Install button doesn't shove the list off-screen on
            // the last frame. Themed checkboxes are taller than the
            // ImGui defaults; use the actual frame height + spacing
            // for the reservation.
            ImGui::BeginChild("##shaderPackList",
                              ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - 8.0f),
                              true);
            int visibleCount = 0;
            for (size_t i = 0; i < s_shaderPackCandidatesUi.size(); ++i) {
                const auto& cand = s_shaderPackCandidatesUi[i];
                if (!filterLower.empty()) {
                    std::string labelLower = cand.displayLabel;
                    std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (labelLower.find(filterLower) == std::string::npos) {
                        continue;
                    }
                }
                ++visibleCount;
                bool checked = s_shaderPackSelected[i];
                if (UIWidgets::Checkbox(cand.displayLabel.c_str(), &checked,
                                        UIWidgets::CheckboxOptions().Color(
                                            UIWidgets::Colors::LightBlue))) {
                    s_shaderPackSelected[i] = checked;
                }
            }
            if (visibleCount == 0 && !filterLower.empty()) {
                ImGui::TextDisabled("No shaders match this filter.");
            }
            ImGui::EndChild();

            int totalChecked = 0;
            for (bool b : s_shaderPackSelected) {
                if (b) ++totalChecked;
            }

            const std::string installLabel =
                "Install " + std::to_string(totalChecked) + " shader" +
                (totalChecked == 1 ? "" : "s");
            const bool canInstall = totalChecked > 0;
            if (!canInstall) {
                ImGui::BeginDisabled();
            }
            if (UIWidgets::Button(installLabel.c_str(),
                                  UIWidgets::ButtonOptions()
                                      .Size(ImVec2(220.0f, 0.0f))
                                      .Color(UIWidgets::Colors::Green))) {
                std::vector<std::string> picks;
                picks.reserve(static_cast<size_t>(totalChecked));
                for (size_t i = 0; i < s_shaderPackCandidatesUi.size(); ++i) {
                    if (s_shaderPackSelected[i]) {
                        picks.push_back(s_shaderPackCandidatesUi[i].stem);
                    }
                }
                ssb64::enhancements::InstallSelectedShaderPackAsync(picks);
            }
            if (!canInstall) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (UIWidgets::Button("Cancel",
                                  UIWidgets::ButtonOptions()
                                      .Size(kSmallButton)
                                      .Color(UIWidgets::Colors::Gray))) {
                closeWithCancel();
            }
            break;
        }
        case ShaderPackPhase::InstallingSelected: {
            ImGui::TextWrapped("%s", status.empty() ? "Installing..." : status.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Just copying files now - this should take less than a second.");
            break;
        }
        case ShaderPackPhase::Done: {
            ImGui::TextWrapped("%s", status.empty() ? "Done." : status.c_str());
            ImGui::Spacing();
            if (UIWidgets::Button("OK",
                                  UIWidgets::ButtonOptions()
                                      .Size(kSmallButton)
                                      .Color(UIWidgets::Colors::Green))) {
                s_shaderPackCandidatesUi.clear();
                s_shaderPackSelected.clear();
                s_shaderPackLastPhase = ShaderPackPhase::Idle;
                ImGui::CloseCurrentPopup();
            }
            break;
        }
    }

    ImGui::EndPopup();
}
#endif // !__ANDROID__ (shader-pack downloader)

// Modal companion to RenderPostProcessShaderPicker. Drawn from
// RenderMenuTopLevel each frame so the popup survives the picker's
// combo closing. The picker sets s_lowResWarnNeedsOpen when the user
// selects a `compat=native` shader while Low Resolution Mode is off.
void RenderPostProcessLowResWarnModal() {
    if (s_lowResWarnNeedsOpen) {
        ImGui::OpenPopup("Low Resolution Mode##postprocess");
        s_lowResWarnNeedsOpen = false;
    }

    // Center the modal even though the picker may have triggered it
    // from a deep ImGui menu — the user shouldn't have to chase it
    // back to the picker's screen position.
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Low Resolution Mode##postprocess",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped("\"%s\" was designed for the native 240p source resolution.",
                       s_lowResWarnShader.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("Low Resolution Mode is off, so the source framebuffer is the "
                       "full window size — the shader will render correctly, but the "
                       "picture will live in a small corner of the screen.");
    ImGui::Spacing();
    ImGui::TextWrapped("Enabling Low Resolution Mode forces the source FB to 320x240 "
                       "and requires a restart to take effect (it's latched at boot to "
                       "avoid mid-session Fast3D resize hazards).");
    ImGui::Spacing();

    if (ImGui::Button("Enable N64 mode (latched, restart required)")) {
        CVarSetInteger("gLowResModePending", 1);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep current settings")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Disable post-process")) {
        CVarSetInteger("gPostProcessEnabled", 0);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

void PortMenu::AddSidebarEntry(std::string sectionName, std::string sidebarName, uint32_t columnCount) {
    assert(!sectionName.empty());
    assert(!sidebarName.empty());
    menuEntries.at(sectionName).sidebars.emplace(sidebarName, SidebarEntry{ .columnCount = columnCount });
    menuEntries.at(sectionName).sidebarOrder.push_back(sidebarName);
}

WidgetInfo& PortMenu::AddWidget(WidgetPath& pathInfo, std::string widgetName, WidgetType widgetType) {
    assert(!widgetName.empty());
    assert(menuEntries.contains(pathInfo.sectionName));
    assert(menuEntries.at(pathInfo.sectionName).sidebars.contains(pathInfo.sidebarName));

    auto& sidebars = menuEntries.at(pathInfo.sectionName).sidebars;
    const uint8_t column = pathInfo.column;
    while (sidebars.at(pathInfo.sidebarName).columnWidgets.size() < column + 1) {
        sidebars.at(pathInfo.sidebarName).columnWidgets.push_back({});
    }

    SidebarEntry& entry = sidebars.at(pathInfo.sidebarName);
    entry.columnWidgets.at(column).push_back({ .name = std::move(widgetName), .type = widgetType });
    WidgetInfo& widget = entry.columnWidgets.at(column).back();

    switch (widgetType) {
        case WIDGET_CHECKBOX:
        case WIDGET_CVAR_CHECKBOX:
            widget.options = std::make_shared<CheckboxOptions>();
            break;
        case WIDGET_SLIDER_FLOAT:
        case WIDGET_CVAR_SLIDER_FLOAT:
            widget.options = std::make_shared<FloatSliderOptions>();
            break;
        case WIDGET_CVAR_BTN_SELECTOR:
            widget.options = std::make_shared<BtnSelectorOptions>();
            break;
        case WIDGET_SLIDER_INT:
        case WIDGET_CVAR_SLIDER_INT:
            widget.options = std::make_shared<IntSliderOptions>();
            break;
        case WIDGET_COMBOBOX:
        case WIDGET_CVAR_COMBOBOX:
        case WIDGET_AUDIO_BACKEND:
        case WIDGET_VIDEO_BACKEND:
            widget.options = std::make_shared<ComboboxOptions>();
            break;
        case WIDGET_BUTTON:
            widget.options = std::make_shared<ButtonOptions>();
            break;
        case WIDGET_WINDOW_BUTTON:
            widget.options = std::make_shared<WindowButtonOptions>();
            break;
        case WIDGET_CVAR_COLOR_PICKER:
        case WIDGET_COLOR_PICKER:
            widget.options = std::make_shared<ColorPickerOptions>();
            break;
        case WIDGET_SEPARATOR_TEXT:
        case WIDGET_TEXT:
            widget.options = std::make_shared<TextOptions>();
            break;
        case WIDGET_SEARCH:
        case WIDGET_SEPARATOR:
        default:
            widget.options = std::make_shared<WidgetOptions>();
            break;
    }

    return widget;
}

PortMenu::PortMenu()
    : Menu("gOpenPortMenu", "Port Menu", 0, UIWidgets::Colors::LightBlue) {
}

void PortMenu::AddMenuSettings() {
    AddSidebarEntry("Settings", "General", 2);

    WidgetPath path = { "Settings", "General", SECTION_COLUMN_1 };
    AddWidget(path, "Menu Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Menu Theme", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("Menu.Theme"))
        .RaceDisable(false)
        .Options(ComboboxOptions().Tooltip("Changes the theme of the menu widgets.")
                     .ComboMap(kMenuThemeOptions)
                     .DefaultIndex(Colors::LightBlue));

    AddWidget(path, "Menu Controller Navigation", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_IMGUI_CONTROLLER_NAV)
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Allows controller navigation of the port menu.\nCAUTION: this disables game inputs while the menu is visible."));

    AddWidget(path, "Allow background inputs", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ALLOW_BACKGROUND_INPUTS)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
                        CVarGetInteger(CVAR_ALLOW_BACKGROUND_INPUTS, 1) ? "1" : "0");
        })
        .Options(CheckboxOptions().Tooltip(
                     "Allows controller input to be picked up by the game even when the window is not focused.")
                     .DefaultValue(true));

    AddWidget(path, "Menu Background Opacity", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("Menu.BackgroundOpacity"))
        .RaceDisable(false)
        .Options(FloatSliderOptions().DefaultValue(0.85f).IsPercentage().Tooltip(
            "Sets the opacity of the background behind the port menu."));

    AddWidget(path, "General Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Cursor Always Visible", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("CursorVisibility"))
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            Ship::Context::GetInstance()->GetWindow()->SetForceCursorVisibility(
                CVarGetInteger(CVAR_SETTING("CursorVisibility"), 0));
        })
        .Options(CheckboxOptions().Tooltip("Makes the cursor always visible, even in fullscreen."));

    AddWidget(path, "Search In Sidebar", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("Menu.SidebarSearch"))
        .RaceDisable(false)
        .Callback([this](WidgetInfo&) {
            if (CVarGetInteger(CVAR_SETTING("Menu.SidebarSearch"), 0)) {
                InsertSidebarSearch();
            } else {
                RemoveSidebarSearch();
            }
        })
        .Options(CheckboxOptions().Tooltip(
            "Displays the Search page as a sidebar entry in Settings instead of in the header."));

    AddWidget(path, "Search Input Autofocus", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("Menu.SearchAutofocus"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Gives the search input focus when it becomes visible."));

#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    // DRP — Android drops discord-rpc entirely (no curl + Play Store
    // distribution friction; the Discord SDK isn't built for mobile).
    AddWidget(path, "Enable Discord Rich Presence", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SETTING("Menu.EnableDRP"))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Adds Discord Rich Presence (DRP)."));
#endif

    AddWidget(path, "Open App Files Folder", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            std::string filesPath = Ship::Context::GetInstance()->GetAppDirectoryPath();
            SDL_OpenURL(std::string("file:///" + fs::absolute(filesPath).string()).c_str());
        })
        .Options(ButtonOptions().Tooltip("Opens the folder that contains the save, config, and asset files."));

    path.sidebarName = "Graphics";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Settings", "Graphics", 2);

    AddWidget(path, "Graphics Options", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Toggle Fullscreen", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) { Ship::Context::GetInstance()->GetWindow()->ToggleFullscreen(); })
        .Options(ButtonOptions().Tooltip("Toggles fullscreen on or off."));

    AddWidget(path, "Internal Resolution", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_INTERNAL_RESOLUTION)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            Ship::Context::GetInstance()->GetWindow()->SetResolutionMultiplier(
                CVarGetFloat(CVAR_INTERNAL_RESOLUTION, 1.0f));
        })
        .PreFunc([this](WidgetInfo& info) {
            if (disabledMap.at(DISABLE_FOR_ADVANCED_RESOLUTION_ON).active &&
                disabledMap.at(DISABLE_FOR_VERTICAL_RES_TOGGLE_ON).active) {
                info.activeDisables.push_back(DISABLE_FOR_ADVANCED_RESOLUTION_ON);
                info.activeDisables.push_back(DISABLE_FOR_VERTICAL_RES_TOGGLE_ON);
            } else if (disabledMap.at(DISABLE_FOR_LOW_RES_MODE_ON).active) {
                info.activeDisables.push_back(DISABLE_FOR_LOW_RES_MODE_ON);
            }
        })
        .Options(FloatSliderOptions()
                     .Tooltip("Multiplies the internal rendering resolution.")
                     .ShowButtons(false)
                     .IsPercentage()
                     .Min(0.5f)
                     .Max(2.0f));

    AddWidget(path, "Low Resolution Mode (Needs reload)", WIDGET_CVAR_COMBOBOX)
        .CVar("gLowResModePending")
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("Forces the internal framebuffer to a low resolution. "
                              "Stretched modes scale the framebuffer to fill the game viewport. "
                              "Pixel-perfect modes render N64-native 320x240 and draw it at an integer "
                              "factor, centred with black borders — sharper retro look, no sub-pixel blur. "
                              "Auto-fit picks the largest factor that still fits in the window. "
                              "Overrides Internal Resolution while active. "
                              "Latched at startup — changes take effect on next launch (toggling mid-session "
                              "would resize the framebuffer and race the Metal renderer).")
                     .ComboMap(kLowResModeMap)
                     .DefaultIndex(0));

#ifndef __WIIU__
    AddWidget(path, "Anti-aliasing (MSAA)", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_MSAA_VALUE)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            Ship::Context::GetInstance()->GetWindow()->SetMsaaLevel(CVarGetInteger(CVAR_MSAA_VALUE, 1));
        })
        .Options(IntSliderOptions()
                     .Tooltip("Sets the MSAA level for rendered geometry.")
                     .Min(1)
                     .Max(8)
                     .DefaultValue(1));
#endif

#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    // Post-process / CRT shader stack — hidden on Android. The libretro
    // slang→SPIR-V→backend transpile path hasn't been validated against
    // GLES on mobile, and the libretro shader catalog download (curl +
    // ZIP extraction) is a poor fit for a Play Store distribution.
    AddWidget(path, "Post-Process Shader", WIDGET_CUSTOM)
        .RaceDisable(false)
        .CustomFunction(RenderPostProcessShaderPicker);

    AddWidget(path, "Shader Pack Downloader", WIDGET_CUSTOM)
        .RaceDisable(false)
        .CustomFunction(RenderShaderPackDownloader);

    AddWidget(path, "Shader Runtime Diagnostics", WIDGET_CUSTOM)
        .RaceDisable(false)
        .CustomFunction(RenderPostProcessDiagnostics);
#endif

    AddWidget(path, "Renderer API (Needs reload)", WIDGET_VIDEO_BACKEND).RaceDisable(false);
    AddWidget(path, "Enable Vsync", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_VSYNC_ENABLED)
        .RaceDisable(false)
        .PreFunc([this](WidgetInfo& info) { info.isHidden = disabledMap.at(DISABLE_FOR_NO_VSYNC).active; })
        .Options(CheckboxOptions().Tooltip("Removes tearing, but can cap the game to the display refresh rate.")
                     .DefaultValue(true));

    AddWidget(path, "Enhanced Framerate (Interpolated)", WIDGET_CVAR_COMBOBOX)
        .CVar(PORT_INTERP_CVAR_FPS)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) { portInterpApplyConfig(); })
        .PreFunc([this](WidgetInfo& info) {
            if (disabledMap.at(DISABLE_FOR_MATCH_REFRESH_RATE_ON).active) {
                info.activeDisables.push_back(DISABLE_FOR_MATCH_REFRESH_RATE_ON);
            }
        })
        .Options(ComboboxOptions()
                     .Tooltip("Renders extra frames between 60 Hz game ticks by interpolating object "
                              "and camera motion. Game logic, physics, and input timing stay at exactly "
                              "60 Hz; only rendering is smoother. Adds up to one tick (16 ms) of visual "
                              "latency. Requires a display refresh rate at or above the chosen value "
                              "(or Vsync off) — if the host can't keep up, the game automatically steps "
                              "back down to protect game speed.")
                     .ComboMap(kEnhancedFpsMap)
                     .DefaultIndex(0));

    AddWidget(path, "Match Display Refresh Rate", WIDGET_CVAR_CHECKBOX)
        .CVar(PORT_INTERP_CVAR_MATCH_DISPLAY)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) { portInterpApplyConfig(); })
        .Options(CheckboxOptions()
                     .Tooltip("Locks interpolated rendering to the active display refresh rate, from 60 through "
                              "240 Hz. Fractional frame cadence preserves the fixed 60 Hz game simulation on "
                              "90/120/144/165 Hz phones, TVs, monitors, and Xbox display modes.")
                     .DefaultValue(false));

#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    AddWidget(path, "Windowed Fullscreen", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_SDL_WINDOWED_FULLSCREEN)
        .RaceDisable(false)
        .PreFunc([this](WidgetInfo& info) {
            info.isHidden = disabledMap.at(DISABLE_FOR_NO_WINDOWED_FULLSCREEN).active;
        })
        .Options(CheckboxOptions().Tooltip("Enables borderless windowed fullscreen mode."));
#endif

    AddWidget(path, "Allow multi-windows", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ENABLE_MULTI_VIEWPORTS)
        .RaceDisable(false)
        .PreFunc([this](WidgetInfo& info) {
            info.isHidden = disabledMap.at(DISABLE_FOR_NO_MULTI_VIEWPORT).active;
        })
        .Options(CheckboxOptions()
                     .Tooltip("Allows multiple ImGui windows to be opened at once. Requires reload to take effect.")
                     .DefaultValue(true));

    AddWidget(path, "Texture Filter (Needs reload)", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_TEXTURE_FILTER)
        .RaceDisable(false)
        .Options(ComboboxOptions().Tooltip("Sets the active texture filtering mode.").ComboMap(kTextureFilteringMap));

    AddWidget(path, "Widescreen (Needs reload)", WIDGET_CVAR_CHECKBOX)
        .CVar(enhancements::WidescreenCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions()
                     .Tooltip(
                         "Renders battle scenes with a wider field of view to fill widescreen "
                         "windows. Title screen, menus, and intros stretch to fill the window "
                         "(no widescreen art). Some stages may show authored background edges "
                         "that were never visible in 4:3. Experimental — restart required for "
                         "the toggle to take effect.")
                     .DefaultValue(true));

    // Overscan crop. SSB64 bakes a 10px title-safe margin into its GBI scissor on
    // every side (decomp sys/rdp.c + sys/objdisplay.c); a CRT's overscan hid it,
    // but a desktop window shows it as a black border framing the whole image.
    // libultraship's Gui::DrawGame crops this proportional margin out of the
    // presented framebuffer so the safe area fills the viewport. Takes effect
    // immediately (read every frame at present time, no reload needed).
    AddWidget(path, "Crop Overscan Border", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_PREFIX_ADVANCED_RESOLUTION ".Overscan.Enabled")
        .RaceDisable(false)
        .Options(CheckboxOptions()
                     .Tooltip("Removes the black border framing the image by cropping the game's built-in "
                              "title-safe margin, so the picture fills the window like it filled a CRT. "
                              "Disable to see the raw N64 framebuffer including its overscan border.")
                     .DefaultValue(true));

    AddWidget(path, "Overscan Crop — Horizontal", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_PREFIX_ADVANCED_RESOLUTION ".Overscan.Horizontal")
        .RaceDisable(false)
        .Options(FloatSliderOptions()
                     .Tooltip("How many N64 pixels (of 320 wide) to crop from each of the left and right "
                              "edges. The game's authored margin is 10.")
                     .Min(0.0f)
                     .Max(32.0f)
                     .DefaultValue(10.0f));

    AddWidget(path, "Overscan Crop — Vertical", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_PREFIX_ADVANCED_RESOLUTION ".Overscan.Vertical")
        .RaceDisable(false)
        .Options(FloatSliderOptions()
                     .Tooltip("How many N64 pixels (of 240 tall) to crop from each of the top and bottom "
                              "edges. The game's authored margin is 10.")
                     .Min(0.0f)
                     .Max(32.0f)
                     .DefaultValue(10.0f));

    path.sidebarName = "Gameplay";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Settings", "Gameplay", 1);

    AddWidget(path, "Disable Stage Hazards", WIDGET_CVAR_CHECKBOX)
        .CVar(enhancements::StageHazardsDisabledCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Disables stage specific hazards such as moving platforms, tornadoes, Arwings, wind, etc."
            "Does not take effect mid-battle, only between battles.")
                     .DefaultValue(false));
    AddWidget(path, "Bonus Stages", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::BonusStagesCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions()
                     .Tooltip("Shows the port-added bonus stage page (Final Destination, "
                              "Metal Cavern, Battlefield) on the VS / Training stage select "
                              "screen. Turn off for the original stage roster. Takes effect "
                              "the next time the stage select screen opens.")
                     .DefaultValue(true));
    AddWidget(path, "Debug", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Hitbox View", WIDGET_CVAR_COMBOBOX)
        .CVar(enhancements::HitboxViewCVarName())
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("Replaces fighter/item/weapon rendering with the debug "
                              "hitbox visualization the decomp ships. Filled mode "
                              "draws red cubes for active hitboxes; Outline mode "
                              "draws hitbox edges and shows hurtboxes as solid "
                              "yellow/green/blue boxes (yellow=normal, green="
                              "invincible, blue=intangible).")
                     .ComboMap(kHitboxViewMap)
                     .DefaultIndex(0));

    // --- Competitive ruleset (still in Gameplay sidebar) ---
    AddWidget(path, "Competitive Rules", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Use Competitive Ruleset", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::CompRulesetCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Forces the following in VS Mode: \n"
                                           "- 4 Stocks \n"
                                           "- 8 Minutes \n"
                                           "- Items Off \n"
                                           "- Team Attack ON \n"
                                           "- Dream Land only"));
    AddWidget(path, "Neutral Spawns on Dreamland", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::NeutralSpawnsCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Forces balanced, opposite-side starting positions for 1v1 and Team Battles."));

    // --- Z-Cancel options ---
    AddWidget(path, "Z-Cancel Options", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Auto Z-Cancel", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::AutoZCancelCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Automatically cancels the landing lag from aerial attacks."));
    AddWidget(path, "Flash on Failed Z-Cancel", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::FailedZCancelCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Makes your character temporarily flash in flames on a failed Z-Cancel."));

    // --- Quality-of-Life (still in Gameplay sidebar) ---
    AddWidget(path, "Quality-of-Life", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Boot to VS CSS", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::BootToVSCSSCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Skips the intro sequences and boots directly to the VS Mode Character Select Screen."));
    AddWidget(path, "Classic Co-op (Needs reload)", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::ClassicCoopCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions()
                     .Tooltip("Classic (1P) mode uses the VS Mode Character Select Screen so a "
                              "second player can join the whole run as a co-op partner. If only "
                              "one player picks a character, the run plays as normal solo "
                              "Classic. Restart required for the toggle to take effect.")
                     .DefaultValue(true));
    AddWidget(path, "Co-op Friendly Fire", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::ClassicCoopFriendlyFireCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions()
                     .Tooltip("Lets the two Classic Co-op players hit each other, like the "
                              "original 1P mode's CPU ally could. Off by default; takes effect "
                              "from the next stage.")
                     .DefaultValue(false));
    AddWidget(path, "Skip Results Screen", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::SkipResultsScreenCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Skips the victory podium and returns directly to the Character Select Screen."));
    AddWidget(path, "Force CPU Level 9", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::CpuLevel9CVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Automatically forces all CPU players to Level 9."));
    AddWidget(path, "Shuffle Music", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::ShuffleMusicCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Randomizes the BGM after a stage is chosen. Useful when enabling the competitive ruleset."));
    AddWidget(path, "Add Music Selection Screen", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::MusicSelectionCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Allows the player to pick a custom BGM track after the stage selection screen (music shuffling is ignored if this is turned on)."));

    // --- Other ---
    AddWidget(path, "Other", WIDGET_SEPARATOR_TEXT);
        AddWidget(path, "Disable HUD", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::DisableHUDCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Disables the in-game HUD. Note that some icons will still be visible (e.g. CPU icons in 1P Mode on the top-left.)"));

    // --- Input customization ---
    path.sectionName = "Controls";
    path.sidebarName = "Input Mappings";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Controls", "Input Mappings", 1);

    AddWidget(path, "Controller", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Controller Configuration", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_CONTROLLER_CONFIGURATION_WINDOW_OPEN)
        .RaceDisable(false)
        .WindowName("Input Editor")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the controller configuration window."));

    // --- Controls sidebar: per-player input remapping ---
    path.sidebarName = "Control Enhancements";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Controls", "Control Enhancements", 1);

    for (int p = 0; p < PORT_ENHANCEMENT_MAX_PLAYERS; ++p) {
        const std::string playerLabel = fmt::format("Player {}", p + 1);
        AddWidget(path, playerLabel, WIDGET_SEPARATOR_TEXT);

        AddWidget(path, fmt::format("Disable Tap Jump (P{})", p + 1), WIDGET_CVAR_CHECKBOX)
            .CVar(enhancements::TapJumpCVarName(p))
            .RaceDisable(false)
            .Options(CheckboxOptions().Tooltip("Disables jumping by pushing up on the analog stick."));

        AddWidget(path, fmt::format("C-Stick Smash (P{})", p + 1), WIDGET_CVAR_CHECKBOX)
            .CVar(enhancements::CStickSmashCVarName(p))
            .RaceDisable(false)
            .Options(CheckboxOptions().Tooltip("Replaces C-Button inputs with instant smash attacks."));

        AddWidget(path, fmt::format("D-Pad to Jump (P{})", p + 1), WIDGET_CVAR_CHECKBOX)
            .CVar(enhancements::DPadJumpCVarName(p))
            .RaceDisable(false)
            .Options(CheckboxOptions().Tooltip("Translates N64 D-Pad inputs into C-Up (Jump)."));

        AddWidget(path, fmt::format("NRage Analog Stick Remap (P{})", p + 1), WIDGET_CVAR_CHECKBOX)
            .CVar(enhancements::AnalogRemapCVarName(p))
            .RaceDisable(false)
            .Options(CheckboxOptions().Tooltip(
                "When ON: replaces libultraship's stick processing with the NRage per-axis "
                "deadzone+range formula (no octagonal gate, no radial deadzone, no notch snap). "
                "When OFF: libultraship's stock pipeline runs unchanged. Both schemes coexist; "
                "this toggle picks which one runs for this player."));

        const char* deadzoneCVar = enhancements::AnalogRemapDeadzoneCVarName(p);
        const char* enableCVar   = enhancements::AnalogRemapCVarName(p);
        AddWidget(path, fmt::format("Remap Deadzone (P{})", p + 1), WIDGET_CVAR_SLIDER_FLOAT)
            .CVar(deadzoneCVar)
            .RaceDisable(false)
            .PreFunc([enableCVar](WidgetInfo& info) {
                if (!CVarGetInteger(enableCVar, 0)) {
                    info.options->disabled = true;
                    info.options->disabledTooltip = "Enable NRage Analog Stick Remap to adjust this.";
                }
            })
            .Options(FloatSliderOptions()
                         .Min(0.0f).Max(0.99f).DefaultValue(0.10f)
                         .IsPercentage()
                         .Tooltip("Per-axis deadzone (0–99%). Inputs inside this fraction of "
                                  "max deflection on each axis read as 0."));

        const char* rangeCVar = enhancements::AnalogRemapRangeCVarName(p);
        AddWidget(path, fmt::format("Remap Range (P{})", p + 1), WIDGET_CVAR_SLIDER_FLOAT)
            .CVar(rangeCVar)
            .RaceDisable(false)
            .PreFunc([enableCVar](WidgetInfo& info) {
                if (!CVarGetInteger(enableCVar, 0)) {
                    info.options->disabled = true;
                    info.options->disabledTooltip = "Enable NRage Analog Stick Remap to adjust this.";
                }
            })
            .Options(FloatSliderOptions()
                         .Min(0.5f).Max(1.5f).DefaultValue(1.0f)
                         .IsPercentage()
                         .Tooltip("Output scale (50–150%). 100% caps at the N64's natural max."));
    }

    // --- Audio ---
    path.sectionName = "Settings";
    path.sidebarName = "Audio";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Settings", "Audio", 1);

    AddWidget(path, "Volume Levels", WIDGET_SEPARATOR_TEXT);


    // Master
    AddWidget(path, "Master Volume", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gSettings.Audio.MasterVolume")
        .RaceDisable(false)
        .Options(FloatSliderOptions()
        .Tooltip("Adjust the overall volume of the entire game.")
        .DefaultValue(1.0f)
        .Min(0.0f)
        .Max(1.0f)
        .IsPercentage());

    // Music Volume Slider
    AddWidget(path, "Music Volume", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gSettings.Audio.MusicVolume")
        .RaceDisable(false)
        .Options(FloatSliderOptions()
        .Tooltip("Adjust the volume of the background music.")
        .DefaultValue(1.0f)
        .Min(0.0f)
        .Max(1.0f)
        .IsPercentage());

    // Sound Effects Volume Slider
    AddWidget(path, "Sound Volume", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gSettings.Audio.SFXVolume") // Standard LUS native CVar
        .RaceDisable(false)
        .Options(FloatSliderOptions()
        .Tooltip("Adjust the volume of sound effects and voices.")
        .DefaultValue(1.0f)
        .Min(0.0f)
        .Max(1.0f)
        .IsPercentage());

    // voice
    AddWidget(path, "Voice Volume", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gSettings.Audio.VoiceVolume")
        .RaceDisable(false)
        .Options(FloatSliderOptions()
        .Tooltip("Adjust the volume of character voices and the announcer.")
        .DefaultValue(1.0f)
        .Min(0.0f)
        .Max(1.0f)
        .IsPercentage());

    AddWidget(path, "Focus Behavior", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Mute on Focus Loss", WIDGET_CVAR_CHECKBOX)
        .CVar("gSettings.FocusControl.MuteOnFocusLoss")
        .RaceDisable(false)
        .Options(CheckboxOptions()
            .Tooltip("Silences game audio while the window is unfocused. Restores volume on refocus.")
            .DefaultValue(false));

    // --- Cheats ---
    path.sectionName = "Enhancements";
    path.sidebarName = "Cheats";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Enhancements", "Cheats", 1);

    AddWidget(path, "Master Unlocks", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Unlock Everything", WIDGET_CVAR_CHECKBOX)
        .CVar("gCheats.UnlockAll")
        .RaceDisable(false);

    AddWidget(path, "Individual Characters", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Unlock Captain Falcon", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockCaptain").RaceDisable(false);
    AddWidget(path, "Unlock Jigglypuff", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockPurin").RaceDisable(false);
    AddWidget(path, "Unlock Luigi", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockLuigi").RaceDisable(false);
    AddWidget(path, "Unlock Ness", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockNess").RaceDisable(false);

    AddWidget(path, "Individual Features", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Unlock Mushroom Kingdom", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockInishie").RaceDisable(false);
    AddWidget(path, "Unlock Item Switch Menu", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockItemSwitch").RaceDisable(false);
    AddWidget(path, "Unlock Sound Test Menu", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockSoundTest").RaceDisable(false);

    AddWidget(path, "Gameplay Cheats (offline only)", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Enable Gameplay Cheats", WIDGET_CVAR_CHECKBOX)
        .CVar("gCheats.Gameplay")
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Master switch for the cheats below. They only affect your own fighters and turn off automatically in online rooms."));
    AddWidget(path, "Infinite Jumps", WIDGET_CVAR_CHECKBOX).CVar("gCheats.InfiniteJumps").RaceDisable(false);
    AddWidget(path, "Infinite Shield", WIDGET_CVAR_CHECKBOX).CVar("gCheats.InfiniteShield").RaceDisable(false);
    AddWidget(path, "Always 0% Damage", WIDGET_CVAR_CHECKBOX).CVar("gCheats.ZeroPercent").RaceDisable(false);
    AddWidget(path, "Infinite Stocks / Lives", WIDGET_CVAR_CHECKBOX).CVar("gCheats.InfiniteStocks").RaceDisable(false);
    AddWidget(path, "CPU Fighters Always at 999%", WIDGET_CVAR_CHECKBOX)
        .CVar("gCheats.Enemy999")
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Every CPU opponent sits at 999% so a single hit sends them flying."));

    // --- Tools ---
    path.sectionName = "Dev Tools";
    path.sidebarName = "Tools";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Dev Tools", "Tools", 1);

    AddWidget(path, "Debug Windows", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Stats", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_STATS_WINDOW_OPEN)
        .RaceDisable(false)
        .WindowName("Stats")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the Stats window."));
    AddWidget(path, "Console", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_CONSOLE_WINDOW_OPEN)
        .RaceDisable(false)
        .WindowName("Console")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the Console window."));
    AddWidget(path, "Gfx Debugger", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_GFX_DEBUGGER_WINDOW_OPEN)
        .RaceDisable(false)
        .WindowName("GfxDebuggerWindow")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the graphics debugger window."));
}

void PortMenu::AddMenuAssets() {

    WidgetPath path = { "Dev Tools", "Extraction", SECTION_COLUMN_1 };
    AddSidebarEntry("Dev Tools", "Extraction", 1);
    AddWidget(path, "Extraction", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, SSB64_O2R_NAME " is generated from your ROM and loaded on launch.", WIDGET_TEXT);
    AddWidget(path, "Schedule Re-extract", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([this](WidgetInfo&) {
            const auto path = Ship::Context::GetPathRelativeToAppDirectory(SSB64_O2R_NAME);
            std::error_code ec;
            fs::remove(path, ec);
            mShowReextractMessage = true;
        })
        .Options(ButtonOptions().Tooltip("Deletes " SSB64_O2R_NAME " so it is regenerated from the ROM on next launch.")
                     .Size(Sizes::Inline));

    path.sidebarName = "Paths";
    AddSidebarEntry("Dev Tools", "Paths", 1);
    AddWidget(path, "Runtime Paths", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, fmt::format("App directory: {}", Ship::Context::GetPathRelativeToAppDirectory("")), WIDGET_TEXT);
    AddWidget(path, fmt::format("Main archive: {}", Ship::Context::GetPathRelativeToAppDirectory(SSB64_O2R_NAME)),
              WIDGET_TEXT);

#ifdef PORT_HIRES_ENABLED
    path.sectionName = "Settings";
    path.sidebarName = "Mods";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Settings", "Mods", 1);

    AddWidget(path, "Hi-Res Texture Pack", WIDGET_SEPARATOR_TEXT);
    AddWidget(path,
              "Drop a PNG pack into the mods/ folder. "
              "Files are matched by decoded-RGBA8 CRC32 — no per-PNG configuration needed. "
              "Toggle takes effect on the next texture cache miss.",
              WIDGET_TEXT);
    AddWidget(path, "Enable Hi-Res Texture Pack", WIDGET_CVAR_CHECKBOX)
        .CVar("gHiResTextures.Enabled")
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("When on, every cache-miss texture upload hashes the decoded RGBA8 image "
                                            "and substitutes a matching PNG from the mods/ index at the pack's higher "
                                            "resolution.")
                     .DefaultValue(ssb64::hires::kHiResEnabledDefault != 0));
    AddWidget(path, "Preload Pack at Boot", WIDGET_CVAR_CHECKBOX)
        .CVar("gHiResTextures.Preload")
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Decodes the pack into RAM on background threads at startup so first use "
                                            "of a texture doesn't stutter on a synchronous PNG decode. Warms up to "
                                            "the cache budget below. Takes effect on next launch.")
                     .DefaultValue(ssb64::hires::kHiResPreloadDefault != 0));
    AddWidget(path, "Decoded Cache Budget (MB)", WIDGET_CVAR_SLIDER_INT)
        .CVar("gHiResTextures.CacheBudgetMB")
        .RaceDisable(false)
        .Options(IntSliderOptions()
                     .Tooltip("RAM budget for decoded pack textures. A large HD pack whose working set "
                              "exceeds this re-decodes evicted textures on the render thread (stutter) — "
                              "raise it if you have RAM to spare. Takes effect on next launch.")
                     .Min(ssb64::hires::kMinLruBudgetMB)
                     .Max(4096)
                     .DefaultValue(ssb64::hires::kDefaultLruBudgetMB));
#if defined(__ANDROID__)
    AddWidget(path,
              "Mobile note: decoded textures are uncompressed in RAM and on the GPU. "
              "The pack is capped to a 128 MB cache (gHiResTextures.CacheBudgetMB) and "
              "per-texture upscale limit, but large packs can still pressure memory on "
              "low-RAM devices. Off by default — enable only if your device has headroom.",
              WIDGET_TEXT);
#endif
    AddWidget(path, "Open Mods Folder", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            std::string modsPath = Ship::Context::GetPathRelativeToAppDirectory("mods");
            std::error_code ec;
            fs::create_directories(modsPath, ec); // make it if it isn't there yet
            SDL_OpenURL(std::string("file:///" + fs::absolute(modsPath).string()).c_str());
        })
        .Options(ButtonOptions().Tooltip("Opens the mods/ folder in your file browser. Drop pack subfolders here."));
    AddWidget(path,
              fmt::format("Indexed PNGs: {}",
                           ssb64::hires::HiResPack::Get().Stats().indexedTextures),
              WIDGET_TEXT);

    // Pack-authoring dump tooling is desktop-only — it writes hundreds of
    // files into <app>/ and is driven from a separate offline conversion
    // pipeline, neither of which makes sense on a touch device.
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    AddWidget(path, "Pack Authoring", WIDGET_SEPARATOR_TEXT);
    AddWidget(path,
              "Dump Source Textures writes one .bin per unique texture into "
              "<app>/hires_dump/. Used by the offline conversion tool to "
              "re-name a third-party Rice-CRC pack into our hash. Leave off "
              "in normal play.",
              WIDGET_TEXT);
    AddWidget(path, "Dump Source Textures", WIDGET_CVAR_CHECKBOX)
        .CVar("gHiResTextures.DumpSource")
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("On: every cache-miss with no pack hit dumps the source bytes + dimensions "
                                            "to <app>/hires_dump/. One file per unique texture per session.")
                     .DefaultValue(false));
    AddWidget(path, "Open Dump Folder", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            std::string dumpPath = Ship::Context::GetPathRelativeToAppDirectory("hires_dump");
            std::error_code ec;
            fs::create_directories(dumpPath, ec);
            SDL_OpenURL(std::string("file:///" + fs::absolute(dumpPath).string()).c_str());
        })
        .Options(ButtonOptions().Tooltip("Opens the hires_dump/ folder where source-texture dumps land."));

    AddWidget(path, "Miss Dump (native key)", WIDGET_SEPARATOR_TEXT);
    AddWidget(path,
              "On: every cache-miss with no pack hit writes a .bmp + .rgba into "
              "<app>/hires_miss_dump/, named with the port's native lookup key "
              "(miss#<hash>#<fmt>#<siz>_<w>x<h>). Rename the matching pack PNG to "
              "\"<anything>#<hash>#<fmt>#<siz>_all.png\" and drop it in mods/ to "
              "bind textures whose Rice CRC no dumper can reach (VS-records "
              "digits, tiny UI glyphs). Captures a full playthrough by default.",
              WIDGET_TEXT);
    AddWidget(path, "Dump Unmatched (native key)", WIDGET_CVAR_CHECKBOX)
        .CVar("gHiResTextures.DumpMissRgba")
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("On: each unique unmatched texture is written as a viewable .bmp plus the "
                                            "raw .rgba the hash was computed over. The filename is the exact "
                                            "<hash>#<fmt>#<siz> the pack scanner looks up.")
                     .DefaultValue(false));
    AddWidget(path, "Open Miss-Dump Folder", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            std::string missPath = Ship::Context::GetPathRelativeToAppDirectory("hires_miss_dump");
            std::error_code ec;
            fs::create_directories(missPath, ec);
            SDL_OpenURL(std::string("file:///" + fs::absolute(missPath).string()).c_str());
        })
        .Options(ButtonOptions().Tooltip("Opens the hires_miss_dump/ folder where native-key dumps land."));
#endif // !__ANDROID__ (pack-authoring dump tooling)
#endif // PORT_HIRES_ENABLED
}

#ifndef DISABLE_SCRIPTING
// Cached, read-only mod list for the Mods panel. The set of installed mods
// only changes on a reload or restart, so we snapshot on demand (first view,
// Rescan, and after a Hot Reload) rather than re-scanning every frame.
static std::vector<ssb64::mods::ModInfo> s_modList;
static bool s_modListLoaded = false;

static void RefreshModList() {
    // Sync the mounted set to what's actually on disk so the list reflects mods
    // added or deleted at runtime: drop archives whose folder is gone, pick up
    // newly dropped ones. New mods appear as "not loaded" until a Hot Reload
    // compiles them; this only refreshes the inventory, it does not recompile.
    ssb64::UnmountMissingMods();
    ssb64::MountModsDir();
    s_modList = ssb64::mods::ModRegistry::Snapshot();
    s_modListLoaded = true;
}

static void DoHotReload() {
    auto scripting = Ship::Context::GetInstance()->GetScriptLoader();
    if (!scripting) {
        return;
    }
    try {
        /* Unload: ModExit fires, then HookManager uninstalls every
         * hook owned by that mod (postExit callback runs BEFORE
         * FreeLibrary so trampolines into the about-to-be-freed mod
         * code get cleanly removed). Then recompile + reload with the
         * SetCurrentOwner wrapper so new Install calls re-tag with
         * the right owner. */
        scripting->UnloadAll(
            /*preExit=*/std::nullopt,
            /*postExit=*/[](const std::string& mod) {
                ssb64::mods::HookManager::UninstallHooksForOwner(mod.c_str());
            });
        /* Now that every script is unloaded, drop archives whose mod folder
         * was deleted at runtime — otherwise it stays mounted and gets
         * recompiled below as if it were still installed. Then pick up any
         * new folders/archives that landed in mods/ since the last load
         * (idempotent: already-mounted archives are skipped). */
        ssb64::UnmountMissingMods();
        ssb64::MountModsDir();
        scripting->CompileAll();
        scripting->LoadAll(
            /*preInit=*/[](const std::string& mod) {
                ssb64::mods::HookManager::SetCurrentOwner(mod.c_str());
            },
            /*postInit=*/[](const std::string&) {
                ssb64::mods::HookManager::ClearCurrentOwner();
            });
        RefreshModList();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Mod reload failed: {}", e.what());
    }
}

// Audio Editor: pick which music file plays for each in-game track (like SoH's audio editor).
// Music files come from mods/ (loose `music/` folder or any .zip/.o2r with `music/*.mp3|ogg|wav`).
static const char* const kBgmNames[] = {
    "Dream Land", "Planet Zebes", "Mushroom Kingdom", "Mushroom Kingdom (hurry)", "Sector Z", "Kongo Jungle",
    "Peach's Castle", "Saffron City", "Yoshi's Island", "Hyrule Castle", "Character / Stage Select",
    "Victory (default)", "Victory: Mario", "Victory: Samus", "Victory: Donkey Kong", "Victory: Kirby", "Victory: Fox",
    "Victory: Ness", "Victory: Yoshi", "Victory: Captain Falcon", "Victory: Pokemon", "Victory: Link",
    "VS Results", "Final Stage (Master Hand)", "Master Hand Appears", "Final Destination", "Bonus Stage",
    "1P Stage Clear", "Bonus Stage Clear", "1P Game Clear", "Bonus Stage Failure", "Continue?", "Game Over",
    "Opening Movie", "How to Play", "1P Stage Cards", "Battlefield", "Meta Crystal", "Ending Movie", "Credits",
    "Secret Found", "Challenger Approaching", "Training Mode", "Character Data / VS Records", "Main Menu",
    "Hammer", "Star",
};

void PortMenu::AddMenuAudioEditor() {
    WidgetPath path = { "Enhancements", "Audio Editor", SECTION_COLUMN_1 };
    AddSidebarEntry("Enhancements", "Audio Editor", 1);
    AddWidget(path, "audio_editor_panel", WIDGET_CUSTOM)
        .CustomFunction([](WidgetInfo&) {
            bool enabled = CVarGetInteger("gMusicOverride.Enabled", 1) != 0;
            if (ImGui::Checkbox("Enable music replacement", &enabled)) {
                CVarSetInteger("gMusicOverride.Enabled", enabled ? 1 : 0);
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }
            ImGui::SameLine();
            if (ImGui::Button("Rescan mods")) {
                portMusicRescan();
            }
            const int n = portMusicTrackCount();
            ImGui::TextDisabled("%d music file(s) found in mods/ (music/*.mp3, .ogg, .wav; also inside .zip / .o2r)", n);
            if (n == 0) {
                ImGui::TextWrapped("Build a music pack with the web editor (Music tab) and drop the .zip / .o2r into the mods folder, "
                                   "then press Rescan mods.");
            }
            ImGui::TextWrapped("Pick what plays on each stage / screen: the default music, another song of the game (e.g. Mushroom Kingdom on Dream Land), a custom file, or random. Changes apply the next time that track starts. "
                               "\"Default\" uses a file named after the track number (e.g. 6.mp3) if present, otherwise the original music.");
            if (ImGui::SmallButton("Reset all")) {
                for (int i = 0; i < (int)(sizeof(kBgmNames) / sizeof(kBgmNames[0])); i++) {
                    CVarClear(fmt::format("gMusicOverride.Sel.{}", i).c_str());
                }
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Randomize all")) {
                for (int i = 0; i < (int)(sizeof(kBgmNames) / sizeof(kBgmNames[0])); i++) {
                    CVarSetString(fmt::format("gMusicOverride.Sel.{}", i).c_str(), "random");
                }
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            }
            ImGui::Separator();
            for (int id = 0; id < (int)(sizeof(kBgmNames) / sizeof(kBgmNames[0])); id++) {
                const std::string cv = fmt::format("gMusicOverride.Sel.{}", id);
                const std::string cur = CVarGetString(cv.c_str(), "");
                std::string preview = cur.empty() ? "Default" : (cur == "orig" ? "Original" : (cur == "random" ? "Random custom file" : (cur == "any" ? "Random (any song)" :
                    (cur.rfind("game:", 0) == 0 ? std::string("Song: ") + kBgmNames[std::clamp(std::atoi(cur.c_str() + 5), 0, 46)] : cur))));
                ImGui::PushID(id);
                ImGui::SetNextItemWidth(260.0f);
                if (ImGui::BeginCombo(fmt::format("{:02d}  {}", id, kBgmNames[id]).c_str(), preview.c_str())) {
                    auto pick = [&](const char* label, const char* value) {
                        const bool sel = (cur == value);
                        if (ImGui::Selectable(label, sel)) {
                            if (value[0] == '\0') CVarClear(cv.c_str()); else CVarSetString(cv.c_str(), value);
                            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                        }
                    };
                    pick("Default", "");
                    pick("Original", "orig");
                    pick("Random (any song)", "any");
                    pick("Random custom file", "random");
                    ImGui::Separator();
                    ImGui::TextDisabled("Swap with a song of the game");
                    for (int g = 0; g < (int)(sizeof(kBgmNames) / sizeof(kBgmNames[0])); g++) {
                        if (g == id) continue;
                        const std::string gv = fmt::format("game:{}", g);
                        pick(fmt::format("{:02d}  {}", g, kBgmNames[g]).c_str(), gv.c_str());
                    }
                    ImGui::Separator();
                    ImGui::TextDisabled("Custom files");
                    for (int t = 0; t < n; t++) {
                        pick(portMusicTrackName(t), portMusicTrackName(t));
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
        });
}

void PortMenu::AddMenuMods() {
    WidgetPath path = { "Settings", "Script Mods", SECTION_COLUMN_1 };
    AddSidebarEntry("Settings", "Script Mods", 1);
    AddWidget(path, "mods_panel", WIDGET_CUSTOM)
        .CustomFunction([](WidgetInfo&) {
            // --- Action bar -------------------------------------------------
            if (ImGui::Button("Hot Reload")) {
                DoHotReload(); // re-snapshots s_modList when it finishes
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Unload + recompile + re-init all TCC mods.\n"
                                  "Adding or removing mod folders still needs an engine restart.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Rescan")) {
                RefreshModList();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Re-read the mods/ folder without reloading the mods.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Mods Folder")) {
                std::string modsPath = Ship::Context::GetPathRelativeToAppDirectory("mods");
                std::error_code ec;
                fs::create_directories(modsPath, ec);
                SDL_OpenURL(std::string("file:///" + fs::absolute(modsPath).string()).c_str());
            }

            // Build the list once on first view; thereafter only on demand.
            if (!s_modListLoaded) {
                RefreshModList();
            }

            // --- Filter -----------------------------------------------------
            static char filter[64] = "";
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##modfilter", "Filter by name or author", filter,
                                     sizeof(filter));

            ImGui::Separator();

            if (s_modList.empty()) {
                ImGui::TextWrapped("No mods found. Drop a mod folder or .o2r into the mods/ "
                                   "folder next to the executable, then press Rescan (or "
                                   "restart).");
                return;
            }

            auto toLower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                return s;
            };
            const std::string needle = toLower(filter);
            auto matches = [&](const ssb64::mods::ModInfo& mod) {
                return needle.empty() ||
                       toLower(mod.name).find(needle) != std::string::npos ||
                       toLower(mod.author).find(needle) != std::string::npos;
            };

            // --- Mod cards --------------------------------------------------
            int loadedCount = 0;
            int issueCount = 0;
            int shown = 0;
            for (const auto& mod : s_modList) {
                using ssb64::mods::ModState;
                if (mod.state == ModState::Loaded) {
                    loadedCount++;
                }
                if (mod.state == ModState::InvalidManifest) {
                    issueCount++;
                }
                if (!matches(mod)) {
                    continue;
                }
                shown++;

                ImGui::PushID(mod.archivePath.c_str()); // unique id per card

                switch (mod.state) {
                    case ModState::Loaded:
                        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.40f, 1.0f), "LOADED");
                        break;
                    case ModState::NotLoaded:
                        ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "not loaded");
                        break;
                    case ModState::InvalidManifest:
                        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f), "manifest issue");
                        break;
                }

                ImGui::SameLine();
                if (!mod.version.empty()) {
                    ImGui::Text("%s  v%s", mod.name.c_str(), mod.version.c_str());
                } else {
                    ImGui::TextUnformatted(mod.name.c_str());
                }
                if (ImGui::IsItemHovered() && !mod.archivePath.empty()) {
                    ImGui::SetTooltip("%s", mod.archivePath.c_str());
                }

                if (!mod.author.empty()) {
                    ImGui::TextDisabled("by %s", mod.author.c_str());
                }
                if (!mod.description.empty()) {
                    ImGui::TextWrapped("%s", mod.description.c_str());
                }
                if (!mod.note.empty()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f), "%s", mod.note.c_str());
                }

                ImGui::PopID();
                ImGui::Separator();
            }

            if (shown == 0) {
                ImGui::TextDisabled("No mods match the filter.");
            }

            ImGui::Spacing();
            std::string footer = std::to_string(static_cast<int>(s_modList.size())) +
                                 " installed  |  " + std::to_string(loadedCount) + " loaded";
            if (issueCount > 0) {
                footer += "  |  " + std::to_string(issueCount) + " issue(s)";
            }
            ImGui::TextUnformatted(footer.c_str());
        });
}
#endif


extern "C" {
int syNetLobbyGetFighterCount(void);
int syNetLobbyIsRoomOpen(void);
const char* syNetLobbyGetFighterName(int index);
}

namespace {

// Online / LAN multiplayer panel (host = Player 1, up to 3 clients = P2..P4).
// See docs/netplay.md.
// ---------------------------------------------------------------------------
// Network menu (LAN Online). State lives here so the three sidebars share it.
// ---------------------------------------------------------------------------
struct NpUi {
    std::string name = "Player";
    std::string hostIp = "26.0.0.1";
    int port = 7777;
    int delay = 5;
    int mode = 0;          // 0 = VS Mode, 1 = 1P Mode (co-op)
    int maxPlayers = 4;
    int difficulty = 2;    // 0 Very Easy .. 4 Very Hard
    int lives = 3;         // 1..5
    bool friendlyFire = false;
    int hostCostume = -1;  // -1 = pick on the character select screen
    bool showPanels = true;
    int fighter = 0;
    int costume = 0;
    int stage = -1;        // -1 = stage select screen, -2 = random, 0.. = stage id
    int cheatMask = 0;     // room cheats (bit0 jumps, 1 shield, 2 0%, 3 stocks, 4 CPU 999%)
    bool loaded = false;

    void Load() {
        if (loaded) return;
        loaded = true;
        name = CVarGetString("gNetplay.Name", "Player");
        hostIp = CVarGetString("gNetplay.HostIp", "26.0.0.1");
        port = CVarGetInteger("gNetplay.Port", 7777);
        delay = CVarGetInteger("gNetplay.Delay", 5);
        mode = CVarGetInteger("gNetplay.Mode", 0);
        maxPlayers = CVarGetInteger("gNetplay.MaxPlayers", 4);
        difficulty = CVarGetInteger("gNetplay.Difficulty", 2);
        lives = CVarGetInteger("gNetplay.Lives", 3);
        friendlyFire = CVarGetInteger("gNetplay.FriendlyFire", 0) != 0;
        hostCostume = CVarGetInteger("gNetplay.HostCostume", -1);
        showPanels = CVarGetInteger("gNetplay.ShowPanels", 1) != 0;
        fighter = CVarGetInteger("gNetplay.Fighter", 0);
        costume = CVarGetInteger("gNetplay.Costume", 0);
        stage = CVarGetInteger("gNetplay.Stage", -1);
        cheatMask = CVarGetInteger("gNetplay.RoomCheats", 0);
        np_set_panels(showPanels ? 1 : 0);
        np_set_stage(stage);
        np_set_room_cheats(cheatMask);
    }
    void Save() {
        CVarSetString("gNetplay.Name", name.c_str());
        CVarSetString("gNetplay.HostIp", hostIp.c_str());
        CVarSetInteger("gNetplay.Port", port);
        CVarSetInteger("gNetplay.Delay", delay);
        CVarSetInteger("gNetplay.Mode", mode);
        CVarSetInteger("gNetplay.MaxPlayers", maxPlayers);
        CVarSetInteger("gNetplay.Difficulty", difficulty);
        CVarSetInteger("gNetplay.Lives", lives);
        CVarSetInteger("gNetplay.FriendlyFire", friendlyFire ? 1 : 0);
        CVarSetInteger("gNetplay.HostCostume", hostCostume);
        CVarSetInteger("gNetplay.ShowPanels", showPanels ? 1 : 0);
        CVarSetInteger("gNetplay.Fighter", fighter);
        CVarSetInteger("gNetplay.Costume", costume);
        CVarSetInteger("gNetplay.Stage", stage);
        CVarSetInteger("gNetplay.RoomCheats", cheatMask);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    // Pushes the host-side options into the session (cheap; safe to call every change).
    void ApplyHost() {
        np_set_input_delay(delay);
        np_set_mode(mode);
        np_set_max_players(mode == 1 ? std::clamp(maxPlayers, 2, 3) : std::clamp(maxPlayers, 2, 4));
        np_set_run_options(difficulty, lives - 1);
        np_set_friendly_fire(friendlyFire ? 1 : 0);
        np_set_host_costume(hostCostume);
        np_set_stage(stage);
        np_set_room_cheats(cheatMask);
    }
};
static NpUi U;

static const std::map<int32_t, const char*> kNpStages = {
    { -2, "Random" }, { -1, "Pick on the stage select screen" }, { 0, "Peach's Castle" }, { 1, "Sector Z" },
    { 2, "Kongo Jungle" }, { 3, "Planet Zebes" }, { 4, "Hyrule Castle" }, { 5, "Yoshi's Island" },
    { 6, "Dream Land" }, { 7, "Saffron City" }, { 8, "Mushroom Kingdom (must be unlocked)" },
};
static const std::map<int32_t, const char*> kNpDifficulty = {
    { 0, "Very Easy" }, { 1, "Easy" }, { 2, "Normal" }, { 3, "Hard" }, { 4, "Very Hard" },
};
static const std::map<int32_t, const char*> kNpCostumes = {
    { -1, "From character select" }, { 0, "Costume 1" }, { 1, "Costume 2" }, { 2, "Costume 3" }, { 3, "Costume 4" },
};

static void NpHeader(const char* text) {
    UIWidgets::PaddedSeparator(true, false);
    ImGui::TextColored(ImVec4(0.65f, 0.80f, 1.0f, 1.0f), "%s", text);
    UIWidgets::PaddedSeparator(false, true);
}

static bool NpCheck(const char* label, bool* v, const char* tip) {
    return UIWidgets::Checkbox(label, v, CheckboxOptions().Tooltip(tip));
}

static bool NpFighterPicker(bool live) {
    bool changed = false;
    const int n = syNetLobbyGetFighterCount();
    std::map<int32_t, const char*> fighters;
    for (int i = 0; i < n; i++) fighters[i] = syNetLobbyGetFighterName(i);
    int32_t f = U.fighter;
    if (UIWidgets::Combobox("Your fighter", &f, fighters,
                            ComboboxOptions().Tooltip("The fighter you will use in the match."))) {
        U.fighter = f;
        changed = true;
    }
    if (UIWidgets::SliderInt("Costume", &U.costume,
                             IntSliderOptions().Min(0).Max(3).DefaultValue(0).Tooltip("Costume (0-3) of your fighter."))) {
        changed = true;
    }
    if (changed) {
        if (live) np_set_choice(U.fighter, U.costume);
        U.Save();
    }
    return changed;
}

// "LAN Online": connect, host, join and the lobby.
void RenderLanOnline(WidgetInfo&) {
    U.Load();
    const int state = np_state();
    const bool idle = (state == NP_IDLE || state == NP_FAILED);

    if (np_last_abort_age_ms() >= 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "Last online match ended: %s", np_last_abort_reason());
    }
    ImGui::TextWrapped("Play with friends over a LAN or a VPN (Radmin, Hamachi, ZeroTier or Tailscale; phones and PCs can play together). Everybody must run the exact same "
                       "build with the same settings. The host shares its IP and the UDP port opened in the firewall.");

    if (idle) {
        NpHeader("Your profile");
        if (UIWidgets::InputString("Your name", &U.name, InputOptions().Tooltip("Name shown to the other players (max 16 characters)."))) {
            if (U.name.size() > 16) U.name.resize(16);
            U.Save();
        }
        if (UIWidgets::InputInt("UDP port", &U.port, InputOptions().Tooltip("Port used by the host (1024-65535)."))) {
            U.port = std::clamp(U.port, 1024, 65535);
            U.Save();
        }

        NpHeader("Host a game (you are Player 1)");
        ImGui::TextDisabled("Game mode, stage, players, rules and cheats are set in \"Match Options\".");
        if (UIWidgets::Button("Host game", ButtonOptions().Size(Sizes::Inline).Color(Colors::Green)
                                              .Tooltip("Opens a room and takes you to the VS character select."))) {
            U.Save();
            if (np_host((uint16_t)U.port, U.name.c_str())) {
                U.ApplyHost();
            }
        }

        NpHeader("Join a game (you become Player 2, 3 or 4)");
        if (UIWidgets::InputString("Host IP", &U.hostIp, InputOptions().Tooltip("IP address of the host (Radmin / LAN)."))) U.Save();
        NpFighterPicker(false);
        if (UIWidgets::Button("Join game", ButtonOptions().Size(Sizes::Inline).Color(Colors::Blue)
                                               .Tooltip("Connects to the host and waits on the VS screen."))) {
            U.Save();
            np_join(U.hostIp.c_str(), (uint16_t)U.port, U.name.c_str());
            np_set_choice(U.fighter, U.costume);
        }
        if (state == NP_FAILED) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "%s", np_status());
        }
        return;
    }

    NpHeader("Lobby");
    ImGui::Text("%s", np_status());
    ImGui::Text("Mode: %s    Max players: %d", np_get_mode() == 1 ? "1P Mode (co-op, experimental)" : "VS Mode",
                np_get_max_players());
    UIWidgets::Spacer(4.0f);
    for (int s = 0; s < 4; s++) {
        if (np_slot_used(s)) {
            int f = 0, c = 0;
            np_slot_choice(s, &f, &c);
            if (s == 0) {
                ImGui::BulletText("P1  %s  (host)%s", np_slot_name(s), np_local_slot() == 0 ? "   <- you" : "");
            } else {
                ImGui::BulletText("P%d  %s  -  %s (costume %d)%s", s + 1, np_slot_name(s), syNetLobbyGetFighterName(f),
                                  c + 1, np_local_slot() == s ? "   <- you" : "");
            }
        } else {
            ImGui::TextDisabled("    P%d  (empty)", s + 1);
        }
    }

    if (np_is_host()) {
        NpHeader("You are the host");
        bool costumeChanged = false;
        int32_t hc = U.hostCostume;
        if (UIWidgets::Combobox("Your costume (Player 1)", &hc, kNpCostumes)) { U.hostCostume = hc; costumeChanged = true; }
        if (costumeChanged) { U.ApplyHost(); U.Save(); }
        if (np_get_mode() == 1) {
            ImGui::TextWrapped("1P Mode: you are on the character select screen. Pick your fighter, then press Start. "
                               "Difficulty and lives come from \"Match Options\". Your friends join the run as Player 2 and 3.");
        } else {
            ImGui::TextWrapped("VS Mode: go to the VS character select, pick P1 and the rules, then press Start. "
                               "Connected players are pulled into the match with the fighter they chose. "
                               "The stage can be forced from \"Match Options\".");
        }
    } else {
        NpHeader("Your fighter");
        NpFighterPicker(true);
        ImGui::TextWrapped("Wait for the host to start the %s. Plug your controller into port 1.",
                           np_get_mode() == 1 ? "1P co-op run" : "match");
    }
    UIWidgets::Spacer(6.0f);
    if (UIWidgets::Button(np_is_host() ? "Close lobby" : "Leave",
                          ButtonOptions().Size(Sizes::Inline).Color(Colors::Red).Tooltip("Leaves the room. Room cheats turn off."))) {
        np_stop();
    }
}

// "Match Options": everything the host decides. Editable before hosting and while the room is open.
void RenderMatchOptions(WidgetInfo&) {
    U.Load();
    const int state = np_state();
    const bool connected = (state == NP_CONNECTED);
    const bool hosting = (state == NP_HOSTING);
    if (connected) {
        ImGui::TextWrapped("Only the host can change these options. Current room: %s, up to %d players.",
                           np_get_mode() == 1 ? "1P Mode" : "VS Mode", np_get_max_players());
        return;
    }
    bool dirty = false;

    NpHeader("Game mode");
    int32_t m = U.mode;
    const std::map<int32_t, const char*> modes = { { 0, "VS Mode (fight each other)" }, { 1, "1P Mode (co-op, experimental)" } };
    const bool can1P = !hosting || np_player_count() <= 3;
    if (UIWidgets::Combobox("Mode", &m, modes,
                            ComboboxOptions().Tooltip("VS Mode: 2-4 players fight each other. 1P Mode: up to 3 players play the story mode together."))) {
        if (m == 0 || can1P) { U.mode = m; dirty = true; }
    }
    if (!can1P) ImGui::TextDisabled("1P Mode allows up to 3 players: ask the extra player to leave first.");

    if (U.mode == 0) {
        NpHeader("Stage (VS Mode)");
        int32_t st = U.stage;
        if (UIWidgets::Combobox("Stage", &st, kNpStages,
                                ComboboxOptions().Tooltip("Forces the stage of the next match for everybody. "
                                                          "\"Pick on the stage select screen\" keeps the normal behaviour."))) {
            U.stage = st;
            dirty = true;
        }
        U.maxPlayers = std::clamp(U.maxPlayers, 2, 4);
        if (UIWidgets::SliderInt("Max players", &U.maxPlayers, IntSliderOptions().Min(2).Max(4).DefaultValue(4)
                                                                  .Tooltip("How many people can be in the room."))) {
            dirty = true;
        }
    } else {
        NpHeader("Co-op run (1P Mode)");
        U.maxPlayers = std::clamp(U.maxPlayers, 2, 3);
        if (UIWidgets::SliderInt("Max players", &U.maxPlayers, IntSliderOptions().Min(hosting ? (std::max)(2, np_player_count()) : 2).Max(3)
                                                                  .DefaultValue(3).Tooltip("1P Mode only allows up to 3 players."))) {
            dirty = true;
        }
        int32_t d = std::clamp(U.difficulty, 0, 4);
        if (UIWidgets::Combobox("Difficulty", &d, kNpDifficulty)) { U.difficulty = d; dirty = true; }
        U.lives = std::clamp(U.lives, 1, 5);
        if (UIWidgets::SliderInt("Lives (stocks)", &U.lives, IntSliderOptions().Min(1).Max(5).DefaultValue(3))) dirty = true;
        if (NpCheck("Friendly fire", &U.friendlyFire, "Players can hurt each other.")) dirty = true;
    }

    NpHeader("Room cheats (optional)");
    ImGui::TextWrapped("Applied to EVERYONE in the match and turned off when the room closes. Only the host chooses them.");
    struct Cheat { const char* label; int bit; const char* tip; };
    static const Cheat kCheats[] = {
        { "Infinite jumps", 1, "Every player can jump in the air without limit." },
        { "Infinite shield", 2, "Shields never break or shrink." },
        { "Always 0% damage", 4, "Nobody takes damage (no knockback)." },
        { "Infinite stocks / lives", 8, "Nobody loses stocks when they are KO'd." },
        { "CPU fighters at 999%", 16, "Every CPU opponent sits at 999%." },
    };
    for (const Cheat& c : kCheats) {
        bool on = (U.cheatMask & c.bit) != 0;
        if (NpCheck(c.label, &on, c.tip)) {
            U.cheatMask = on ? (U.cheatMask | c.bit) : (U.cheatMask & ~c.bit);
            dirty = true;
        }
    }

    NpHeader("Connection");
    if (UIWidgets::SliderInt("Input delay (frames)", &U.delay, IntSliderOptions().Min(1).Max(12).DefaultValue(5)
                                                                  .Tooltip("Higher = smoother over bad connections but more input lag. 4-6 is good on Radmin."))) {
        dirty = true;
    }
    if (NpCheck("Show connected players on the character select panels", &U.showPanels,
                "Experimental. If the character select screen glitches, turn this off (a name strip is shown instead).")) {
        np_set_panels(U.showPanels ? 1 : 0);
        dirty = true;
    }
    if (dirty) {
        U.Save();
        if (hosting) U.ApplyHost();
    }
}

// "How to Play": the tutorial.
void RenderNetHelp(WidgetInfo&) {
    NpHeader("How it works");
    ImGui::BulletText("The host is Player 1. Up to 3 friends can join as Player 2, 3 and 4.");
    ImGui::BulletText("HOST: open \"LAN Online\", press \"Host game\" and share your IP (Radmin / ZeroTier / Tailscale / LAN) and the port.");
    ImGui::BulletText("JOINERS: type the host's IP and the same port, choose your fighter and press \"Join game\".");
    ImGui::BulletText("HOST: choose the rules (and the stage in \"Match Options\" if you want), then press START.");
    ImGui::BulletText("Each player uses their own controller on port 1 of their own PC.");
    ImGui::BulletText("Lag or stuttering? The host can raise the input delay in \"Match Options\".");
    ImGui::BulletText("If someone disconnects during a match, the match ends for everybody.");
    ImGui::BulletText("VS Mode: 2 to 4 players fight each other.");
    ImGui::BulletText("1P Mode (experimental): up to 3 players play the story mode together as allies.");
    ImGui::BulletText("Room cheats chosen by the host apply to every player and vanish when the room closes.");
}

} // namespace

void PortMenu::AddMenuNetplay() {
    WidgetPath path = { "Network", "LAN Online", SECTION_COLUMN_1 };
    AddSidebarEntry("Network", "LAN Online", 1);
    AddWidget(path, "LAN Online (up to 4 players)", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "LAN Online Panel", WIDGET_CUSTOM).RaceDisable(false).CustomFunction(RenderLanOnline);

    path.sidebarName = "Match Options";
    AddSidebarEntry("Network", "Match Options", 1);
    AddWidget(path, "Match Options", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Match Options Panel", WIDGET_CUSTOM).RaceDisable(false).CustomFunction(RenderMatchOptions);

    path.sidebarName = "How to Play";
    AddSidebarEntry("Network", "How to Play", 1);
    AddWidget(path, "How to Play Online", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "How to Play Panel", WIDGET_CUSTOM).RaceDisable(false).CustomFunction(RenderNetHelp);
}

namespace {
void OpenFolder(const std::string& dir) {
#if defined(_WIN32)
    std::string cmd = "explorer \"" + dir + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + dir + "\"";
#else
    std::string cmd = "xdg-open \"" + dir + "\" >/dev/null 2>&1 &";
#endif
    (void)std::system(cmd.c_str());
}

void RenderDiagnostics(WidgetInfo&) {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Frame rate: %.1f FPS  (%.2f ms/frame)", io.Framerate, io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
    ImGui::Text("Display: %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Text("Version: %s", BATTLESHIP_CURRENT_VERSION);
    ImGui::Text("App directory: %s", Ship::Context::GetPathRelativeToAppDirectory("").c_str());
    ImGui::Text("Music tracks found: %d", portMusicTrackCount());
    ImGui::Text("Online room: %s", syNetLobbyIsRoomOpen() ? "open" : "closed");
}

void RenderDevUtilities(WidgetInfo&) {
    const std::string app = Ship::Context::GetPathRelativeToAppDirectory("");
    const std::string mods = Ship::Context::GetPathRelativeToAppDirectory("mods");
    if (UIWidgets::Button("Open app folder", ButtonOptions().Size(Sizes::Inline).Tooltip("Opens the folder with BattleShip.o2r, saves and config."))) {
        OpenFolder(app);
    }
    ImGui::SameLine();
    if (UIWidgets::Button("Open mods folder", ButtonOptions().Size(Sizes::Inline).Tooltip("Opens the mods folder (texture packs, music, script mods)."))) {
        std::error_code ec;
        fs::create_directories(mods, ec);
        OpenFolder(mods);
    }
    UIWidgets::Spacer(4.0f);
    if (UIWidgets::Button("Rescan music", ButtonOptions().Size(Sizes::Inline).Tooltip("Looks again for replacement music in the mods folders."))) {
        portMusicRescan();
    }
    ImGui::SameLine();
    if (UIWidgets::Button("Save settings now", ButtonOptions().Size(Sizes::Inline).Tooltip("Writes the configuration file immediately."))) {
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
    UIWidgets::Spacer(4.0f);
    if (UIWidgets::Button("Turn off all gameplay cheats", ButtonOptions().Size(Sizes::Inline).Color(Colors::Red)
                                                               .Tooltip("Disables every gameplay cheat checkbox."))) {
        const char* names[] = { "gCheats.Gameplay", "gCheats.InfiniteJumps", "gCheats.InfiniteShield",
                                "gCheats.ZeroPercent", "gCheats.InfiniteStocks", "gCheats.Enemy999" };
        for (const char* n : names) CVarSetInteger(n, 0);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }
}

void RenderNetDebug(WidgetInfo&) {
    static const char* kStateNames[] = { "Idle", "Hosting", "Connecting", "Connected", "Failed" };
    const int st = np_state();
    ImGui::Text("State: %s", (st >= 0 && st < 5) ? kStateNames[st] : "?");
    ImGui::Text("Status: %s", np_status());
    ImGui::Text("Role: %s   Local slot: %d", np_is_host() ? "host" : "client", np_local_slot());
    ImGui::Text("Players connected: %d", np_player_count());
    ImGui::Text("Input delay: %d frames", np_get_input_delay());
    ImGui::Text("Match active: %s   mask: 0x%X", np_match_active() ? "yes" : "no", np_match_mask());
    ImGui::Text("Room cheats (match): 0x%02X   (host setting: 0x%02X)", np_match_cheats(), np_get_room_cheats());
    ImGui::Text("Forced stage: %d  (-1 = stage select, -2 = random)", np_get_stage());
    ImGui::Text("Settings hash: %08X", np_settings_hash());
    ImGui::Text("Last match ended: %s", np_last_abort_age_ms() >= 0 ? np_last_abort_reason() : "(none)");
    ImGui::TextDisabled("The settings hash must be identical on every peer.");
}
} // namespace

void PortMenu::AddMenuDevTools() {
    WidgetPath path = { "Dev Tools", "Diagnostics", SECTION_COLUMN_1 };
    AddSidebarEntry("Dev Tools", "Diagnostics", 1);
    AddWidget(path, "Diagnostics", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Diagnostics Panel", WIDGET_CUSTOM).RaceDisable(false).CustomFunction(RenderDiagnostics);

    path.sidebarName = "Netplay Debug";
    AddSidebarEntry("Dev Tools", "Netplay Debug", 1);
    AddWidget(path, "Netplay Debug", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Netplay Debug Panel", WIDGET_CUSTOM).RaceDisable(false).CustomFunction(RenderNetDebug);

    path.sidebarName = "Utilities";
    AddSidebarEntry("Dev Tools", "Utilities", 1);
    AddWidget(path, "Utilities", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Utilities Panel", WIDGET_CUSTOM).RaceDisable(false).CustomFunction(RenderDevUtilities);
}

void PortMenu::AddMenuAbout() {
    WidgetPath path = { "About", "BattleShipDX", SECTION_COLUMN_1 };
    AddSidebarEntry("About", "BattleShipDX", 1);
    AddWidget(path, "BattleShipDX", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "A modification of BattleShip (JRickey/BattleShip), the native Smash 64 PC port.", WIDGET_TEXT);
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    AddWidget(path, "Version: " + std::string(ssb64::enhancements::GetDxVersion()), WIDGET_TEXT);
#else
    AddWidget(path, "Version: " + std::string(BATTLESHIP_CURRENT_VERSION), WIDGET_TEXT);
#endif
    AddWidget(path, "BattleShipDX is a fan-made fork. It keeps everything the original port does and adds:", WIDGET_TEXT);

    AddWidget(path, "What BattleShipDX adds", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "LAN / online multiplayer for 2 to 4 players (Radmin, Hamachi or LAN), with name tags", WIDGET_TEXT);
    AddWidget(path, "1P Mode online co-op (up to 3 players) with optional friendly fire", WIDGET_TEXT);
    AddWidget(path, "Host-chosen stage and room cheats that apply to everybody in the match", WIDGET_TEXT);
    AddWidget(path, "Desync detection and a transport self-test", WIDGET_TEXT);
    AddWidget(path, "Music replacement with MP3 / OGG / WAV from the mods folder, a zip or an .o2r", WIDGET_TEXT);
    AddWidget(path, "Audio Editor: choose the music of every stage, swap in-game songs or randomize", WIDGET_TEXT);
    AddWidget(path, "Gameplay cheats (infinite jumps, shield, stocks, 0% damage, CPU at 999%)", WIDGET_TEXT);
    AddWidget(path, "Hi-res texture packs from .zip and .o2r files", WIDGET_TEXT);
    AddWidget(path, "Web asset editor (textures, 3D levels, music) for your BattleShip.o2r", WIDGET_TEXT);
    AddWidget(path, "Reorganised menu: Settings, Enhancements, Controls, Network, Dev Tools, About", WIDGET_TEXT);

    path.sidebarName = "Credits";
    AddSidebarEntry("About", "Credits", 1);
    AddWidget(path, "Original BattleShip", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "BattleShipDX is built on BattleShip: all credit for the port goes to its authors.", WIDGET_TEXT);
    AddWidget(path, "Native Smash 64 PC port powered by libultraship", WIDGET_TEXT);
    AddWidget(path, "Built from the ssb-decomp-re decompilation", WIDGET_TEXT);
    AddWidget(path, "JRickey: Lead Developer", WIDGET_TEXT);
    AddWidget(path, "Jameriquiah: Developer", WIDGET_TEXT);
    AddWidget(path, "the-outcaster: Developer", WIDGET_TEXT);
    AddWidget(path, "TechnicallyComputers and NyxTheShield: Netcode Developers", WIDGET_TEXT);
    AddWidget(path, "VetriTheRetri: Former Decompilation Lead", WIDGET_TEXT);
    AddWidget(path, "MarioReincarnate: Decompilation Lead", WIDGET_TEXT);
    AddWidget(path, "Krix08: Playtesting", WIDGET_TEXT);
    AddWidget(path, "Bleee: Playtesting", WIDGET_TEXT);
    AddWidget(path, "Fray: Nrage Control Advising", WIDGET_TEXT);
    AddWidget(path, "ElBateSoli: Raphnet Playtesting", WIDGET_TEXT);
    AddWidget(path, "MickleMoose: C Modding Toolkit Developer", WIDGET_TEXT);
    AddWidget(path, "Zorkats: C Modding Documentation", WIDGET_TEXT);
    AddWidget(path, "Third-party code", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "minimp3 (CC0) and stb_vorbis (public domain) decode the replacement music.", WIDGET_TEXT);
}

void PortMenu::AddMenuElements() {
    // Top bar order: Settings, Enhancements, Controls, Network, Dev Tools, About.
    AddMenuEntry("Settings", CVAR_SETTING("Menu.SettingsSidebarSection"));
    AddMenuEntry("Enhancements", CVAR_SETTING("Menu.EnhancementsSidebarSection"));
    AddMenuEntry("Controls", CVAR_SETTING("Menu.ControlsSidebarSection"));
    AddMenuEntry("Network", CVAR_SETTING("Menu.NetworkSidebarSection"));
    AddMenuEntry("Dev Tools", CVAR_SETTING("Menu.DevToolsSidebarSection"));
    AddMenuEntry("About", CVAR_SETTING("Menu.AboutSidebarSection"));

    AddMenuSettings();
    AddMenuNetplay();
    AddMenuAssets();
#ifndef DISABLE_SCRIPTING
    AddMenuMods();
    AddMenuAudioEditor();
#endif
    AddMenuDevTools();
    AddMenuAbout();

#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    ssb64::enhancements::CheckForDxReleaseAsync(); // background check for a newer BattleShipDX release
#endif

    if (CVarGetInteger(CVAR_SETTING("Menu.SidebarSearch"), 0)) {
        InsertSidebarSearch();
    }

    for (auto& initFunc : MenuInit::GetInitFuncs()) {
        initFunc();
    }

    mMenuElementsInitialized = true;
}

void PortMenu::InitElement() {
    if (!mMenuElementsInitialized) {
        AddMenuElements();
    }

    Ship::Menu::InitElement();
    disabledMap = {
        { DISABLE_FOR_NO_VSYNC,
          { [](disabledInfo&) -> bool { return !Ship::Context::GetInstance()->GetWindow()->CanDisableVerticalSync(); },
            "Disabling VSync not supported" } },
        { DISABLE_FOR_NO_WINDOWED_FULLSCREEN,
          { [](disabledInfo&) -> bool {
               return !Ship::Context::GetInstance()->GetWindow()->SupportsWindowedFullscreen();
           },
            "Windowed Fullscreen not supported" } },
        { DISABLE_FOR_NO_MULTI_VIEWPORT,
          { [](disabledInfo&) -> bool {
               return !Ship::Context::GetInstance()->GetWindow()->GetGui()->SupportsViewports();
           },
            "Multi-viewports not supported" } },
        { DISABLE_FOR_NOT_DIRECTX,
          { [](disabledInfo&) -> bool {
               return Ship::Context::GetInstance()->GetWindow()->GetWindowBackend() !=
                      Ship::WindowBackend::FAST3D_DXGI_DX11;
           },
            "Available only on DirectX" } },
        { DISABLE_FOR_DIRECTX,
          { [](disabledInfo&) -> bool {
               return Ship::Context::GetInstance()->GetWindow()->GetWindowBackend() ==
                      Ship::WindowBackend::FAST3D_DXGI_DX11;
           },
            "Not available on DirectX" } },
        { DISABLE_FOR_MATCH_REFRESH_RATE_ON,
          { [](disabledInfo&) -> bool { return CVarGetInteger(PORT_INTERP_CVAR_MATCH_DISPLAY, 0); },
            "Match Display Refresh Rate is enabled" } },
        { DISABLE_FOR_ADVANCED_RESOLUTION_ON,
          { [](disabledInfo&) -> bool { return CVarGetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled", 0); },
            "Advanced Resolution enabled" } },
        { DISABLE_FOR_VERTICAL_RES_TOGGLE_ON,
          { [](disabledInfo&) -> bool {
               return CVarGetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalResolutionToggle", 0);
           },
            "Vertical Resolution Toggle enabled" } },
        { DISABLE_FOR_LOW_RES_MODE_ON,
          { [](disabledInfo&) -> bool { return CVarGetInteger(CVAR_LOW_RES_MODE, 0); }, "N64 mode enabled" } },
        { DISABLE_FOR_ADVANCED_RESOLUTION_OFF,
          { [](disabledInfo&) -> bool { return !CVarGetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled", 0); },
            "Advanced Resolution is disabled" } },
        { DISABLE_FOR_VERTICAL_RESOLUTION_OFF,
          { [](disabledInfo&) -> bool {
               return !CVarGetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalResolutionToggle", 0);
           },
            "Vertical Resolution Toggle is off" } },
    };
}

void PortMenu::UpdateElement() {
    Ship::Menu::UpdateElement();
}

namespace {

// Player names drawn above the fighters during an online match. The decomp
// reports where it draws each fighter's P1/P2/.. tag (320x240 screen space);
// we draw the name right above it with ImGui.
struct NameTag {
    float x = 0, y = 0;
    std::chrono::steady_clock::time_point stamp{};
    bool valid = false;
};
NameTag sNameTags[4];

void DrawNetplayBanner(const char* text, ImU32 color) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = (std::max)(16.0f, vp->Size.y / 28.0f);
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float x = vp->Pos.x + (vp->Size.x - sz.x) * 0.5f;
    const float y = vp->Pos.y + vp->Size.y * 0.04f;
    dl->AddRectFilled(ImVec2(x - 12, y - 6), ImVec2(x + sz.x + 12, y + sz.y + 6), IM_COL32(0, 0, 0, 170), 6.0f);
    dl->AddText(font, size, ImVec2(x, y), color, text);
}

// Connected players strip shown on the character select screen.
// Player names above the native character select panels (320x240 screen space, like the name tags).
void DrawNetplayPanelNames() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float gx = vp->Pos.x, gy = vp->Pos.y, gw = vp->Size.x, gh = vp->Size.y;
    if (!port_widescreen_enabled()) {
        const float w43 = gh * 4.0f / 3.0f;
        if (w43 <= gw) { gx += (gw - w43) * 0.5f; gw = w43; }
        else { const float h43 = gw * 3.0f / 4.0f; gy += (gh - h43) * 0.5f; gh = h43; }
    }
    static const ImU32 cols[4] = { IM_COL32(237, 54, 54, 255), IM_COL32(78, 78, 233, 255),
                                   IM_COL32(255, 223, 26, 255), IM_COL32(78, 185, 78, 255) };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = (std::max)(13.0f, gh / 30.0f);
    for (int s = 0; s < 4; s++) {
        if (!np_slot_used(s)) continue;
        char label[48];
        snprintf(label, sizeof(label), "%s%s", np_slot_name(s), np_local_slot() == s ? " (you)" : "");
        const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
        // Panel s sits at x = 22 + s*69 (about 64 wide), y = 126 in 320x240 coordinates.
        const float cx = gx + ((22.0f + s * 69.0f + 32.0f) / 320.0f) * gw;
        const float top = gy + (121.0f / 240.0f) * gh - sz.y;
        dl->AddRectFilled(ImVec2(cx - sz.x * 0.5f - 4, top - 2), ImVec2(cx + sz.x * 0.5f + 4, top + sz.y + 2), IM_COL32(0, 0, 0, 175), 4.0f);
        dl->AddText(font, size, ImVec2(cx - sz.x * 0.5f, top), cols[s], label);
    }
}

void DrawNetplayRoster() {
    if (np_get_panels()) { DrawNetplayPanelNames(); return; }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    static const ImU32 cols[4] = { IM_COL32(237, 54, 54, 255), IM_COL32(78, 78, 233, 255),
                                   IM_COL32(255, 223, 26, 255), IM_COL32(78, 185, 78, 255) };
    const float size = (std::max)(13.0f, vp->Size.y / 38.0f);
    const float cw = vp->Size.x * 0.2f, ch = size * 3.4f, gap = vp->Size.x * 0.01f;
    const float total = cw * 4 + gap * 3;
    const float x0 = vp->Pos.x + (vp->Size.x - total) * 0.5f;
    const float y0 = vp->Pos.y + vp->Size.y * 0.11f;
    for (int s = 0; s < 4; s++) {
        const float x = x0 + s * (cw + gap);
        dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + cw, y0 + ch), IM_COL32(0, 0, 0, 165), 6.0f);
        dl->AddRect(ImVec2(x, y0), ImVec2(x + cw, y0 + ch), np_slot_used(s) ? cols[s] : IM_COL32(120, 120, 120, 255), 6.0f, 0, 2.0f);
        char l1[64], l2[96];
        if (np_slot_used(s)) {
            snprintf(l1, sizeof(l1), "P%d  %s", s + 1, np_slot_name(s));
            if (s == 0) {
                snprintf(l2, sizeof(l2), "Host");
            } else {
                int f = 0, c = 0;
                np_slot_choice(s, &f, &c);
                snprintf(l2, sizeof(l2), "%s (costume %d)", syNetLobbyGetFighterName(f), c + 1);
            }
        } else {
            snprintf(l1, sizeof(l1), "P%d", s + 1);
            snprintf(l2, sizeof(l2), "(empty)");
        }
        dl->AddText(font, size, ImVec2(x + 8, y0 + 4), np_slot_used(s) ? cols[s] : IM_COL32(150, 150, 150, 255), l1);
        dl->AddText(font, size * 0.9f, ImVec2(x + 8, y0 + 4 + size * 1.5f), IM_COL32(235, 235, 235, 255), l2);
    }
}

void DrawNetplayNameTags() {
    if (!np_match_active()) {
        const int st = np_state();
        if (st == NP_CONNECTED || st == NP_HOSTING) DrawNetplayRoster();
        if (st == NP_CONNECTED) {
            DrawNetplayBanner(np_get_mode() == 1 ? "Connected! Waiting for the host to start the 1P co-op run..."
                                                 : "Connected! Waiting for the host to start the match...",
                              IM_COL32(255, 255, 255, 255));
        } else if (st == NP_HOSTING && np_player_count() > 1) {
            DrawNetplayBanner("Players connected. Pick your fighter and press START when ready.", IM_COL32(255, 255, 255, 255));
        }
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float gx = vp->Pos.x, gy = vp->Pos.y, gw = vp->Size.x, gh = vp->Size.y;
    if (!port_widescreen_enabled()) {
        // 4:3 image centered in the window
        const float w43 = gh * 4.0f / 3.0f;
        if (w43 <= gw) { gx += (gw - w43) * 0.5f; gw = w43; }
        else { const float h43 = gw * 3.0f / 4.0f; gy += (gh - h43) * 0.5f; gh = h43; }
    }
    static const ImU32 colors[4] = { IM_COL32(237, 54, 54, 255), IM_COL32(78, 78, 233, 255),
                                     IM_COL32(255, 223, 26, 255), IM_COL32(78, 185, 78, 255) };
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = (std::max)(14.0f, gh / 34.0f);
    const auto now = std::chrono::steady_clock::now();
    for (int p = 0; p < 4; p++) {
        NameTag& t = sNameTags[p];
        if (!t.valid || !np_slot_used(p)) continue;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t.stamp).count() > 120) continue;
        const char* name = np_slot_name(p);
        const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, name);
        const float px = gx + (t.x / 320.0f) * gw - sz.x * 0.5f;
        const float py = gy + (t.y / 240.0f) * gh - sz.y - 2.0f;
        dl->AddText(font, fontSize, ImVec2(px + 1, py + 1), IM_COL32(0, 0, 0, 230), name);
        dl->AddText(font, fontSize, ImVec2(px, py), colors[p], name);
    }
}

} // namespace

extern "C" void portNetplayReportNameTag(int player, float centerX, float topY) {
    if (player < 0 || player > 3) return;
    sNameTags[player] = { centerX, topY, std::chrono::steady_clock::now(), true };
}

// Shows why an online match ended (desync, lost connection, load timeout, ...) for ~15 seconds,
// even though the game has already gone back to the main menu.
static void DrawNetplayNotice() {
    const int age = np_last_abort_age_ms();
    if (age < 0 || age > 15000) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 14.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##NetplayNotice", nullptr, flags)) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "Online match ended");
        ImGui::PushTextWrapPos(vp->Size.x * 0.8f);
        ImGui::TextUnformatted(np_last_abort_reason());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void PortMenu::Draw() {
    Ship::Menu::Draw();
    DrawNetplayNameTags();
    DrawNetplayNotice();
}

void PortMenu::DrawElement() {
    if (mMenuElementsInitialized) {
        Ship::Menu::DrawElement();
    }

#if defined(__ANDROID__)
    // Show the soft keyboard while a text field (name / host IP) is being edited.
    {
        static bool sImeOn = false;
        const bool want = ImGui::GetIO().WantTextInput;
        if (want != sImeOn) {
            sImeOn = want;
            if (want) SDL_StartTextInput(); else SDL_StopTextInput();
        }
    }
#endif

#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    // "New update released" notice, top right. Click opens the GitHub releases page.
    if (ssb64::enhancements::IsDxReleaseAvailable()) {
        const std::string text = "A new BattleShipDX update was released (" + ssb64::enhancements::GetDxLatestTag() +
                                 ") - click here to download it";
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 12.0f, vp->Pos.y + 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##BattleShipDXUpdateNotice", nullptr, flags)) {
            const bool hovered = ImGui::IsWindowHovered();
            ImGui::TextColored(hovered ? ImVec4(1.0f, 1.0f, 0.5f, 1.0f) : ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "%s", text.c_str());
            if (hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Open https://github.com/JuanFunkin/BattleShipDX/releases");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ssb64::enhancements::OpenDxReleasesPage();
                }
            }
        }
        ImGui::End();
    }
#endif

    // Re-extract confirmation: render as an in-context ImGui modal rather
    // than calling SDL_ShowSimpleMessageBox, which would nest a blocking
    // native modal (NSAlert / Win32 MessageBox) inside the in-progress
    // ImGui frame and the GPU command encode for that frame. ImGui's own
    // popup runs in the same context with no thread/native-modal nesting.
    if (mShowReextractMessage) {
        ImGui::OpenPopup("Re-extract scheduled");
        mShowReextractMessage = false;
    }
    const ImVec2 viewportCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(viewportCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Re-extract scheduled", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped(
            SSB64_O2R_NAME " will be regenerated from your ROM the next time the game launches.");
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    RenderPostProcessLowResWarnModal();
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    RenderShaderPackModal();
#endif
}

} // namespace ssb64
