#!/usr/bin/env lua5.4
-- Time how long a DHT announce takes to reach a search from another host.
-- Run "announce" on one host and "search" on another, with the same -k and -T.
-- Each round uses a new infohash. Clocks must agree to about a second.
local mbt = require("mbt")

local USAGE = [[
usage: dht-converge.lua announce|search -s addr:port -k key -T unix_start
                        [-r rounds] [-i seconds]
  -s addr:port  bootstrap node
  -k key        shared text, makes the infohashes
  -T time       unix time of round 1, leave 30 seconds to bootstrap
  -r rounds     rounds (default: 10)
  -i seconds    seconds per round (default: 30)
]]

local PEER_PORT = 7011
local POLL_MS = 100

local function usage()
	io.stderr:write(USAGE)
	os.exit(1)
end

local role = arg[1]
if role ~= "announce" and role ~= "search" then
	usage()
end
local opts = {rounds = 10, interval = 30}
for i = 2, #arg, 2 do
	local flag, value = arg[i], arg[i + 1]

	if value == nil then
		usage()
	elseif flag == "-s" then
		opts.seed = value
	elseif flag == "-k" then
		opts.key = value
	elseif flag == "-T" then
		opts.start = math.tointeger(tonumber(value)) or usage()
	elseif flag == "-r" then
		opts.rounds = math.tointeger(tonumber(value)) or usage()
	elseif flag == "-i" then
		opts.interval = math.tointeger(tonumber(value)) or usage()
	else
		usage()
	end
end
if not (opts.seed and opts.key and opts.start) then
	usage()
end

local function wait_until(unix)
	while os.time() < unix do
		mbt.sleep(POLL_MS)
	end
end

local function round_hash(n)
	return mbt.sha1(string.format("mbt-converge %s %d", opts.key, n))
end

local function announce(d, n, hash)
	local t0 = mbt.now_ms()
	local peers = assert(d:get_peers(hash))
	local lookup_ms = mbt.now_ms() - t0

	print(string.format("%d\tannounce\tlookup_ms %d\tnodes %s\tpeers %d", n, lookup_ms,
	    tostring(d:announce(hash, PEER_PORT)), #peers))
end

local function search(d, n, hash, deadline)
	local t0 = mbt.now_ms()
	local tries = 0

	while os.time() < deadline do
		tries = tries + 1
		local peers = d:get_peers(hash)

		for _, p in ipairs(peers or {}) do
			if p.port == PEER_PORT then
				print(string.format("%d\tfound\tms %d\ttries %d", n, mbt.now_ms() - t0, tries))
				return
			end
		end
	end
	print(string.format("%d\tmissed\tms %d\ttries %d", n, mbt.now_ms() - t0, tries))
end

mbt.log(false)
local d <close> = assert(mbt.dht())
local addr, port = opts.seed:match("^(.*):(%d+)$")

mbt.run(function()
	assert(d:bootstrap(addr, math.tointeger(tonumber(port))))
	for n = 1, opts.rounds do
		local at = opts.start + (n - 1) * opts.interval

		wait_until(at)
		if role == "announce" then
			announce(d, n, round_hash(n))
		else
			search(d, n, round_hash(n), at + opts.interval - 2)
		end
		io.stdout:flush()
	end
	print("nodes", d:node_count())
end)
