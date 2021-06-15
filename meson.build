project('drm-timetables', 'c',
  version : '0.1',
  default_options : ['warning_level=3'])

libdrm = dependency('libdrm')
cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)

src = [ 'drm_timetables.c', 'draw.c', 'utils.c', 'drm_helper.c']
incdir = ['.']
exe = executable('drm_timetables', sources: src,
  dependencies : [libdrm, m_dep],
  install : true)
