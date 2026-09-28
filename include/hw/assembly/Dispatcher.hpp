#pragma once
#include <thread>
#include <stdexcept>
#include <immintrin.h>
#include <string_view>

#include <hw/utility/CPU.hpp>
#include <hw/utility/Clock.hpp>
#include <hw/utility/Buffer.hpp>
#include <hw/utility/EPoller.hpp>
#include <hw/utility/Format.hpp>
#include <hw/type/NamedType.hpp>
#include <hw/type/TypeList.hpp>
#include <hw/assembly/Timer.hpp>

namespace hw::assembly {

using namespace boost::mp11;

struct EtherPlaceholder {
  constexpr std::string_view name_tag() { return "EtherPlaceholder"; }
  static constexpr bool SHARED_ETHER = false;
  static constexpr size_t REQUIRED_MEM_SIZE = 0;
  using MsgList = type::type_list<>;
  struct EtherMsg {};
  struct Cursor {
    Cursor(EtherPlaceholder &) {}
  };
  void initialize(uint8_t *, size_t , bool) {}
};

struct DispatcherWithTimer {};
struct DispatcherWithEpoll {};
struct DispatcherWithBatchEnd {};
struct DispatcherNonCritical {};
struct DefaultDispatcherTraits : DispatcherWithBatchEnd {};

template<type::NameTag Name, typename AppContext, typename Ether, typename ComponentList, typename Traits = DefaultDispatcherTraits>
class Dispatcher : public type::NamedType< Name, Dispatcher<Name, AppContext, Ether, ComponentList, Traits> > {

  static_assert(!mp_empty<ComponentList>::value, "One or more components are expected");
  static_assert(mp_is_set<ComponentList>::value, "Component list cannot have duplicates");

public:
  using Self = Dispatcher<Name, AppContext, Ether, ComponentList, Traits>;
  using AppContextType  = AppContext;
  using AssemblyType	  = AppContext::Assembly;
  using EtherType	      = Ether;
  using EtherMsg	      = Ether::EtherMsg;
  using EtherMsgList    = Ether::MsgList;
  using ComponentSet	  = mp_transform<type::make_unique_ptr_t, typename ComponentList::tuple_type>;
  using LocalClock      = utility::SystemClockTSC;
  using EPoller         = utility::EPoller;

  static constexpr size_t COMPONENT_CNT = mp_size<ComponentList>::value;
  static constexpr bool USING_ETHER = false == std::is_same_v<EtherType, EtherPlaceholder>;
  static constexpr bool USING_TIMER = std::is_base_of_v<DispatcherWithTimer, Traits>;
  static constexpr bool USING_EPOLL = std::is_base_of_v<DispatcherWithEpoll, Traits>;
  static constexpr bool USING_BATCH_END = std::is_base_of_v<DispatcherWithBatchEnd, Traits>;
  static constexpr bool USING_YIELD = std::is_base_of_v<DispatcherNonCritical, Traits>;

	Dispatcher(AssemblyType & assembly, AppContext & context, EtherType & ether, int core = -1)
    : _assembly(assembly), _context (context), _ether(ether), _cursor (ether),
      _clock(_assembly.clock()), _core(core), _name (Name)
  {
    if (USING_EPOLL) {
      _epoller = std::make_unique<EPoller> ();
    }

    mp_for_each<mp_iota_c<COMPONENT_CNT>>( [this] (auto idx) {
      using ComponentType = mp_at_c<ComponentList, idx>;
      std::get<idx>(_components).reset(new ComponentType(*this, _context));
    });
  }

  Dispatcher (const Dispatcher &) = delete;
  Dispatcher & operator = (const Dispatcher &) = delete;

  template <typename EtherType>
  std::shared_ptr<EtherType> getEther() {
    return _assembly.template getEther<EtherType>();
  }

  template <typename MsgType, typename ... Args>
  MsgType & allocMsg(Args &&... args) noexcept {
    return _cursor.template allocMsg<MsgType>(std::forward<Args>(args)...);
  }

	template <typename MsgType>
  bool commitMsg(MsgType & msg) noexcept {
    return _cursor.template commitMsg (msg) ;
  }

  void setTimer(std::chrono::system_clock::time_point when, std::function<void()> callback) {
    if(!_timers.scheduleAt(when, std:: move(callback))) {
      fatalExit("Failed to schedule timer: queue full");
    }
  }

  template <typename Rep, typename Period>
  void setTimer(TimerType type, std::chrono::duration<Rep, Period> wait, std::function<void()> callback) {
    if(!_timers.scheduleAfter(type, wait, std::move(callback))) {
      fatalExit("Failed to schedule timer: queue full");
    }
  }

  LocalClock & clock() const { return _clock; }

