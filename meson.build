project('drm-timetables', 'c', version
        : '0.1', default_options
        : ['warning_level=3'])

add_global_arguments('-Wno-format', language : 'c')

cc = meson.get_compiler('c')

libdrm_dep = dependency('libdrm') 
m_dep = cc.find_library('m', required : true)

src = [ 'main.c', 'src/draw.c', 'src/utils.c', 'src/drm_helper.c' ] 
incdir = ['include']

exe = executable('drm_timetables', sources : src, 
                 include_directories : incdir, 
                 dependencies : [ libdrm_dep, m_dep ], 
                 install : true)
