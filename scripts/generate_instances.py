#!/usr/bin/env python3
#
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#

"""Generate glTF v2.1 scenes that instantiate external glTF assets.

Examples:
  python generate_instances.py out.gltf --asset ../objects/newtons_cradle.gltf \
      --count 64 --layout grid --spacing 10 --phases 4
  python generate_instances.py ring.gltf --asset a.gltf --asset b.glb --count 24 --layout circle \
      --radius 40 --face-center --phases 3 --phase-assignment random
  python generate_instances.py field.gltf --asset a.gltf --count 200 --layout random --area 200 200 \
      --min-distance 6 --random-yaw --seed 7
"""

import argparse
import json
import math
import os
import random
import struct
import sys


def read_gltf_json(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"glTF":
        chunk_length, chunk_type = struct.unpack_from("<II", data, 12)
        if chunk_type != 0x4E4F534A:
            raise ValueError("first GLB chunk is not JSON")
        return json.loads(data[20:20 + chunk_length].decode("utf-8"))
    return json.loads(data.decode("utf-8"))


def detect_animation_duration(path):
    """Longest animation input range from accessor min/max, or None if unavailable."""
    try:
        root = read_gltf_json(path)
    except (OSError, ValueError) as e:
        print(f"warning: could not read '{path}' to detect animation length: {e}", file=sys.stderr)
        return None
    accessors = root.get("accessors", [])
    duration = None
    for animation in root.get("animations", []):
        for sampler in animation.get("samplers", []):
            accessor = accessors[sampler["input"]]
            if "max" in accessor:
                end = accessor["max"][0]
                duration = end if duration is None else max(duration, end)
    return duration


def layout_grid(count, args, rng):
    columns = args.columns or math.ceil(math.sqrt(count))
    rows = math.ceil(count / columns)
    x0 = -(columns - 1) * args.spacing * 0.5
    z0 = -(rows - 1) * args.spacing * 0.5
    positions = []
    for i in range(count):
        row, col = divmod(i, columns)
        positions.append((x0 + col * args.spacing, 0.0, z0 + row * args.spacing))
    return positions


def layout_circle(count, args, rng):
    radius = args.radius
    if radius is None:
        # Pick a radius that keeps neighbors roughly `spacing` apart.
        radius = max(args.spacing * count / (2.0 * math.pi), args.spacing) if count > 1 else 0.0
    positions = []
    for i in range(count):
        angle = 2.0 * math.pi * i / count
        positions.append((radius * math.cos(angle), 0.0, radius * math.sin(angle)))
    return positions


def layout_random(count, args, rng):
    width, depth = args.area
    min_dist_sq = args.min_distance * args.min_distance
    positions = []
    for i in range(count):
        for _ in range(args.max_attempts):
            p = (rng.uniform(-width * 0.5, width * 0.5), 0.0, rng.uniform(-depth * 0.5, depth * 0.5))
            if all((p[0] - q[0]) ** 2 + (p[2] - q[2]) ** 2 >= min_dist_sq for q in positions):
                break
        else:
            sys.exit(f"error: could not place instance {i} with --min-distance {args.min_distance} "
                     f"in a {width}x{depth} area; enlarge --area or reduce --min-distance/--count")
        positions.append(p)
    return positions


LAYOUTS = {"grid": layout_grid, "circle": layout_circle, "random": layout_random}


def assign_groups(count, groups, mode, rng, positions):
    if groups <= 1:
        return [0] * count
    if mode == "round-robin":
        return [i % groups for i in range(count)]
    if mode == "block":
        return [min(i * groups // count, groups - 1) for i in range(count)]
    if mode == "random":
        return [rng.randrange(groups) for _ in range(count)]
    if mode == "spatial":
        # Sweep along X so neighbors tend to share a phase.
        order = sorted(range(count), key=lambda i: (positions[i][0], positions[i][2]))
        result = [0] * count
        for rank, i in enumerate(order):
            result[i] = min(rank * groups // count, groups - 1)
        return result
    raise ValueError(mode)


def yaw_quaternion(yaw):
    return [0.0, math.sin(yaw * 0.5), 0.0, math.cos(yaw * 0.5)]


def round_list(values):
    return [round(v, 6) + 0.0 for v in values]


def main():
    parser = argparse.ArgumentParser(
        description="Generate a glTF v2.1 scene instancing external glTF assets.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("output", help="output .gltf path")
    parser.add_argument("--asset", action="append", required=True,
                        help="external glTF/GLB to instantiate (repeatable); path relative to the CWD")
    parser.add_argument("--asset-assignment", choices=["round-robin", "random"], default="round-robin",
                        help="how assets are distributed across instances when several are given")
    parser.add_argument("--count", type=int, default=16, help="number of instances")
    parser.add_argument("--layout", choices=sorted(LAYOUTS), default="grid", help="placement strategy")
    parser.add_argument("--spacing", type=float, default=10.0,
                        help="grid cell size; also default neighbor spacing for circle")
    parser.add_argument("--columns", type=int, help="grid columns (default: ceil(sqrt(count)))")
    parser.add_argument("--radius", type=float, help="circle radius (default: derived from --spacing)")
    parser.add_argument("--area", type=float, nargs=2, metavar=("WIDTH", "DEPTH"), default=(100.0, 100.0),
                        help="random layout area on the XZ plane, centered at origin")
    parser.add_argument("--min-distance", type=float, default=0.0,
                        help="minimum distance between random instances")
    parser.add_argument("--max-attempts", type=int, default=1000,
                        help="placement attempts per random instance")
    parser.add_argument("--height", type=float, default=0.0, help="Y coordinate of all instances")
    parser.add_argument("--scale", type=float, default=1.0, help="uniform scale for all instances")
    parser.add_argument("--face-center", action="store_true",
                        help="circle layout: rotate instances to face the center")
    parser.add_argument("--random-yaw", action="store_true", help="random rotation about Y")
    parser.add_argument("--phases", type=int, default=1,
                        help="number of distinct animation phases (playback groups)")
    parser.add_argument("--phase-assignment", choices=["round-robin", "block", "random", "spatial"],
                        default="round-robin", help="how phases are assigned to instances")
    parser.add_argument("--period", type=float,
                        help="animation length the phases are spread across "
                             "(default: detected from the first asset, else 2.0)")
    parser.add_argument("--speed", type=float, default=1.0, help="animationSpeed for every instance")
    parser.add_argument("--no-extras", action="store_true",
                        help="omit animation extras (all instances share default playback)")
    parser.add_argument("--seed", type=int, default=0, help="random seed")
    args = parser.parse_args()

    if args.count < 1:
        parser.error("--count must be >= 1")
    if args.phases < 1:
        parser.error("--phases must be >= 1")
    if args.columns is not None and args.columns < 1:
        parser.error("--columns must be >= 1")

    rng = random.Random(args.seed)
    output_dir = os.path.dirname(os.path.abspath(args.output))

    for asset in args.asset:
        if not os.path.isfile(asset):
            print(f"warning: asset '{asset}' does not exist", file=sys.stderr)

    period = args.period
    if period is None:
        period = detect_animation_duration(args.asset[0]) or 2.0
    phase_step = period / args.phases

    # The loader resolves external file URIs relative to the referencing glTF.
    files = []
    external_assets = []
    for asset in args.asset:
        try:
            uri = os.path.relpath(os.path.abspath(asset), output_dir)
        except ValueError:  # different Windows drives
            uri = os.path.abspath(asset)
        uri = uri.replace(os.sep, "/")
        external_assets.append({"name": os.path.splitext(os.path.basename(asset))[0], "file": len(files)})
        files.append({"mimeType": "model/gltf-binary" if asset.lower().endswith(".glb") else "model/gltf+json",
                      "uri": uri})

    positions = LAYOUTS[args.layout](args.count, args, rng)
    groups = assign_groups(args.count, args.phases, args.phase_assignment, rng, positions)

    nodes = []
    for i, (x, _, z) in enumerate(positions):
        if args.asset_assignment == "random":
            asset_index = rng.randrange(len(external_assets))
        else:
            asset_index = i % len(external_assets)

        node = {"name": f"instance_{i}_phase_{groups[i]}",
                "translation": round_list([x, args.height, z]),
                "externalAsset": asset_index}

        yaw = None
        if args.random_yaw:
            yaw = rng.uniform(0.0, 2.0 * math.pi)
        elif args.face_center and args.layout == "circle":
            # Rotate local +Z toward the origin.
            yaw = math.atan2(-x, -z)
        if yaw is not None:
            node["rotation"] = round_list(yaw_quaternion(yaw))
        if args.scale != 1.0:
            node["scale"] = [args.scale] * 3
        if not args.no_extras:
            node["extras"] = {"animationSpeed": args.speed,
                              "animationOffset": round(groups[i] * phase_step, 6)}
        nodes.append(node)

    document = {
        "asset": {"version": "2.1", "generator": "AccelerationStructurePlayground generate_instances.py"},
        "files": files,
        "externalAssets": external_assets,
        "nodes": nodes,
        "animations": [],
    }

    os.makedirs(output_dir, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(document, f, indent=2)
        f.write("\n")

    used_phases = len(set(groups))
    print(f"Wrote {args.output}: {args.count} instances, {args.layout} layout, "
          f"{used_phases} phase(s) spaced {phase_step:g}s over a {period:g}s period")


if __name__ == "__main__":
    main()