  void run (int core) {
    if (core >= 0) {
      if (utility::setCpuAffinity(core) != 0) {
        fatalExit(frmt::format("failed to set cpu-affinity to core: {}; errno: {}", core, errno));
      }
    }



    // 1024 for Epoll/BatchEnd (prioritize latency).
    // 2048 for Timer (moderate latency).
    // 65536 otherwise (prioritize throughput).
    constexpr size_t MAX_BATCH_LIMIT = USING_EPOLL || USING_BATCH_END ? 1024
                                     : USING_TIMER ? 2048 : 65536;

    size_t batchSize = 64;
    size_t maxBatchSize = MAX_BATCH_LIMIT;
    const size_t initialBatchSize = batchSize;

    try {
      processBegin();

      int msgRead = 0;

      while (!_stop) {
        if constexpr (USING_ETHER) {
          msgRead = poll(batchSize);
          if (msgRead < 0) [[unlikely]] {
            break;
          }
          if (_cursor.queueLength() > (batchSize << 3)) [[unlikely]] {
            batchSize = std::min(maxBatchSize, batchSize << 1);
          } else if (msgRead < batchSize && batchSize > initialBatchSize) [[unlikely]] {
            batchSize = std::max(initialBatchSize, batchSize >> 1);
          }
        }
        if constexpr (USING_EPOLL) {
          _epoller->poll() ;
        }
        if constexpr (USING_TIMER) {
          _timers.poll();
        }
        if constexpr (USING_BATCH_END) {
          processBatchEnd();
        }

        if (msgRead == 0) {
          if constexpr (USING_YIELD) {
            std::this_thread::yield();
          }
          else {
            _mm_pause();
          }
        }

        if constexpr (USING_ETHER) {
          if (msgRead < 0) [[unlikely]] {
            fatalExit(frmt::format("Ring buffer overflow; cursor.queueLength:{} batchSize:{}",
              _cursor.queueLength(), batchSize));
          }
        }

        processEnd();
      }
    }
    catch (const std::exception & ex) {
      fatalExit(ex.what());
    }
  }

  void start() {
    _thread = std::thread(&Self::run, this, _core);
  }

  void stop() {
    if (!_stop) {
      _stop = true;
      _thread.join();
    }
  }

  utility::EPoller & epoller() requires (USING_EPOLL) {
    return *_epoller;
  }

private:
  template <typename MsgType>
  void dispatchMsg(const MsgType & msg) noexcept {
    mp_for_each<mp_iota_c<COMPONENT_CNT>>( [this, &msg] (auto idx) {
      using ComponentType = mp_at_c<ComponentList, idx>;
      ComponentType & component = *std::get<idx>(_components);
      if constexpr (ComponentType::template ToCall<MsgType>::value) {
        component.template forwardMsg(msg);
      }
    });
  }

  int readMsg() noexcept {
    return _cursor.readMsg([this] (EtherMsg & msg) {
      std::visit([this, &msg] (auto && var) {
        using MsgType = std::remove_pointer_t<std::decay_t<decltype(var)>>;
        dispatchMsg(*reinterpret_cast<const MsgType*>(msg.data));
      }, msg.selector);
    });
  }

  __attribute__ ((flatten))  int poll(size_t maxcnt = 100'000) noexcept {
    size_t cnt = 0;
     while (cnt < maxcnt) [[likely]] {
      const int rc = readMsg();
      if (rc < 0) [[unlikely]] {
        return rc;
      }
      else if(rc == 0) {
        break;
      }
      ++cnt;
     }
    return cnt;
  }

  // to-do: look similar. to make generic maybe
  void processBegin () {
    mp_for_each<mp_iota_c<COMPONENT_CNT>>( [this] (auto idx) {
      using ComponentType = mp_at_c<ComponentList, idx>;
      ComponentType & component = *std::get<idx>(_components);
      component.processBegin();
    });
  }

  void processEnd () {
    mp_for_each<mp_iota_c<COMPONENT_CNT>>( [this] (auto idx) {
      using ComponentType = mp_at_c<ComponentList, idx>;
      ComponentType & component = *std::get<idx>(_components);
      component.processEnd();
    });
  }

  void processBatchEnd () {
    mp_for_each<mp_iota_c<COMPONENT_CNT>> ( [this] (auto idx) {
      using ComponentType = mp_at_c<ComponentList, idx>;
      ComponentType & component = *std::get<idx>(_components);
      component.processBatchEnd();
    });
  }

  void fatalExit(const std:: string & errmsg) {
    std::cerr << frmt::format ("Dispatcher '{}'  fatal error '{}'",  _name, errmsg) << std::endl;
    exit (1);
  }

  AssemblyType &            _assembly;
  AppContext &	            _context;
  EtherType &	              _ether;
  typename Ether::Cursor	  _cursor;
  LocalClock &	            _clock;
  ComponentSet	            _components;
  bool	                    _stop = false;
  std::thread	              _thread;
  const int	                _core;
  std::string	              _name;
  TimerQueue<1<<10>         _timers;
  std::unique_ptr<EPoller>  _epoller;
};

}

