#!/usr/bin/env python3
"""Generate a subdivided icosphere OBJ file for testing the Nanite demo."""

import math
import sys

def normalize(v):
    length = math.sqrt(v[0]**2 + v[1]**2 + v[2]**2)
    return (v[0]/length, v[1]/length, v[2]/length)

def midpoint(v1, v2):
    return normalize(((v1[0]+v2[0])/2, (v1[1]+v2[1])/2, (v1[2]+v2[2])/2))

def generate_icosphere(subdivisions=4):
    """Generate an icosphere with the given number of subdivisions."""
    # Golden ratio
    t = (1.0 + math.sqrt(5.0)) / 2.0

    # Initial icosahedron vertices
    vertices = [
        normalize((-1,  t,  0)),
        normalize(( 1,  t,  0)),
        normalize((-1, -t,  0)),
        normalize(( 1, -t,  0)),
        normalize(( 0, -1,  t)),
        normalize(( 0,  1,  t)),
        normalize(( 0, -1, -t)),
        normalize(( 0,  1, -t)),
        normalize(( t,  0, -1)),
        normalize(( t,  0,  1)),
        normalize((-t,  0, -1)),
        normalize((-t,  0,  1)),
    ]

    # Initial icosahedron faces (0-indexed)
    faces = [
        (0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11),
        (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
        (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9),
        (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1),
    ]

    # Subdivide
    midpoint_cache = {}

    def get_midpoint(i1, i2):
        key = (min(i1, i2), max(i1, i2))
        if key in midpoint_cache:
            return midpoint_cache[key]
        v1 = vertices[i1]
        v2 = vertices[i2]
        mid = midpoint(v1, v2)
        idx = len(vertices)
        vertices.append(mid)
        midpoint_cache[key] = idx
        return idx

    for _ in range(subdivisions):
        new_faces = []
        midpoint_cache = {}
        for tri in faces:
            a, b, c = tri
            ab = get_midpoint(a, b)
            bc = get_midpoint(b, c)
            ca = get_midpoint(c, a)
            new_faces.extend([
                (a, ab, ca),
                (b, bc, ab),
                (c, ca, bc),
                (ab, bc, ca),
            ])
        faces = new_faces

    return vertices, faces

def main():
    subdivisions = int(sys.argv[1]) if len(sys.argv) > 1 else 4
    output = sys.argv[2] if len(sys.argv) > 2 else "sphere.obj"

    vertices, faces = generate_icosphere(subdivisions)

    print(f"Generating icosphere: {len(vertices)} vertices, {len(faces)} triangles")
    print(f"Writing to: {output}")

    with open(output, 'w') as f:
        f.write(f"# Icosphere with {subdivisions} subdivisions\n")
        f.write(f"# {len(vertices)} vertices, {len(faces)} triangles\n\n")

        # Vertices and normals (for a unit sphere, position = normal)
        for v in vertices:
            f.write(f"v {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        f.write("\n")
        for v in vertices:
            f.write(f"vn {v[0]:.6f} {v[1]:.6f} {v[2]:.6f}\n")
        f.write("\n")

        # Faces (1-indexed)
        for tri in faces:
            a, b, c = tri[0]+1, tri[1]+1, tri[2]+1
            f.write(f"f {a}//{a} {b}//{b} {c}//{c}\n")

    print("Done!")

if __name__ == "__main__":
    main()
