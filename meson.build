project('fuzzydev', 'c',
	version : '1',
	license : 'MIT/Expat',
	default_options : [ 'c_std=gnu99', 'warning_level=2' ])

cflags = ['-Wno-unused-parameter', '-Wmissing-prototypes', '-Wstrict-prototypes', '-D_GNU_SOURCE=1']
add_project_arguments(cflags, language : 'c')

dep_libinput = dependency('libinput')
dep_evemu = dependency('evemu')
dep_libudev = dependency('libudev')
dep_libevdev = dependency('libevdev')

fuzzy_src = files('fuzzydevice.c')
fuzzy_deps = [
	dep_libinput,
	dep_evemu,
	dep_libudev,
	dep_libevdev,
]
fuzzydevice = executable('fuzzydevice',
			 fuzzy_src,
			 dependencies: fuzzy_deps,
			 install: false)
