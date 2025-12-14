import socket
import struct

MCAST_GRP = 'ff02::666'
MCAST_PORT = 6666
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
if_index = socket.if_nametoindex('en14')
mreq = struct.pack('16sI', group_bin, if_index)
sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, mreq)

while True:
    data, addr = sock.recvfrom(10240)
    print(data)