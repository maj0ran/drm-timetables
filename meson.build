project('drm-test', 'c',
  version : '0.1',
  default_options : ['warning_level=3'])

libdrm = dependency('libdrm')
cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)
exe = executable('drm_test', 'drm_test.c',
  dependencies : [libdrm, m_dep],
  install : true)

test('basic', exe)
