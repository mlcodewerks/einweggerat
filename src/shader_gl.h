#pragma once
// Returns an FBO with an output-sized image, or zero for ordinary presentation.
unsigned shader_gl_render(unsigned source_fbo, unsigned input_width, unsigned input_height,
    unsigned output_width, unsigned output_height);
void shader_gl_destroy();
