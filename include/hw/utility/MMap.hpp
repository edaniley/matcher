#pragma once

	#include <cstdint>
	#include <iterator>
	#include <string>
	#include <system_error>
	#include <stdexcept>
	#include <utility>
	#include <sys/types.h>
	#include <sys/stat.h>
	#include <fcntl.h>
	#include <sys/mman.h>
	#include <unistd.h>
	#include <string.h>


namespace hw::utility {

enum class MMode { read, write, };

template <MMode Mode>
class MMap {
public:
  using handle_type = std::int32_t;
  using value_type = std::uint8_t;
  using size_type = std::size_t;
  using reference = value_type &;
  using const_reference = const value_type &;
  using pointer = value_type *;
  using const_pointer = const value_type *;
  using difference_type = std::ptrdiff_t;
  using iterator = pointer;
  using const_iterator = const_pointer;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;
  using iterator_category = std::random_access_iterator_tag;

  MMap() = delete;
  MMap(const std::string filename, const size_type size = 0, bool fill_zero = true);
  MMap (MMap&&);
  MMap(const MMap&) = delete;
  ~MMap();

  MMap& operator = (const MMap &) = delete;
  MMap& operator = (MMap&&) = delete;
  bool operator == (const MMap&) = delete;

  handle_type handle() const noexcept {
    return _handle;
  }

  bool is_open() const noexcept {
    return _handle != -1;
  }

  size_type length() const noexcept {
    return _length;
  }


  size_type size() const noexcept {
    return length();
  }

  bool empty() const noexcept {
    return length() == 0;
  }


  pointer data() noexcept {
    return _data;
  }

  const_pointer data() const noexcept {
    return _data;
  }

  template <MMode M = Mode> requires (M == MMode::write)
  iterator begin() noexcept (M == MMode::write) {
    return data();
  }

  const_iterator begin() const noexcept {
    return data();
  }

  const_iterator cbegin() const noexcept {
    return data();
  }

  iterator end() noexcept requires (Mode == MMode::write) {
    return data() + length();
  }

  const_iterator end() const noexcept {
    return data() + length();
  }

  const_iterator cend() const noexcept {
      return data() + length();
  }

  template <MMode M = Mode> requires (M == MMode::write)
  reverse_iterator rbegin() noexcept {
    return reverse_iterator(end());
  }
  const_reverse_iterator rbegin() const noexcept {
    return const_reverse_iterator(end());
  }

  const_reverse_iterator crbegin() const noexcept {
    return const_reverse_iterator(end());
  }

  template <MMode M = Mode> requires (M == MMode::write)
  reverse_iterator rend() noexcept {
    return reverse_iterator(begin());
  }

  const_reverse_iterator rend() const noexcept {
    return const_reverse_iterator(begin());
  }

  const_reverse_iterator crend() const noexcept {
    return const_reverse_iterator(begin()) ;
  }

  template <MMode M = Mode> requires (M == MMode::write)
  reference operator[]( const size_type i) noexcept {
    return _data[i];
  }

  const_reference operator [](const size_type i) const noexcept {
    return _data[i];
  }

  private:
  pointer _data = nullptr;
  size_type _length = 0;
  handle_type _handle = -1;

  template <MMode M = Mode> requires (M == MMode::write)
  void map(const std::string filename, const size_type size, bool fill_zero);

  template <MMode M = Mode> requires (M == MMode::read)
  void map(const std::string filename, const size_type size, bool fill_zero);

  template <MMode M = Mode> requires (M == MMode::write)
  void sync();

  template <MMode M = Mode> requires (M == MMode::read)
  void sync();

