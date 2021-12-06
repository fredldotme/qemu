
#version 100

uniform sampler2D image;
varying  mediump vec2 ex_tex_coord;

void main(void) {
     gl_FragColor = texture2D(image, ex_tex_coord);
}
