-- The tracker server, saved state, and DHT and LSD toggles from Lua.
local mbt = require("mbt")

local tmp = os.tmpname()
os.remove(tmp)
assert(os.execute("mkdir -p " .. tmp .. "/a " .. tmp .. "/b " .. tmp .. "/state"))

local path = tmp .. "/a/data.bin"
local f = assert(io.open(path, "wb"))
f:write(string.rep("knobs ", 20000))
f:close()

local function wait(cond, ms)
	local start = mbt.now_ms()
	while not cond() and mbt.now_ms() - start < ms do
		mbt.sleep(20)
	end
	return cond()
end

local function swarm(url, tracker)
	local torrent = assert(mbt.create(path, {piece_length = 16384, trackers = {url}}))
	local a = assert(mbt.session{dir = tmp .. "/a", utp = false, http = true})
	local dir = tmp .. "/b/" .. tostring(math.random(1 << 30))
	assert(os.execute("mkdir -p " .. dir))
	local b = assert(mbt.session{dir = dir, utp = false, http = true})
	local done
	mbt.run(function()
		assert(assert(a:add_torrent_buf(torrent)):wait())
		local t = assert(b:add_torrent_buf(torrent))
		assert(wait(function() return t:complete() end, 15000))
		assert(tracker:peers() >= 1 and tracker:torrents() == 1)
		done = true
	end)
	a:close()
	b:close()
	assert(done)
end

local function check()
	local tr <close> = assert(mbt.tracker{http = true})
	assert(tr:port() > 0 and tr:http_port() > 0)
	swarm("udp://127.0.0.1:" .. tr:port() .. "/announce", tr)

	local h <close> = assert(mbt.tracker{http = true})
	swarm("http://127.0.0.1:" .. h:http_port() .. "/announce", h)

	local udp_only <close> = assert(mbt.tracker())
	assert(udp_only:http_port() == nil)
	udp_only:close()
	assert(not pcall(udp_only.port, udp_only))
	udp_only:close()

	-- Saved state brings a torrent back in a new session.
	local torrent = assert(mbt.create(path, {piece_length = 16384}))
	local state = tmp .. "/state"
	local s1 = assert(mbt.session{dir = tmp .. "/a", state = state, tcp = false, utp = false})
	mbt.run(function()
		assert(assert(s1:add_torrent_buf(torrent)):wait())
	end)
	s1:close()
	local s2 = assert(mbt.session{dir = tmp .. "/a", state = state, tcp = false, utp = false})
	assert(#s2:torrents() == 1)
	s2:close()

	-- A restart keeps the .torrent as added, and what was added at run time.
	local st2 = tmp .. "/state2"
	assert(os.execute("mkdir -p " .. st2))
	local url1, url2 = "udp://127.0.0.1:1/a", "udp://127.0.0.1:1/b"
	local ws = "http://127.0.0.1:1/x"
	local orig = assert(mbt.create(path, {piece_length = 16384, trackers = {url1},
	    comment = "keep me"}))
	local s4 = assert(mbt.session{dir = tmp .. "/a", state = st2, tcp = false, utp = false})
	mbt.run(function()
		local t = assert(s4:add_torrent_buf(orig))
		assert(t:wait())
		assert(t:add_tracker(url2))
		assert(t:add_webseed(ws))
		t:add_fallback_trackers()
		assert(#t:trackers() > 2)
	end)
	s4:close()
	local h = assert(io.popen("cat " .. st2 .. "/*.torrent"))
	assert(h:read("a") == orig)
	h:close()
	local s5 = assert(mbt.session{dir = tmp .. "/a", state = st2, tcp = false, utp = false})
	local t5 = assert(s5:torrents()[1])
	local urls = t5:trackers()
	assert(#urls == 2 and urls[1] == url1 and urls[2] == url2)
	assert(t5:webseeds()[1] == ws)
	s5:close()

	local s3 = assert(mbt.session{dht_bootstrap = false})
	assert(s3:enable_dht())
	assert(s3:dht_port() ~= nil)
	s3:disable_dht()
	assert(s3:enable_lsd())
	s3:disable_lsd()
	s3:close()
end

local ok, err = pcall(check)
os.execute("rm -rf " .. tmp)
assert(ok, err)
os.exit(0)
