# Third-party notices

The Unlicense covers einweggerat's original frontend code and original adaptations.
It does not replace the licenses or copyright notices in `third_party/`.
Keep these notices when redistributing source or binaries.

| Component | License / notice location |
| --- | --- |
| Dear ImGui and Win32/OpenGL/Vulkan backends | MIT; `third_party/imgui/LICENSE.txt` |
| ImGuiColorTextEdit shader source editor | MIT; `third_party/ImGuiColorTextEdit/LICENSE` |
| Roboto Regular | Apache-2.0; `third_party/fonts/Roboto-LICENSE.txt` |
| Fork Awesome icon font and icon definitions | Font license and notices in `third_party/fonts/ForkAwesome-LICENSES.txt`; retained attribution in `IconsForkAwesome.h` |
| SpessaSynth C | Apache-2.0; `third_party/spessasynth/LICENSE` and its README copyright notices |
| SpessaSynth's embedded JSON library | Notices in `third_party/spessasynth/spessasynth_core/extern/json/` |
| Libretro API headers | MIT; notices in `third_party/libretro/libretro.h` and `libretro_vulkan.h` |
| Libretro DSP filters and FFT support | Per-file notices in `third_party/libretro-dsp/audio/dsp_filters/` |
| Libretro DSP support headers | Per-file notices in `third_party/libretro-dsp/include/` |
| Volk | MIT; full notice at the end of `third_party/graphics/volk.h` |
| Vulkan headers (Win32 subset and video declarations) | Apache-2.0 OR MIT; `third_party/vulkan-headers/LICENSE`, full Apache/MIT license texts and per-file notices |
| GLAD generated loader / Khronos declarations | `third_party/graphics/GLAD-LICENSE.txt` and generated source/Khronos notices in `third_party/graphics/` |
| Librashader C header | MIT; notice in `third_party/librashader/librashader.h` |

The optional librashader DLL has its own accompanying notices and dependency
licenses. A user's cores, firmware, games and SoundFont are separate components;
they are not supplied by this project.
