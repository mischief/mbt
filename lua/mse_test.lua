-- MSE policy between two sessions over loopback.
local mbt = require("mbt")

local tmp = os.tmpname()
os.remove(tmp)
assert(os.execute("mkdir -p " .. tmp .. "/seed"))

local function cleanup()
	os.execute("rm -rf " .. tmp)
end

local payload = string.rep("mse payload ", 20000)
local path = tmp .. "/seed/data.bin"
local f = assert(io.open(path, "wb"))
f:write(payload)
f:close()
local torrent = assert(mbt.create(path, {piece_length = 16384}))

local function transfer(seed_mode, leech_mode, wait_ms)
	local dir = tmp .. "/" .. seed_mode .. "-" .. leech_mode
	assert(os.execute("mkdir -p " .. dir))
	local a = assert(mbt.session{dir = tmp .. "/seed", utp = false,
	    encryption = seed_mode})
	local b = assert(mbt.session{dir = dir, utp = false,
	    encryption = leech_mode})
	local ok
	mbt.run(function()
		assert(assert(a:add_torrent_buf(torrent)):wait())
		local t = assert(b:add_torrent_buf(torrent))
		assert(t:add_peer("127.0.0.1", a:listen_port()))
		for _ = 1, wait_ms / 10 do
			if t:complete() then break end
			mbt.sleep(10)
		end
		ok = t:complete()
	end)
	a:close()
	b:close()
	return ok
end

local function check()
	assert(transfer("require", "require", 10000))
	assert(transfer("require", "prefer", 10000))
	assert(transfer("prefer", "require", 10000))
	assert(transfer("prefer", "prefer", 10000))
	assert(transfer("disable", "disable", 10000))
	-- The leecher redials in plaintext.
	assert(transfer("disable", "prefer", 30000))
	assert(not transfer("require", "disable", 1000))
	assert(not transfer("disable", "require", 1000))
	assert(not pcall(mbt.session, {encryption = "rot13"}))
	assert(mbt.storage.fd(-1) == nil)
	local ws = assert(mbt.session({webseed = {mode = "off"}, webseed_max_requests = 2}))
	local wt = assert(ws:add_torrent_buf(torrent, {dir = tmp .. "/ws", webseed = {mode = "always", no_peer_delay_ms = 0}}))
	assert(wt:add_webseed("http://127.0.0.1:1/x"))
	assert(wt:add_webseed("gopher://x") == nil)
	assert(wt:webseed_bytes() == 0)

	-- A session with no default storage, and no default DHT node.
	local ns = assert(mbt.session{storage = false, dht = true, dht_bootstrap = {"127.0.0.1:1"}})
	assert(ns:bytes_downloaded() == 0 and ns:bytes_uploaded() == 0)
	assert(mbt.session{dht = true, dht_bootstrap = {"no-port"}} == nil)
	assert(mbt.session{dht = true, dht_bootstrap = false})

	local st = assert(mbt.storage.file(tmp .. "/st"))
	st:close()
	assert(not pcall(st.dir, st))
	st:close()
end

local ok, err = pcall(check)
cleanup()
assert(ok, err)
