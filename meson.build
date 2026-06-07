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
  'hex.c',
  'infohash.c',
  'log.c',
  'lsd.c',
  'magnet.c',
  'pack.c',
  'peer.c',
  'peerdb.c',
  'picker.c',
  'session.c',
  'storage_file.c',
  'storage_memory.c',
  'bitmap.c',
  'torrent.c',
  'tracker_http.c',
  'tracker_udp.c',
  'dht.c',
] + ev_src

mbt_inc = include_directories('.', 'mbt')

libmbt = library('mbt', bt_src, dependencies: deps, install: true,
                 include_directories: mbt_inc,
                 link_args: ['-Wl,--version-script=' + meson.current_source_dir() / 'libmbt.map'],
                 link_depends: files('libmbt.map'))

install_headers(
  'mbt/mbt.h',
  'mbt/be.h',
  'mbt/bitmap.h',
  'mbt/conn.h',
  'mbt/dht.h',
  'mbt/event.h',
  'mbt/hex.h',
  'mbt/infohash.h',
  'mbt/log.h',
  'mbt/lsd.h',
  'mbt/magnet.h',
  'mbt/pack.h',
  'mbt/peer.h',
  'mbt/peerdb.h',
  'mbt/picker.h',
  'mbt/session.h',
  'mbt/storage.h',
  'mbt/torrent.h',
  'mbt/tracker.h',
  subdir: 'mbt',
)

# Tests
tbmap_test = executable('tbmap_test', 'test/bitmap_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('tbmap', tbmap_test)

be_test = executable('be_test', 'test/test.c', link_with: [libmbt], include_directories: mbt_inc)
test('be', be_test)

pack_test = executable('pack_test', 'test/pack_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('pack', pack_test)

hex_test = executable('hex_test', 'test/hex_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('hex', hex_test)

infohash_test = executable('infohash_test', 'test/infohash_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('infohash', infohash_test)

magnet_test = executable('magnet_test', 'test/magnet_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('magnet', magnet_test)

tracker_test = executable('tracker_test', 'test/tracker_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)

picker_test = executable('picker_test', 'test/picker_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('picker', picker_test)

storage_test = executable('storage_test', 'test/storage_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('storage', storage_test)

be_test_file = executable('be_test_file', 'test/test_file.c', link_with: [libmbt], include_directories: mbt_inc)

tfiles = [
  'openbsd76.torrent',
  'debian-12.11.0-amd64-netinst.iso.torrent',
  'ubuntu-24.04.2-desktop-amd64.iso.torrent',
]

foreach t : tfiles
  test('be_test_file ' + t, be_test_file,
    args: [meson.current_source_dir() / 'test' / 'testdata' / t])
endforeach

# New session-based test
session_test = executable('session_test', 'test/session_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)

# Event loop test
ev_test = executable('ev_test', 'test/ev_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('ev', ev_test)

# Tools
executable('mbt-lsd', 'tools/mbt-lsd.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
executable('mbt-torrent', 'tools/mbt-torrent.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
executable('mbt-dht', 'tools/mbt-dht.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)

libcurl_dep = dependency('libcurl', required: false)
if libcurl_dep.found()
  executable('mbt-get', 'tools/mbt-get.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps + [libcurl_dep], include_directories: mbt_inc)
endif
header_test = executable('header_test', 'test/header_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('header', header_test)

resume_test = executable('resume_test', 'test/resume_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('resume', resume_test)

e2e_test = executable('e2e_test', 'test/e2e_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('e2e', e2e_test, timeout: 15)

storage_file_test = executable('storage_file_test', 'test/storage_file_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('storage_file', storage_file_test)

peerdb_test = executable('peerdb_test', 'test/peerdb_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('peerdb', peerdb_test)

toggle_test = executable('toggle_test', 'test/toggle_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('toggle', toggle_test)

dht_test = executable('dht_test', 'test/dht_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('dht', dht_test, timeout: 10)

# Fuzz
executable('be_fuzz', 'fuzz/fuzz.c', link_with: [libmbt], include_directories: mbt_inc)
