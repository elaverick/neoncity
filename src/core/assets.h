#pragma once

/* Authored assets (assets/: materials, textures, models) are files read at
 * run time, never data compiled into the code (docs/code-structure-and-
 * terrain-guidelines.md, "Assets"). The build copies assets/ beside the
 * executable. Fills roots with the directories to look for it in: beside the
 * executable, then the working directory. Returns how many. */
int asset_roots(char roots[2][1024]);

/* The data the world is made from (data/: the city list, data/earth) is read
 * at run time too; the build copies what the game reads beside the
 * executable. Fills roots with the directories to look for a path under
 * data/ in, each ending in a separator or empty: beside the executable, the
 * working directory and its parent (the source tree, seen from build/).
 * Returns how many. */
int data_roots(char roots[3][1024]);
