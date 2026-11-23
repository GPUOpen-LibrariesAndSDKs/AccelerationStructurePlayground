//
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "gui.h"

#include "assets/scene.h"
#include "render/plan_format.h"

#include <imgui.h>

#include <algorithm>
#include <map>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace
{
std::string DisplayName(const std::string& name, const char* fallback, size_t index)
{
    return name.empty() ? std::string(fallback) + " " + std::to_string(index) : name;
}

std::string DisplayNodeName(const Gltf::Node& node, size_t index)
{
    std::string label = DisplayName(node.name, "Node", index);
    if (node.pExternalAsset && node.pExternalAsset->pFile)
    {
        const std::filesystem::path path(node.pExternalAsset->pFile->uri);
        const std::string fileName = path.filename().string();
        if (!fileName.empty())
        {
            label += " (" + fileName + ")";
        }
    }
    return label;
}

// Indexed by AccelStructGroupingTarget.
constexpr const char* kGroupingTargetNames[] = {"Meshes", "Scenes", "Hybrid"};
constexpr int kGroupingTargetCount = static_cast<int>(sizeof(kGroupingTargetNames) / sizeof(kGroupingTargetNames[0]));
constexpr const char* kDebugModeNames[] = {"Full", "Selected BLAS", "Selected MDLAS"};
constexpr const char* kSceneLevelOriginNames[] = {"From leaves (height)", "From root (depth)"};
constexpr int kSceneLevelOriginCount =
    static_cast<int>(sizeof(kSceneLevelOriginNames) / sizeof(kSceneLevelOriginNames[0]));

// Edits a SceneLevel as an origin combo and a distance field, labelled "<label> origin" and
// "<label> distance".
void InputSceneLevel(const std::string& label, SceneLevel& level)
{
    int origin = static_cast<int>(level.origin);
    ImGui::Combo((label + " origin").c_str(), &origin, kSceneLevelOriginNames, kSceneLevelOriginCount);
    level.origin = static_cast<SceneLevelOrigin>(origin);
    const uint32_t step = 1;
    ImGui::InputScalar((label + " distance").c_str(), ImGuiDataType_U32, &level.distance, &step);
}

void AddSizes(AccelStructSizes& total, const AccelStructSizes& sizes)
{
    total.accelStructBytes += sizes.accelStructBytes;
    total.scratchBytes += sizes.scratchBytes;
}

void DrawNode(const GltfAsset& asset, const Gltf::Node& node)
{
    const int nodeIndex = Gltf::PointerIndex(asset.nodes, &node, "node");
    const std::string label = DisplayNodeName(node, static_cast<size_t>(nodeIndex));
    ImGui::PushID(&node);
    const ImGuiTreeNodeFlags flags = node.children.empty()
                                         ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
                                         : ImGuiTreeNodeFlags_OpenOnArrow;
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (node.pMesh && ImGui::IsItemHovered())
    {
        const Gltf::Mesh& mesh = *node.pMesh;
        const int meshIndex = Gltf::PointerIndex(asset.meshes, node.pMesh, "mesh");
        ImGui::SetTooltip("Mesh: %s\nPrimitives: %zu", DisplayName(mesh.name, "Mesh", meshIndex).c_str(),
                          mesh.primitives.size());
    }
    if (open && !node.children.empty())
    {
        for (const Gltf::Node* child : node.children)
        {
            DrawNode(asset, *child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// Draws a level of the acceleration structure tree where instances of the same acceleration structure are grouped
// together as a single row.
void DrawAccelStructChildren(const Scene& scene, const RenderDiagnostics& accelStruct, const AccelStructPlan& plan,
                       const std::vector<AccelStructInstancePlan>& instances)
{
    struct ChildGroup
    {
        AccelStructRef accelStruct;
        uint32_t count = 0;
    };

    // Track instance count for unique acceleration structures
    std::vector<ChildGroup> groups;
    std::map<AccelStructRef, size_t> groupIndices;
    for (const AccelStructInstancePlan& instance : instances)
    {
        const auto [entry, inserted] = groupIndices.emplace(instance.accelStruct, groups.size());
        if (inserted)
        {
            groups.push_back({instance.accelStruct, 1});
        }
        else
        {
            ++groups[entry->second].count;
        }
    }

    for (const ChildGroup& group : groups)
    {
        ImGui::PushID(static_cast<int>(group.accelStruct.kind));
        ImGui::PushID(static_cast<int>(group.accelStruct.value));
        if (group.accelStruct.kind == AccelStructRefKind::Blas)
        {
            const BlasPlan& blas = plan.blases[group.accelStruct.value];
            const bool open =
                ImGui::TreeNode("blas", "BLAS %u  (x%u, %zu geometries, %s)", group.accelStruct.value, group.count,
                                blas.geometries.size(), blas.updateMode == BlasUpdateMode::Refit ? "refit" : "static");
            const AccelStructSizes sizes = accelStruct.blases.at(group.accelStruct.value);
            ImGui::SameLine();
            ImGui::TextDisabled("AS: %s | scratch: %s", FormatBytes(sizes.accelStructBytes).c_str(),
                                FormatBytes(sizes.scratchBytes).c_str());
            if (open)
            {
                for (GeometrySourceId geometryId : blas.geometries)
                {
                    const GeometrySource& geometry = plan.geometrySources[geometryId.value];
                    const GltfAsset& asset = scene.Asset(geometry.assetId);
                    const std::string nodeName =
                        DisplayNodeName(*geometry.pGltfNode, Gltf::PointerIndex(asset.nodes, geometry.pGltfNode, "node"));
                    ImGui::BulletText("%s  mesh %d / primitive %d%s%s", nodeName.c_str(),
                                      Gltf::PointerIndex(asset.meshes, geometry.pMesh, "mesh"),
                                      Gltf::PointerIndex(geometry.pMesh->primitives, geometry.pPrimitive, "primitive"),
                                      geometry.transformMode == GeometryTransformMode::BakedFromNode ? "  baked" : "",
                                      geometry.deformed ? "  skinned" : "");
                }
                ImGui::TreePop();
            }
        }
        else
        {
            const MlasPlan& mlas = plan.mlases[group.accelStruct.value];
            const bool open = ImGui::TreeNode("mdlas", "MDLAS %u  (x%u, %zu instances)", group.accelStruct.value,
                                              group.count, mlas.instances.size());
            const AccelStructSizes sizes = accelStruct.mlases.at(group.accelStruct.value);
            ImGui::SameLine();
            ImGui::TextDisabled("AS: %s | scratch: %s", FormatBytes(sizes.accelStructBytes).c_str(),
                                FormatBytes(sizes.scratchBytes).c_str());
            if (open)
            {
                DrawAccelStructChildren(scene, accelStruct, plan, mlas.instances);
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
        ImGui::PopID();
    }
}
} // namespace

void Gui::DrawPerformancePanel(const PanelContext& context)
{
    const RenderDiagnostics& accelStruct = context.accelStruct;

    if (ImGui::CollapsingHeader("Performance Counters", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const float plotWidth = 2.0f * static_cast<float>(TimingSnapshot::kHistorySize);
        const float plotHeight = 40.0f;
        ImGui::Text("Frame: %.3f ms (avg %.3f, median %.3f)", m_frameTiming.latest, m_frameTiming.average,
                    m_frameTiming.median);
        ImGui::PlotLines("##frameHistory", m_frameTiming.Samples(), TimingSnapshot::kHistorySize,
                         m_frameTiming.Offset(), nullptr, 0.0f, FLT_MAX, ImVec2(plotWidth, plotHeight));
        ImGui::Text("AS build: %.3f ms (avg %.3f, median %.3f)", accelStruct.totalBuild.latest, accelStruct.totalBuild.average,
                    accelStruct.totalBuild.median);
        ImGui::PlotLines("##totalBuildHistory", accelStruct.totalBuild.Samples(), TimingSnapshot::kHistorySize,
                         accelStruct.totalBuild.Offset(), nullptr, 0.0f, FLT_MAX, ImVec2(plotWidth, plotHeight));
        if (ImGui::TreeNode("AS build breakdown"))
        {
            ImGui::Text("BLAS build: %.3f ms (avg %.3f, median %.3f)", accelStruct.blasBuild.latest, accelStruct.blasBuild.average,
                        accelStruct.blasBuild.median);
            ImGui::PlotLines("##blasBuildHistory", accelStruct.blasBuild.Samples(), TimingSnapshot::kHistorySize,
                             accelStruct.blasBuild.Offset(), nullptr, 0.0f, FLT_MAX, ImVec2(plotWidth, plotHeight));
            ImGui::Text("MDLAS/TLAS build: %.3f ms (avg %.3f, median %.3f)", accelStruct.mlasTlasBuild.latest,
                        accelStruct.mlasTlasBuild.average, accelStruct.mlasTlasBuild.median);
            ImGui::PlotLines("##mdlasTlasBuildHistory", accelStruct.mlasTlasBuild.Samples(), TimingSnapshot::kHistorySize,
                             accelStruct.mlasTlasBuild.Offset(), nullptr, 0.0f, FLT_MAX, ImVec2(plotWidth, plotHeight));
            ImGui::TreePop();
        }
        ImGui::Text("Trace rays: %.3f ms (avg %.3f, median %.3f)", accelStruct.traceRays.latest, accelStruct.traceRays.average,
                    accelStruct.traceRays.median);
        ImGui::PlotLines("##traceRaysHistory", accelStruct.traceRays.Samples(), TimingSnapshot::kHistorySize,
                         accelStruct.traceRays.Offset(), nullptr, 0.0f, FLT_MAX, ImVec2(plotWidth, plotHeight));
    }
}

void Gui::DrawMemoryPanel(const PanelContext& context)
{
    const RenderDiagnostics& accelStruct = context.accelStruct;

    if (ImGui::CollapsingHeader("Memory Utilization", ImGuiTreeNodeFlags_DefaultOpen))
    {
        AccelStructSizes tlas = accelStruct.tlas;
        AccelStructSizes mlases;
        AccelStructSizes blases;
        for (const auto& sizes : accelStruct.mlases)
        {
            AddSizes(mlases, sizes);
        }
        for (const auto& sizes : accelStruct.blases)
        {
            AddSizes(blases, sizes);
        }
        AccelStructSizes total = tlas;
        AddSizes(total, mlases);
        AddSizes(total, blases);

        if (ImGui::BeginTable("##memoryUtilization", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Group");
            ImGui::TableSetupColumn("AS size");
            ImGui::TableSetupColumn("Scratch size");
            ImGui::TableHeadersRow();

            const char* labels[] = {"TLAS", "MDLASs", "BLASs", "Total"};
            const AccelStructSizes* rows[] = {&tlas, &mlases, &blases, &total};
            for (size_t row = 0; row < 4; ++row)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(labels[row]);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(FormatBytes(rows[row]->accelStructBytes).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(FormatBytes(rows[row]->scratchBytes).c_str());
            }
            ImGui::EndTable();
        }
    }
}

void Gui::DrawPlanPanel(const PanelContext& context)
{
    const BackendCapabilities& capabilities = context.capabilities;
    AccelStructPlanRequest& request = context.request;
    const bool mlasExtensionEnabled = context.capabilities.maxMlasLevels > 0;

    if (ImGui::CollapsingHeader("Acceleration Structure Plan", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int target = static_cast<int>(m_pendingHighLevelPlan.blasLevel.target);
        ImGui::Combo("BLAS target", &target, kGroupingTargetNames, kGroupingTargetCount);
        m_pendingHighLevelPlan.blasLevel.target = static_cast<AccelStructGroupingTarget>(target);
        if (m_pendingHighLevelPlan.blasLevel.target == AccelStructGroupingTarget::Scenes)
        {
            InputSceneLevel("BLAS scene", m_pendingHighLevelPlan.blasLevel.sceneLevel);
        }
        if (mlasExtensionEnabled)
        {
            ImGui::SliderInt("MDLAS levels", &m_pendingMlasLevelCount, 0, static_cast<int>(capabilities.maxMlasLevels));
            // New levels default to root depth 1: one MDLAS per top-level placement.
            m_pendingHighLevelPlan.mlasLevels.resize(static_cast<size_t>(m_pendingMlasLevelCount),
                                                     SceneLevel{SceneLevelOrigin::FromRoot, 1});
            for (int level = 0; level < m_pendingMlasLevelCount; ++level)
            {
                InputSceneLevel("MDLAS level " + std::to_string(level + 1),
                                m_pendingHighLevelPlan.mlasLevels[static_cast<size_t>(level)]);
            }
        }
        else
        {
            ImGui::TextWrapped("RT-MLAS is unavailable on the active device.");
        }

        if (ImGui::Button("Rebuild acceleration structures"))
        {
            request.rebuild = true;
            request.highLevelPlan = m_pendingHighLevelPlan;
        }
    }
}

void Gui::DrawDebugPanel(const PanelContext& context)
{
    const RenderDiagnostics& accelStruct = context.accelStruct;
    AccelStructPlanRequest& request = context.request;

    if (ImGui::CollapsingHeader("Debug Visualization", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DebugSelection& selection = m_debug.selection;
        ImGui::Checkbox("Shade by BLAS", &m_debug.shadeByBlas);

        int debugMode = static_cast<int>(selection.mode);
        ImGui::Combo("View", &debugMode, kDebugModeNames, 3);
        selection.mode = static_cast<AccelStructDebugMode>(debugMode);

        const bool selectsMlas = selection.mode == AccelStructDebugMode::SelectedMlas;
        const uint32_t itemCount = static_cast<uint32_t>(selectsMlas ? accelStruct.mlases.size() : accelStruct.blases.size());
        if (selectsMlas || selection.mode == AccelStructDebugMode::SelectedBlas)
        {
            if (itemCount > 0)
            {
                int index = static_cast<int>(std::min(selection.index, itemCount - 1));
                ImGui::SliderInt(selectsMlas ? "MDLAS index" : "BLAS index", &index, 0,
                                 static_cast<int>(itemCount) - 1);
                selection.index = static_cast<uint32_t>(std::max(0, index));
            }
            else
            {
                selection.index = 0;
                ImGui::TextUnformatted(selectsMlas ? "No MDLASs in this plan."
                                                   : "No BLASs in this plan.");
            }
        }
        request.debug = m_debug;
    }
}

void Gui::DrawObjectsPanel(const PanelContext& context)
{
    const Scene& scene = context.scene;
    const AccelStructPlan& plan = context.accelStruct.plan;

    if (ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("%zu objects  |  %zu assets  |  %zu BLAS", scene.SceneNodes().size(), scene.Assets().size(),
                    plan.blases.size());
        ImGui::Separator();
        for (size_t assetIndex = 0; assetIndex < scene.Assets().size(); ++assetIndex)
        {
            const GltfAsset* asset = scene.Assets()[assetIndex];
            const std::string assetName = std::filesystem::path(asset->sourcePath).filename().string();
            const AssetId assetId{static_cast<uint32_t>(assetIndex)};

            uint32_t instanceCount = 0;
            std::unordered_set<uint32_t> animationGroups;
            for (const SceneNode& sceneNode : scene.SceneNodes())
            {
                if (sceneNode.assetId == assetId)
                {
                    ++instanceCount;
                    animationGroups.insert(sceneNode.playbackGroup.value);
                }
            }

            ImGui::PushID(static_cast<int>(assetIndex));
            if (ImGui::TreeNodeEx(assetName.c_str(), ImGuiTreeNodeFlags_DefaultOpen, "%s", assetName.c_str()))
            {
                ImGui::Text("Instances: %u  |  Animation groups: %zu", instanceCount, animationGroups.size());
                if (ImGui::TreeNode("hierarchy", "Node hierarchy"))
                {
                    const auto& roots =
                        asset->scenes.empty() ? asset->GetAllRootNodes() : asset->GetSceneRootNodes();
                    for (const Gltf::Node* root : roots)
                    {
                        DrawNode(*asset, *root);
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
}

void Gui::DrawTlasTreePanel(const PanelContext& context)
{
    const Scene& scene = context.scene;
    const RenderDiagnostics& accelStruct = context.accelStruct;
    const AccelStructPlan& plan = context.accelStruct.plan;

    if (ImGui::CollapsingHeader("TLAS Tree", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const bool open = ImGui::TreeNodeEx("tlas", ImGuiTreeNodeFlags_DefaultOpen, "TLAS  (%zu instances)",
                                            plan.instances.size());
        const AccelStructSizes sizes = accelStruct.tlas;
        ImGui::SameLine();
        ImGui::TextDisabled("AS: %s | scratch: %s", FormatBytes(sizes.accelStructBytes).c_str(),
                            FormatBytes(sizes.scratchBytes).c_str());
        if (open)
        {
            DrawAccelStructChildren(scene, accelStruct, plan, plan.instances);
            ImGui::TreePop();
        }
    }
}

AccelStructPlanRequest Gui::Draw(const Scene& scene, const RenderDiagnostics& accelStruct,
                           const AccelStructHighLevelPlan& highLevelPlan,
                           const BackendCapabilities& capabilities)
{
    const bool mlasExtensionEnabled = capabilities.maxMlasLevels > 0;
    AccelStructPlanRequest request;

    // Keep old debug settings in case debug panel is collapsed
    request.debug = m_debug;
    ImGui::SetNextWindowSize(ImVec2(430.0f, 650.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Acceleration Structure Playground", nullptr, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    const PanelContext context{scene, accelStruct, capabilities, request};

    if (!m_planInitialized)
    {
        m_pendingHighLevelPlan = highLevelPlan;
        m_pendingMlasLevelCount = static_cast<int>(highLevelPlan.mlasLevels.size());
        m_planInitialized = true;
    }
    if (!mlasExtensionEnabled)
    {
        m_pendingMlasLevelCount = 0;
        m_pendingHighLevelPlan.mlasLevels.clear();
    }

    DrawPerformancePanel(context);
    DrawMemoryPanel(context);
    DrawPlanPanel(context);
    DrawDebugPanel(context);
    DrawObjectsPanel(context);
    DrawTlasTreePanel(context);

    ImGui::End();
    return request;
}

ImDrawData* Gui::EndFrame()
{
    ImGui::Render();
    return ImGui::GetDrawData();
}

bool Gui::WantCaptureMouse() const
{
    return ImGui::GetIO().WantCaptureMouse;
}

bool Gui::WantCaptureKeyboard() const
{
    return ImGui::GetIO().WantCaptureKeyboard;
}