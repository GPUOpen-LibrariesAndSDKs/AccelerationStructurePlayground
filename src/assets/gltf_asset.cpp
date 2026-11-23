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

#include "gltf_asset.h"
#include <json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

using json = nlohmann::json;

namespace Gltf
{
namespace
{

constexpr uint32_t GlbMagic = 0x46546C67;
constexpr uint32_t JsonChunk = 0x4E4F534A;
constexpr uint32_t BinChunk = 0x004E4942;

struct Document
{
    json root;
    std::optional<std::vector<uint8_t>> embeddedBuffer; // GLB binary chunk
    std::filesystem::path baseDirectory;
};

struct GltfParserContext
{
    std::filesystem::path baseDirectory;
};

//////////////////////
// Helper functions //
//////////////////////

// URI Helper functions
///////////////////////

int HexDigit(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

// Replaces %xx sequences in a URI with their corresponding characters, e.g. %20 is replaced with ' '
std::string DecodeUriString(std::string_view uri)
{
    std::string result;
    for (size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size())
        {
            const int high = HexDigit(uri[i + 1]);
            const int low = HexDigit(uri[i + 2]);
            if (high >= 0 && low >= 0)
            {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        result.push_back(uri[i]);
    }
    return result;
}

// Compile-time function used to generate decode table in DecodeBase64
constexpr std::array<int8_t, 256> GenerateBase64DecodeTable()
{
    std::array<int8_t, 256> table{-1};

    for (int i = 0; i < 256; ++i)
    {
        table[i] = -1;
    }

    int8_t value = 0;
    for (char c = 'A'; c <= 'Z'; c++)
    {
        table[static_cast<unsigned char>(c)] = value++;
    }
    for (char c = 'a'; c <= 'z'; c++)
    {
        table[static_cast<unsigned char>(c)] = value++;
    }
    for (char c = '0'; c <= '9'; c++)
    {
        table[static_cast<unsigned char>(c)] = value++;
    }
    table[static_cast<unsigned char>('+')] = value++;
    table[static_cast<unsigned char>('/')] = value++;

    if (value != 64)
    {
        // This will cause a compilation error if this function is used at compile-time and this code is reached.
        throw std::runtime_error("Base64 decode table generation error.");
    }

    return table;
};

// Decodes base 64 encoded string
std::vector<uint8_t> DecodeBase64(std::string_view encoded)
{
    constexpr std::array<int8_t, 256> decodeTable = GenerateBase64DecodeTable();

    std::vector<uint8_t> result;
    result.reserve(encoded.size() * 3 / 4);
    uint32_t accumulator = 0;
    int bits = 0;
    for (char character : encoded)
    {
        if (character == '=')
        {
            break;
        }
        const int8_t value = decodeTable[static_cast<unsigned char>(character)];
        if (value == -1)
        {
            Fatal("Unexpected character '%c' in base64 encoded string.", character);
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            result.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
        }
    }
    return result;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        Fatal("Could not open '%s'.", path.string().c_str());
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> LoadDataFromUri(const GltfParserContext& ctx, const std::string_view& uri)
{
    // URI contains immediate data
    if (IsDataUri(uri))
    {
        // Only support base 64 for now
        constexpr std::string_view base64Prefix = ";base64,";
        auto base64Pos = uri.find(base64Prefix);
        if (base64Pos == std::string_view::npos)
        {
            // Truncate URI to make it printable:
            constexpr size_t maxStringLength = 20;
            std::string printedUri{uri.substr(0, maxStringLength)};
            if (uri.length() > maxStringLength)
            {
                printedUri += "...";
            }
            Fatal("Unsupported data URI format in '%s'.", printedUri.c_str());
        }

        // base64Pos is valid
        std::string_view base64Data = uri.substr(base64Pos + base64Prefix.size());
        return DecodeBase64(base64Data);
    }
    else
    {
        // If the URI is not a data URI, it should be a file path
        return ReadFile(ctx.baseDirectory / DecodeUriString(uri));
    }
}

// JSON Parsing helper functions
////////////////////////////////

template<typename T>
T* ResolveReference(const json& value, std::vector<std::unique_ptr<T>>& objects, const char* key)
{
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
    {
        Fatal("glTF reference '%s' must be a nonnegative integer.", key);
    }
    const int64_t index = value.get<int64_t>();
    if (index < 0 || static_cast<uint64_t>(index) >= objects.size())
    {
        Fatal("glTF reference '%s' is out of range: %lld.", key, static_cast<long long>(index));
    }
    return objects[static_cast<size_t>(index)].get();
}

template<typename T>
T* ParseReference(const json& object, const char* key, std::vector<std::unique_ptr<T>>& objects, bool required = false)
{
    if (!required && !object.contains(key))
    {
        return nullptr;
    }
    return ResolveReference(object.at(key), objects, key);
}

template<typename T>
std::vector<T*> ParseReferences(const json& object, const char* key, std::vector<std::unique_ptr<T>>& objects)
{
    std::vector<T*> references;
    if (object.contains(key))
    {
        const json& values = object.at(key);
        if (!values.is_array())
        {
            Fatal("glTF property '%s' must be an array.", key);
        }
        references.reserve(values.size());
        for (const auto& value : values)
        {
            references.push_back(ResolveReference(value, objects, key));
        }
    }
    return references;
}

template<typename T, size_t Size>
bool CopyArray(const json& object, const char* key, T& destination)
{
    if (!object.contains(key))
    {
        return false;
    }
    const json& values = object.at(key);
    if (!values.is_array() || values.size() != Size)
    {
        Fatal("glTF property '%s' must contain %zu numbers.", key, Size);
    }
    float* output = reinterpret_cast<float*>(&destination);
    for (size_t i = 0; i < Size; ++i)
    {
        output[i] = values[i].get<float>();
    }
    return true;
}

AccessorType ParseAccessorType(const std::string& typeString)
{
    if (typeString == "SCALAR") return AccessorType::Scalar;
    if (typeString == "VEC2") return AccessorType::Vec2;
    if (typeString == "VEC3") return AccessorType::Vec3;
    if (typeString == "VEC4") return AccessorType::Vec4;
    if (typeString == "MAT2") return AccessorType::Mat2;
    if (typeString == "MAT3") return AccessorType::Mat3;
    if (typeString == "MAT4") return AccessorType::Mat4;
    Fatal("Unknown accessor type: '%s'", typeString.c_str());
}

Interpolation ParseInterpolation(const std::string& interpString)
{
    if (interpString == "LINEAR") return Interpolation::Linear;
    if (interpString == "STEP") return Interpolation::Step;
    if (interpString == "CUBICSPLINE") return Interpolation::CubicSpline;
    Fatal("Unknown interpolation type: '%s'", interpString.c_str());
}

AnimationPath ParseAnimationPath(const std::string& pathString)
{
    if (pathString == "translation") return AnimationPath::Translation;
    if (pathString == "rotation") return AnimationPath::Rotation;
    if (pathString == "scale") return AnimationPath::Scale;
    Fatal("Unknown animation path: '%s'", pathString.c_str());
}

///////////////////////
// Parsing Functions //
///////////////////////

Buffer ParseBuffer(const json& buffer, GltfAsset&)
{
    Buffer ret;
    ret.uri = buffer.value("uri", "");
    ret.byteLength = buffer.at("byteLength").get<uint64_t>();

    return ret;
}

BufferView ParseBufferView(const json& bufferView, GltfAsset& asset)
{
    BufferView ret;
    ret.pBuffer = ParseReference(bufferView, "buffer", asset.buffers, true);
    ret.byteOffset = bufferView.value("byteOffset", 0ull);
    ret.byteLength = bufferView.at("byteLength").get<uint64_t>();
    ret.byteStride = bufferView.value("byteStride", 0u);

    return ret;
}

Accessor ParseAccessor(const json& accessor, GltfAsset& asset)
{
    Accessor ret;
    ret.pBufferView = ParseReference(accessor, "bufferView", asset.bufferViews);
    ret.byteOffset = accessor.value("byteOffset", 0ull);
    ret.componentType = static_cast<AccessorComponentType>(accessor.at("componentType").get<int>());
    ret.normalized = accessor.value("normalized", false);
    ret.count = accessor.at("count").get<uint64_t>();
    ret.type = ParseAccessorType(accessor.at("type").get<std::string>());
    return ret;
}

Material ParseMaterial(const json& material, GltfAsset& asset)
{
    Material ret;
    ret.name = material.value("name", "");
    if (material.contains("pbrMetallicRoughness"))
    {
        const json& pbr = material.at("pbrMetallicRoughness");
        CopyArray<float4, 4>(pbr, "baseColorFactor", ret.pbrMetallicRoughness.baseColorFactor);
        if (pbr.contains("baseColorTexture"))
        {
            const json& baseColorTexture = pbr.at("baseColorTexture");
            ret.pbrMetallicRoughness.baseColorTexture.pTexture =
                ParseReference(baseColorTexture, "index", asset.textures);
            ret.pbrMetallicRoughness.baseColorTexture.texCoord = baseColorTexture.value("texCoord", 0);
        }
        ret.pbrMetallicRoughness.metallicFactor = pbr.value("metallicFactor", 1.0f);
        ret.pbrMetallicRoughness.roughnessFactor = pbr.value("roughnessFactor", 1.0f);
    }
    ret.doubleSided = material.value("doubleSided", false);
    return ret;
}

Image ParseImage(const json& image, GltfAsset& asset)
{
    Image ret;
    if (image.contains("uri"))
    {
        // Loading image from a uri string, either by a file name or immediate data
        if (image.contains("bufferView") || image.contains("mimeType"))
        {
            Fatal("Image with a 'uri' should not contain 'bufferView' or 'mimeType'.");
        }
        ret.uri = image.at("uri").get<std::string>();
    }
    else if (image.contains("bufferView") && image.contains("mimeType"))
    {
        // Image comes from data in a buffer view
        ret.pBufferView = ParseReference(image, "bufferView", asset.bufferViews, true);
        ret.mimeType = image.at("mimeType").get<std::string>();
    }
    else
    {
        Fatal("Image should contain 'uri' or 'bufferView' and 'mimeType'.");
    }

    ret.name = image.value("name", "");

    return ret;
}

Sampler ParseSampler(const json& sampler, GltfAsset&)
{
    Sampler ret;
    if (sampler.contains("magFilter")) ret.magFilter = sampler.at("magFilter").get<int32_t>();
    if (sampler.contains("minFilter")) ret.minFilter = sampler.at("minFilter").get<int32_t>();
    ret.wrapS = sampler.value("wrapS", 10497);
    ret.wrapT = sampler.value("wrapT", 10497);
    ret.name = sampler.value("name", "");
    return ret;
}

Texture ParseTexture(const json& texture, GltfAsset& asset)
{
    Texture ret;
    ret.pSampler = ParseReference(texture, "sampler", asset.samplers);
    ret.pSource = ParseReference(texture, "source", asset.images);
    ret.name = texture.value("name", "");
    return ret;
}

Primitive ParsePrimitive(const json& primitive, GltfAsset& asset)
{
    Primitive ret;
    ret.mode = static_cast<PrimitiveMode>(primitive.value("mode", 4));
    ret.pIndices = ParseReference(primitive, "indices", asset.accessors);
    ret.pMaterial = ParseReference(primitive, "material", asset.materials);

    if (!primitive.contains("attributes"))
    {
        Fatal("glTF primitive is missing its required 'attributes' property.");
    }
    const json& attributes = primitive.at("attributes");
    ret.pPosition = ParseReference(attributes, "POSITION", asset.accessors);
    ret.pNormal = ParseReference(attributes, "NORMAL", asset.accessors);
    ret.pTexcoord0 = ParseReference(attributes, "TEXCOORD_0", asset.accessors);
    ret.pJoints0 = ParseReference(attributes, "JOINTS_0", asset.accessors);
    ret.pWeights0 = ParseReference(attributes, "WEIGHTS_0", asset.accessors);

    return ret;
}

Mesh ParseMesh(const json& mesh, GltfAsset& asset)
{
    Mesh ret;
    ret.name = mesh.value("name", "");
    if (mesh.contains("primitives"))
    {
        const json& primitives = mesh.at("primitives");
        for (const auto& primitive : primitives)
        {
            // TODO: Make this a vector of objects instead of unique pointers
            ret.primitives.push_back(std::make_unique<Primitive>(ParsePrimitive(primitive, asset)));
        }
    }

    return ret;
}

ExternalFile ParseExternalFile(const json& file, GltfAsset& asset)
{
    ExternalFile ret;
    ret.mimeType = file.value("mimeType", "");
    ret.uri = file.at("uri").get<std::string>();
    if (IsDataUri(ret.uri))
    {
        Fatal("Data URI not supported for external asset files.");
    }
    // TODO: CORRECTLY GET THE URI FILENAME, IT MAY BE ABSOLUTE
    ret.canonicalFileName = std::filesystem::weakly_canonical(asset.baseDirectory / DecodeUriString(ret.uri)).string();
    return ret;
}

ExternalAsset ParseExternalAsset(const json& externalAsset, GltfAsset& asset)
{
    ExternalAsset ret;
    ret.name = externalAsset.value("name", "");
    ret.pFile = ParseReference(externalAsset, "file", asset.files, true);
    return ret;
}

Node ParseNode(const json& node, GltfAsset& asset)
{
    Node ret{};
    ret.children = ParseReferences(node, "children", asset.nodes);
    ret.pSkin = ParseReference(node, "skin", asset.skins);
    ret.pMesh = ParseReference(node, "mesh", asset.meshes);
    CopyArray<float3, 3>(node, "translation", ret.translation);
    CopyArray<float4, 4>(node, "rotation", ret.rotation);
    CopyArray<float3, 3>(node, "scale", ret.scale);
    ret.name = node.value("name", "");
    ret.pExternalAsset = ParseReference(node, "externalAsset", asset.externalAssets);

    if (node.contains("extras"))
    {
        const json& extras = node.at("extras");

        ret.extras.playbackParameters.speed = extras.value("animationSpeed", 1.0f);
        ret.extras.playbackParameters.timeOffset = extras.value("animationOffset", 0.0f);
    }

    return ret;
}

Skin ParseSkin(const json& skin, GltfAsset& asset)
{
    Skin ret;
    ret.pInverseBindMatrices = ParseReference(skin, "inverseBindMatrices", asset.accessors);
    ret.pSkeleton = ParseReference(skin, "skeleton", asset.nodes);
    skin.at("joints");
    ret.joints = ParseReferences(skin, "joints", asset.nodes);
    ret.name = skin.value("name", "");
    return ret;
}

AnimationSampler ParseAnimationSampler(const json& sampler, GltfAsset& asset)
{
    AnimationSampler ret;
    ret.pInput = ParseReference(sampler, "input", asset.accessors, true);
    ret.interpolation = ParseInterpolation(sampler.value("interpolation", "LINEAR"));
    ret.pOutput = ParseReference(sampler, "output", asset.accessors, true);
    return ret;
}

AnimationChannel ParseAnimationChannel(const json& channel, GltfAsset& asset, Animation& animation)
{
    AnimationChannel ret;
    ret.pSampler = ParseReference(channel, "sampler", animation.samplers, true);
    const json& target = channel.at("target");
    ret.pNode = ParseReference(target, "node", asset.nodes, true);
    ret.path = ParseAnimationPath(target.at("path").get<std::string>());
    return ret;
}

Animation ParseAnimation(const json& animation, GltfAsset& asset)
{
    Animation ret;
    ret.name = animation.value("name", "");

    if (!animation.contains("samplers") || !animation.contains("channels"))
    {
        Fatal("glTF animation is missing its required 'samplers' or 'channels' property.");
    }
    const json& samplers = animation.at("samplers");
    for (const auto& sampler : samplers)
    {
        // TODO: Make this a vector of objects instead of unique pointers
        ret.samplers.push_back(std::make_unique<AnimationSampler>(ParseAnimationSampler(sampler, asset)));
    }

    const json& channels = animation.at("channels");
    for (const auto& channel : channels)
    {
        // TODO: Make this a vector of objects instead of unique pointers
        ret.channels.push_back(std::make_unique<AnimationChannel>(ParseAnimationChannel(channel, asset, ret)));
    }

    return ret;
}

Scene ParseScene(const json& scene, GltfAsset& asset)
{
    Scene ret;
    ret.name = scene.value("name", "");
    ret.nodes = ParseReferences(scene, "nodes", asset.nodes);
    return ret;
}

/////////////////////
// GLB File Format //
/////////////////////

struct GlbHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t length;
};

struct GlbChunkHeader
{
    uint32_t chunkLength;
    uint32_t chunkType;
};

Document LoadDocument(const std::filesystem::path& path)
{
    Document doc;
    doc.baseDirectory = path.parent_path();
    std::vector<uint8_t> fileData = ReadFile(path);

    // Check if it's a GLB file by magic number
    const bool isGlb =
        fileData.size() >= sizeof(GlbHeader) && *reinterpret_cast<const uint32_t*>(fileData.data()) == GlbMagic;

    if (isGlb)
    {
        // Validate header
        if (fileData.size() < sizeof(GlbHeader))
        {
            Fatal("GLB file too small: '%s'", path.string().c_str());
        }

        GlbHeader header;
        std::memcpy(&header, fileData.data(), sizeof(GlbHeader));

        if (header.version != 2)
        {
            Fatal("Unsupported GLB version %u in '%s'", header.version, path.string().c_str());
        }

        if (header.length != fileData.size())
        {
            Fatal("GLB header length mismatch in '%s'", path.string().c_str());
        }

        // Parse chunks
        size_t offset = sizeof(GlbHeader);
        bool hasJsonChunk = false;

        while (offset < fileData.size())
        {
            if (offset + sizeof(GlbChunkHeader) > fileData.size())
            {
                Fatal("Truncated chunk header in GLB file '%s'", path.string().c_str());
            }

            GlbChunkHeader chunkHeader;
            std::memcpy(&chunkHeader, fileData.data() + offset, sizeof(GlbChunkHeader));
            offset += sizeof(GlbChunkHeader);

            if (offset + chunkHeader.chunkLength > fileData.size())
            {
                Fatal("Truncated chunk data in GLB file '%s'", path.string().c_str());
            }

            if (chunkHeader.chunkType == JsonChunk)
            {
                if (hasJsonChunk)
                {
                    Fatal("Multiple JSON chunks in GLB file '%s'", path.string().c_str());
                }
                hasJsonChunk = true;
                doc.root =
                    json::parse(reinterpret_cast<const char*>(fileData.data() + offset),
                                reinterpret_cast<const char*>(fileData.data() + offset + chunkHeader.chunkLength));
            }
            else if (chunkHeader.chunkType == BinChunk)
            {
                if (doc.embeddedBuffer)
                {
                    Fatal("Multiple BIN chunks in GLB file '%s'", path.string().c_str());
                }
                doc.embeddedBuffer = std::vector<uint8_t>(fileData.begin() + offset,
                                                          fileData.begin() + offset + chunkHeader.chunkLength);
            }

            offset += chunkHeader.chunkLength;
        }

        if (!hasJsonChunk)
        {
            Fatal("GLB file missing JSON chunk: '%s'", path.string().c_str());
        }
    }
    else
    {
        // Regular GLTF file
        doc.root = json::parse(fileData.begin(), fileData.end());
    }

    return doc;
}

template<typename T, typename ParseFunc>
void ParseArray(const json& root, const char* key, std::vector<std::unique_ptr<T>>& output, ParseFunc parser,
                GltfAsset& asset)
{
    if (!root.contains(key)) return;
    const json& array = root.at(key);
    if (output.size() != array.size())
    {
        Fatal("glTF array '%s' was not allocated before parsing.", key);
    }
    for (size_t i = 0; i < array.size(); ++i)
    {
        *output[i] = parser(array[i], asset);
    }
}

template<typename T>
void AllocateArray(const json& root, const char* key, std::vector<std::unique_ptr<T>>& output)
{
    if (!root.contains(key)) return;
    if (!root.at(key).is_array())
    {
        Fatal("glTF property '%s' must be an array.", key);
    }
    output.reserve(root.at(key).size());
    for (size_t i = 0; i < root.at(key).size(); ++i)
    {
        output.push_back(std::make_unique<T>());
    }
}

// Every array is allocated before any is parsed, so that references between arrays resolve
// whichever order they appear in the file. Each array is named once here and visited twice,
// which is what keeps the allocate and parse passes from drifting apart - ParseArray rejects a
// mismatch rather than indexing past the end.
void ParseDocumentArrays(const json& root, GltfAsset& asset)
{
    enum class Pass
    {
        Allocate,
        Parse
    };

    const auto visitArrays = [&root, &asset](Pass pass) {
        const auto array = [&](const char* key, auto member, auto parser) {
            if (pass == Pass::Allocate)
            {
                AllocateArray(root, key, asset.*member);
            }
            else
            {
                ParseArray(root, key, asset.*member, parser, asset);
            }
        };

        array("buffers", &GltfAsset::buffers, ParseBuffer);
        array("bufferViews", &GltfAsset::bufferViews, ParseBufferView);
        array("accessors", &GltfAsset::accessors, ParseAccessor);
        array("files", &GltfAsset::files, ParseExternalFile);
        array("externalAssets", &GltfAsset::externalAssets, ParseExternalAsset);
        array("materials", &GltfAsset::materials, ParseMaterial);
        array("images", &GltfAsset::images, ParseImage);
        array("textures", &GltfAsset::textures, ParseTexture);
        array("samplers", &GltfAsset::samplers, ParseSampler);
        array("meshes", &GltfAsset::meshes, ParseMesh);
        array("skins", &GltfAsset::skins, ParseSkin);
        array("animations", &GltfAsset::animations, ParseAnimation);
        array("nodes", &GltfAsset::nodes, ParseNode);
        array("scenes", &GltfAsset::scenes, ParseScene);
    };

    visitArrays(Pass::Allocate);
    visitArrays(Pass::Parse);
}

} // anonymous namespace

////////////////////////
// Main Load Function //
////////////////////////

namespace
{

// Reads every buffer's bytes, then checks each buffer view fits inside the buffer it names.
void LoadBufferData(const Document& doc, const GltfParserContext& ctx, GltfAsset& result)
{
    result.bufferData.reserve(result.buffers.size());

    for (size_t i = 0; i < result.buffers.size(); ++i)
    {
        Buffer& buffer = *result.buffers[i];
        auto pBufferData = std::make_unique<BufferData>();

        if (buffer.uri.empty() && doc.embeddedBuffer && i == 0)
        {
            pBufferData->data = *doc.embeddedBuffer;
        }
        else
        {
            pBufferData->data = LoadDataFromUri(ctx, buffer.uri);
        }

        if (pBufferData->data.size() != buffer.byteLength)
        {
            Fatal("Buffer %zu size mismatch: expected %llu bytes, got %zu bytes", i, buffer.byteLength,
                  pBufferData->data.size());
        }
        buffer.pData = pBufferData.get();
        result.bufferData.push_back(std::move(pBufferData));
    }

    for (const auto& pView : result.bufferViews)
    {
        if (pView->byteOffset > pView->pBuffer->byteLength ||
            pView->byteLength > pView->pBuffer->byteLength - pView->byteOffset)
        {
            Fatal("BufferView exceeds its buffer data.");
        }
    }
}

// Decodes every image to RGBA, from a data URI, an external file, or a buffer view.
void DecodeImages(const GltfParserContext& ctx, GltfAsset& result)
{
    result.imageData.reserve(result.images.size());
    for (size_t i = 0; i < result.images.size(); ++i)
    {
        Image& image = *result.images[i];
        std::vector<uint8_t> encodedImageData;

        if (!image.uri.empty())
        {
            encodedImageData = LoadDataFromUri(ctx, image.uri);
        }
        else if (image.pBufferView)
        {
            const BufferView& bufferView = *image.pBufferView;
            const BufferData& bufferData = *bufferView.pBuffer->pData;
            encodedImageData.assign(bufferData.data.begin() + bufferView.byteOffset,
                                    bufferData.data.begin() + bufferView.byteOffset + bufferView.byteLength);
        }
        else
        {
            Fatal("Image %zu has neither uri nor bufferView", i);
        }

        if (encodedImageData.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            Fatal("Encoded image %zu exceeds stb_image's input size limit.", i);
        }

        int width = 0;
        int height = 0;
        stbi_uc* pixels = stbi_load_from_memory(encodedImageData.data(), static_cast<int>(encodedImageData.size()),
                                                &width, &height, nullptr, ImageData::kChannels);

        if (!pixels)
        {
            Fatal("Failed to decode image %zu: %s", i, stbi_failure_reason());
        }

        auto pImageData = std::make_unique<ImageData>();
        pImageData->width = width;
        pImageData->height = height;
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (width <= 0 || height <= 0 || pixelCount > std::numeric_limits<size_t>::max() / ImageData::kChannels)
        {
            stbi_image_free(pixels);
            Fatal("Decoded image %zu has invalid dimensions (%d x %d).", i, width, height);
        }
        const size_t byteCount = pixelCount * ImageData::kChannels;
        pImageData->data.assign(pixels, pixels + byteCount);
        stbi_image_free(pixels);

        image.pData = pImageData.get();
        result.imageData.push_back(std::move(pImageData));
    }
}

} // namespace

// Keyframe times must be finite and ascending for sampling to binary search them. This is a
// property of the asset, so it is checked once here rather than on every frame of playback.
void ValidateAnimationSamplers(const GltfAsset& result)
{
    for (const auto& animation : result.animations)
    {
        for (const auto& sampler : animation->samplers)
        {
            if (!sampler->pInput)
            {
                continue;
            }
            const AccessorView<float> times = result.GetAccessorView<float>(sampler->pInput);
            for (size_t i = 0; i < times.GetSize(); ++i)
            {
                if (!std::isfinite(times[i]) || (i > 0 && times[i] < times[i - 1]))
                {
                    Fatal("Animation keyframe times must be finite and ordered.");
                }
            }
        }
    }
}

} // namespace Gltf

// Fills in NodeInfo: which nodes an animation targets, parent links, ancestor chains and the
// inherited playback parameters each node ends up with.
void GltfAsset::DeriveNodeInfo()
{
    for (const auto& animation : animations)
    {
        for (const auto& pChannel : animation->channels)
        {
            if (pChannel->pNode)
            {
                pChannel->pNode->info.rigidAnimationTarget = true;
            }
        }
    }

    for (const auto& pNode : nodes)
    {
        for (Gltf::Node* pChild : pNode->children)
        {
            if (pChild->info.pParent)
            {
                Gltf::Fatal("glTF node has multiple parents.");
            }
            pChild->info.pParent = pNode.get();
        }
    }

    // Iterate through all nodes to fill in Node::info
    std::vector<Gltf::Node*> nodesToVisit;
    for (const auto& pNode : nodes)
    {
        if (!pNode->info.pParent)
        {
            m_rootNodes.push_back(pNode.get());
            nodesToVisit.push_back(pNode.get());
        }
    }

    size_t visitedNodes = 0;
    while (!nodesToVisit.empty())
    {
        Gltf::Node* node = nodesToVisit.back();
        nodesToVisit.pop_back();
        ++visitedNodes;
        Gltf::NodeInfo& info = node->info;
        info.localPlaybackParameters = node->extras.playbackParameters;
        if (info.pParent)
        {
            info.ancestors = info.pParent->info.ancestors;
            info.ancestors.push_back(info.pParent);
            info.localPlaybackParameters.UpdateWithParent(info.pParent->info.localPlaybackParameters);
        }
        // A node only moves if an animation targets it *and* its effective playback speed is
        // nonzero; paused playback leaves the node at a fixed pose. Motion is still inherited, so
        // a paused node under an animated parent is itself animated in world space.
        info.localRigidAnimation = info.rigidAnimationTarget && info.localPlaybackParameters.speed != 0.0f;
        if (info.pParent)
        {
            info.localRigidAnimation |= info.pParent->info.localRigidAnimation;
        }
        nodesToVisit.insert(nodesToVisit.end(), node->children.begin(), node->children.end());
    }

    // Every node of a forest is reachable from some root. A node left unvisited here therefore has
    // a parent but no root ancestor, which means it sits on a cycle. Reject it now: the default
    // scene walk below follows child pointers with no visited set and would otherwise run until it
    // exhausted memory.
    if (visitedNodes != nodes.size())
    {
        Gltf::Fatal("Cycle detected in the glTF node hierarchy.");
    }
}

// Resolves the scene to walk and records the node lists derived from it.
void GltfAsset::BuildDefaultSceneViews()
{
    std::vector<Gltf::Node*> nodesToVisit;
    // pDefaultScene reflects an explicitly declared "scene" only; GetSceneRootNodes() already
    // falls back to the first declared scene when it is absent. An asset that declares no scenes
    // at all still needs something to walk, so synthesize one covering every root node.
    if (scenes.empty())
    {
        scenes.push_back(std::make_unique<Gltf::Scene>());
        scenes.back()->nodes = m_rootNodes;
    }

    // Walk through all nodes starting with the default scene's roots
    const std::vector<Gltf::Node*>& sceneRoots = GetSceneRootNodes();
    nodesToVisit.insert(nodesToVisit.end(), sceneRoots.begin(), sceneRoots.end());
    while (!nodesToVisit.empty())
    {
        Gltf::Node* pNode = nodesToVisit.back();
        nodesToVisit.pop_back();
        if (pNode->pMesh)
        {
            defaultSceneMeshNodes.push_back(pNode);
        }
        if (pNode->pExternalAsset)
        {
            defaultSceneExternalAssetNodes.push_back(pNode);
        }
        nodesToVisit.insert(nodesToVisit.end(), pNode->children.begin(), pNode->children.end());
    }
}

GltfAsset GltfAsset::Load(const std::string& path)
{
    std::filesystem::path fsPath(path);
    if (!std::filesystem::exists(fsPath))
    {
        Gltf::Fatal("File does not exist: '%s'", path.c_str());
    }

    fsPath = std::filesystem::canonical(fsPath);

    Gltf::Document doc = Gltf::LoadDocument(fsPath);
    const json& root = doc.root;

    GltfAsset result;
    result.sourcePath = path;
    result.baseDirectory = doc.baseDirectory;

    Gltf::ParseDocumentArrays(root, result);
    result.pDefaultScene = Gltf::ParseReference(root, "scene", result.scenes);

    const Gltf::GltfParserContext ctx{doc.baseDirectory};
    Gltf::LoadBufferData(doc, ctx, result);
    Gltf::DecodeImages(ctx, result);
    Gltf::ValidateAnimationSamplers(result);
    result.DeriveNodeInfo();
    result.BuildDefaultSceneViews();
    return result;
}

const std::vector<Gltf::Node*>& GltfAsset::GetSceneRootNodes() const
{
    if (pDefaultScene) return pDefaultScene->nodes;
    if (!scenes.empty()) return scenes.front()->nodes;
    Gltf::Fatal("glTF asset has no scenes.");
}

const std::vector<Gltf::Node*>& GltfAsset::GetAllRootNodes() const
{
    return m_rootNodes;
}
