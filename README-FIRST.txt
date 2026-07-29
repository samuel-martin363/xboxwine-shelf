XboxWine v0.2.7 — Official SDL2 UWP Fix

Copy every item in this ZIP over the root of your xboxwine-shelf repository.

This version:
- Restores the complete Shelf implementation.
- Builds official SDL2 for UWP and uses its Direct3D 11 renderer.
- Removes the custom SDL OpenGL binary that crashes on Xbox.
- Forces the package version to 0.2.7.0 even when a cached manifest is older.
- Uses a fresh build cache key.
- Produces an artifact named XboxWine-Shelf-v0.2.7-x64.
- Includes THIS-IS-XBOXWINE-v0.2.7.txt in the artifact.
