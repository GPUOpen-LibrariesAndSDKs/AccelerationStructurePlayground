# Acceleration Structure Playground

The Acceleration Structure Playground is a demo app for playing with different configurations
of acceleration structures, including Mid-Level Acceleration Structures (MDLAS) introduced by
the Ray Tracing Multi-Level Acceleration Structures (RT-MLAS) extension. Currently RT-MLAS is
only supported by AMD's latest Vulkan driver.

This app loads in a hierarchical scene, builds a configurable hierarchy of acceleration
structures, and then renders the scene using simple real-time ray tracing. The configurable
hierarchy of acceleration structures is what makes this the Acceleration Structure Playground.
You have control over what objects are put into BLASs and what objects are put into MDLASs.
This is done by constructing a plan for each level of acceleration structure that reference's
the scene's hierarchy to specify what should be included in that level of acceleration structures.

BLASs have the most options for acceleration structures. They can choose between putting each glTF
Mesh into their own BLAS, putting nodes at a specified level of the scene's hierarchy into their
own BLAS, or a hybrid approach. The hybrid approach groups skinned meshes into a single BLAS per
glTF file and each non-skinned mesh into its own BLAS. The hybrid approach efficiently animates
objects that use a combination of skinned and rigid animations.

MDLASs can be constructed at any level of the scene's hierarchy. Levels can either be specified
from leafs (height) or from the root (depth).

Using the Acceleration Structure Playground requires an AMD Radeon RX 9000 Series GPU and
a Vulkan driver with `VK_AMDX_multi_level_acceleration_structure` support.

## glTF Input Files
The input glTF files support a few additional features beyond standard glTF v2.0.

First they support external asset references (a proposed feature for glTF v2.1).
This looks like the following in a glTF file:
```json
{
  "asset": { "version": "2.1" },
  "files": [ { "mimeType": "model/gltf+json", "uri": "../../objects/newtons_cradle.gltf" } ],
  "externalAssets": [ { "name": "cradle", "file": 0 } ],
  "nodes": [
    { "name": "cradle_0", "translation": [ -5.0, 0.0, 0.0 ], "externalAsset": 0 },
    { "name": "cradle_1", "translation": [  5.0, 0.0, 0.0 ], "externalAsset": 0 }
  ]
}
```
`files` lists the referenced glTF files where `uri` is the relative path to the file.
`externalAssets` references a file and names it so nodes can reference it in their
`externalAsset` field. `externalAsset` places an instance of that whole asset under the node
using the node's transform. Referenced files can themselves reference other files, so scenes
can be nested arbitrarily deep. Each referenced file is loaded only once, regardless of how
many nodes reference it.

Second they support specifying animation parameters to get scenes where multiple instances of the
same animated object exist but have different phase offsets in their animation. 
This looks like the following in a glTF file:
```json
"nodes": [
  {
    "name": "cradle_1",
    "externalAsset": 0,
    "extras": {
      "animationSpeed": 2.0,
      "animationOffset": 0.5
    }
  }
]
```
Both values go in the node's `extras` dictionary. `animationSpeed` multiplies the animation
time, and `animationOffset` specifies a fixed offset for the animation time. By default
`animationSpeed` is `1.0` and `animationOffset` to `0.0`. It is possible for the animation
parameters to be updated multiple times in the hierarchy. In this case, the child's speed
is multiplied by its parent's, and the parent's offset is scaled by the child's speed and
added to the child's offset. An `animationSpeed` of `0.0` pauses the node's animation.

See `objects/10k_newtons_cradles.gltf` for a complete example.

## Building

Building Acceleration Structure Playground requires Windows and the Vulkan SDK. Using RT-MLAS
requires a Vulkan driver with the `VK_AMDX_multi_level_acceleration_structure` extension which
provides the necessary functionality to build and use MDLASs.

To build Acceleration Structure Playground, use CMake 3.20 or later and run

```powershell
cmake -S . -B build
cmake --build build --config RelWithDebInfo
```

If successful, the build will produce the executable `build/RelWithDebInfo/AccelerationStructurePlayground.exe`.

## Running

To run Acceleration Structure Playground, you must provide a glTF file (either `.gltf` or `.glb`) to render.
The following command renders the provided scene composed of 10,000 Newton's cradle object:

```powershell
build/RelWithDebInfo/AccelerationStructurePlayground.exe objects/10k_newtons_cradles.gltf
```

By default, this will build a BLAS for each glTF mesh in the Newton's cradle and then position them in the TLAS
according to their animation each frame. Instead, RT-MLAS can be used to build an MDLAS for each differently
animated Newton's cradle, then the TLAS scene is just instances of the MDLASs. To use RT-MLAS in this program,
you can either use command line arguments to specify which level in the scene hierarchy to build MDLASs for,
or you can use the GUI controls.

The command line options for adding a layer of MDLASs to the scene is below:
 
- `--mdlas-level from-leaves:<n>` - builds MDLASs `n` levels above the leaves in the scene graph. If the leaves are
  rigidly animated objects, then `--mlas-level from-leaves:0` would build an MLAS for each uniquely animated instance
  of that object.
- `--mdlas-level from-root:<n>` - builds MDLASs `n` levels below the root in the scene graph.

In `10k_newtons_cradles.gltf`, the scene hierarchy is a single root (`10k_newtons_cradles.gltf`) with 10000 instances
of Newton's cradles leaf nodes (`newtons_cradle.gltf`), so the following command will build a 

```powershell
build/RelWithDebInfo/AccelerationStructurePlayground.exe --mlas-level from-leaves:0 objects/10k_newtons_cradles.gltf
```

## Generating Large Test Scenes

RT-MLAS becomes interesting once scenes get large and complex. This repo contains the script
`scripts/generate_instances.py` which takes in glTF assets and constructs scenes with many
instances of those assets. The script is highly configurable enabling you to place
objects with a configurable number of staggered animation phases. Use `-h` to see all
available options. For example, `objects/10k_newtons_cradles.gltf` was created with the
following command line:

```powershell
py .\scripts\generate_instances.py --asset .\objects\newtons_cradle.gltf --count 10000 --layout grid --columns 100 --spacing 20 --phases 10 --phase-assignment random objects/10k_newtons_cradles.gltf
```

## AI Disclosure

Portions of this repo were created using assistance from AI tools.
