No photo, just the doggo

<img width="649" height="546" alt="image" src="https://github.com/user-attachments/assets/4791ab09-f843-401c-8113-18f3a1b771e1" />

## Remote process connections

The project provides a TLS-protected WebSocket transport for graph frames. It is
a transport adapter, not an HTTP route or listener: the application accepts a
TCP socket and passes it to `WebSocketConnection::accept`, or opens an outgoing
connection with `WebSocketConnection::connect`. Each connection must use
mutual TLS. Configure the TLS context with the trusted CA and the local
certificate/private key before connecting.

```cpp
#include <Process.hpp>
#include <WebSocketChannel.hpp>
#include <boost/asio/ssl/context.hpp>

namespace ssl = boost::asio::ssl;

Process process(1);
constexpr ID remoteID = 2;
auto tls = std::make_shared<ssl::context>(ssl::context::tls_client);
tls->set_default_verify_paths();
tls->load_verify_file("nodes-ca.pem");
tls->use_certificate_chain_file("node-cert.pem");
tls->use_private_key_file("node-key.pem", ssl::context::pem);

auto connection = WebSocketConnection::connect(
    "graph.example.net", "9443", "/graph", tls
);
process.registerChannel(remoteID, WebSocketChannel(connection));
process.addChild(remoteID); // for a remote child
process.position(remoteID, {{process.identity, remoteID}});
```

For an accepted connection, configure a `tls_server` context with the server
certificate/key and the CA that issued client certificates. Accept a TCP socket
using a shared `boost::asio::io_context`, then pass both to
`WebSocketConnection::accept(ioContext, std::move(socket), tls)`:

```cpp
auto ioContext = std::make_shared<boost::asio::io_context>();
boost::asio::ip::tcp::acceptor listener(
    *ioContext,
    {boost::asio::ip::tcp::v4(), 9443}
);
boost::asio::ip::tcp::socket socket(*ioContext);
listener.accept(socket);
auto connection = WebSocketConnection::accept(
    ioContext, std::move(socket), serverTls
);
```

The `io_context` must remain available while accepting more sockets; the connection
retains shared ownership of it. The adapter verifies the peer certificate;
applications remain responsible for mapping an authenticated certificate to an
authorized process ID.

On the remote child, call `setParent(parentID)` and register the accepted
connection as its parent channel. Both sides should call `pollChannels()`
regularly to deliver received frames to `Process`. A process can announce a
position with `position(id, neighbours)`. The resulting `POSITION` frame is
forwarded through connected channels when a process first learns the node or a
new edge; repeated announcements do not circulate indefinitely. The payload
encodes a 32-bit edge count followed by source/destination ID pairs in network
byte order.

Connections are not restricted to a central root: any process can open or
accept a WebSocket and use it as a channel to a remote child. To join two
existing trees, the application can connect the coordinator processes and
explicitly attach one under the other using the existing parent/child APIs.
Each process still has at most one parent, so this creates a tree rather than a
general mesh. There is no node-ID registration handshake or automatic
certificate-to-ID binding yet.

The adapter uses Boost.Beast/Asio for WebSockets and OpenSSL for TLS. Graph
frames are sent as binary WebSocket messages using the same checksummed,
network-byte-order frame format as the raw socket transport.
