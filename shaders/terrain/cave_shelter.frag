#version 450

/* The caves in the shelter map (renderer.c: record_shelter; cave.vert built
 * with CAVE_SHELTER): depth only, so the rain (composite.frag) falls in no
 * cave. Cut off where the mesh stands over the ground at a mouth, as
 * cave.frag cuts it (by its vertices' heights over the ground here: the
 * shelter map is coarser than the cut), so the field's air poking out of a
 * hillside doesn't shelter the slope in front of the mouth. */
layout(location = 6) in float vAbove;

void main() {
    if (vAbove > 0.0) discard;
}
