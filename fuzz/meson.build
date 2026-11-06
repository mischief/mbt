executable('fuzz_bencode', 'fuzz_bencode.c', link_with: [libmbt], include_directories: mbt_inc)
executable('fuzz_metainfo', 'fuzz_metainfo.c', link_with: [libmbt], include_directories: mbt_inc)
executable('fuzz_lsd', 'fuzz_lsd.c', link_with: [libmbt], include_directories: mbt_inc)
# Links the objects: mbt_dht_input is not in the shared library.
executable('fuzz_dht', 'fuzz_dht.c', objects: libmbt.extract_all_objects(recursive: false),
           dependencies: deps, include_directories: [mbt_inc, include_directories('..')])
executable('fuzz_magnet', 'fuzz_magnet.c', link_with: [libmbt], include_directories: mbt_inc)
executable('fuzz_torrent', 'fuzz_torrent.c', link_with: [libmbt], include_directories: mbt_inc)
