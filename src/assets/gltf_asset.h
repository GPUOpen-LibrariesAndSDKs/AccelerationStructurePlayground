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

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

#include "core/math.h"

// CPU-side mirror of the glTF asset. Owning arrays retain source order at load.
// References are non-owning pointers to individually allocated objects, whose
// addresses survive vector growth and reordering but not object destruction.
namespace Gltf
{

struct BufferData;
struct ImageData;
struct Texture;
struct Skin;
struct Node;

// Error handling - throws std::runtime_error with formatted message
template<typename... Args>
[[noreturn]] void Fatal(const char* format, Args... args)
{
    char buffer[512];
    snprintf(buffer, sizeof(buffer), format, args...);
    throw std::runtime_error(buffer);
}

// Resolve loader pointers only at boundaries that require source-order numeric IDs.
template<typename T>
int PointerIndex(const std::vector<std::unique_ptr<T>>& objects, const T* object, const char* label)
{
    if (!object)
    {
        return -1;
    }
    const auto found = std::find_if(objects.begin(), objects.end(),
                                    [object](const std::unique_ptr<T>& entry) { return entry.get() == object; });
    if (found == objects.end())
    {
        Fatal("Invalid %s pointer in loaded glTF asset.", label);
    }
    return static_cast<int>(found - objects.begin());
}

struct PlaybackParameters
{
    float speed = 1.0f;
    float timeOffset = 0.0f;

    void UpdateWithParent(const PlaybackParameters& parent)
    {
        timeOffset += speed * parent.timeOffset;
        speed *= parent.speed;
    }
};

// Extra properties used by this app
struct NodeExtras
{
    // Modifies the animation of the current node and all descendents
    PlaybackParameters playbackParameters{};
};

struct Buffer
{
    std::string uri;
    uint64_t byteLength;

    // Points to the actual buffer data in memory, if loaded
    BufferData* pData = nullptr;
};

struct BufferView
{
    Buffer* pBuffer = nullptr;
    uint64_t byteOffset = 0;
    uint64_t byteLength = 0;
    // A byteStride of 0 implies a stride value was not specified and data is tightly packed
    uint32_t byteStride = 0;
};

enum class AccessorComponentType
{
    Undefined = 0,
    Byte = 5120,
    UnsignedByte = 5121,
    Short = 5122,
    UnsignedShort = 5123,
    UnsignedInt = 5125,
    Float = 5126,
};

static size_t GetAccessorComponentTypeSize(AccessorComponentType componentType)
{
    switch (componentType)
    {
    case AccessorComponentType::Byte:
    case AccessorComponentType::UnsignedByte:
        return 1;
    case AccessorComponentType::Short:
    case AccessorComponentType::UnsignedShort:
        return 2;
    case AccessorComponentType::UnsignedInt:
    case AccessorComponentType::Float:
        return 4;
    default:
        throw std::runtime_error("Unknown AccessorComponentType");
    }
}

enum class AccessorType
{
    Undefined,
    Scalar,
    Vec2,
    Vec3,
    Vec4,
    Mat2,
    Mat3,
    Mat4,
};

inline size_t GetAccessorTypeSize(AccessorType type)
{
    switch (type)
    {
    case AccessorType::Scalar:
        return 1;
    case AccessorType::Vec2:
        return 2;
    case AccessorType::Vec3:
        return 3;
    case AccessorType::Vec4:
        return 4;
    case AccessorType::Mat2:
        return 4; // 2x2 matrix
    case AccessorType::Mat3:
        return 9; // 3x3 matrix
    case AccessorType::Mat4:
        return 16; // 4x4 matrix
    default:
        throw std::runtime_error("Unknown AccessorType");
    }
}

struct Accessor
{
    BufferView* pBufferView = nullptr;
    uint64_t byteOffset = 0;
    AccessorComponentType componentType = AccessorComponentType::Undefined;
    bool normalized = false;
    uint64_t count = 0;
    AccessorType type = AccessorType::Undefined;
};

struct TextureInfo
{
    Texture* pTexture = nullptr;
    int32_t texCoord = 0; // TEXCOORD_0
};

struct PbrMetallicRoughness
{
    float4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    TextureInfo baseColorTexture{};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
};

struct Material
{
    std::string name;
    PbrMetallicRoughness pbrMetallicRoughness;
    bool doubleSided = false;
    // other material properties not supported
};

struct Image
{
    std::string uri;
    std::string mimeType;
    BufferView* pBufferView = nullptr;
    std::string name;

