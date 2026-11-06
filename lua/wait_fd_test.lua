-- mbt.wait_fd on a FIFO. Linux only: /proc gives the descriptor number.
local mbt = require("mbt")

local stat = io.open("/proc/self/stat")
if stat == nil then
	os.exit(77)
end
local pid = stat:read("n")
stat:close()

local path = os.tmpname()
os.remove(path)
assert(os.execute("mkfifo " .. path))

local function fd_of(p)
	local h = assert(io.popen("ls -l /proc/" .. pid .. "/fd"))
	local out = h:read("a")
	h:close()
	return assert(tonumber(out:match("(%d+) %-> " .. p:gsub("%p", "%%%0"))))
end

local function check()
	local fifo = assert(io.open(path, "r+"))
	local fd = fd_of(path)
	fifo:setvbuf("no")

	assert(not pcall(mbt.wait_fd, fd, "r"))
	assert(not pcall(mbt.wait_fd, fd, "bad"))

	local lfd, lms = mbt.loop_fd()
	if lfd ~= nil then
		assert(math.type(lfd) == "integer" and math.type(lms) == "integer")
	end

	mbt.run(function()
		assert(not pcall(mbt.loop_fd))
		-- Nothing to read: a timeout.
		local start = mbt.now_ms()
		local ok, why = mbt.wait_fd(fd, "r", 50)
		assert(ok == nil and why == "timeout")
		assert(mbt.now_ms() - start >= 40)

		assert(mbt.wait_fd(fd, "w") == "w")

		-- A write from another coroutine wakes the wait, with no timer.
		mbt.spawn(function()
			mbt.sleep(50)
			fifo:write("x")
		end)
		assert(mbt.wait_fd(fd, "r") == "r")
		assert(fifo:read(1) == "x")

		-- The same descriptor cannot have two waits.
		local parked = 0
		mbt.spawn(function()
			parked = 1
			mbt.wait_fd(fd, "r", 100)
			parked = 2
		end)
		mbt.sleep(10)
		assert(parked == 1)
		assert(mbt.wait_fd(fd, "r", 10) == nil)
		mbt.sleep(150)
		assert(parked == 2)
	end)
	fifo:close()
end

local ok, err = pcall(check)
os.remove(path)
assert(ok, err)
os.exit(0)
