#pragma once

#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <stdexcept>

namespace hw::utility {

enum class SocketState {
  DATA_READY,   // epoll indicated that there is unread data
  ACCEPT_READY,
  CONNECTED,    // connection established
  DISCONNECTED, // remote end close or error
  ERROR         // unrecoverable error
};

enum class SocketType {
  TCP_CLIENT, TCP_SERVER
};

using EventHandler = std::function<void(int fd, SocketState state, int err)>;

class EPoller {
  struct Connection {
    int fd = -1;
    bool connected = false;
    SocketType type = SocketType::TCP_CLIENT;
    EventHandler handler;
  };

  int _epfd = -1;
  // Pointers in _events point to values in this map.
  // Address stability is guaranteed by std::unordered_map node allocation.
  std::unordered_map<int, Connection> _connections; 
  
  // Safety: Track active event count to allow close() to scrub the event list
  int _current_event_count = 0;

  static constexpr int MAX_EVENTS = 64;
  epoll_event _events[MAX_EVENTS];

static int make_address (sockaddr_in & addr, const std::string & host, uint16_t port) {
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  return ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
}

static std::pair<int, int> return_error(int sock) {
  int ec = errno;
  ::close (sock);
  return std::make_pair(-1, ec);
}

public:
  EPoller() {
    _epfd = epoll_create1(0);
  }

  ~EPoller() {
    for (auto & [sock, _] : _connections) {
      ::close(sock);
    }
    ::close(_epfd);
  }

  std::pair<int, int> listen (const std::string & host, uint16_t port, const EventHandler & handler) {
    int sock = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int optval = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    sockaddr_in addr {};
    if (make_address(addr, host, port) <= 0) {
      return return_error(sock);
    }

    if (::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 ||
        ::listen(sock, SOMAXCONN) < 0) {
      return return_error(sock);
    }

    // Insert first to get stable address
    auto [it, _] = _connections.emplace(sock, Connection{sock, true, SocketType::TCP_SERVER, handler});
    
    epoll_event ev {};
    ev.events = EPOLLIN;
    ev.data.ptr = &it->second; // Fast pointer access
    ::epoll_ctl(_epfd, EPOLL_CTL_ADD, sock, &ev);
    
    return std::make_pair(sock, 0);
  }


  std::pair<int, int> accept (int svrsock, const EventHandler &handler) {
    ::sockaddr_in addr;
    ::socklen_t addrlen = sizeof (addr);
    int sock = ::accept4(svrsock, reinterpret_cast<sockaddr *>(&addr), &addrlen, SOCK_NONBLOCK);
    if (sock > 0) {
      auto [it, _] = _connections.emplace(sock, Connection{sock, true, SocketType::TCP_CLIENT, handler});
      
      epoll_event ev {};
      ev.events = EPOLLIN | EPOLLRDHUP;
      ev.data.ptr = &it->second; // Fast pointer access
      ::epoll_ctl(_epfd, EPOLL_CTL_ADD, sock, &ev);
      
      return std::make_pair(sock, 0);
    }
    return return_error(-1);
  }

  std::pair<int, int> connect (const std::string & host, uint16_t port, const EventHandler & handler) {
    int sock = ::socket (AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    ::sockaddr_in addr {};
    if (make_address(addr, host, port) <= 0) {
      return return_error(sock);
    }

    int rc = ::connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
      return return_error (sock);
    }

    auto [it, _] = _connections.emplace(sock, Connection{sock, false, SocketType::TCP_CLIENT, handler});
    
    epoll_event ev {};
    ev.events = EPOLLOUT;
    ev.data.ptr = &it->second; // Fast pointer access
    ::epoll_ctl(_epfd, EPOLL_CTL_ADD, sock, &ev);
    
    return std::make_pair(sock, 0);
  }

