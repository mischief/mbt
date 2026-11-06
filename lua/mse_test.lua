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
end

local ok, err = pcall(check)
cleanup()
assert(ok, err)
