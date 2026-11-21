# Dependency provenance

All bundled dependencies were checked against upstream on 19 November 2026.
These pinned source subsets build offline and require no parent checkout.

| Component | Upstream revision | Notes |
| --- | --- | --- |
| Dear ImGui core and Win32/OpenGL/Vulkan backends | [`77fe4588e6af934b4167b91312174ca3fbb0d71a`](https://github.com/ocornut/imgui/commit/77fe4588e6af934b4167b91312174ca3fbb0d71a) | 1.93.1 WIP |
| ImGuiColorTextEdit | [`9f812a0b772e6c60c86c295a78a9fcdb2df6d59a`](https://github.com/goossens/ImGuiColorTextEdit/commit/9f812a0b772e6c60c86c295a78a9fcdb2df6d59a) |  |
| Volk | [`051977aa2412e8a1b3f5362c975bba99c4728703`](https://github.com/zeux/volk/commit/051977aa2412e8a1b3f5362c975bba99c4728703) | Vulkan 1.4.368 |
| Vulkan headers | [`b26454f3ccd67e704117a39295fa13935fd3804c`](https://github.com/KhronosGroup/Vulkan-Headers/commit/b26454f3ccd67e704117a39295fa13935fd3804c) | 1.4.368; Win32 C headers and video declarations |
| GLAD generator | [`0a7cf1af9e25e02821587bd4095807fc90b6426a`](https://github.com/Dav1dde/glad/commit/0a7cf1af9e25e02821587bd4095807fc90b6426a) | 2.0.8; OpenGL 4.6 compatibility, no extensions, application-supplied loader |
| Libretro headers/DSP | [`52781c4a8b5aae19d858f4a2326c8f8308c70f54`](https://github.com/libretro/libretro-common/commit/52781c4a8b5aae19d858f4a2326c8f8308c70f54) | with einweggerat DSP adaptations |
| SpessaSynth C, including embedded JSON | [`625d1a152bdf610a2fb29844cb39b6ade0399a83`](https://github.com/kode54/spessasynth_core_c/commit/625d1a152bdf610a2fb29844cb39b6ade0399a83) |  |
| Librashader C header | [`07ac790cdb9826f37cf8e1daf5011f03e3c4daa7`](https://github.com/SnowflakePowered/librashader/commit/07ac790cdb9826f37cf8e1daf5011f03e3c4daa7) |  |
| Roboto Regular | [`38062f4b4a0be4346d07a928408da21602545e9e`](https://github.com/googlefonts/roboto/commit/38062f4b4a0be4346d07a928408da21602545e9e) |  |
| Fork Awesome font | [`28e69719d9e5a0840d6d90b50898bcc2761531ff`](https://github.com/ForkAwesome/Fork-Awesome/commit/28e69719d9e5a0840d6d90b50898bcc2761531ff) |  |
| Fork Awesome icon definitions | [`210b5a399a64270674560d633638952d1e8d804d`](https://github.com/juliettef/IconFontCppHeaders/commit/210b5a399a64270674560d633638952d1e8d804d) |  |

Local integration notes:

- ImGui's OpenGL backend shares the frontend's GLAD loader; its Vulkan backend
  uses Volk. All three backends now come from the same ImGui revision, including
  the upstream Win32 process DPI awareness fix. No SDL or Direct3D backend is bundled.
- GLAD 2 replaces the generated 0.1.36 loader. The frontend supplies its existing
  WGL resolver through the GLAD 2 callback API. OpenGL and Vulkan remain loaded
  at runtime without loader DLL imports. Generated includes retain the flat
  `graphics/` layout. Generate with the pinned GLAD tree and
  `python -m glad --api gl:compatibility=4.6 --extensions= --reproducible --out-path OUTPUT c`.
  The generator uses its bundled Khronos registry; generated sources are bundled,
  so Python generator dependencies are unnecessary for normal builds.
- ImGuiColorTextEdit includes its language registry header and language definitions.
  Shader editors select GLSL through `TextEditor::LookupLanguage("GLSL")`.
- Libretro DSP keeps the frontend's filter fixes, additional filters and presets.
  Files absent from upstream are local additions.
- SpessaSynth, its embedded JSON library, Librashader and Roboto already matched
  the pinned upstream snapshots. Fork Awesome's embedded font and matching icon
  definitions were verified against the pinned upstream sources.

See `../THIRD_PARTY.md` for license locations.

Verification on 19 November 2026: clean release build in `build-updated/`,
all 18 Meson tests passed, real Librashader OpenGL pixel/compiler checks passed,
and the Vulkan shader run verified all 120 frames.
