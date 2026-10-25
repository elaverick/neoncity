#pragma once

/* Authored assets (assets/: materials, textures, models) are files read at
 * run time, never data compiled into the code (docs/code-structure-and-
 * terrain-guidelines.md, "Assets"). The build copies assets/ beside the
 * executable. Fills roots with the directories to look for it in: beside the
 * executable, then the working directory. Returns how many. */
int asset_roots(char roots[2][1024]);