  int close (int sock) {
    auto it = _connections.find(sock);
    if (it == _connections.end()) return -1;
    
    // Stop events
    ::epoll_ctl(_epfd, EPOLL_CTL_DEL, sock, nullptr);
    ::shutdown (sock, SHUT_WR);
    ::close(sock);

    // CRITICAL FIX: If we are currently polling, we must remove any pending
    // references to this connection in the current event batch to prevent Use-After-Free.
    // This scan is extremely fast (max 64 integers in L1 cache).
    if (_current_event_count > 0) [[unlikely]] {
        Connection* target = &it->second;
        for (int i = 0; i < _current_event_count; ++i) {
            if (_events[i].data.ptr == target) {
                _events[i].data.ptr = nullptr; // Nullify the pointer
                // We don't break here because a socket *could* theoretically 
                // appear multiple times if dup() was used, though rare. 
                // Safety first.
            }
        }
    }

    _connections.erase(it); // Immediate deletion
    return 0;
  }

  bool connected (int sock) const {
    auto it =_connections.find(sock);
    return it != _connections.end() && it->second.connected;
  }

  int write (int sock, const void *data, size_t datalen, size_t & bytes_written) {
    ssize_t n = ::write(sock, data, datalen);
    if (n >= 0) [[likely]] {
      bytes_written = static_cast<size_t>(n);
      return 0;
    }

    const int rc = errno;
    if (rc == EAGAIN || rc == EWOULDBLOCK) [[likely]] {
      return EAGAIN;
    }

    auto it = _connections.find(sock);
    if (it != _connections.end()) {
        it->second.handler(sock, SocketState::ERROR, rc);
        close(sock);
    }
    return rc;
  }

  int poll (int timeout_ms = 0) {
    int n = ::epoll_wait(_epfd, _events, MAX_EVENTS, timeout_ms);
    if (n < 0) return -1;

    _current_event_count = n; // Publish count for close()

    for (int i = 0; i < n; ++i) {
      epoll_event &ev = _events[i];
      
      // FAST: Direct pointer access
      Connection *conn = static_cast<Connection*>(ev.data.ptr);

      // SAFETY: Check if close() nullified this pointer
      if (!conn) [[unlikely]] {
          continue;
      }

      int sock = conn->fd;

      if (ev.events & EPOLLIN) [[likely]] {
        conn->handler(sock, conn->type == SocketType::TCP_SERVER ? SocketState::ACCEPT_READY : SocketState::DATA_READY, 0);
        
        // SAFETY: Check again. The handler might have called close(sock) (suicide)
        // or close() on another socket that happens to be next in the list.
        if (_events[i].data.ptr == nullptr) continue;
      }

      if (ev.events & EPOLLOUT) [[unlikely]] {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) [[likely]] {
          conn->connected = true;
          conn->handler(sock, SocketState::CONNECTED, 0);
          
          if (_events[i].data.ptr != nullptr) {
             epoll_event mod_ev {};
             mod_ev.events = EPOLLIN | EPOLLRDHUP;
             mod_ev.data.ptr = conn; 
             ::epoll_ctl(_epfd, EPOLL_CTL_MOD, sock, &mod_ev);
          }
        }
        else {
          conn->handler(sock, SocketState::ERROR, err);
          close(sock);
        }
      }
    }
    
    _current_event_count = 0; // Reset
    return n;
  }


  std::pair<std::string, uint16_t> peerinfo(int sock) {
    std::string ip;
    uint16_t port = 0;
    sockaddr_storage as;
    socklen_t len = sizeof (as);
    if (getpeername(sock, reinterpret_cast<sockaddr*>(&as), &len) == 0 && as.ss_family == AF_INET) {
      sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(&as);
      char ip_buf[INET_ADDRSTRLEN];
      inet_ntop (AF_INET, & (sin->sin_addr), ip_buf, INET_ADDRSTRLEN);
      ip = ip_buf;
      port = ntohs(sin->sin_port);
    }
    return {ip, port};
  }

};

}
