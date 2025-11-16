opendslm_daemon = executable('opendslm-daemon',
                             files('opendslm_daemon.cpp', '../daemon/camera_daemon.cpp'),
                             include_directories : include_directories('..'),
                             dependencies : [libcamera_dep, boost_dep, threads_dep, jpeg_dep],
                             link_with : rpicam_app,
                             install : false)

alias_target('opendslm-daemon', opendslm_daemon)
