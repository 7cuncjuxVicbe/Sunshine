#include "graphics.h"
#include "sunshine/video.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <fcntl.h>

// I want to have as little build dependencies as possible
// There aren't that many DRM_FORMAT I need to use, so define them here
//
// They aren't likely to change any time soon.
#define fourcc_code(a, b, c, d) ((std::uint32_t)(a) | ((std::uint32_t)(b) << 8) | \
                                 ((std::uint32_t)(c) << 16) | ((std::uint32_t)(d) << 24))
#define DRM_FORMAT_R8 fourcc_code('R', '8', ' ', ' ')       /* [7:0] R */
#define DRM_FORMAT_GR88 fourcc_code('G', 'R', '8', '8')     /* [15:0] G:R 8:8 little endian */
#define DRM_FORMAT_ARGB8888 fourcc_code('A', 'R', '2', '4') /* [31:0] A:R:G:B 8:8:8:8 little endian */
#define DRM_FORMAT_XRGB8888 fourcc_code('X', 'R', '2', '4') /* [31:0] x:R:G:B 8:8:8:8 little endian */
#define DRM_FORMAT_XBGR8888 fourcc_code('X', 'B', '2', '4') /* [31:0] x:B:G:R 8:8:8:8 little endian */


#define SUNSHINE_SHADERS_DIR SUNSHINE_ASSETS_DIR "/shaders/opengl"

using namespace std::literals;
namespace gl {
GladGLContext ctx;

void drain_errors(const std::string_view &prefix) {
  GLenum err;
  while((err = ctx.GetError()) != GL_NO_ERROR) {
    BOOST_LOG(error) << "GL: "sv << prefix << ": ["sv << util::hex(err).to_string_view() << ']';
  }
}

tex_t::~tex_t() {
  if(!size() == 0) {
    ctx.DeleteTextures(size(), begin());
  }
}

tex_t tex_t::make(std::size_t count) {
  tex_t textures { count };

  ctx.GenTextures(textures.size(), textures.begin());

  float color[] = { 0.0f, 0.0f, 0.0f, 1.0f };

  for(auto tex : textures) {
    gl::ctx.BindTexture(GL_TEXTURE_2D, tex);
    gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // x
    gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // y
    gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::ctx.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::ctx.TexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, color);
  }

  return textures;
}

frame_buf_t::~frame_buf_t() {
  if(begin()) {
    ctx.DeleteFramebuffers(size(), begin());
  }
}

frame_buf_t frame_buf_t::make(std::size_t count) {
  frame_buf_t frame_buf { count };

  ctx.GenFramebuffers(frame_buf.size(), frame_buf.begin());

  return frame_buf;
}

std::string shader_t::err_str() {
  int length;
  ctx.GetShaderiv(handle(), GL_INFO_LOG_LENGTH, &length);

  std::string string;
  string.resize(length);

  ctx.GetShaderInfoLog(handle(), length, &length, string.data());

  string.resize(length - 1);

  return string;
}

util::Either<shader_t, std::string> shader_t::compile(const std::string_view &source, GLenum type) {
  shader_t shader;

  auto data    = source.data();
  GLint length = source.length();

  shader._shader.el = ctx.CreateShader(type);
  ctx.ShaderSource(shader.handle(), 1, &data, &length);
  ctx.CompileShader(shader.handle());

  int status = 0;
  ctx.GetShaderiv(shader.handle(), GL_COMPILE_STATUS, &status);

  if(!status) {
    return shader.err_str();
  }

  return shader;
}

GLuint shader_t::handle() const {
  return _shader.el;
}

buffer_t buffer_t::make(util::buffer_t<GLint> &&offsets, const char *block, const std::string_view &data) {
  buffer_t buffer;
  buffer._block   = block;
  buffer._size    = data.size();
  buffer._offsets = std::move(offsets);

  ctx.GenBuffers(1, &buffer._buffer.el);
  ctx.BindBuffer(GL_UNIFORM_BUFFER, buffer.handle());
  ctx.BufferData(GL_UNIFORM_BUFFER, data.size(), (const std::uint8_t *)data.data(), GL_DYNAMIC_DRAW);

  return buffer;
}

GLuint buffer_t::handle() const {
  return _buffer.el;
}

const char *buffer_t::block() const {
  return _block;
}

void buffer_t::update(const std::string_view &view, std::size_t offset) {
  ctx.BindBuffer(GL_UNIFORM_BUFFER, handle());
  ctx.BufferSubData(GL_UNIFORM_BUFFER, offset, view.size(), (const void *)view.data());
}