    // Actual decoded data for the image
    ImageData* pData = nullptr;
};

struct Sampler
{
    // TODO: If this is not ignored, clean this up or remove
    std::optional<int32_t> magFilter;
    std::optional<int32_t> minFilter;
    int32_t wrapS = 10497;
    int32_t wrapT = 10497;
    std::string name;
};

struct Texture
{
    Sampler* pSampler = nullptr;
    Image* pSource = nullptr;
    std::string name;
};

enum class PrimitiveMode
{
    Points = 0,
    Lines = 1,
    LineLoop = 2,
    LineStrip = 3,
    Triangles = 4,
    TriangleStrip = 5,
    TriangleFan = 6,
};

struct Primitive
{
    PrimitiveMode mode = PrimitiveMode::Triangles;
    Accessor* pIndices = nullptr;
    Material* pMaterial = nullptr;

    // common attributes
    Accessor* pPosition = nullptr;  // POSITION
    Accessor* pNormal = nullptr;    // NORMAL
    Accessor* pTexcoord0 = nullptr; // TEXCOORD_0
    Accessor* pJoints0 = nullptr;   // JOINTS_0
    Accessor* pWeights0 = nullptr;  // WEIGHTS_0

    bool HasSkinAttributes() const { return pJoints0 && pWeights0; }
};

struct Mesh
{
    std::string name;
    // TODO: Make this a vector of objects instead of unique pointers
    std::vector<std::unique_ptr<Primitive>> primitives;

    // weights not supported
};

struct ExternalFile
{
    std::string mimeType;
    std::string uri;

    // derived from uri
    std::string canonicalFileName;
};

struct ExternalAsset
{
    std::string name;
    ExternalFile* pFile = nullptr;
};

// Derived values for each node
struct NodeInfo
{
    Node* pParent = nullptr;

    // first node in ancestors is the root node, last node in ancestors is the parent
    std::vector<Node*> ancestors;

    // If this node is the target of a rigid animation
    bool rigidAnimationTarget = false;

    // If this node or one of its ancestors within this glTF file is affected by a rigid animation
    bool localRigidAnimation = false;

    // Combination of this node's and its ancestors' playback properties
    PlaybackParameters localPlaybackParameters{};
};

struct Node
{
    // camera not supported
    std::vector<Node*> children;
    Skin* pSkin = nullptr;
    // matrix not supported
    Mesh* pMesh = nullptr;
    float3 translation{0.0f, 0.0f, 0.0f};
    float4 rotation{0.0f, 0.0f, 0.0f, 1.0f}; // xyzw
    float3 scale{1.0f, 1.0f, 1.0f};
    // weights not supported
    std::string name;
    ExternalAsset* pExternalAsset = nullptr;
    NodeExtras extras{};

    // Derived values for each node
    NodeInfo info{};
};

struct Skin
{
    Accessor* pInverseBindMatrices = nullptr;
    Node* pSkeleton = nullptr; // root node of the skeleton
    std::vector<Node*> joints; // nodes of each joint in the skeleton
    std::string name;
};

enum class Interpolation
{
    Linear,
    Step,
    CubicSpline,
};

enum class AnimationPath
{
    Translation,
    Rotation,
    Scale,
};

struct AnimationSampler
{
    Accessor* pInput = nullptr;
    Interpolation interpolation = Interpolation::Linear;
    Accessor* pOutput = nullptr;
};

struct AnimationChannel
{
    AnimationSampler* pSampler = nullptr;
    // part of target
    Node* pNode = nullptr;
    AnimationPath path = AnimationPath::Translation;
};

struct Animation
{
    std::string name;
    // TODO: Make this a vector of objects instead of unique pointers
    std::vector<std::unique_ptr<AnimationSampler>> samplers;
    // TODO: Make this a vector of objects instead of unique pointers
    std::vector<std::unique_ptr<AnimationChannel>> channels;
};

struct Scene
{
    std::string name;
    std::vector<Node*> nodes;
};

// The raw data referenced by glTF buffers
struct BufferData
{
    std::vector<uint8_t> data;
};

// The raw data referenced by glTF images
struct ImageData
{
    int width = 0;
    int height = 0;
    static constexpr int kChannels = 4;
    std::vector<uint8_t> data;
};

inline bool IsDataUri(const std::string_view& uri)
{
    return uri.substr(0, 5) == "data:";
}

template<typename T>
class AccessorView
{
public:
    AccessorView(const Accessor& accessor, const BufferView& bufferView, const BufferData& bufferData)
    {
        m_pRawDataBase = bufferData.data.data() + bufferView.byteOffset + accessor.byteOffset;
        m_componentSize = GetAccessorComponentTypeSize(accessor.componentType);
        m_stride = bufferView.byteStride;
        if (m_stride == 0)
        {
            m_stride = m_componentSize * GetAccessorTypeSize(accessor.type);
        }
        m_numElements = accessor.count;

        // Error checking
        if (sizeof(T) != m_componentSize * GetAccessorTypeSize(accessor.type))
        {
            throw std::runtime_error("AccessorView type size mismatch");
        }
    }

