project('openDSLM', 'c', 'cpp',
        meson_version : '>= 0.64.0',
        version : '0.2.0',
        default_options : [
            'werror=true',
            'warning_level=3',
            'cpp_std=c++17',
            'c_std=c11',
            'buildtype=release',
        ],
        license : 'BSD-2-Clause')

fs = import('fs')

cpp_arguments = ['-pedantic', '-Wno-unused-parameter', '-faligned-new']

cpp_arguments += '-D_FILE_OFFSET_BITS=64'

metadata_conf = configuration_data()
metadata_conf.set_quoted('ODS_DEFAULT_MAKE', get_option('metadata_make'))
metadata_conf.set_quoted('ODS_DEFAULT_MODEL_PREFIX', get_option('metadata_model_prefix'))
metadata_conf.set_quoted('ODS_DEFAULT_SOFTWARE',
    '@0@ (v@1@)'.format(get_option('metadata_software_name'), meson.project_version()))
configure_file(
    input : 'metadata_config.hpp.in',
    output : 'metadata_config.hpp',
    configuration : metadata_conf)

cxx = meson.get_compiler('cpp')
cpu = host_machine.cpu()
neon = get_option('neon_flags')

if cxx.get_id() == 'gcc'
    cpp_arguments += '-Wno-psabi'
endif

if cpu == 'aarch64' or neon == 'arm64'
    cpp_arguments += '-ftree-vectorize'
elif neon == 'armv8-neon'
    cpp_arguments += ['-mfpu=neon-fp-armv8', '-ftree-vectorize']
endif

dl_dep = dependency('dl', required : true)
threads_dep = dependency('threads', required : true)
libcamera_dep = dependency('libcamera', required : true)

rpicam_app_src = []
rpicam_app_dep = [libcamera_dep, dl_dep]

subdir('core')
subdir('encoder')
subdir('image')
subdir('output')
subdir('preview')
subdir('utils')

add_project_arguments(cpp_arguments, language : 'cpp')

subdir('post_processing_stages')

version_cmd = [meson.project_source_root() / 'utils' / 'version.py', meson.project_version()]

dist_version_file = meson.project_source_root() / 'version.gen'
if fs.is_file(dist_version_file)
    version_cmd += fs.read(dist_version_file)
endif

version_cpp = vcs_tag(command : version_cmd,
                      replace_string: '@VER@',
                      input : meson.project_source_root() / 'core' / 'version.cpp.in',
                      output : 'version.cpp',
                      fallback : meson.project_version())

rpicam_app_src += version_cpp

rpicam_app = library(
    'rpicam_app',
    rpicam_app_src,
    soversion : meson.project_version(),
    include_directories : include_directories('.'),
    install : false,
    name_prefix : '',
    dependencies : rpicam_app_dep,
)

subdir('apps')