void buffer_t::update(std::string_view *members, std::size_t count, std::size_t offset) {
  util::buffer_t<std::uint8_t> buffer { _size };

  for(int x = 0; x < count; ++x) {
    auto val = members[x];

    std::copy_n((const std::uint8_t *)val.data(), val.size(), &buffer[_offsets[x]]);
  }

  update(util::view(buffer.begin(), buffer.end()), offset);
}

std::string program_t::err_str() {
  int length;
  ctx.GetProgramiv(handle(), GL_INFO_LOG_LENGTH, &length);

  std::string string;
  string.resize(length);

  ctx.GetShaderInfoLog(handle(), length, &length, string.data());

  string.resize(length - 1);

  return string;
}

util::Either<program_t, std::string> program_t::link(const shader_t &vert, const shader_t &frag) {
  program_t program;

  program._program.el = ctx.CreateProgram();

  ctx.AttachShader(program.handle(), vert.handle());
  ctx.AttachShader(program.handle(), frag.handle());

  // p_handle stores a copy of the program handle, since program will be moved before
  // the fail guard funcion is called.
  auto fg = util::fail_guard([p_handle = program.handle(), &vert, &frag]() {
    ctx.DetachShader(p_handle, vert.handle());
    ctx.DetachShader(p_handle, frag.handle());
  });

  ctx.LinkProgram(program.handle());

  int status = 0;
  ctx.GetProgramiv(program.handle(), GL_LINK_STATUS, &status);

  if(!status) {
    return program.err_str();
  }

  return program;
}

void program_t::bind(const buffer_t &buffer) {
  ctx.UseProgram(handle());
  auto i = ctx.GetUniformBlockIndex(handle(), buffer.block());

  ctx.BindBufferBase(GL_UNIFORM_BUFFER, i, buffer.handle());
}

std::optional<buffer_t> program_t::uniform(const char *block, std::pair<const char *, std::string_view> *members, std::size_t count) {
  auto i = ctx.GetUniformBlockIndex(handle(), block);
  if(i == GL_INVALID_INDEX) {
    BOOST_LOG(error) << "Couldn't find index of ["sv << block << ']';
    return std::nullopt;
  }

  int size;
  ctx.GetActiveUniformBlockiv(handle(), i, GL_UNIFORM_BLOCK_DATA_SIZE, &size);

  bool error_flag = false;

  util::buffer_t<GLint> offsets { count };
  auto indices = (std::uint32_t *)alloca(count * sizeof(std::uint32_t));
  auto names   = (const char **)alloca(count * sizeof(const char *));
  auto names_p = names;

  std::for_each_n(members, count, [names_p](auto &member) mutable {
    *names_p++ = std::get<0>(member);
  });

  std::fill_n(indices, count, GL_INVALID_INDEX);
  ctx.GetUniformIndices(handle(), count, names, indices);

  for(int x = 0; x < count; ++x) {
    if(indices[x] == GL_INVALID_INDEX) {
      error_flag = true;

      BOOST_LOG(error) << "Couldn't find ["sv << block << '.' << members[x].first << ']';
    }
  }

  if(error_flag) {
    return std::nullopt;
  }

  ctx.GetActiveUniformsiv(handle(), count, indices, GL_UNIFORM_OFFSET, offsets.begin());
  util::buffer_t<std::uint8_t> buffer { (std::size_t)size };

  for(int x = 0; x < count; ++x) {
    auto val = std::get<1>(members[x]);

    std::copy_n((const std::uint8_t *)val.data(), val.size(), &buffer[offsets[x]]);
  }

  return buffer_t::make(std::move(offsets), block, std::string_view { (char *)buffer.begin(), buffer.size() });
}

GLuint program_t::handle() const {
  return _program.el;
}

} // namespace gl

namespace gbm {
device_destroy_fn device_destroy;
create_device_fn create_device;

int init() {
  static void *handle { nullptr };
  static bool funcs_loaded = false;

  if(funcs_loaded) return 0;

  if(!handle) {
    handle = dyn::handle({ "libgbm.so.1", "libgbm.so" });
    if(!handle) {
      return -1;
    }
  }

  std::vector<std::tuple<GLADapiproc *, const char *>> funcs {
    { (GLADapiproc *)&device_destroy, "gbm_device_destroy" },
    { (GLADapiproc *)&create_device, "gbm_create_device" },
  };

  if(dyn::load(handle, funcs)) {
    return -1;
  }

  funcs_loaded = true;
  return 0;
}
} // namespace gbm

