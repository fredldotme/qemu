
#version 100

uniform sampler2D image;
attribute  mediump vec2 ex_tex_coord;
varying mediump vec4 out_frag_color;

void main(void) {
     out_frag_color = texture(image, ex_tex_coord);
}
