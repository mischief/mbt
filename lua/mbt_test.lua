local mbt = require("mbt")

local tmp = os.tmpname()
os.remove(tmp)
assert(os.execute("mkdir -p " .. tmp .. "/seed " .. tmp .. "/leech " .. tmp .. "/magnet"))

local function cleanup()
	os.execute("rm -rf " .. tmp)
end

local function read_file(path)
	local f = assert(io.open(path, "rb"))
	local s = f:read("a")
	f:close()
	return s
end

local function write_file(path, data)
	local f = assert(io.open(path, "wb"))
	f:write(data)
	f:close()
end

local function raises(fn, want)
	local ok, err = pcall(fn)
	assert(not ok, "expected an error")
	assert(tostring(err):find(want, 1, true), err)
end

-- Bound a wait, so a broken transfer fails and does not hang.
local function deadline(ms, what)
	mbt.spawn(function()
		mbt.sleep(ms)
		io.stderr:write("timeout: ", what, "\n")
		cleanup()
		os.exit(1)
	end)
end

-- magnet
local hex = string.rep("AB", 20)
local m = assert(mbt.magnet("magnet:?xt=urn:btih:" .. hex ..
    "&dn=name&tr=udp%3A%2F%2Fexample.org%3A80"))
assert(m.infohash == hex)
assert(m.name == "name")
assert(m.trackers[1] == "udp://example.org:80")
local r, err = mbt.magnet("not a magnet")
assert(r == nil and type(err) == "string")

-- create and metainfo
local parts = {}
for i = 1, 300000 do
	parts[i] = string.char(i % 251)
end
local payload = table.concat(parts)
local seed_path = tmp .. "/seed/data.bin"
write_file(seed_path, payload)

local with_tracker = assert(mbt.create(seed_path, {piece_length = 16384,
    trackers = {"udp://example.org:80"}, comment = "test"}))
