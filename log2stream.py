#
# convert a series of log lines to a car command stream 
#
fd = open('zz.txt', 'r')
lines = fd.read().split('\n')
last_ts = -1
buf = ''

for line in lines:
	try:
		# [238344371] Tx: {0x1c} Forward Speed=12
		rb = line.find(']')
		ts = int(line[1:rb])
		rb = line.find('}')
		lb = line.find('{')
		cmd = int(line[lb+1:rb], 16)
	except:
		continue

	if last_ts == -1:
		buf += ('0x%x,' % (cmd))
	else:
		delay = ts - last_ts
		buf += ('%d,0x%x,' % (delay, cmd))

	last_ts = ts

print(buf)