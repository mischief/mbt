-- mbt.wait_fd on a FIFO. A shell opens it on fd 9, so the number is known.
local mbt = require("mbt")

if arg[1] ~= "child" then
	local path = os.tmpname()
	os.remove(path)
	assert(os.execute("mkfifo " .. path))
	local cmd = string.format("exec 9<>%s; exec %s %s child %s", path, arg[-1], arg[0], path)
	local ok, how, code = os.execute(cmd)
	os.remove(path)
	if not ok then
		error("child failed: " .. tostring(how) .. " " .. tostring(code))
	end
	os.exit(0)
end

local fd = 9
local fifo = assert(io.open(arg[2], "r+"))
fifo:setvbuf("no")

assert(not pcall(mbt.wait_fd, fd, "r"))
assert(not pcall(mbt.wait_fd, fd, "bad"))

-- loop_fd works from the main chunk, and not inside a run.
local lfd, lms, lerr = mbt.loop_fd()
if lfd == nil then
	assert(lerr ~= nil)
else
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

	-- The wait ends, and the loop keeps running.
	local parked = 0
	mbt.spawn(function()
		parked = 1
		mbt.wait_fd(fd, "r", 100)
		parked = 2
	end)
	mbt.sleep(10)
	assert(parked == 1)
	mbt.sleep(150)
	assert(parked == 2)
end)
fifo:close()
os.exit(0)