namespace egl {
constexpr auto EGL_LINUX_DMA_BUF_EXT         = 0x3270;
constexpr auto EGL_LINUX_DRM_FOURCC_EXT      = 0x3271;
constexpr auto EGL_DMA_BUF_PLANE0_FD_EXT     = 0x3272;
constexpr auto EGL_DMA_BUF_PLANE0_OFFSET_EXT = 0x3273;
constexpr auto EGL_DMA_BUF_PLANE0_PITCH_EXT  = 0x3274;

bool fail() {
  return eglGetError() != EGL_SUCCESS;
}

display_t make_display(gbm::gbm_t::pointer gbm) {
  constexpr auto EGL_PLATFORM_GBM_MESA = 0x31D7;

  display_t display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, gbm, nullptr);

  if(fail()) {
    BOOST_LOG(error) << "Couldn't open EGL display: ["sv << util::hex(eglGetError()).to_string_view() << ']';
    return nullptr;
  }

  int major, minor;
  if(!eglInitialize(display.get(), &major, &minor)) {
    BOOST_LOG(error) << "Couldn't initialize EGL display: ["sv << util::hex(eglGetError()).to_string_view() << ']';
    return nullptr;
  }

  const char *extension_st = eglQueryString(display.get(), EGL_EXTENSIONS);
  const char *version      = eglQueryString(display.get(), EGL_VERSION);
  const char *vendor       = eglQueryString(display.get(), EGL_VENDOR);
  const char *apis         = eglQueryString(display.get(), EGL_CLIENT_APIS);

  BOOST_LOG(debug) << "EGL: ["sv << vendor << "]: version ["sv << version << ']';
  BOOST_LOG(debug) << "API's supported: ["sv << apis << ']';

  const char *extensions[] {
    "EGL_KHR_create_context",
    "EGL_KHR_surfaceless_context",
    "EGL_EXT_image_dma_buf_import",
    "EGL_KHR_image_pixmap"
  };

  for(auto ext : extensions) {
    if(!std::strstr(extension_st, ext)) {
      BOOST_LOG(error) << "Missing extension: ["sv << ext << ']';
      return nullptr;
    }
  }

  return display;
}

std::optional<ctx_t> make_ctx(display_t::pointer display) {
  constexpr int conf_attr[] {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE
  };

  int count;
  EGLConfig conf;
  if(!eglChooseConfig(display, conf_attr, &conf, 1, &count)) {
    BOOST_LOG(error) << "Couldn't set config attributes: ["sv << util::hex(eglGetError()).to_string_view() << ']';
    return std::nullopt;
  }

  if(!eglBindAPI(EGL_OPENGL_API)) {
    BOOST_LOG(error) << "Couldn't bind API: ["sv << util::hex(eglGetError()).to_string_view() << ']';
    return std::nullopt;
  }

  constexpr int attr[] {
    EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE
  };

  ctx_t ctx { display, eglCreateContext(display, conf, EGL_NO_CONTEXT, attr) };
  if(fail()) {
    BOOST_LOG(error) << "Couldn't create EGL context: ["sv << util::hex(eglGetError()).to_string_view() << ']';
    return std::nullopt;
  }

  TUPLE_EL_REF(ctx_p, 1, ctx.el);
  if(!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_p)) {
    BOOST_LOG(error) << "Couldn't make current display"sv;
    return std::nullopt;
  }

  if(!gladLoadGLContext(&gl::ctx, eglGetProcAddress)) {
    BOOST_LOG(error) << "Couldn't load OpenGL library"sv;
    return std::nullopt;
  }

  BOOST_LOG(debug) << "GL: vendor: "sv << gl::ctx.GetString(GL_VENDOR);
  BOOST_LOG(debug) << "GL: renderer: "sv << gl::ctx.GetString(GL_RENDERER);
  BOOST_LOG(debug) << "GL: version: "sv << gl::ctx.GetString(GL_VERSION);
  BOOST_LOG(debug) << "GL: shader: "sv << gl::ctx.GetString(GL_SHADING_LANGUAGE_VERSION);

  gl::ctx.PixelStorei(GL_UNPACK_ALIGNMENT, 1);

  return ctx;
}
std::optional<rgb_t> import_source(display_t::pointer egl_display, const surface_descriptor_t &xrgb) {
  EGLAttrib img_attr_planes[13] {
    EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
    EGL_WIDTH, xrgb.width,
    EGL_HEIGHT, xrgb.height,
    EGL_DMA_BUF_PLANE0_FD_EXT, xrgb.fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, xrgb.offset,
    EGL_DMA_BUF_PLANE0_PITCH_EXT, xrgb.pitch,
    EGL_NONE
  };

  rgb_t rgb {
    egl_display,
    eglCreateImage(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, img_attr_planes),
    gl::tex_t::make(1)
  };

  if(!rgb->xrgb8) {
    BOOST_LOG(error) << "Couldn't import RGB Image: "sv << util::hex(eglGetError()).to_string_view();

    return std::nullopt;
  }

  gl::ctx.BindTexture(GL_TEXTURE_2D, rgb->tex[0]);
  gl::ctx.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, rgb->xrgb8);

  gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

  gl_drain_errors;

  return rgb;
}

