#pragma once
#include "dsp_chain.h"

class dsp_chain_window {
public:
    bool visible = false;
    void draw(dsp_chain &core, dsp_chain &midi, const std::filesystem::path &directory);
private:
    struct editor {
        bool initialized = false;
        bool dirty = false;
        dsp_settings draft;
        std::string status;
        int add_filter = 0;
    } editors[2];
    int picker_source = -1;
    bool picker_save = false;
    void draw_editor(editor &edit, dsp_chain &chain, const std::filesystem::path &path, int source);
};
