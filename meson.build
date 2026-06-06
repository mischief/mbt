project('mbt', 'c', version: '0.1', default_options: ['c_std=c23', 'warning_level=3'])

add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')
add_project_arguments('-Werror', language: 'c')

cc = meson.get_compiler('c')

checkargs = '''
-Wno-gnu-case-range
-Wno-unused-parameter
-Wno-unused-function
-Wno-gnu-statement-expression-from-macro-expansion
'''.split()
foreach a : checkargs
  if cc.has_argument(a)
    add_project_arguments(a, language: 'c')
  endif
endforeach

deps = []

libsystemd_dep = dependency('libsystemd')
deps += libsystemd_dep

libbsd_dep = dependency('libbsd-overlay')
deps += libbsd_dep

libmd_dep = dependency('libmd')
deps += libmd_dep

# Core library
bt_src = [
  'be.c',
  'bt.c',
  'hex.c',
  'infohash.c',
  'lsd.c',
  'node.c',
  'remote.c',
  'sdbuf.c',
  'tbmap.c',
]

libmbt = library('mbt', bt_src, dependencies: deps, install: true)

# Tests
tbmap_test = executable('tbmap_test', 'test/tbmap_test.c', link_with: [libmbt])
test('tbmap', tbmap_test)

be_test = executable('be_test', 'test/test.c', link_with: [libmbt])
test('be', be_test)

be_test_file = executable('be_test_file', 'test/test_file.c', link_with: [libmbt])

tfiles = [
  'openbsd76.torrent',
  'debian-12.11.0-amd64-netinst.iso.torrent',
  'ubuntu-24.04.2-desktop-amd64.iso.torrent',
]

foreach t : tfiles
  test('be_test_file ' + t, be_test_file,
    args: [meson.current_source_dir() / 'test' / 'testdata' / t])
endforeach

bt_test = executable('bt_test', 'test/bt_test.c', link_with: [libmbt], dependencies: deps)

# Tools
executable('mbt-lsd', 'tools/mbt-lsd.c', link_with: [libmbt], dependencies: deps)

# Fuzz
executable('be_fuzz', 'fuzz/fuzz.c', link_with: [libmbt])
