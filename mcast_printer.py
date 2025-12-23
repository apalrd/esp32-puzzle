import socket
import struct
import os

MCAST_GRP = 'ff02::6969'
MCAST_PORT = 6969
sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
except AttributeError:
    pass

# bind to all IPv6 interfaces on the port
sock.bind(('::', MCAST_PORT))

# join the IPv6 multicast group
group_bin = socket.inet_pton(socket.AF_INET6, MCAST_GRP)
# for link-local groups (ff02::/16) you must supply a non-zero interface index
if_index = socket.if_nametoindex('en0')
mreq = struct.pack('16sI', group_bin, if_index)
sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, mreq)

while True:
    data, addr = sock.recvfrom(10240)
    length = len(data)
    #Expected packet length
    if length != 12: continue
    #Only care about leaders (active 2)
    active = data[4]
    if active != 2: continue
    #Now look for a phase 9
    phase = data[6]
    if phase != 9: continue
    #Now print the stuff we should print
    print("Got the answer correct, printing pdf file")
    pdf_file = "/Users/apalrd/Downloads/chempuzzle.pdf"
    os.system(f"lp {pdf_file} -o sides=one-sided")
    print("Finished sending document to printer")
    exit()