namespace render::shaders {
inline constexpr char const *default_wgsl{R"fafac6093d01969a(struct vertex_input {
  @location(0) position: vec2f,
  //@location(1) color: vec3f,
};

@vertex
fn vs_main(in: vertex_input) -> @builtin(position) vec4f {
  return vec4f(in.position, 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4f {
  return vec4f(1.0, 1.0, 0.0, 1.0);
}
)fafac6093d01969a"};}