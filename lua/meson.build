# require("mbt"): BitTorrent sessions from Lua. See README.md here.
lua_dep_name = which_lua
lua_bin_name = which_lua
if host_machine.system() == 'openbsd'
  lua_dep_name = which_lua.replace('.', '')
  lua_bin_name = lua_dep_name
elif host_machine.system() == 'freebsd'
  lua_bin_name = which_lua.replace('.', '')
  lua_dep_name = which_lua.replace('lua', 'lua-')
endif

lua_dep = dependency(lua_dep_name)

lua_cmod_dir = get_option('lua-module-directory')
if lua_cmod_dir == ''
  lua_cmod_dir = get_option('libdir') / 'lua' / which_lua.replace('lua', '')
endif

# The installed module finds libmbt in a non-system prefix.
mbt_lua_rpath = get_option('prefix') == '/usr' ? '' : get_option('prefix') / get_option('libdir')

mbt_lua = shared_module('mbt', 'mbt_lua.c',
  name_prefix: '',
  dependencies: [lua_dep],
  link_with: [libmbt],
  include_directories: mbt_inc,
  install: true,
  install_dir: lua_cmod_dir,
  install_rpath: mbt_lua_rpath,
)

lua_bin = find_program(lua_bin_name, required: false)
if lua_bin.found() and get_option('tests')
  # The built module comes first, before any installed one.
  test('lua', lua_bin, depends: mbt_lua, args: files('mbt_test.lua'),
    env: {'LUA_CPATH': meson.current_build_dir() / '?.so;;'},
    timeout: 60)
  test('lua_mse', lua_bin, depends: mbt_lua, args: files('mse_test.lua'),
    env: {'LUA_CPATH': meson.current_build_dir() / '?.so;;'},
    timeout: 60)
endif
