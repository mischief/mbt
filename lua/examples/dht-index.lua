#!/usr/bin/env lua5.4
-- Sample infohashes from the DHT (BEP 51) and fetch their metadata, no piece data.
-- Columns: unix time, infohash, status, ms, bytes, files, piece length, private, name.
-- The names come from strangers and can be anything. Treat the log with care.
local mbt = require("mbt")

local USAGE = [[
usage: dht-index.lua -s addr:port [-o dir] [-n hashes] [-c fetches] [-q per_sec]
                     [-w wait_ms] [-t seconds]
  -s addr:port  seed node, an IP address (can repeat)
  -o dir        log directory (default: .)
  -n hashes     stop after this many fetch attempts (default: 300)
  -c fetches    fetches at one time (default: 5)
  -q per_sec    sample queries per second (default: 10)
  -w wait_ms    time to wait for metadata (default: 30000)
  -t seconds    stop after this long (default: no limit)
]]

local TICK_MS = 100
local FLUSH_MS = 5000
local MAX_KNOWN = 200000
local MAX_IN_FLIGHT = 100
local COMPACT_LEN = 26
local HASH_LEN = 20

local function usage()
	io.stderr:write(USAGE)
	os.exit(1)
end

local function parse_args(argv)
	local opts = {seeds = {}, dir = ".", hashes = 300, fetches = 5, qps = 10, wait = 30000}
	local names = {["-o"] = "dir"}
	local numbers = {["-n"] = "hashes", ["-c"] = "fetches", ["-q"] = "qps", ["-w"] = "wait",
	    ["-t"] = "seconds"}

	for i = 1, #argv, 2 do
		local flag, value = argv[i], argv[i + 1]

		if value == nil then
			usage()
		elseif flag == "-s" then
			opts.seeds[#opts.seeds + 1] = value
		elseif names[flag] then
			opts[names[flag]] = value
		elseif numbers[flag] then
			opts[numbers[flag]] = math.tointeger(tonumber(value)) or usage()
		else
			usage()
		end
	end
	if #opts.seeds == 0 then
		usage()
	end
	return opts
end

local function hex(s)
	return (s:gsub(".", function(c) return string.format("%02X", c:byte()) end))
end

local function random_id()
	local t = {}

	for i = 1, HASH_LEN do
		t[i] = string.char(math.random(0, 255))
	end
	return table.concat(t)
end

local function is_private(a, b)
	return a == 0 or a == 10 or a == 127 or a >= 224 or (a == 169 and b == 254) or
	    (a == 172 and b >= 16 and b <= 31) or (a == 192 and b == 168)
end

-- Names are untrusted bytes: keep each record on one line and in one field.
local function clean_name(name)
	return (tostring(name or ""):gsub("[%c]", " "))
end

local function run(d, s, opts)
	local path = string.format("%s/meta-%s.tsv", opts.dir, os.date("!%Y%m%d"))
	local buf = {}
	local known, known_count = {}, 0
	local nodes, node_head, node_tail = {}, 1, 0
	local hashes, hash_head, hash_tail = {}, 1, 0
	local seen = {}
	local stats = {queried = 0, replied = 0, sampled = 0, started = 0, ok = 0, failed = 0}
	local in_flight, running, done = 0, 0, false

	local function flush()
		if #buf == 0 then
			return
		end
		local f = assert(io.open(path, "ab"))

		f:write(table.concat(buf, "\n"), "\n")
		f:close()
		buf = {}
	end

	local function add_node(addr, port)
		local key = addr .. ":" .. port

		if known[key] or known_count >= MAX_KNOWN then
			return
		end
		known[key] = true
		known_count = known_count + 1
		node_tail = node_tail + 1
		nodes[node_tail] = {addr = addr, port = port}
	end

	local function learn(data)
		if type(data) ~= "string" then
			return
		end
		for i = 0, #data // COMPACT_LEN - 1 do
			local a, b, c, e, p1, p2 = data:byte(i * COMPACT_LEN + 21, i * COMPACT_LEN + 26)

			if p1 * 256 + p2 > 0 and not is_private(a, b) then
				add_node(string.format("%d.%d.%d.%d", a, b, c, e), p1 * 256 + p2)
			end
		end
	end

	local function sample(node)
		local r = d:query(node.addr, node.port, "sample_infohashes", {target = random_id()})

		-- Most nodes lack BEP 51, but they still lead to other nodes.
		if r ~= nil and r.y == "e" then
			r = d:query(node.addr, node.port, "find_node", {target = random_id()})
		end
		in_flight = in_flight - 1
		if r == nil or type(r.r) ~= "table" then
			return
		end
		stats.replied = stats.replied + 1
		learn(r.r.nodes)
		local data = r.r.samples

		if type(data) ~= "string" then
			return
		end
		for i = 1, #data - HASH_LEN + 1, HASH_LEN do
			local h = hex(data:sub(i, i + HASH_LEN - 1))

			if not seen[h] then
				seen[h] = true
				stats.sampled = stats.sampled + 1
				hash_tail = hash_tail + 1
				hashes[hash_tail] = h
			end
		end
	end

	local function record(h, status, ms, t)
		local bytes, count, plen, private, name = 0, 0, 0, 0, ""

		if status == "ok" then
			local st = t:stats()

			for _, f in ipairs(t:files()) do
				bytes = bytes + f.length
				count = count + 1
			end
			plen = st.piece_length
			private = st.private and 1 or 0
			name = clean_name(st.name)
		end
		buf[#buf + 1] = table.concat({os.time(), h, status, ms, bytes, count, plen, private, name},
		    "\t")
	end

	local function fetch(h)
		local t0 = mbt.now_ms()
		local t, err = s:add_infohash(h, {storage = false})

		if t == nil then
			record(h, "add_failed", 0)
			stats.failed = stats.failed + 1
		else
			local ok, msg = t:wait_metadata(opts.wait)
			local ms = mbt.now_ms() - t0

			if ok then
				stats.ok = stats.ok + 1
				record(h, "ok", ms, t)
			else
				stats.failed = stats.failed + 1
				record(h, tostring(msg):gsub("%s", "_"), ms)
			end
			t:remove()
		end
		running = running - 1
	end

	for _, seed in ipairs(opts.seeds) do
		local addr, port = seed:match("^(.*):(%d+)$")

		add_node(addr, math.tointeger(tonumber(port)))
		s:dht_bootstrap(addr, math.tointeger(tonumber(port)))
	end

	mbt.spawn(function()
		while not done do
			mbt.sleep(FLUSH_MS)
			flush()
			io.stderr:write(string.format(
			    "nodes %d queried %d replied %d sampled %d started %d ok %d failed %d\n",
			    known_count, stats.queried, stats.replied, stats.sampled, stats.started,
			    stats.ok, stats.failed))
		end
	end)

	local start = mbt.now_ms()
	local credit, per_tick = 0, opts.qps * TICK_MS / 1000

	while stats.started < opts.hashes or running > 0 do
		if opts.seconds and (mbt.now_ms() - start) / 1000 >= opts.seconds then
			break
		end
		credit = math.min(credit + per_tick, opts.qps)
		while credit >= 1 and node_head <= node_tail and in_flight < MAX_IN_FLIGHT and
		    hash_tail - hash_head < opts.hashes do
			local node = nodes[node_head]

			nodes[node_head] = nil
			node_head = node_head + 1
			credit = credit - 1
			in_flight = in_flight + 1
			stats.queried = stats.queried + 1
			mbt.spawn(sample, node)
		end
		while running < opts.fetches and stats.started < opts.hashes and hash_head <= hash_tail do
			local h = hashes[hash_head]

			hashes[hash_head] = nil
			hash_head = hash_head + 1
			running = running + 1
			stats.started = stats.started + 1
			mbt.spawn(fetch, h)
		end
		if node_head > node_tail and hash_head > hash_tail and in_flight == 0 and running == 0 then
			break
		end
		mbt.sleep(TICK_MS)
	end
	done = true
	flush()
	io.stderr:write(string.format("done: sampled %d started %d ok %d failed %d\n",
	    stats.sampled, stats.started, stats.ok, stats.failed))
end

local opts = parse_args(arg)

mbt.log(false)
math.randomseed(os.time())
local d <close> = assert(mbt.dht{port = 0})
local s <close> = assert(mbt.session{dir = opts.dir, dht = true, utp = true})
local ok, err = pcall(mbt.run, run, d, s, opts)

if not ok and not tostring(err):find("interrupted", 1, true) then
	io.stderr:write("dht-index: ", tostring(err), "\n")
	os.exit(1)
end
