#pragma once

#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <cassert>
#include <string>
#include <stdexcept>
#include <sys/syscall.h>

#include <hw/utility/Format.hpp>

namespace hw::utility {
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1U
#endif

namespace detail {
  // Runtime check if memfd_create is supported
  inline bool supports_memfd() {
    static bool supported = [] () {
      int fd = syscall(SYS_memfd_create, "anything", MFD_CLOEXEC);
      if (fd != -1) {
        close(fd);
        return true;
      }
      return false;
    } ();
    return supported;
  }
}

template <size_t SIZE>
class BaseBuffer {
  static constexpr size_t PAGE_SIZE = 4096;
  static_assert(!(SIZE & (SIZE - 1)), "Buffer size must be a power of 2");
  static_assert(SIZE >= PAGE_SIZE, "Buffer size must be at least PAGE_SIZE");

public:
  explicit BaseBuffer(const char* name) : _name(name) {
    if (_name.empty()) {
      throw std::invalid_argument("Buffer name is required");
    }

    int fd = -1;

    if (detail::supports_memfd()) {
      // RHEL 8+
      fd = syscall(SYS_memfd_create, _name.c_str(), MFD_CLOEXEC);
    }

    // if RHEL 7 or if memfd fails
    if (fd == -1) {
      fd = shm_open(_name.c_str(), O_RDWR | O_CREAT, 0600);
      _is_shm = true;
    }

    if (fd == -1)
      throw std::runtime_error(frmt::format("Buffer creation failed for '{}' (errno: {})", _name, errno));

    if (ftruncate(fd, SIZE) == -1) {
      close(fd);
      throw std::runtime_error(frmt::format("ftruncate failed for '{}'", _name));
    }

    // lock memory if possible
    struct rlimit rlim {};
    getrlimit(RLIMIT_MEMLOCK, &rlim);
    int flags = MAP_SHARED;
    if (2 * SIZE <= rlim.rlim_max) flags |= MAP_LOCKED;

    // reserve 2x SIZE virtual address space (PROT_NONE)
    _buff = static_cast<char*>(mmap(nullptr, 2 * SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (_buff == MAP_FAILED) {
      close(fd);
      throw std::runtime_error(frmt::format("mmap reservation failed for '{}'", _name));
    }

    // map first half
    if (mmap(_buff, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
      close(fd);
      throw std::runtime_error(frmt::format("mmap first half failed for '{}'", _name));
    }

    // map second half as a mirror
    if (mmap(_buff + SIZE, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
      close(fd);
      throw std::runtime_error(frmt::format("mmap mirror failed for '{}'", _name));
    }

    close(fd);

    _buff[0] = 'X';
    assert (_buff[0] == _buff[SIZE]);
  }

  ~BaseBuffer() {
    if (_buff && _buff != MAP_FAILED) {
      munmap(_buff, 2 * SIZE);
    }
    if (_is_shm) {
      shm_unlink(_name.c_str());
    }
  }

  size_t capacity() const { return SIZE; }

protected:
  char* _buff = nullptr;
  const std::string _name;
  bool _is_shm = false;
};


template <size_t SIZE>
class OldBuffer{
  static constexpr size_t PAGE_SIZE = 4*1024;
  static_assert(!(SIZE & (SIZE-1)));
  static_assert(SIZE >= PAGE_SIZE);
public:
  OldBuffer(const char *name) : _name(name) {
    int fd = shm_open(_name.c_str(), O_RDWR|O_CREAT, 0600);
    if (fd == -1)
      throw std::runtime_error(frmt::format ("shm_open : error: (}", errno));

    ftruncate(fd, SIZE);

    struct limit rlim {};
    getrlimit (RLIMIT_MEMLOCK, &rlim) ;

    int flags = MAP_SHARED;
    if (2 * SIZE <= rlim.rlim_max)
      flags |= MAP_LOCKED;
    _buff = static_cast<char*>(mmap(NULL, 2 * SIZE, PROT_READ | PROT_WRITE, flags, fd, 0));
    if (_buff == MAP_FAILED)
      throw std::runtime_error(frmt::format ("mmap : error: {}", errno));

    char *rem = static_cast<char*>(mmap(_buff + SIZE, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0));
    if (_buff == MAP_FAILED)
      throw std::runtime_error(frmt::format("mmap: error: ()", errno));

    close(fd);
    rem[0] = 'X';
    assert (_buff[0] == rem[0]);
  }

  ~OldBuffer() {
    if (_buff != nullptr) {
      munmap(_buff, 2 * SIZE);
      shm_unlink(_name.c_str());
    }
  }

  size_t capacity () const { return SIZE; }

protected:
  char *_buff = nullptr;
  const std::string _name;
};

template <size_t SIZE>
class BoundedBuffer : public BaseBuffer<SIZE> {
  using Parent = BaseBuffer<SIZE>;
public:
  BoundedBuffer(const char *name) : Parent (name) {
    reset();
  }

  void          reset       ()            { _read = _write = this->_buff; }
  size_t        size        () const      { return _write - _read; }
  size_t        available   () const      { return std::max(0, this->capacity() - size()); }
  char *        beginWrite  () const      { return this->_buff + (((size_t)_write) & (SIZE-1)); }
  const char *  beginRead   () const      { return this->_buff + (((size_t)_read) & (SIZE-1)); }
  void          commitwrite (size_t size) { _write += size; }
  void          commitRead  (size_t size) { _read += size; }

protected:
  char * _read = nullptr;
  char * _write = nullptr;
};

template <size_t SIZE>
class UnboundedBuffer : public BaseBuffer<SIZE> {
  using Parent = BaseBuffer<SIZE>;
public:
  UnboundedBuffer (const  char *name) : Parent (name) {
    _ptr = this->_buff;
  }

  char * getPtr() { return this->_buff + (((size_t)_ptr) & (SIZE-1)); }
  void advancePtr (size_t size) { _ptr += size; }

protected:
  char * _ptr = nullptr;
};

}