    size_t GetSize() const { return m_numElements; }

    const T& operator[](size_t index) const
    {
        if (index >= m_numElements)
        {
            throw std::out_of_range("AccessorView index out of range");
        }
        return *reinterpret_cast<const T*>(m_pRawDataBase + index * m_stride);
    }

private:
    const uint8_t* m_pRawDataBase;
    size_t m_componentSize;
    size_t m_stride = 0;
    size_t m_numElements = 0;
};

} // namespace Gltf

class GltfAsset
{
public:
    GltfAsset() = default;
    GltfAsset(const GltfAsset&) = delete;
    GltfAsset& operator=(const GltfAsset&) = delete;
    GltfAsset(GltfAsset&&) noexcept = default;
    GltfAsset& operator=(GltfAsset&&) noexcept = default;

    static GltfAsset Load(const std::string& path);

    template<typename T>
    Gltf::AccessorView<T> GetAccessorView(const Gltf::Accessor* accessor) const
    {
        if (!accessor || !accessor->pBufferView || !accessor->pBufferView->pBuffer ||
            !accessor->pBufferView->pBuffer->pData)
        {
            Gltf::Fatal("Accessor has no loaded buffer data.");
        }
        return Gltf::AccessorView<T>(*accessor, *accessor->pBufferView, *accessor->pBufferView->pBuffer->pData);
    }

    const std::vector<Gltf::Node*>& GetSceneRootNodes() const;
    const std::vector<Gltf::Node*>& GetAllRootNodes() const;

    std::string sourcePath;
    std::filesystem::path baseDirectory;

    std::vector<std::unique_ptr<Gltf::Buffer>> buffers;
    std::vector<std::unique_ptr<Gltf::BufferView>> bufferViews;
    std::vector<std::unique_ptr<Gltf::Accessor>> accessors;
    std::vector<std::unique_ptr<Gltf::ExternalFile>> files;
    std::vector<std::unique_ptr<Gltf::ExternalAsset>> externalAssets;
    std::vector<std::unique_ptr<Gltf::Node>> nodes;
    std::vector<std::unique_ptr<Gltf::Mesh>> meshes;
    std::vector<std::unique_ptr<Gltf::Material>> materials;
    std::vector<std::unique_ptr<Gltf::Image>> images;
    std::vector<std::unique_ptr<Gltf::Texture>> textures;
    std::vector<std::unique_ptr<Gltf::Sampler>> samplers;
    std::vector<std::unique_ptr<Gltf::Skin>> skins;
    std::vector<std::unique_ptr<Gltf::Animation>> animations;
    std::vector<std::unique_ptr<Gltf::Scene>> scenes;
    Gltf::Scene* pDefaultScene = nullptr;

private:
    // Load stages that need the private root-node list.
    void DeriveNodeInfo();
    void BuildDefaultSceneViews();

public:
    // Derived and loaded values
    std::vector<std::unique_ptr<Gltf::BufferData>> bufferData;
    std::vector<std::unique_ptr<Gltf::ImageData>> imageData;
    std::vector<Gltf::Node*> defaultSceneMeshNodes;
    std::vector<Gltf::Node*> defaultSceneExternalAssetNodes;

private:
    std::vector<Gltf::Node*> m_rootNodes;
};