std::optional<nv12_t> import_target(display_t::pointer egl_display, std::array<file_t, nv12_img_t::num_fds> &&fds, const surface_descriptor_t &r8, const surface_descriptor_t &gr88) {
  int img_attr_planes[2][13] {
    { EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
      EGL_WIDTH, r8.width,
      EGL_HEIGHT, r8.height,
      EGL_DMA_BUF_PLANE0_FD_EXT, r8.fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, r8.offset,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, r8.pitch,
      EGL_NONE },

    { EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
      EGL_WIDTH, gr88.width,
      EGL_HEIGHT, gr88.height,
      EGL_DMA_BUF_PLANE0_FD_EXT, r8.fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, gr88.offset,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, gr88.pitch,
      EGL_NONE },
  };

  nv12_t nv12 {
    egl_display,
    eglCreateImageKHR(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, img_attr_planes[0]),
    eglCreateImageKHR(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, img_attr_planes[1]),
    gl::tex_t::make(2),
    gl::frame_buf_t::make(2),
    std::move(fds)
  };

  if(!nv12->r8 || !nv12->bg88) {
    BOOST_LOG(error) << "Couldn't create KHR Image"sv;

    return std::nullopt;
  }

  gl::ctx.BindTexture(GL_TEXTURE_2D, nv12->tex[0]);
  gl::ctx.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, nv12->r8);

  gl::ctx.BindTexture(GL_TEXTURE_2D, nv12->tex[1]);
  gl::ctx.EGLImageTargetTexture2DOES(GL_TEXTURE_2D, nv12->bg88);

  nv12->buf.bind(std::begin(nv12->tex), std::end(nv12->tex));

  gl_drain_errors;

  return nv12;
}

void egl_t::set_colorspace(std::uint32_t colorspace, std::uint32_t color_range) {
  video::color_t *color_p;
  switch(colorspace) {
  case 5: // SWS_CS_SMPTE170M
    color_p = &video::colors[0];
    break;
  case 1: // SWS_CS_ITU709
    color_p = &video::colors[2];
    break;
  case 9: // SWS_CS_BT2020
  default:
    BOOST_LOG(warning) << "Colorspace: ["sv << colorspace << "] not yet supported: switching to default"sv;
    color_p = &video::colors[0];
  };

  if(color_range > 1) {
    // Full range
    ++color_p;
  }

  std::string_view members[] {
    util::view(color_p->color_vec_y),
    util::view(color_p->color_vec_u),
    util::view(color_p->color_vec_v),
    util::view(color_p->range_y),
    util::view(color_p->range_uv),
  };

  color_matrix.update(members, sizeof(members) / sizeof(decltype(members[0])));
}

