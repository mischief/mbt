#!/usr/bin/env lua5.4
-- Download a torrent. A Lua version of the mbt-get tool.
local mbt = require("mbt")

local USAGE = [[
usage: mbt-get.lua [-dRqDPSLTUF] [-A addr:port] [-l port] [-o dir] [-p max_peers]
                   [-r KB/s] [-s strategy] [-W pkts]
                   <file.torrent | magnet:?... | infohash>
  -d            enable debug logging
  -R            enable trace logging (all components, very verbose)
  -q            suppress progress output
  -D            enable DHT
  -L            disable LSD (Local Service Discovery)
  -P            enable PCP port mapping
  -S            seed after download completes
  -T            disable TCP (uTP only)
  -U            disable uTP (TCP only)
  -F            do not add fallback public trackers
  -A addr:port  add a peer (can repeat)
  -l port       listen port (default: random)
  -o dir        download directory (default: .)
  -p N          max peers per torrent (default: 30)
  -r KB/s       download rate limit (default: unlimited)
  -s strategy   piece picker: rarest (default), sequential or random
  -W pkts       uTP window size in packets (default: 32)
]]

local FLAGS = "dRqDPSLTUF"
local VALUES = "AloprsW"
local KIB = 1024
local PROGRESS_MS = 1000
local EXIT_INTERRUPTED = 130

local function usage()
	io.stderr:write(USAGE)
	os.exit(1)
end

local function die(...)
	io.stderr:write("mbt-get: ", ...)
	io.stderr:write("\n")
	os.exit(1)
end

-- Options go in a table by letter. -A collects a list.
local function parse_args(argv)
	local opts = {peers = {}}
	local i = 1

	while i <= #argv and argv[i]:sub(1, 1) == "-" and #argv[i] > 1 do
		local word = argv[i]
		local j = 2

		while j <= #word do
			local c = word:sub(j, j)

			if FLAGS:find(c, 1, true) then
				opts[c] = true
				j = j + 1
			elseif VALUES:find(c, 1, true) then
				local value = word:sub(j + 1)

				if value == "" then
					i = i + 1
					value = argv[i]
				end
				if value == nil then
					usage()
				end
				if c == "A" then
					opts.peers[#opts.peers + 1] = value
				else
					opts[c] = value
				end
				j = #word + 1
			else
				usage()
			end
		end
		i = i + 1
	end
	if argv[i] == nil then
		usage()
	end
	opts.target = argv[i]
	return opts
end

local function number_opt(value, name)
	if value == nil then
		return nil
	end
	return math.tointeger(tonumber(value)) or die("bad ", name, ": ", value)
end

local function set_logging(opts)
	if opts.R then
		mbt.log{trace = {"all"}}
	elseif opts.d then
		mbt.log{debug = {"session", "torrent", "peer", "dht"}}
	else
		mbt.log(false)
	end
end

local function file_exists(path)
	local f = io.open(path, "rb")

	if f == nil then
		return false
	end
	f:close()
	return true
end

local function add_target(s, target)
	if target:sub(1, 7) == "magnet:" then
		return s:add_magnet(target)
	end
	if not file_exists(target) and target:match("^%x+$") and #target == 40 then
		return s:add_infohash(target)
	end
	return s:add_torrent_file(target)
end

local function format_rate(bytes)
	return string.format("%.1f KiB/s", bytes / KIB)
end

local function show_progress(t)
	local st = t:stats()

	io.stderr:write(string.format("\r%-11s %5.1f%% %d/%d pieces, %d peers, %s   ",
	    st.state, st.progress * 100, st.have_count, st.piece_count, st.peer_count,
	    format_rate(st.download_rate)))
end

local function download(s, opts)
	local t, err = add_target(s, opts.target)

	if t == nil then
		die("add: ", err)
	end
	if not opts.F then
		t:add_fallback_trackers()
	end
	for _, peer in ipairs(opts.peers) do
		local addr, port = peer:match("^(.*):(%d+)$")

		if addr == nil then
			die("bad peer: ", peer)
		end
		t:add_peer(addr, number_opt(port, "port"))
	end
	if not opts.q then
		io.stderr:write("downloading: ", t:name() or "(metadata pending)", "\n")
	end

	local finished = false

	if not opts.q then
		mbt.spawn(function()
			while not finished do
				show_progress(t)
				mbt.sleep(PROGRESS_MS)
			end
		end)
	end

	local ok, msg = t:wait()

	finished = true
	if not ok then
		die("\n", msg, ": ", t:error() or "")
	end
	if not opts.q then
		io.stderr:write("\ndownload complete: ", t:name(), "\n")
	end
	if opts.S then
		if not opts.q then
			io.stderr:write("seeding...\n")
		end
		while true do
			mbt.sleep(PROGRESS_MS)
		end
	end
end

local function main()
	local opts = parse_args(arg)

	set_logging(opts)
	local s, err = mbt.session{
		dir = opts.o,
		port = number_opt(opts.l, "port"),
		tcp = not opts.T,
		utp = not opts.U,
		lsd = not opts.L,
		dht = opts.D,
		pcp = opts.P,
		picker = opts.s,
		max_peers = number_opt(opts.p, "max_peers"),
		download_rate = (number_opt(opts.r, "rate") or 0) * KIB,
		utp_window_pkts = number_opt(opts.W, "window"),
	}

	if s == nil then
		die("session: ", err)
	end

	local ok, run_err = pcall(mbt.run, download, s, opts)

	s:close()
	if not ok then
		if tostring(run_err):find("interrupted", 1, true) then
			io.stderr:write("\ninterrupted, shutting down...\n")
			os.exit(EXIT_INTERRUPTED)
		end
		die(run_err)
	end
end

main()
