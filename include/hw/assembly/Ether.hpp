#pragma once

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <functional>
#include <cstring>

#include <hw/type/NamedType.hpp>
#include <hw/type/TypeList.hpp>
#include <hw/utility/CPU.hpp>


namespace hw::assembly {

using namespace boost::mp11;

struct PrivateEther {};
struct SharedEther {};
struct DefaultEtherTraits : SharedEther {};


template <type::NameTag Name, typename MessageList, size_t MaxMsgCnt, typename Traits = DefaultEtherTraits>
class Ether : public type::NamedType< Name, Ether<Name, MessageList, MaxMsgCnt> > {
  static constexpr size_t MSG_INDEX_MASK = MaxMsgCnt - 1;
  static_assert(0 == (MaxMsgCnt & MSG_INDEX_MASK));
  static_assert(mp_is_list<MessageList>::value, "type list of message types is expected");
  static_assert(!mp_empty<MessageList>::value, "one or more message types are expected");
  static_assert (mp_is_set<MessageList>::value, "Type list cannot have duplicates");

public:
  using MsgList     = MessageList;
  using MsgSelector = mp_transform<std::add_pointer_t, typename MsgList::variant_type>;
  using SeqNo       = int64_t;

  struct alignas (ALIGNAS) EtherHdr {
    std::atomic<SeqNo>  seqno;
    uint64_t            signature;
    size_t              capacity;
  };

	struct alignas (ALIGNAS) EtherMsg {
    MsgSelector	        selector;
    std::atomic<SeqNo>  seqno;
    SeqNo               commitno;
	  alignas (ALIGNAS)
    uint8_t             data[MsgList::SIZE];
    static constexpr size_t DATA_OFFSET = offsetof(EtherMsg, data);
  };

  static constexpr bool SHARED_ETHER = std::is_base_of_v<SharedEther, Traits>;
  static constexpr SeqNo CAPACITY = static_cast<SeqNo> (MaxMsgCnt);
  static constexpr size_t MAX_MSG_SIZE = (sizeof(EtherMsg) + ALIGNAS) & ~ALIGNAS;
  static constexpr size_t REQUIRED_MEM_SIZE = MaxMsgCnt * MAX_MSG_SIZE + sizeof(EtherHdr);
  static constexpr size_t MSG_LIST_SIGNATURE = type::TypeListSignature<MsgList>();

  Ether() : _name(Name) {}
  Ether (const Ether &) = delete;
  Ether& operator = (const Ether &) = delete;

  void initialize(uint8_t *buffer, size_t size, bool reset = false) {
    assert (size >= REQUIRED_MEM_SIZE);
    _hdr = reinterpret_cast<EtherHdr *>(buffer);
    _data = reinterpret_cast<EtherMsg *>(buffer + sizeof (EtherHdr));

    if (reset) {
      std::memset(buffer, 0, REQUIRED_MEM_SIZE);
      _hdr->seqno = 0;
      _hdr->signature = MSG_LIST_SIGNATURE;
      _hdr->capacity = CAPACITY;
    }
    else if (_hdr->signature != MSG_LIST_SIGNATURE) {
      throw (std::invalid_argument(std::string("Ether signature mismatch :" + _name)));
    }
    else if (_hdr->capacity != CAPACITY) {
      throw (std::invalid_argument(std::string("Ether capacity mismatch: " + _name)));
    }
  }

  class Cursor {
  public:
    Cursor(Ether & ether) : _ether(ether), _hdr (*ether._hdr), _data(ether._data) {
      reset(_hdr.seqno.load(std::memory_order_acquire));
    }

    template<typename MsgType, typename ... Args>
    MsgType & allocMsg (Args &&... args) noexcept {
      static_assert(mp_contains<MsgList, MsgType>::value);
      SeqNo seqno = _hdr.seqno.load(std::memory_order_relaxed);
      while (!_hdr.seqno.compare_exchange_weak(
        seqno, seqno + 1, std::memory_order_release, std::memory_order_relaxed));
      ++seqno;
      Ether::EtherMsg & msg = _data[seqno & MSG_INDEX_MASK];
      msg.commitno = 0;
      msg.seqno.store(seqno, std::memory_order_release);
      std::memset(msg.data, 0, sizeof (MsgType));
      return *new (msg.data) MsgType(std::forward<Args>(args)...);
    }

    template<typename MsgType>
    bool commitMsg (MsgType & msg) noexcept {
      static_assert(mp_contains<MsgList, MsgType>::value);
      Ether::EtherMsg &emsg = *reinterpret_cast<EtherMsg *>(reinterpret_cast<uint8_t *>(&msg) - EtherMsg::DATA_OFFSET);
      emsg.selector = static_cast<MsgType *> (nullptr);
      emsg.commitno = emsg.seqno.load(std::memory_order_relaxed);
      return true;
    }

    int readMsg (std::function<void(EtherMsg &)> handler) noexcept {
      _lastSeqno = _hdr.seqno.load(std::memory_order_relaxed);
      if (_lastSeqno >= _nextSeqno) [[likely]] {
        if ((_lastSeqno - _nextSeqno) < CAPACITY) [[likely]] {
          Ether::EtherMsg &msg = _data[_nextSeqno & MSG_INDEX_MASK];
          if (_nextSeqno == msg.seqno.load(std:: memory_order_relaxed)) [[likely]] {
            if (_nextSeqno == msg.commitno) [[likely]] {
              handler (msg);
              ++ _nextSeqno;
              return 1;
            }
          }
        }
        else {
          return -1;
        }
      }
      return 0;
    }

    size_t queueLength() const noexcept {
      return _hdr.seqno.load(std::memory_order_relaxed) - _lastSeqno;
    }

  private:
    void reset(SeqNo lastSeqno) {
      _lastSeqno = lastSeqno;
      _nextSeqno = lastSeqno + 1;
    }

    friend class Ether;
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;

    Ether & _ether;
    SeqNo _nextSeqno;
    SeqNo _lastSeqno;
    Ether::EtherHdr & _hdr;
    Ether:: EtherMsg * const _data;
  };

private:
  friend class Cursor;
  EtherHdr * _hdr = nullptr;
  EtherMsg * _data = nullptr;
  const std::string _name;
};

}






