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

libbsd_dep = dependency('libbsd-overlay')
deps += libbsd_dep

libmd_dep = dependency('libmd')
deps += libmd_dep

# Event backend (one or the other)
event_backend = get_option('event_backend')

if event_backend == 'sd-event'
  libsystemd_dep = dependency('libsystemd')
  deps += libsystemd_dep
  ev_src = ['ev_sd.c', 'conn_sd.c', 'sdbuf.c']
  add_project_arguments('-DHAVE_SD_EVENT', language: 'c')
elif event_backend == 'libevent'
  libevent_dep = dependency('libevent')
  deps += libevent_dep
  ev_src = ['ev_libevent.c', 'conn_libevent.c']
  add_project_arguments('-DHAVE_LIBEVENT', language: 'c')
endif

# Core library
bt_src = [
  'be.c',
  'bt_legacy.c',
  'hex.c',
  'infohash.c',
  'log.c',
  'lsd.c',
  'magnet.c',
  'node.c',
  'pack.c',
  'peer.c',
  'picker.c',
  'remote.c',
  'session.c',
  'storage_file.c',
  'bitmap.c',
  'torrent.c',
  'tracker_udp.c',
] + ev_src

libmbt = library('mbt', bt_src, dependencies: deps, install: true)

# Tests
tbmap_test = executable('tbmap_test', 'test/bitmap_test.c', link_with: [libmbt])
test('tbmap', tbmap_test)

be_test = executable('be_test', 'test/test.c', link_with: [libmbt])
test('be', be_test)

pack_test = executable('pack_test', 'test/pack_test.c', link_with: [libmbt])
test('pack', pack_test)

hex_test = executable('hex_test', 'test/hex_test.c', link_with: [libmbt])
test('hex', hex_test)

infohash_test = executable('infohash_test', 'test/infohash_test.c', link_with: [libmbt])
test('infohash', infohash_test)

magnet_test = executable('magnet_test', 'test/magnet_test.c', link_with: [libmbt])
test('magnet', magnet_test)

tracker_test = executable('tracker_test', 'test/tracker_test.c', link_with: [libmbt], dependencies: deps)

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

# New session-based test
session_test = executable('session_test', 'test/session_test.c', link_with: [libmbt], dependencies: deps)

# Event loop test
ev_test = executable('ev_test', 'test/ev_test.c', link_with: [libmbt], dependencies: deps)
test('ev', ev_test)

# Tools
executable('mbt-lsd', 'tools/mbt-lsd.c', link_with: [libmbt], dependencies: deps)

# Fuzz
executable('be_fuzz', 'fuzz/fuzz.c', link_with: [libmbt])
