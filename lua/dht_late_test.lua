-- A seed that starts before any DHT node must still announce.
local mbt = require("mbt")

local tmp = os.tmpname()
os.remove(tmp)
assert(os.execute("mkdir -p " .. tmp .. "/seed " .. tmp .. "/leech"))

local function cleanup()
	os.execute("rm -rf " .. tmp)
end

local path = tmp .. "/seed/data.bin"
local f = assert(io.open(path, "wb"))
f:write(string.rep("late dht ", 20000))
f:close()
local torrent = assert(mbt.create(path, {piece_length = 16384}))

local function check()
	math.randomseed(os.time())
	local port = 20000 + math.random(20000)
	local boot = {"127.0.0.1:" .. port}
	local timers = {maintain_interval = 1}
	local seed = assert(mbt.session{dir = tmp .. "/seed", dht = true, dht_bootstrap = boot,
	    timers = timers})
	local done
	mbt.run(function()
		local s = assert(seed:add_torrent_buf(torrent))
		assert(s:wait())
		mbt.sleep(2000)
		-- The first lookup found nobody. Now a node comes up.
		local node = assert(mbt.dht{port = port})
		local leech = assert(mbt.session{dir = tmp .. "/leech", dht = true,
		    dht_bootstrap = boot, timers = timers})
		local t = assert(leech:add_torrent_buf(torrent))
		local start = mbt.now_ms()
		while not t:complete() and mbt.now_ms() - start < 30000 do
			mbt.sleep(200)
		end
		done = t:complete()
		leech:close()
		node:close()
		seed:close()
	end)
	assert(done, "the leecher found no peer")
end

local ok, err = pcall(check)
cleanup()
assert(ok, err)
os.exit(0)