local mi = assert(mbt.metainfo(with_tracker))
assert(mi.name == "data.bin")
assert(mi.length == #payload)
assert(mi.piece_length == 16384)
assert(mi.piece_count == 19)
assert(mi.comment == "test")
assert(mi.trackers[1] == "udp://example.org:80")
assert(mi.files[1].length == #payload)
assert(#mi.infohash == 40)
assert(mbt.metainfo("garbage") == nil)

local torrent = assert(mbt.create(seed_path, {piece_length = 16384}))
local torrent_path = tmp .. "/data.torrent"
write_file(torrent_path, torrent)
mi = assert(mbt.metainfo(torrent))

-- loop misuse
raises(function() mbt.sleep(1) end, "inside mbt.run")
raises(function() mbt.run(function() error("boom") end) end, "boom")
raises(function()
	mbt.run(function() mbt.run(function() end) end)
end, "main chunk")

-- step, log and the close attribute
mbt.step()
mbt.step(1)
mbt.log(false)
raises(function() mbt.log{debug = {"nope"}} end, "bad component")
do
	local closed
	do
		local s <close> = assert(mbt.session{dir = tmp .. "/leech"})
		closed = s
	end
	raises(function() closed:torrents() end, "closed")
end

-- sleeping coroutines run side by side. Timers that expire in one loop
-- iteration have no set order, so the test waits on state, not on delays.
local order = {}
mbt.run(function()
	mbt.spawn(function()
		while #order == 0 do
			mbt.sleep(1)
		end
		order[#order + 1] = "second"
	end)
	mbt.spawn(function()
		mbt.sleep(1)
		order[#order + 1] = "first"
	end)
	while #order < 2 do
		mbt.sleep(1)
	end
end)
assert(table.concat(order, " ") == "first second", table.concat(order, " "))

-- handles
do
	local s = assert(mbt.session{dir = tmp .. "/leech"})
	local t = assert(s:add_infohash(hex))
	assert(t:infohash() == hex)
	assert(t == s:find(hex))
	assert(t == s:find(hex:lower()))
	assert(s:find(string.rep("cd", 20)) == nil)
	assert(#s:torrents() == 1)
	assert(tostring(t):find(hex, 1, true))
	assert(t:state() == "init")
	assert(not t:complete())
	assert(not t:has_metadata())
	assert(#t:files() == 0)
	assert(#t:peers() == 0)
	assert(t:stats().piece_count == 0)
	assert(s:add_infohash(hex) == t)
	assert(s:add_infohash("zz") == nil)
	assert(s:add_torrent_buf("garbage") == nil)
	assert(s:add_torrent_file(tmp .. "/none") == nil)
	raises(function() t:wait() end, "inside mbt.run")
	raises(function() t:wait(-1) end, "negative")
	assert(t:piece_map() == "")
	assert(t:read_piece(0) == nil)
	raises(function() t:read_piece(-1) end, "out of range")

	-- a wait times out and leaves the torrent, and two waits end alone
	mbt.run(function()
		local a, b
		mbt.spawn(function() a = {t:wait(1000000)} end)
		local ok, msg = t:wait(0)
		assert(ok == nil and msg == "timeout")
		ok, msg = t:wait(5)
		assert(ok == nil and msg == "timeout")
		assert(a == nil)
		assert(s:find(hex) == t)
		mbt.spawn(function() b = {t:wait(1000000)} end)
		assert(t:remove())
		mbt.sleep(10)
		assert(a[2] == "torrent removed" and b[2] == "torrent removed")
		t = assert(s:add_infohash(hex))
	end)

	-- a waiter ends when its torrent goes, and when its session closes
	mbt.run(function()
		local got
		mbt.spawn(function() got = {t:wait()} end)
		assert(t:remove())
		mbt.sleep(10)
		assert(got[1] == nil and got[2] == "torrent removed")
	end)
	raises(function() t:state() end, "removed")
	assert(t:infohash() == hex)
	assert(#s:torrents() == 0)

	t = assert(s:add_magnet("magnet:?xt=urn:btih:" .. hex))
	mbt.run(function()
		local got
		mbt.spawn(function() got = {t:wait()} end)
		s:close()
		s:close()
		mbt.sleep(10)
		assert(got[1] == nil and got[2] == "session closed")
	end)
	raises(function() t:state() end, "closed")
	raises(function() s:torrents() end, "closed")
end
collectgarbage()

-- the collector frees a session, also through a callback cycle
do
	local s = assert(mbt.session{dir = tmp .. "/leech"})
	s = nil
	local c
	c = assert(mbt.session{dir = tmp .. "/leech", on_complete = function() return c end})
	c:add_infohash(hex)
	c = nil
end
collectgarbage()
collectgarbage()

-- storage objects
do
	local st = assert(mbt.storage.file(tmp .. "/leech"))
	assert(st:dir() == tmp .. "/leech")
	raises(function() mbt.session{dir = "x", storage = st} end, "not both")
	local s = assert(mbt.session{storage = st})
	raises(function() s:add_infohash(hex, {dir = "x", storage = st}) end, "not both")
	raises(function() mbt.session{storage = "x"} end, "mbt.storage")
	local t = assert(s:add_infohash(hex, {storage = st}))
	assert(t:stats().dir == st:dir())
	assert(t:error() == nil)
	st = nil
	collectgarbage()
	assert(t:stats().dir == tmp .. "/leech")
	s:close()
end

-- transfer over loopback, then a magnet fetch from the same seed
local port = 20000 + (os.time() * 7 + math.floor(os.clock() * 1e6)) % 30000
local seed = assert(mbt.session{dir = tmp .. "/seed", port = port, utp = false})
local leech = assert(mbt.session{dir = tmp .. "/leech", port = port + 1,
    utp = false, picker = "sequential"})

local done = {}
local fetch
fetch = assert(mbt.session{dir = tmp .. "/magnet", port = port + 2, utp = false,
    on_complete = function(t)
	done[#done + 1] = t:infohash()
	assert(t:complete())
	-- a callback can remove its torrent
	t:remove()
end})

mbt.run(function()
	deadline(30000, "transfer")
	local st = assert(seed:add_torrent_buf(torrent))
	assert(st:wait())
	assert(st:complete())
	assert(st:state() == "seeding")
	assert(seed:listen_port() == port)

	local t = assert(leech:add_torrent_file(torrent_path))
	assert(t:name() == "data.bin")
	assert(t:infohash() == mi.infohash)
	assert(#t:trackers() == 0)
	assert(t:files()[1].length == #payload)
	assert(leech:add_torrent_file(torrent_path) == t)
	assert(t:add_peer("127.0.0.1", port))
	assert(t:wait())
	assert(t:complete())
	assert(t:progress() == 1)
	assert(t:stats().bytes_downloaded >= #payload)
	assert(read_file(tmp .. "/leech/data.bin") == payload)
	assert(t:wait(1) == true)
	assert(t:piece_map() == string.rep("1", mi.piece_count))
	assert(t:read_piece(0) == payload:sub(1, mi.piece_length))
	assert(t:read_piece(mi.piece_count - 1) ==
	    payload:sub((mi.piece_count - 1) * mi.piece_length + 1))
	local r, msg, errno = t:read_piece(mi.piece_count)
	assert(r == nil and type(msg) == "string" and errno > 0)

	local f = assert(fetch:add_magnet("magnet:?xt=urn:btih:" .. mi.infohash))
	assert(f:add_peer("127.0.0.1", port))
	assert(f:wait())
	mbt.sleep(50)
	assert(done[1] == mi.infohash)
	assert(read_file(tmp .. "/magnet/data.bin") == payload)
	assert(#fetch:torrents() == 0)
end)

-- a shared storage serves two torrents
do
	local payload2 = payload:reverse()
	write_file(tmp .. "/seed/other.bin", payload2)
	local other = assert(mbt.create(tmp .. "/seed/other.bin", {piece_length = 16384}))
	assert(os.execute("mkdir -p " .. tmp .. "/shared"))
	write_file(tmp .. "/notdir", "x")
	local st = assert(mbt.storage.file(tmp .. "/shared"))
	local s = assert(mbt.session{storage = st, utp = false})

	mbt.run(function()
		deadline(30000, "shared storage")
		local so = assert(seed:add_torrent_buf(other))
		assert(so:wait())
		local a = assert(s:add_torrent_buf(torrent))
		local b = assert(s:add_torrent_buf(other))
		assert(a:add_peer("127.0.0.1", port))
		assert(b:add_peer("127.0.0.1", port))
		assert(a:wait())
		assert(b:wait())
		assert(read_file(tmp .. "/shared/data.bin") == payload)
		assert(read_file(tmp .. "/shared/other.bin") == payload2)
		assert(s:add_torrent_buf(other) == b)
	end)
	s:close()
end

-- a session closes with an HTTP announce pending
do
	local listener = assert(mbt.session{dir = tmp .. "/leech", utp = false})
	local url = "http://127.0.0.1:" .. listener:listen_port() .. "/announce"
	local data = assert(mbt.create(seed_path, {piece_length = 16384, trackers = {url}}))
	local s = assert(mbt.session{dir = tmp .. "/seed", utp = false})
	assert(s:add_torrent_buf(data))
	mbt.run(function() mbt.sleep(20) end)
	s:close()
	local off = assert(mbt.session{dir = tmp .. "/seed", utp = false, http = false})
	assert(off:add_torrent_buf(data))
	off:close()
	listener:close()
end

-- a failed storage stops the torrent and reaches on_error and t:wait
do
	local errs = {}
	local s = assert(mbt.session{dir = tmp .. "/notdir", utp = false,
	    on_error = function(t, msg, errno)
		errs[#errs + 1] = {t = t, msg = msg, errno = errno}
	end})
	mbt.run(function()
		deadline(30000, "failed storage")
		local t = assert(s:add_magnet("magnet:?xt=urn:btih:" .. mi.infohash))
		assert(t:add_peer("127.0.0.1", port))
		local ok, msg = t:wait()
		assert(ok == nil and msg == "torrent failed", msg)
		mbt.sleep(10)
		assert(#errs == 1 and errs[1].t == t and type(errs[1].msg) == "string")
		assert(errs[1].errno > 0)
		assert(t:state() == "error")
		local m, e = t:error()
		assert(m == errs[1].msg and e == errs[1].errno)
		assert(t:stats().error == m)
		assert(({t:wait()})[2] == "torrent failed")
	end)
	s:close()
end

-- a torrent moves between storages, and a magnet holds until it gets storage
do
	assert(os.execute("mkdir -p " .. tmp .. "/move_a " .. tmp .. "/move_b " ..
	    tmp .. "/move_c " .. tmp .. "/hold"))
	write_file(tmp .. "/move_a/data.bin", payload)
	local a = assert(mbt.storage.file(tmp .. "/move_a"))
	local b = assert(mbt.storage.file(tmp .. "/move_b"))
	local c = assert(mbt.storage.file(tmp .. "/move_c"))
	local s = assert(mbt.session{storage = a, utp = false})
	local t = assert(s:add_torrent_buf(torrent))

	raises(function() t:move(b) end, "inside mbt.run")
	mbt.run(function()
		assert(t:wait())
		assert(t:move(b))
		assert(read_file(tmp .. "/move_b/data.bin") == payload)
		assert(io.open(tmp .. "/move_a/data.bin", "rb") == nil)
		assert(t:stats().dir == b:dir())
		assert(t:state() == "seeding")

		local r, msg, errno = t:move(b)
		assert(r == nil and type(msg) == "string" and errno > 0)

		-- a complete torrent still reads
		assert(t:read_piece(0) == payload:sub(1, mi.piece_length))

		-- removal ends a move, and the data stays where it was
		local got
		mbt.spawn(function() got = {t:move(c)} end)
		assert(t:remove())
		mbt.sleep(10)
		assert(got[1] == nil and got[2] == "torrent removed")
		assert(read_file(tmp .. "/move_b/data.bin") == payload)
		assert(io.open(tmp .. "/move_c/data.bin", "rb") == nil)
	end)
	s:close()

	local late = assert(mbt.storage.file(tmp .. "/hold"))
	local h = assert(mbt.session{dir = tmp .. "/leech", utp = false})
	raises(function() h:add_infohash(hex, {storage = false, dir = "x"}) end, "not both")
	local x = assert(h:add_magnet("magnet:?xt=urn:btih:" .. mi.infohash, {storage = false}))
	mbt.run(function()
		assert(({x:wait_metadata(1)})[2] == "timeout")
		assert(x:add_peer("127.0.0.1", port))
		assert(x:wait_metadata())
		assert(x:wait_metadata())
		assert(x:state() == "no_storage")
		assert(x:name() == "data.bin")
		assert(x:set_storage(late))
		assert(x:set_storage(late) == nil)
		assert(x:wait())
		assert(read_file(tmp .. "/hold/data.bin") == payload)
	end)
	h:close()
end

-- the example client downloads from the seed, and stops on SIGINT
do
	local dir = arg[0]:match("^(.*)/") or "."
	local script = dir .. "/examples/mbt-get.lua"
	local function start(sub, flags, target)
		local out = tmp .. "/" .. sub
		local status = out .. "/status"

		assert(os.execute("mkdir -p " .. out))
		assert(os.execute(string.format(
		    "(sh -c 'echo $$ > %s/pid; exec %s %s -q -F -L -U %s -o %s -A 127.0.0.1:%d %s';" ..
		    " echo $? > %s.tmp; mv %s.tmp %s) > %s/log 2>&1 &",
		    out, arg[-1], script, flags, out, port, target, status, status, status, out)))
		return out, status
	end
	local function wait_file(path)
		mbt.run(function()
			while not io.open(path, "rb") do
				mbt.sleep(20)
			end
		end)
	end
	local function fetch_with(sub, target)
		local out, status = start(sub, "", target)

		wait_file(status)
		assert(read_file(status) == "0\n", read_file(out .. "/log"))
		assert(read_file(out .. "/data.bin") == payload)
	end

	mbt.run(function() deadline(30000, "mbt-get.lua") end)
	fetch_with("get_file", torrent_path)
	fetch_with("get_hash", mi.infohash)

	local out, status = start("get_seed", "-S", torrent_path)
	mbt.run(function()
		while not io.open(out .. "/data.bin", "rb") do
			mbt.sleep(20)
		end
		mbt.sleep(200)
	end)
	assert(os.execute("kill -INT $(cat " .. out .. "/pid)"))
	wait_file(status)
	assert(read_file(status) == "130\n", read_file(status) .. read_file(out .. "/log"))
end

seed:close()
leech:close()
fetch:close()
cleanup()

-- DHT nodes find each other, and a peer announced on one is found from another
do
	local ih = string.rep("EF", 20)
	local a <close> = assert(mbt.dht{id = string.rep("A0", 20)})
	local b <close> = assert(mbt.dht())
	local c <close> = assert(mbt.dht())
	raises(function() mbt.dht{id = "zz"} end, "40 hex")
	raises(function() a:get_peers(ih) end, "inside mbt.run")
	assert(a:get_peers("zz") == nil)
	assert(a:announce(ih, 6881) == nil)
	assert(a:port() > 0)
	assert(#a:nodes() == 0)

	mbt.run(function()
		deadline(40000, "dht")
		assert(b:bootstrap("127.0.0.1", a:port()))
		assert(c:bootstrap("127.0.0.1", a:port()))
		local function knows(node, id)
			for _, n in ipairs(node:nodes()) do
				if n.id == id then
					return n
				end
			end
		end
		while not knows(b, a:id()) or not knows(c, a:id()) do
			mbt.sleep(10)
		end
		local seen = knows(c, a:id())
		assert(seen.addr == "127.0.0.1" and seen.port == a:port())

		assert(#assert(b:get_peers(ih)) == 0)
		assert(b:announce(ih, 7001) >= 1)
		mbt.sleep(50)
		local peers = assert(c:get_peers(ih))
		assert(#peers == 1 and peers[1].addr == "127.0.0.1" and peers[1].port == 7001)

		-- raw queries, with the whole reply and errors from the node
		local r = assert(b:query("127.0.0.1", a:port(), "ping"))
		assert(r.y == "r" and #r.r.id == 20)
		r = assert(b:query("127.0.0.1", a:port(), "find_node",
		    {target = string.rep("x", 20)}))
		assert(r.y == "r" and type(r.r.nodes) == "string")
		r = assert(b:query("127.0.0.1", a:port(), "get_peers",
		    {info_hash = string.rep("y", 20), want = {"n4", "n6"}}))
		assert(#r.r.token > 0)
		r = assert(b:query("127.0.0.1", a:port(), "announce_peer",
		    {info_hash = string.rep("y", 20), port = 7002, token = "bad"}))
		assert(r.y == "e" and r.e[1] == 203 and r.e[2] == "bad token")
		local function is_error(reply, code)
			assert(reply.y == "e" and reply.e[1] == code, reply.y)
		end
		local peer_of = string.rep("q", 20)

		is_error(assert(b:query("127.0.0.1", a:port(), "nosuch")), 204)
		is_error(assert(b:query("127.0.0.1", a:port(), "ping", {id = false})), 203)
		is_error(assert(b:query("127.0.0.1", a:port(), "ping", {id = "short"})), 203)
		is_error(assert(b:query("127.0.0.1", a:port(), "find_node")), 203)
		is_error(assert(b:query("127.0.0.1", a:port(), "get_peers",
		    {info_hash = "short"})), 203)
		is_error(assert(b:query("127.0.0.1", a:port(), "get")), 203)

		r = assert(b:query("127.0.0.1", a:port(), "get_peers", {info_hash = peer_of}))
		is_error(assert(b:query("127.0.0.1", a:port(), "announce_peer",
		    {info_hash = peer_of, token = r.r.token})), 203)
		assert(assert(b:query("127.0.0.1", a:port(), "announce_peer",
		    {info_hash = peer_of, port = 1234, token = r.r.token, implied_port = 1})).y == "r")
		r = assert(b:query("127.0.0.1", a:port(), "get_peers", {info_hash = peer_of}))
		local hi, lo = r.r.values[1]:byte(5, 6)
		assert(hi * 256 + lo == b:port())

		assert(b:query("not an ip", 1, "ping") == nil)
		assert(b:query("127.0.0.1", 1, "ping", {x = print}) == nil)
		assert(b:send_raw("127.0.0.1", a:port(), "garbage"))
		assert(#b:id() == 40)

		-- close ends a query that gets no reply
		local silent = assert(mbt.dht())
		local got
		mbt.spawn(function() got = {b:query("127.0.0.1", silent:port() + 1, "ping")} end)
		silent:close()
		mbt.sleep(1)
		assert(got == nil)

		-- the peer store: one entry per address, newest kept, and it expires
		local store = assert(mbt.dht{peer_ttl = 1})
		local function announce(hash, peer_port)
			local g = assert(b:query("127.0.0.1", store:port(), "get_peers", {info_hash = hash}))
			r = assert(b:query("127.0.0.1", store:port(), "announce_peer",
			    {info_hash = hash, port = peer_port, token = g.r.token}))
			assert(r.y == "r")
		end
		local function values(hash)
			local g = assert(b:query("127.0.0.1", store:port(), "get_peers", {info_hash = hash}))
			return g.r.values or {}
		end
		local function port_of(v)
			local p1, p2 = v:byte(5, 6)
			return p1 * 256 + p2
		end
		local hash = string.rep("h", 20)

		announce(hash, 5000)
		announce(hash, 5000)
		assert(#values(hash) == 1)
		for p = 5001, 5008 do
			announce(hash, p)
		end
		assert(#values(hash) == 8)
		for _, v in ipairs(values(hash)) do
			assert(port_of(v) ~= 5000)
		end
		for i = 1, 65 do
			announce(string.format("%020d", i), 6000)
		end
		assert(#values(string.format("%020d", 65)) == 1)
		mbt.sleep(2100)
		assert(#values(string.format("%020d", 65)) == 0)
		assert(assert(b:query("127.0.0.1", store:port(), "sample_infohashes",
		    {target = hash})).r.num == 0)
		store:close()

		-- the store size is a setting, and the oldest hash gives way
		store = assert(mbt.dht{max_stored = 100})
		for i = 1, 100 do
			announce(string.format("%020d", i), 6000)
		end
		r = assert(b:query("127.0.0.1", store:port(), "sample_infohashes", {target = hash}))
		assert(r.r.num == 100 and #r.r.samples == 400)
		mbt.sleep(1100)
		announce(string.format("%020d", 101), 6000)
		assert(#values(string.format("%020d", 101)) == 1)
		r = assert(b:query("127.0.0.1", store:port(), "sample_infohashes", {target = hash}))
		assert(r.r.num == 100)
		store:close()

		-- the item store: a put keeps an item alive, and the oldest gives way
		local shelf = assert(mbt.dht{max_items = 2})
		local one = assert(shelf:put("one"))
		mbt.sleep(1100)
		local two = assert(shelf:put("two"))
		mbt.sleep(1100)
		assert(shelf:put("one") == one)
		local three = assert(shelf:put("three"))
		assert(shelf:get(one) and shelf:get(three) and shelf:get(two) == nil)
		shelf:close()

		-- silent nodes cost a lookup little more than one node timeout
		local slowpoke = assert(mbt.dht())
		for p = 1, 6 do
			assert(slowpoke:bootstrap("127.0.0.1", 1000 + p))
		end
		local t0 = mbt.now_ms()
		assert(slowpoke:get_peers(string.rep("31", 20)))
		local took = mbt.now_ms() - t0
		assert(took < 4000, took)
		slowpoke:close()

		-- a lookup across many nodes tells only the closest ones
		local crowd = {}
		for i = 1, 30 do
			crowd[i] = assert(mbt.dht())
			assert(crowd[i]:bootstrap("127.0.0.1", a:port()))
		end
		mbt.sleep(300)
		local far = string.rep("7A", 20)
		assert(crowd[1]:get_peers(far))
		local told = assert(crowd[1]:announce(far, 7444))
		assert(told >= 1 and told <= 8, told)
		mbt.sleep(100)
		local got_peers = assert(crowd[30]:get_peers(far))
		assert(#got_peers == 1 and got_peers[1].port == 7444)
		for i = 1, 30 do
			crowd[i]:close()
		end

		-- a node that reaches itself forgets the entry, and does not answer
		local own = assert(mbt.dht())
		assert(own:bootstrap("127.0.0.1", own:port()))
		mbt.sleep(50)
		assert(own:node_count() == 0)
		assert(({own:query("127.0.0.1", own:port(), "ping")})[3] ~= nil)
		own:close()

		-- a bootstrap placeholder is replaced, and never told to other nodes
		local e = assert(mbt.dht())
		assert(e:bootstrap("127.0.0.1", a:port()))
		while not knows(e, a:id()) do
			mbt.sleep(10)
		end
		for _, n in ipairs(e:nodes()) do
			assert(n.port ~= a:port() or n.id == a:id())
		end
		local found = assert(b:query("127.0.0.1", e:port(), "find_node",
		    {target = string.rep("z", 20)}))
		assert(#found.r.nodes % 26 == 0)
		e:close()

		-- BEP 44 items, with the network side in Lua through d:query
		assert(mbt.bdecode(mbt.bencode({a = 1, b = {"x", 2}})).b[1] == "x")
		assert(mbt.sha1("abc") == "A9993E364706816ABA3E25717850C26C9CD0D89D")
		assert(mbt.bencode(print) == nil)
		assert(mbt.bencode(mbt.empty_list) == "le")
		assert(mbt.bencode({}) == "de")
		assert(mbt.bencode({a = mbt.empty_list, b = {mbt.empty_list}}) == "d1:ale1:blleee")
		local decoded, used = mbt.bdecode("d1:ai1eeTRAILING")
		assert(decoded.a == 1 and used == 8)
		assert(mbt.bdecode("garbage") == nil)
		assert(mbt.bencode(mbt.bdecode("d3:a\0bi1e1:ai2ee")) == "d1:ai2e3:a\0bi1ee")

		local here = assert(a:put("stored here"))
		assert(a:get(here).v == "stored here" and a:get(here).k == nil)
		assert(here == mbt.sha1(mbt.bencode("stored here")))
		assert(a:get(string.rep("00", 20)) == nil)
		local function raw_target(hexstr)
			return (hexstr:gsub("%x%x", function(x) return string.char(tonumber(x, 16)) end))
		end

		r = assert(b:query("127.0.0.1", a:port(), "get", {target = raw_target(here)}))
		assert(r.r.v == "stored here" and #r.r.token > 0)

		local value = {note = "from b", n = 7}
		is_error(assert(b:query("127.0.0.1", a:port(), "put", {v = value, token = "bad"})), 203)
		local tok = r.r.token
		assert(assert(b:query("127.0.0.1", a:port(), "put", {v = value, token = tok})).y == "r")
		local target = mbt.sha1(mbt.bencode(value))
		assert(a:get(target).v.note == "from b")
		is_error(assert(b:query("127.0.0.1", a:port(), "put",
		    {v = string.rep("x", 1100), token = tok})), 205)

		local k, sig = string.rep("k", 32), string.rep("s", 64)
		local mut = {v = "one", k = k, sig = sig, seq = 2, token = tok}
		is_error(assert(b:query("127.0.0.1", a:port(), "put", mut)), 206)

		local seen, accept = nil, true
		local signer = assert(mbt.dht{verify = function(key, s, payload)
			seen = payload
			assert(key == k and s == sig)
			return accept
		end})
		r = assert(b:query("127.0.0.1", signer:port(), "get", {target = raw_target(here)}))
		mut.token = r.r.token
		assert(assert(b:query("127.0.0.1", signer:port(), "put", mut)).y == "r")
		assert(seen == "3:seqi2e1:v3:one")
		local mtarget = mbt.sha1(k)
		assert(signer:get(mtarget).v == "one" and signer:get(mtarget).seq == 2)
		r = assert(b:query("127.0.0.1", signer:port(), "get", {target = raw_target(mtarget)}))
		assert(r.r.v == "one" and r.r.seq == 2 and r.r.k == k and r.r.sig == sig)
		mut.seq = 1
		is_error(assert(b:query("127.0.0.1", signer:port(), "put", mut)), 302)
		accept = false
		mut.seq = 3
		is_error(assert(b:query("127.0.0.1", signer:port(), "put", mut)), 206)
		assert(signer:put_mutable{k = k, sig = sig, seq = 4, v = "two"} == nil)
		accept = true
		assert(signer:put_mutable{k = k, sig = sig, seq = 4, v = "two"} == mtarget)
		assert(signer:get(mtarget).v == "two")

		-- BEP 44: same seq, cas, and get with a seq
		local function mput(fields)
			for key, val in pairs(fields) do
				mut[key] = val
			end
			return assert(b:query("127.0.0.1", signer:port(), "put", mut))
		end
		assert(mput{seq = 4, v = "two"}.y == "r")
		is_error(mput{seq = 4, v = "other"}, 302)
		assert(signer:get(mtarget).v == "two")
		is_error(mput{seq = 5, v = "three", cas = 3}, 301)
		is_error(mput{seq = 3, v = "three", cas = 3}, 301)
		is_error(mput{seq = 3, v = "three", cas = 4}, 302)
		is_error(mput{seq = 5, v = "three", cas = "4"}, 203)
		assert(signer:get(mtarget).v == "two" and signer:get(mtarget).seq == 4)
		assert(mput{seq = 5, v = "three", cas = 4}.y == "r")
		assert(signer:get(mtarget).v == "three" and signer:get(mtarget).seq == 5)
		assert(mput{salt = "fresh", seq = 1, v = "new", cas = 9}.y == "r")
		mut.salt, mut.cas = nil, nil

		local function mget(have)
			return assert(b:query("127.0.0.1", signer:port(), "get",
			    {target = raw_target(mtarget), seq = have})).r
		end
		r = mget(5)
		assert(r.seq == 5 and r.token and r.v == nil and r.k == nil and r.sig == nil)
		assert(mget(6).v == nil)
		r = mget(4)
		assert(r.seq == 5 and r.v == "three" and r.k == k and r.sig == sig)
		assert(mget("not a number").v == "three")
		signer:close()

		local lone = assert(mbt.dht())
		assert(#assert(lone:get_peers(ih)) == 0)
		lone:close()
		raises(function() lone:port() end, "closed")
	end)
end

-- the crawler walks local nodes and logs what it finds
do
	local dir = arg[0]:match("^(.*)/") or "."
	local out = tmp .. "/crawl"
	assert(os.execute("mkdir -p " .. out))
	local a <close> = assert(mbt.dht())
	local b <close> = assert(mbt.dht())
	local status = out .. "/status"

	mbt.run(function()
		deadline(30000, "crawl")
		assert(b:bootstrap("127.0.0.1", a:port()))
		mbt.sleep(50)
		assert(os.execute(string.format(
		    "(%s %s/examples/dht-crawl.lua -s 127.0.0.1:%d -o %s -p 0 -q 200 -t 2 -P;" ..
		    " echo $? > %s.tmp; mv %s.tmp %s) > %s/log 2>&1 &",
		    arg[-1], dir, a:port(), out, status, status, status, out)))
		while not io.open(status, "rb") do
			mbt.sleep(20)
		end
	end)
	assert(read_file(status) == "0\n", read_file(out .. "/log"))
	local name = os.date("!crawl-%Y%m%d.tsv")
	local lines = read_file(out .. "/" .. name)
	assert(lines:find("\treply\t" .. a:id():lower(), 1, true), lines)
end

-- state close frees a timed wait still pending
left_open = assert(mbt.session{utp = false})
mbt.run(function()
	local t = assert(left_open:add_infohash(hex))
	mbt.spawn(function() t:wait(1000000) end)
end)
