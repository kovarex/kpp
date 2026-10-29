-- The Lua compatibility check (docs/path-to-factorio.md, "Converting Lua", step 3's gate): what the
-- game's Lua must do the same in the C++ build and in the conversion, for saves and for multiplayer
-- between the two. lua_compat.cpp runs it with the game's lua.lib (its output is
-- lua_compat.expected), lua_compat.kpp with the converted Lua.

local function show(name, ...)
  local parts = {}
  for i = 1, select('#', ...) do
    parts[#parts + 1] = tostring((select(i, ...)))
  end
  print(name .. ': ' .. table.concat(parts, ' '))
end

-- Numbers as tostring and print write them (LUAI_NUMFFORMAT "%.14g", through trio).
show('tostring', 1 / 3, 1e300, -0.0, 2 ^ 53, 0.1 + 0.2, 100, -7.25, 1e-310, math.huge, -math.huge, 1e15, 1e16, 123456789012345678)
show('nan', tostring(0 / 0) == tostring(0 / 0), 0 / 0 ~= 0 / 0)
show('format', string.format('%d %5.2f %g %.14g %.17g %x %X %o %e %E %i', 42, 3.14159, 1e20, 2 / 3, 0.1, 255, 255, 8, 12345.678, 0.000123, -5))
show('format rounding', string.format('%.0f %.0f %.0f %.1f %.1f %.2f %.3f', 0.5, 1.5, 2.5, 0.25, 0.35, 1.005, 2.0005))
show('format width', string.format('[%5s] [%-5s] [%05d] [%+d] [% d] [%.3s] [%10.4f] [%-10.2e]', 'ab', 'ab', 42, 42, 42, 'abcdef', math.pi, 12345.6789))
show('format 64-bit', string.format('%d %d %d %x', 2 ^ 53, -2 ^ 53, 2 ^ 62, 2 ^ 40))
show('format q', string.format('%q', 'a "quoted"\n\0line\r\\'))
show('format c s', string.format('%c%c%c %s %s %s', 76, 117, 97, nil, true, 12.5))

-- Strings to numbers.
show('tonumber', tonumber('0x10'), tonumber('  12  '), tonumber('1e2'), tonumber('10', 2), tonumber('zz', 36), tonumber('0x1p4'), tonumber(''), tonumber('1e'), tonumber('.5'), tonumber('5.'))
show('coercion', '10' + 5, '3' * '4', 10 .. 20, '0x10' + 0, 2 ^ '3')

-- Arithmetic: modulo and division of negative and fractional numbers, powers.
show('modulo', -7 % 3, 7 % -3, -7.5 % 2, 5.5 % -2, 7 % 0 ~= 7 % 0, 2 ^ 0.5, (-8) ^ (1 / 3) ~= (-8) ^ (1 / 3), 10 / 4, -10 / 4)
show('floor', math.floor(-0.5), math.floor(2.5), math.ceil(-2.5), math.ceil(2.1), math.floor(-2 ^ 31 - 0.5))
local count, last = 0, 0
for i = 1, 2, 0.1 do count = count + 1; last = i end
show('float loop', count, string.format('%.17g', last))

-- The math library, which the game computes itself for the same results on every platform: to the bit.
local function bits(x) return string.format('%.17g', x) end
show('trig', bits(math.sin(1)), bits(math.cos(1)), bits(math.tan(1)), bits(math.asin(0.5)), bits(math.acos(0.5)), bits(math.atan(1)))
show('atan2', bits(math.atan2(1, 2)), bits(math.atan2(-1, -2)), bits(math.atan2(0, -1)), bits(math.atan2(1, 0)))
show('exp log', bits(math.exp(1)), bits(math.log(10)), bits(math.log10(2)), bits(math.log(8, 2)), bits(math.sqrt(2)), bits(math.pow(2, 0.5)))
show('big trig', bits(math.sin(1e6)), bits(math.cos(123456.789)), bits(math.sin(-3.0e-8)), bits(math.cos(math.pi / 3)))
show('fmod modf frexp', math.fmod(7.5, 2), math.fmod(-7.5, 2), math.modf(3.7), math.modf(-3.7), math.frexp(10), math.ldexp(0.5, 3))
show('abs max min', math.abs(-3), math.max(1, 5, 3), math.min(4, -2, 8), math.max(-0.0, 0.0), math.min(1))
show('hyperbolic', bits(math.sinh(1)), bits(math.cosh(1)), bits(math.tanh(0.5)))

-- Tables: the game iterates in insertion order.
local t = {}
t.z = 1; t.a = 2; t[10] = 3; t[1.5] = 4; t.m = 5; t[1] = 6; t[2] = 7; t[true] = 8
local order = {}
for k, v in pairs(t) do order[#order + 1] = tostring(k) .. '=' .. v end
show('pairs', table.concat(order, ' '))
t.a = nil; t.b = 9; t.z = nil; t.z = 10
order = {}
for k, v in pairs(t) do order[#order + 1] = tostring(k) .. '=' .. v end
show('pairs after removal', table.concat(order, ' '))
local big = {}
for i = 1, 100 do big['k' .. (i * 7919 % 101)] = i end
local sum, firstKeys = 0, {}
for k, v in pairs(big) do sum = sum + v; if #firstKeys < 6 then firstKeys[#firstKeys + 1] = k end end
show('pairs 100', sum, table.concat(firstKeys, ' '))
local seq = {10, 20, 30, nil, 50}
show('length', #{1, 2, 3}, #'abc', #seq == 5 or #seq == 3, select('#', 1, nil, 3))
local list = {5, 2, 8, 1, 9, 3, 7}
table.sort(list)
show('sort', table.concat(list, ','))
table.sort(list, function(a, b) return a > b end)
show('sort descending', table.concat(list, ','))
local words = {'pear', 'Apple', 'fig', 'banana', 'apple'}
table.sort(words)
show('sort strings', table.concat(words, ','))
table.insert(list, 1, 0); table.insert(list, 11); show('insert remove', table.remove(list), table.remove(list, 1), table.concat(list, ','))
show('unpack', table.unpack({1, 2, 3}, 2), select(2, 'a', 'b', 'c'))
show('next', next({}), type(next))

-- Strings.
show('find', string.find('hello world', 'o w'), string.find('hello', 'l+'), string.find('a.b', '.', 1, true), string.find('abc', 'x'))
show('match', string.match('key = value', '(%w+)%s*=%s*(%w+)'), string.match('2024-10-29', '(%d+)-(%d+)-(%d+)'))
show('gsub', string.gsub('hello world', 'o', '0'), string.gsub('abc', '%w', '%0%0'), string.gsub('hello', '', '-'), (string.gsub('$name is $age', '%$(%w+)', {name = 'Lua', age = 20})))
local gm = {}
for w in string.gmatch('one two  three', '%a+') do gm[#gm + 1] = w end
show('gmatch', table.concat(gm, '|'))
show('rep sub', string.rep('ab', 3, ','), string.sub('hello', 2, -2), string.sub('hello', -3), ('x'):rep(0))
show('byte char', string.byte('ABC', 1, -1), string.char(72, 105), string.upper('MiXeD'), string.lower('MiXeD'), string.reverse('abc'), string.len('\0a'))
show('balance', string.match('f(a(b)c)d', '%b()'), string.find('THE (quick) fox', '%f[%a]%a+', 5))

-- string.pack and friends (from Lua 5.4.6 in the game's Lua).
local function hex(s) return (s:gsub('.', function(c) return string.format('%02x', c:byte()) end)) end
show('pack', hex(string.pack('<i4', 100)), hex(string.pack('>i2', -2)), hex(string.pack('<d', 1.5)), hex(string.pack('z', 'ab')), hex(string.pack('s1', 'xyz')))
show('unpack', string.unpack('<i4', string.pack('<i4', -123456)), string.unpack('<I2 >I2', '\1\2\1\2'), string.unpack('<d', string.pack('<d', math.pi)) == math.pi)
show('packsize', string.packsize('i4'), string.packsize('<i4 i8 d'), string.packsize('!8 i1 i8'))

-- bit32.
show('bit32', bit32.band(0xFF, 0x0F), bit32.bor(1, 2, 4), bit32.bxor(5, 3), bit32.bnot(0), bit32.lshift(1, 31), bit32.rshift(0x80000000, 4), bit32.arshift(-16, 2))
show('bit32 more', bit32.extract(0xABCD, 4, 8), bit32.replace(0, 7, 4, 3), bit32.lrotate(1, 33), bit32.rrotate(1, 1), bit32.btest(6, 1), bit32.band(-1))

-- Errors and their messages.
local function caught(f, ...) local ok, message = pcall(f, ...); return tostring(ok) .. ' ' .. tostring(message) end
show('error', caught(error, 'plain'), caught(error, {}) ~= nil, caught(error, 'level 0', 0))
show('error messages', caught(function() local x; return x.y end), caught(function() return 1 + {} end), caught(function() return #5 end))
show('error messages 2', caught(function() return ('x') < 1 end), caught(function() undefinedFunction() end), caught(string.rep))
show('xpcall', xpcall(function() error('deep') end, function(m) return 'handled: ' .. m end))
show('assert', caught(assert, false, 'assertion message'), caught(assert, nil), select('#', assert(1, 2, 3)))

-- Chunks, metatables, closures.
show('load', load('return 1 + 1')(), load('syntax error here') == nil, select(2, load('x =')))
local mt = {__index = function(_, k) return k .. '!' end, __add = function(a, b) return 'added' end, __eq = function() return true end,
  __lt = function() return true end, __le = function() return false end, __concat = function() return 'joined' end,
  __call = function(_, a) return 'called ' .. a end, __len = function() return 42 end, __tostring = function() return 'object' end}
local a, b = setmetatable({}, mt), setmetatable({}, mt)
show('metatables', a.key, a + 1, a == b, a < b, a <= b, a .. 'x', a('arg'), #a, tostring(a))
local counters = {}
for i = 1, 3 do counters[i] = function() i = i + 10; return i end end
show('closures', counters[1](), counters[1](), counters[2](), counters[3]())
local proxy = setmetatable({}, {__newindex = function(t, k, v) rawset(t, k, v * 2) end})
proxy.x = 21
show('newindex', proxy.x, rawget(proxy, 'x'), rawequal(proxy, proxy), rawlen({1, 2}))
show('select', select(-1, 'a', 'b', 'c'), select('#'), type(select), math.type == nil)
show('varargs', (function(...) return select('#', ...), ... end)(nil, nil))
local finalized = {}
do
  for i = 1, 3 do setmetatable({}, {__gc = function() finalized[#finalized + 1] = i end}) end
end
collectgarbage()
collectgarbage()
show('gc', table.concat(finalized, ','))
show('string methods', ('%d items'):format(3), ('abc'):upper(), ('a,b,c'):find(',', 3, true))
show('integer keys', ({[1] = 'a', [1.0] = 'b'})[1], ({[2 ^ 53] = 'big'})[2 ^ 53])