  void unmap();
};


template <MMode Mode>
MMap<Mode>::MMap(const std::string filename, const size_type size, bool fill_zero) {
  map (filename, size, fill_zero);
}

template <MMode Mode>
MMap<Mode>::MMap (MMap&& other)
  : _data(std::move(other._data)), _length(std::move(other._length)), _handle(std::move(other._handle)) {
  other._data = nullptr;
  other._length = 0;
  other._handle = -1;
}

template <MMode Mode>
template <MMode M> requires (M == MMode::write)
void MMap<Mode>::map(const std::string filename, const size_type size, bool fill_zero) {
  using std::string_literals::operator ""s;

  if (_handle != -1 || filename.empty()) {
    throw std::invalid_argument ("invalid file "s + filename) ;
  }

  if (size <= 0) {
    throw std::invalid_argument ("invalid size "s + std::to_string(size));
  }

  const auto handle = ::open(filename.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (handle < 0) {
    throw std::system_error(std::error_code(errno, std::system_category()),
      "open('"s + filename + "') failed"s);
  }

  if (::ftruncate(handle, size) < 0) {
    auto cerrno = errno;
    ::close(handle);
    ::unlink(filename.c_str());
    throw std::system_error(std::error_code(cerrno, std::system_category()),
      "ftruncate('"s + filename + "') failed"s);
  }

  auto* addr = ::mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
  if (addr == MAP_FAILED) {
    auto cerrno = errno;
    ::close(handle);
    ::unlink(filename.c_str());
    throw std::system_error(std::error_code(cerrno, std::system_category()),
      "mmap('"s + filename + "') failed"s);
  }

  if (fill_zero) {
    ::memset (addr, 0, size);
  }

  // ::mlock(addr, size); // assuming mock was mlock, but maybe unnecessary. removing unknown call.
  _data = reinterpret_cast<pointer>(addr);
  _length = size;
  _handle = handle;
}

template <MMode Mode>
template <MMode M> requires (M == MMode::read)
void MMap<Mode>::map(const std::string filename, const size_type size, bool/*fill zero*/) {
  using std::string_literals::operator ""s;

  if (_handle != -1 || filename.empty()) {
    throw std::invalid_argument("invalid file "s + filename);
  }

  const auto handle = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
  if (handle < 0) {
    throw std::system_error(std::error_code(errno, std::system_category()),
      "mmap('"s + filename + "') failed"s);
  }

  struct stat s;
  if (fstat (handle, &s) < 0) {
    auto cerrno = errno;
    ::close (handle);
    throw std::system_error(std::error_code(cerrno, std::system_category()),
      "fstat('"s + filename + "') failed"s);
  }

  if (size && size != static_cast<size_type>(s.st_size)) {
    ::close(handle);
    throw std::logic_error(
    "requested size "s + std::to_string(size) +
    " and existing filesize "s + std::to_string(s.st_size) +
    " mismatch for " + filename);
  }

  auto* addr = ::mmap(NULL, s.st_size, PROT_READ, MAP_SHARED, handle, 0);
  if (addr == MAP_FAILED) {
    auto cerrno = errno;
    ::close(handle);
    throw std::system_error(std::error_code(cerrno, std::system_category()),
      "mmap('"s + filename + "') failed"s);
  }

  _data = reinterpret_cast<pointer>(addr);
  _length = s.st_size;
  _handle = handle;
}


template <MMode Mode>
template <MMode M> requires (M == MMode::write)
void MMap<Mode>::sync() {
  if (!empty() && data() != nullptr) {
   ::msync(data(), length(), MS_SYNC);
   }
}

template <MMode Mode>
template <MMode M> requires (M == MMode::read)
void MMap<Mode>::sync() {
}

template <MMode Mode>
void MMap<Mode>::unmap() {
  if (!empty() && data() != nullptr) {
    ::munmap(_data, length());
    _data = nullptr;
    _length = 0;
  }
  if (is_open()) {
    ::close(handle());
    _handle = -1;
  }
}

template <MMode Mode>
MMap<Mode>::~MMap() {
  sync();
  unmap();
}

using ReadableMmap = MMap<MMode::read>;
using WritableMmap = MMap<MMode::write>;

}
