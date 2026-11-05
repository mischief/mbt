#!/usr/bin/env lua5.4
-- Crawl the DHT with find_node, and log each reply as one TSV line.
-- Columns: unix time, event, node id, addr, port, rtt ms, nodes returned, client.
-- Load into sqlite with .mode tabs and .import. The log holds public addresses.
local mbt = require("mbt")

local USAGE = [[
usage: dht-crawl.lua -s addr:port [-o dir] [-p port] [-q per_sec] [-t seconds] [-P]
  -s addr:port  seed node, an IP address (can repeat)
  -o dir        log directory (default: .)
  -p port       our UDP port (default: 7781)
  -q per_sec    queries per second (default: 20)
  -t seconds    stop after this long (default: run until interrupted)
  -P            also query private addresses, for tests
]]

local TICK_MS = 50
local FLUSH_MS = 5000
local REVISIT_SEC = 3600
local MAX_IN_FLIGHT = 300
local MAX_KNOWN = 500000
local COMPACT_LEN = 26

local function usage()
	io.stderr:write(USAGE)
	os.exit(1)
end

local function parse_args(argv)
	local opts = {seeds = {}, dir = ".", port = 7781, qps = 20}
	local i = 1

	while i <= #argv do
		local flag, value = argv[i], argv[i + 1]

		if flag == "-P" then
			opts.private = true
			i = i + 1
		elseif value == nil then
			usage()
		elseif flag == "-s" then
			opts.seeds[#opts.seeds + 1] = value
			i = i + 2
		elseif flag == "-o" then
			opts.dir = value
			i = i + 2
		elseif flag == "-p" then
			opts.port = math.tointeger(tonumber(value)) or usage()
			i = i + 2
		elseif flag == "-q" then
			opts.qps = tonumber(value) or usage()
			i = i + 2
		elseif flag == "-t" then
			opts.seconds = tonumber(value) or usage()
			i = i + 2
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
	return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

local function random_id()
	local t = {}

	for i = 1, 20 do
		t[i] = string.char(math.random(0, 255))
	end
	return table.concat(t)
end

local function is_private(a, b)
	return a == 0 or a == 10 or a == 127 or a >= 224 or (a == 169 and b == 254) or
	    (a == 172 and b >= 16 and b <= 31) or (a == 192 and b == 168)
end

-- A client name from the "v" key is untrusted bytes: keep it to one TSV field.
local function clean_client(v)
	if type(v) ~= "string" then
		return ""
	end
	return hex(v)
end

local Log = {}
Log.__index = Log

function Log.open(dir)
	return setmetatable({dir = dir, buf = {}}, Log)
end

function Log:add(event, id, addr, port, rtt, count, client)
	self.buf[#self.buf + 1] = table.concat({os.time(), event, id, addr, port, rtt, count,
	    client}, "\t")
end

function Log:flush()
	if #self.buf == 0 then
		return
	end
	local path = string.format("%s/crawl-%s.tsv", self.dir, os.date("!%Y%m%d"))
	local f = assert(io.open(path, "ab"))

	f:write(table.concat(self.buf, "\n"), "\n")
	f:close()
	self.buf = {}
end

local function crawl(d, opts, log)
	local known, known_count = {}, 0
	local queue, head, tail = {}, 1, 0
	local in_flight, sent = 0, 0

	local function enqueue(addr, port)
		local key = addr .. ":" .. port
		local last = known[key]

		if last ~= nil and os.time() - last < REVISIT_SEC then
			return
		end
		if last == nil then
			if known_count >= MAX_KNOWN then
				return
			end
			known_count = known_count + 1
		end
		known[key] = os.time()
		tail = tail + 1
		queue[tail] = {addr = addr, port = port}
	end

	local function learn(nodes)
		if type(nodes) ~= "string" then
			return 0
		end
		local n = #nodes // COMPACT_LEN

		for i = 0, n - 1 do
			local a, b, c, e, p1, p2 = nodes:byte(i * COMPACT_LEN + 21, i * COMPACT_LEN + 26)
			local port = p1 * 256 + p2

			if port > 0 and (opts.private or not is_private(a, b)) then
				enqueue(string.format("%d.%d.%d.%d", a, b, c, e), port)
			end
		end
		return n
	end

	local function visit(node)
		local t0 = mbt.now_ms()
		local r, msg = d:query(node.addr, node.port, "find_node", {target = random_id()})
		local rtt = mbt.now_ms() - t0

		in_flight = in_flight - 1
		if r == nil then
			log:add("timeout", "", node.addr, node.port, rtt, 0, "")
		elseif r.y == "e" then
			log:add("error", "", node.addr, node.port, rtt, 0, clean_client(r.v))
		elseif type(r.r) == "table" then
			local id = type(r.r.id) == "string" and hex(r.r.id) or ""

			log:add("reply", id, node.addr, node.port, rtt, learn(r.r.nodes), clean_client(r.v))
		else
			log:add("bad", "", node.addr, node.port, rtt, 0, clean_client(r.v))
		end
	end

	for _, seed in ipairs(opts.seeds) do
		local addr, port = seed:match("^(.*):(%d+)$")

		enqueue(addr, math.tointeger(tonumber(port)))
	end

	mbt.spawn(function()
		while true do
			mbt.sleep(FLUSH_MS)
			log:flush()
			io.stderr:write(string.format("known %d queued %d in flight %d sent %d\n",
			    known_count, tail - head + 1, in_flight, sent))
		end
	end)

	local start = mbt.now_ms()
	local per_tick = opts.qps * TICK_MS / 1000
	local credit = 0

	while opts.seconds == nil or (mbt.now_ms() - start) / 1000 < opts.seconds do
		credit = math.min(credit + per_tick, opts.qps)
		while credit >= 1 and head <= tail and in_flight < MAX_IN_FLIGHT do
			local node = queue[head]

			queue[head] = nil
			head = head + 1
			credit = credit - 1
			in_flight = in_flight + 1
			sent = sent + 1
			mbt.spawn(visit, node)
		end
		mbt.sleep(TICK_MS)
	end
end

local opts = parse_args(arg)

mbt.log(false)
math.randomseed(os.time())
local d <close> = assert(mbt.dht{port = opts.port})
local log = Log.open(opts.dir)
local ok, err = pcall(mbt.run, crawl, d, opts, log)

log:flush()
if not ok and not tostring(err):find("interrupted", 1, true) then
	io.stderr:write("dht-crawl: ", tostring(err), "\n")
	os.exit(1)
end
