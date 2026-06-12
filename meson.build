project('mbt', 'c', version: '0.1', default_options: ['c_std=c23', 'warning_level=3'], license: 'ISC')

add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')
add_project_arguments('-Werror', language: 'c')

cc = meson.get_compiler('c')

checkargs = '''
-Wno-gnu-case-range
-Wno-unused-parameter
-Wno-unused-function
-Wno-unused-result
-Wno-gnu-statement-expression-from-macro-expansion
'''.split()
foreach a : checkargs
  if cc.has_argument(a)
    add_project_arguments(a, language: 'c')
  endif
endforeach

deps = []

if host_machine.system() == 'linux'
  libbsd_dep = dependency('libbsd-overlay')
  deps += libbsd_dep
  libmd_dep = dependency('libmd')
  deps += libmd_dep
elif host_machine.system() == 'freebsd'
  deps += cc.find_library('md')
endif

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

# DNS resolver backend
dns_backend = get_option('dns_backend')
if dns_backend == 'cares'
  cares_dep = dependency('libcares')
  deps += cares_dep
  resolve_src = ['resolve_cares.c']
  add_project_arguments('-DHAVE_CARES', language: 'c')
elif dns_backend == 'asr'
  resolve_src = ['resolve_asr.c']
  add_project_arguments('-DHAVE_ASR', language: 'c')
else
  resolve_src = ['resolve_blocking.c']
endif

# Core library
bt_src = [
  'be.c',
  'hex.c',
  'infohash.c',
  'log.c',
  'ratelimit.c',
  'lsd.c',
  'magnet.c',
  'metainfo.c',
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
  'tracker_server_udp.c',
  'tracker_server_http.c',
  'tracker_store.c',
  'pcp.c',
  'net.c',
  'dht.c',
] + ev_src + resolve_src

mbt_inc = include_directories('.', 'mbt')

libmbt = library('mbt', bt_src, dependencies: deps, install: true,
                 include_directories: mbt_inc,
                 c_args: ['-fvisibility=hidden', '-DMBT_BUILDING'],
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
  'mbt/metainfo.h',
  'mbt/pack.h',
  'mbt/peer.h',
  'mbt/peerdb.h',
  'mbt/pcp.h',
  'mbt/picker.h',
  'mbt/session.h',
  'mbt/storage.h',
  'mbt/torrent.h',
  'mbt/timers.h',
  'mbt/tracker.h',
  'mbt/tracker_server.h',
  subdir: 'mbt',
)

# Tests
if get_option('tests')
  subdir('test')
endif

# Tools
executable('mbt-lsd', 'tools/mbt-lsd.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
executable('mbt-torrent', 'tools/mbt-torrent.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
executable('mbt-dht', 'tools/mbt-dht.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
executable('mbt-tracker', 'tools/mbt-tracker.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)

libcurl_dep = dependency('libcurl', required: false)
if libcurl_dep.found()
  executable('mbt-get', 'tools/mbt-get.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps + [libcurl_dep])
endif

# Fuzz
executable('be_fuzz', 'fuzz/fuzz.c', link_with: [libmbt], include_directories: mbt_inc)
executable('mbt-probe', 'tools/mbt-probe.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
executable('mbt-pcp', 'tools/mbt-pcp.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)

ncurses_dep = dependency('ncurses', required: false)
if ncurses_dep.found() and libcurl_dep.found()
  executable('mbt-tui', 'tools/mbt-tui.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps + [ncurses_dep, libcurl_dep])
endif
