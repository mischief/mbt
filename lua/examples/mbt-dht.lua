#!/usr/bin/env lua5.4
-- Look up peers for an infohash on the DHT. A Lua version of the mbt-dht tool.
local mbt = require("mbt")

local BOOTSTRAP_HOST = "dht.transmissionbt.com"
local BOOTSTRAP_PORT = 6881
local BOOTSTRAP_WAIT_MS = 5000
local POLL_MS = 100
local ROUNDS = 3

local function die(...)
	io.stderr:write("mbt-dht: ", ...)
	io.stderr:write("\n")
	os.exit(1)
end

local infohash = arg[1]
if infohash == nil or #infohash ~= 40 then
	die("usage: mbt-dht.lua <infohash hex> [host port]")
end
local host = arg[2] or BOOTSTRAP_HOST
local port = math.tointeger(tonumber(arg[3] or BOOTSTRAP_PORT)) or die("bad port")

mbt.log(false)
local d <close> = assert(mbt.dht())

mbt.run(function()
	assert(d:bootstrap(host, port))
	local waited = 0
	while d:node_count() == 0 and waited < BOOTSTRAP_WAIT_MS do
		mbt.sleep(POLL_MS)
		waited = waited + POLL_MS
	end
	if d:node_count() == 0 then
		die("no reply from ", host)
	end

	local seen = {}
	for _ = 1, ROUNDS do
		for _, peer in ipairs(assert(d:get_peers(infohash))) do
			local key = peer.addr .. ":" .. peer.port
			if not seen[key] then
				seen[key] = true
				print(key)
			end
		end
		io.stderr:write("routing table: ", d:node_count(), " nodes\n")
	end
end)