int egl_t::init(int in_width, int in_height, file_t &&fd) {
  file = std::move(fd);

  if(!gbm::create_device) {
    BOOST_LOG(warning) << "libgbm not initialized"sv;
    return -1;
  }

  gbm.reset(gbm::create_device(file.el));
  if(!gbm) {
    BOOST_LOG(error) << "Couldn't create GBM device: ["sv << util::hex(eglGetError()).to_string_view() << ']';
    return -1;
  }

  display = make_display(gbm.get());
  if(!display) {
    return -1;
  }

  auto ctx_opt = make_ctx(display.get());
  if(!ctx_opt) {
    return -1;
  }

  ctx = std::move(*ctx_opt);

  {
    const char *sources[] {
      SUNSHINE_SHADERS_DIR "/ConvertUV.frag",
      SUNSHINE_SHADERS_DIR "/ConvertUV.vert",
      SUNSHINE_SHADERS_DIR "/ConvertY.frag",
      SUNSHINE_SHADERS_DIR "/Scene.vert",
      SUNSHINE_SHADERS_DIR "/Scene.frag",
    };

    GLenum shader_type[2] {
      GL_FRAGMENT_SHADER,
      GL_VERTEX_SHADER,
    };

    constexpr auto count = sizeof(sources) / sizeof(const char *);

    util::Either<gl::shader_t, std::string> compiled_sources[count];

    bool error_flag = false;
    for(int x = 0; x < count; ++x) {
      auto &compiled_source = compiled_sources[x];

      compiled_source = gl::shader_t::compile(read_file(sources[x]), shader_type[x % 2]);
      gl_drain_errors;

      if(compiled_source.has_right()) {
        BOOST_LOG(error) << sources[x] << ": "sv << compiled_source.right();
        error_flag = true;
      }
    }

    if(error_flag) {
      return -1;
    }

    auto program = gl::program_t::link(compiled_sources[1].left(), compiled_sources[0].left());
    if(program.has_right()) {
      BOOST_LOG(error) << "GL linker: "sv << program.right();
      return -1;
    }

    // UV - shader
    this->program[1] = std::move(program.left());

    program = gl::program_t::link(compiled_sources[3].left(), compiled_sources[2].left());
    if(program.has_right()) {
      BOOST_LOG(error) << "GL linker: "sv << program.right();
      return -1;
    }

    // Y - shader
    this->program[0] = std::move(program.left());
  }

  auto color_p = &video::colors[0];
  std::pair<const char *, std::string_view> members[] {
    std::make_pair("color_vec_y", util::view(color_p->color_vec_y)),
    std::make_pair("color_vec_u", util::view(color_p->color_vec_u)),
    std::make_pair("color_vec_v", util::view(color_p->color_vec_v)),
    std::make_pair("range_y", util::view(color_p->range_y)),
    std::make_pair("range_uv", util::view(color_p->range_uv)),
  };

  auto color_matrix = program[0].uniform("ColorMatrix", members, sizeof(members) / sizeof(decltype(members[0])));
  if(!color_matrix) {
    return -1;
  }

  this->color_matrix = std::move(*color_matrix);

  tex_in = gl::tex_t::make(1);

  this->in_width  = in_width;
  this->in_height = in_height;
  return 0;
}

int egl_t::convert(platf::img_t &img) {
  auto tex = tex_in[0];

  gl::ctx.BindTexture(GL_TEXTURE_2D, tex);
  gl::ctx.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in_width, in_height, GL_BGRA, GL_UNSIGNED_BYTE, img.data);

  GLenum attachments[] {
    GL_COLOR_ATTACHMENT0,
    GL_COLOR_ATTACHMENT1
  };

  for(int x = 0; x < sizeof(attachments) / sizeof(decltype(attachments[0])); ++x) {
    gl::ctx.BindFramebuffer(GL_FRAMEBUFFER, nv12->buf[x]);
    gl::ctx.DrawBuffers(1, &attachments[x]);

    auto status = gl::ctx.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if(status != GL_FRAMEBUFFER_COMPLETE) {
      BOOST_LOG(error) << "Pass "sv << x << ": CheckFramebufferStatus() --> [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    gl::ctx.BindTexture(GL_TEXTURE_2D, tex);

    gl::ctx.UseProgram(program[x].handle());
    program[x].bind(color_matrix);

    gl::ctx.Viewport(offsetX / (x + 1), offsetY / (x + 1), out_width / (x + 1), out_height / (x + 1));
    gl::ctx.DrawArrays(GL_TRIANGLES, 0, 3);
  }

  return 0;
}

int egl_t::_set_frame(AVFrame *frame) {
  this->hwframe.reset(frame);
  this->frame = frame;

  // Ensure aspect ratio is maintained
  auto scalar       = std::fminf(frame->width / (float)in_width, frame->height / (float)in_height);
  auto out_width_f  = in_width * scalar;
  auto out_height_f = in_height * scalar;

  // result is always positive
  auto offsetX_f = (frame->width - out_width_f) / 2;
  auto offsetY_f = (frame->height - out_height_f) / 2;

  out_width  = out_width_f;
  out_height = out_height_f;

  offsetX = offsetX_f;
  offsetY = offsetY_f;

  auto tex = tex_in[0];

  gl::ctx.BindTexture(GL_TEXTURE_2D, tex);
  gl::ctx.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, in_width, in_height);

  auto loc_width_i = gl::ctx.GetUniformLocation(program[1].handle(), "width_i");
  if(loc_width_i < 0) {
    BOOST_LOG(error) << "Couldn't find uniform [width_i]"sv;
    return -1;
  }

  auto width_i = 1.0f / out_width;
  gl::ctx.UseProgram(program[1].handle());
  gl::ctx.Uniform1fv(loc_width_i, 1, &width_i);

  gl_drain_errors;
  return 0;
}

egl_t::~egl_t() {
  if(gl::ctx.GetError) {
    gl_drain_errors;
  }
}
} // namespace egl

void free_frame(AVFrame *frame) {
  av_frame_free(&frame);
}