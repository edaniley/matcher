#include <boost/test/unit_test.hpp>
#include <hw/utility/EPoller.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <vector>
#include <iostream>

using namespace hw::utility;

BOOST_AUTO_TEST_SUITE(EPollerTests)

// Use a unique port to avoid conflicts
static uint16_t PORT = 12346;

BOOST_AUTO_TEST_CASE(TestUseAfterFree_CrossClose) {
    EPoller server;
    std::vector<int> accepted_fds;
    bool crash_imminent = false;

    // 1. Setup Server
    auto [listen_sock, err] = server.listen("127.0.0.1", PORT, [&](int fd, SocketState state, int /*err*/) {
        if (state == SocketState::ACCEPT_READY) {
            auto [new_sock, a_err] = server.accept(fd, [&](int cfd, SocketState cstate, int /*cerr*/) {
                if (cstate == SocketState::DATA_READY) {
                    // This is the handler that will trigger the bug
                    // We want to close the *other* socket if it exists
                    if (accepted_fds.size() == 2) {
                        int other_fd = (cfd == accepted_fds[0]) ? accepted_fds[1] : accepted_fds[0];
                        server.close(other_fd);
                        crash_imminent = true;
                    }
                    
                    // Consume data so we don't spin
                    char buf[1024];
                    ::read(cfd, buf, sizeof(buf));
                }
            });
            if (new_sock > 0) {
                accepted_fds.push_back(new_sock);
            }
        }
    });
    BOOST_REQUIRE_GE(listen_sock, 0);

    // 2. Connect two clients manually
    int c1 = socket(AF_INET, SOCK_STREAM, 0);
    int c2 = socket(AF_INET, SOCK_STREAM, 0);
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    connect(c1, (sockaddr*)&addr, sizeof(addr));
    connect(c2, (sockaddr*)&addr, sizeof(addr));

    // 3. Pump server to accept connections
    // Accept c1
    server.poll(100); 
    // Accept c2
    server.poll(100); 

    BOOST_REQUIRE_EQUAL(accepted_fds.size(), 2);

    // 4. Send data to both to trigger DATA_READY on both simultaneously
    send(c1, "A", 1, 0);
    send(c2, "B", 1, 0);

    // Sleep briefly to ensure kernel has both events ready
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 5. Poll once. This should pick up both events.
    server.poll(100);

    // Cleanup
    close(c1);
    close(c2);
    // Server dtor closes listen_sock and accepted_fds
}

BOOST_AUTO_TEST_CASE(BasicEchoTest) {
    EPoller server;
    EPoller client;
    
    // Increment port for safety
    uint16_t echo_port = PORT + 1;
    std::string message = "Hello EPoller";
    bool server_received = false;
    bool client_received = false;
    
    // Server Setup
    auto [listen_fd, lerr] = server.listen("127.0.0.1", echo_port, [&](int fd, SocketState state, int /*err*/) {
        if (state == SocketState::ACCEPT_READY) {
            server.accept(fd, [&](int cfd, SocketState cstate, int /*cerr*/) {
                if (cstate == SocketState::DATA_READY) {
                    char buf[1024];
                    int n = ::read(cfd, buf, sizeof(buf));
                    if (n > 0) {
                        std::string s(buf, n);
                        if (s == message) server_received = true;
                        size_t written;
                        server.write(cfd, buf, n, written);
                    } else if (n == 0) {
                         // EOF
                         server.close(cfd);
                    }
                }
            });
        }
    });
    BOOST_REQUIRE_GE(listen_fd, 0);

    // Client Setup
    auto [client_fd, cerr] = client.connect("127.0.0.1", echo_port, [&](int fd, SocketState state, int /*err*/) {
        if (state == SocketState::CONNECTED) {
            size_t written;
            client.write(fd, message.c_str(), message.size(), written);
        } else if (state == SocketState::DATA_READY) {
            char buf[1024];
            int n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                std::string s(buf, n);
                if (s == message) client_received = true;
                client.close(fd);
            }
        }
    });
    BOOST_REQUIRE_GE(client_fd, 0);

    // Loop until done
    for (int i = 0; i < 50; ++i) {
        server.poll(10);
        client.poll(10);
        if (server_received && client_received) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    BOOST_CHECK(server_received);
    BOOST_CHECK(client_received);
}


BOOST_AUTO_TEST_SUITE_END()
