tbmap_test = executable('tbmap_test', 'bitmap_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('tbmap', tbmap_test)

be_test = executable('be_test', 'test.c', link_with: [libmbt], include_directories: mbt_inc)
test('be', be_test)

pack_test = executable('pack_test', 'pack_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('pack', pack_test)

hex_test = executable('hex_test', 'hex_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('hex', hex_test)

infohash_test = executable('infohash_test', 'infohash_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('infohash', infohash_test)

magnet_test = executable('magnet_test', 'magnet_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('magnet', magnet_test)

picker_test = executable('picker_test', 'picker_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('picker', picker_test)

storage_test = executable('storage_test', 'storage_test.c', link_with: [libmbt], include_directories: mbt_inc)
test('storage', storage_test)

be_test_file = executable('be_test_file', 'test_file.c', link_with: [libmbt], include_directories: mbt_inc)
foreach tf : ['openbsd76.torrent', 'debian-12.11.0-amd64-netinst.iso.torrent', 'ubuntu-24.04.2-desktop-amd64.iso.torrent']
  test('be_test_file ' + tf, be_test_file, args: [meson.project_source_root() / 'fuzz' / 'corpus' / tf])
endforeach

ev_test = executable('ev_test', 'ev_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('ev', ev_test)

header_test = executable('header_test', 'header_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('header', header_test)

resume_test = executable('resume_test', 'resume_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('resume', resume_test)

e2e_test = executable('e2e_test', 'e2e_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('e2e', e2e_test, timeout: 15)

storage_file_test = executable('storage_file_test', 'storage_file_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('storage_file', storage_file_test)

peerdb_test = executable('peerdb_test', 'peerdb_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('peerdb', peerdb_test)

toggle_test = executable('toggle_test', 'toggle_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('toggle', toggle_test)

dht_test = executable('dht_test', 'dht_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('dht', dht_test, timeout: 10)

tracker_server_test = executable('tracker_server_test', 'tracker_server_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('tracker_server', tracker_server_test, timeout: 15)

metainfo_test = executable('metainfo_test', 'metainfo_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('metainfo', metainfo_test)

magnet_flow_test = executable('magnet_flow_test', 'magnet_flow_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('magnet_flow', magnet_flow_test, timeout: 15)

multi_peer_test = executable('multi_peer_test', 'multi_peer_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('multi_peer', multi_peer_test, timeout: 15)

choke_test = executable('choke_test', 'choke_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('choke', choke_test, timeout: 20)

lsd_test = executable('lsd_test', 'lsd_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('lsd', lsd_test, timeout: 5)

partial_resume_test = executable('partial_resume_test', 'partial_resume_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('partial_resume', partial_resume_test, timeout: 15)

picker_full_test = executable('picker_full_test', 'picker_full_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('picker_full', picker_full_test, timeout: 15)

tracker_shared_test = executable('tracker_shared_test', 'tracker_shared_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('tracker_shared', tracker_shared_test, timeout: 15)

pcp_test = executable('pcp_test', 'pcp_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('pcp', pcp_test, timeout: 10)

rarest_test = executable('rarest_test', 'rarest_test.c', link_with: [libmbt], include_directories: mbt_inc, dependencies: deps)
test('rarest', rarest_test, timeout: 20)
